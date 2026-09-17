/*
 * Copyright 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 * 
 */

#include "baseobj.h"
#include "bitops.h"
#include "viogpum.h"
#include "viogpu_allocation.h"
#include "viogpu_adapter.h"
#include "viogpu_device.h"
#include "virgl_hw.h"

extern "C" NTSYSAPI VOID NTAPI KeAttachProcess(_Inout_ PRKPROCESS Process);
extern "C" NTSYSAPI VOID NTAPI KeDetachProcess(VOID);

static MEMORY_CACHING_TYPE VioGpuCacheTypeFromMapInfo(ULONG map_info)
{
    switch (map_info & VIRTIO_GPU_MAP_CACHE_MASK)
    {
        case VIRTIO_GPU_MAP_CACHE_CACHED:
            return MmCached;
        case VIRTIO_GPU_MAP_CACHE_UNCACHED:
            return MmNonCached;
        case VIRTIO_GPU_MAP_CACHE_WC:
            return MmWriteCombined;
        case VIRTIO_GPU_MAP_CACHE_NONE:
        default:
            return MmNonCached;
    }
}

static void VioGpuGetTargetProcess(VioGpuDevice *device, PEPROCESS *out_process, HANDLE *out_pid)
{
    if (device && device->GetOwnerProcess())
    {
        *out_process = device->GetOwnerProcess();
        *out_pid = device->GetOwnerProcessId();
    }
    else
    {
        *out_process = PsGetCurrentProcess();
        *out_pid = PsGetCurrentProcessId();
    }
}

void VioGpuAllocation::BlobCreateCompleteCB(void *ctx_void)
{
    PVIOGPU_COMPLETE_CTX ctx = (PVIOGPU_COMPLETE_CTX)ctx_void;
    if (!ctx) {
        return;
    }
    if (!ctx->owner) {
        delete ctx;
        return;
    }

    VioGpuAllocation *alloc = reinterpret_cast<VioGpuAllocation *>(ctx->owner);
    PGPU_CTRL_HDR resp = ctx->vbuf ? (PGPU_CTRL_HDR)ctx->vbuf->resp_buf : NULL;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    if (resp && resp->type < VIRTIO_GPU_RESP_ERR_UNSPEC) {
        status = STATUS_SUCCESS;
    }

    alloc->m_blob_create_status = status;
    if (NT_SUCCESS(status)) {
        alloc->m_blob_created = true;
        alloc->m_resource_created = 1;
        InterlockedExchange(&alloc->m_blob_create_state, 2);
    } else {
        alloc->m_blob_created = false;
        InterlockedExchange(&alloc->m_blob_create_state, 0);
    }

    KeSetEvent(&alloc->m_blob_create_complete_event, IO_NO_INCREMENT, FALSE);
    if (ctx->user_cb) {
        ctx->user_cb(ctx->user_ctx);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id=0x%x\n", __FUNCTION__, alloc->GetId()));

    delete ctx;
    ExReleaseRundownProtection(&alloc->m_blob_create_rundown);
}

VioGpuAllocation::VioGpuAllocation(VioGpuAdapter *adapter, VIOGPU_CREATE_ALLOCATION_EXCHANGE *exchange)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    m_adapter = adapter;
    m_Id = m_adapter->resourceIdr.GetId();
    memcpy(&m_options, &exchange->ResourceOptions, sizeof(VIOGPU_RESOURCE_OPTIONS));
    m_options.bind = VIRGL_BIND_FOR_TARGET(m_options.target, m_options.bind);
    m_is_blob = exchange->BlobMem != 0;
    m_valid = true;
    m_stride = exchange->Stride;
    m_scanout_offset = exchange->ScanoutOffset;
    m_blob_size = ALIGN_UP_BY(exchange->Size, PAGE_SIZE);
    m_blob_id = exchange->BlobId;
    m_blob_mem = exchange->BlobMem;
    m_blob_flags = exchange->BlobFlags;
    m_blob_offset = 0;
    m_blob_shmem_allocated = false;
    m_blob_map_info = 0;
    m_blob_mapped = false;
    m_blob_kernel_va = NULL;
    m_blob_kernel_map_size = 0;
    m_blob_created = false;
    m_blob_create_status = STATUS_PENDING;
    m_blob_create_state = 0;
    KeInitializeEvent(&m_blob_create_complete_event, NotificationEvent, TRUE);
    ExInitializeRundownProtection(&m_blob_create_rundown);
    ExInitializeFastMutex(&m_blob_map_mutex);
    InitializeListHead(&m_blob_map_list);
    m_blob_map_user_refs = 0;
    m_resource_created = 0;

    if (!m_is_blob)
    {
        m_adapter->ctrlQueue.CreateResource3D(m_Id, &m_options);
        m_resource_created = 1;
    }

    m_pMDL = NULL;
    m_pageCount = 0;
    m_pageOffset = 0;
    ExInitializeFastMutex(&m_cpu_copy_mutex);
    m_cpu_copy_va = NULL;
    m_cpu_copy_map_size = 0;
    m_DxPhysicalAddress = 0;

    KeInitializeEvent(&m_busyNotification, NotificationEvent, TRUE);
    m_busy = 0;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s res_id=%d\n", __FUNCTION__, m_Id));
}

UINT VioGpuAllocation::GetStride(void) const
{
    if (m_stride)
    {
        return m_stride;
    }

    switch (m_options.format)
    {
        case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
        case VIRGL_FORMAT_B8G8R8A8_SRGB:
        case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
        case VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM:
        case VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM:
        case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
        case VIRGL_FORMAT_R8G8B8A8_SRGB:
        case VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM:
        case VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM:
        case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
            return m_options.width * 4;
        default:
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("%s unsupported scanout format=%u, assuming 4 bytes per pixel\n",
                      __FUNCTION__,
                      m_options.format));
            return m_options.width * 4;
    }
}

NTSTATUS VioGpuAllocation::CreateBlobResource(UINT ctx_id,
                                              void (*complete_cb)(void *),
                                              void *complete_ctx)
{
    if (!ExAcquireRundownProtection(&m_blob_create_rundown))
    {
        return STATUS_DELETE_PENDING;
    }

    KeClearEvent(&m_blob_create_complete_event);
    LONG prev_state = InterlockedCompareExchange(&m_blob_create_state, 1, 0);
    if (prev_state == 1) {
        ExReleaseRundownProtection(&m_blob_create_rundown);
        return STATUS_PENDING;
    }
    if (prev_state == 2) {
        KeSetEvent(&m_blob_create_complete_event, IO_NO_INCREMENT, FALSE);
        NTSTATUS status = m_blob_created ? STATUS_SUCCESS : m_blob_create_status;
        ExReleaseRundownProtection(&m_blob_create_rundown);
        return status;
    }

    m_blob_create_status = STATUS_PENDING;

    NTSTATUS status = STATUS_UNSUCCESSFUL;

    PVIOGPU_COMPLETE_CTX ctx = new (NonPagedPoolNx) VIOGPU_COMPLETE_CTX();
    if (!ctx) {
        m_blob_create_status = STATUS_INSUFFICIENT_RESOURCES;
        InterlockedExchange(&m_blob_create_state, 0);
        KeSetEvent(&m_blob_create_complete_event, IO_NO_INCREMENT, FALSE);
        ExReleaseRundownProtection(&m_blob_create_rundown);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ctx->vbuf = NULL;
    ctx->user_cb = complete_cb;
    ctx->user_ctx = complete_ctx;
    ctx->owner = this;

    status = m_adapter->ctrlQueue.CreateResourceBlob(m_Id,
                                                        m_blob_size,
                                                        m_blob_mem,
                                                        m_blob_flags,
                                                        m_blob_id,
                                                        ctx_id,
                                                        nullptr,
                                                        0,
                                                        VioGpuAllocation::BlobCreateCompleteCB,
                                                        ctx);
    if (!NT_SUCCESS(status)) {
        m_blob_create_status = status;
        InterlockedExchange(&m_blob_create_state, 0);
        KeSetEvent(&m_blob_create_complete_event, IO_NO_INCREMENT, FALSE);
        delete ctx;
        ExReleaseRundownProtection(&m_blob_create_rundown);
        return status;
    }

    // The completion callback owns the rundown reference after the request is queued.
    return status;
}

void VioGpuAllocation::EnsureBlobCreated(ULONG ctx_id)
{
    if (!m_is_blob)
    {
        return;
    }

    if (!m_resource_created)
    {
        CreateBlobResource(ctx_id);
    }
}

NTSTATUS VioGpuAllocation::EnsureBlobCreatedAndWait(ULONG ctx_id)
{
    if (!m_is_blob)
    {
        return STATUS_SUCCESS;
    }

    EnsureBlobCreated(ctx_id);
    WaitForPendingBlobCreate();

    if (!m_blob_created)
    {
        return NT_SUCCESS(m_blob_create_status) ? STATUS_UNSUCCESSFUL : m_blob_create_status;
    }

    return STATUS_SUCCESS;
}

void VioGpuAllocation::WaitForPendingBlobCreate()
{
    if (!m_is_blob || InterlockedCompareExchange(&m_blob_create_state, 1, 1) != 1)
    {
        return;
    }

    ASSERT(KeGetCurrentIrql() <= APC_LEVEL);
    if (KeGetCurrentIrql() > APC_LEVEL)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s cannot wait for pending blob create at IRQL=%u res_id=0x%x\n",
                  __FUNCTION__,
                  KeGetCurrentIrql(),
                  m_Id));
        return;
    }

    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("%s waiting for pending blob create res_id=0x%x\n", __FUNCTION__, m_Id));
    KeWaitForSingleObject(&m_blob_create_complete_event, Executive, KernelMode, FALSE, NULL);
}

VioGpuAllocation::~VioGpuAllocation(void)
{
    DbgPrint(TRACE_LEVEL_INFORMATION, ("---> %s res_id=0x%x\n", __FUNCTION__, m_Id));

    ASSERT(m_busy == 0);
    ExWaitForRundownProtectionRelease(&m_blob_create_rundown);

    ExAcquireFastMutex(&m_blob_map_mutex);
    if (!IsListEmpty(&m_blob_map_list))
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s res_id=0x%x blob maps still active\n", __FUNCTION__, m_Id));
    }
    while (!IsListEmpty(&m_blob_map_list))
    {
        PLIST_ENTRY entry = RemoveHeadList(&m_blob_map_list);
        BlobUserMapping *map = CONTAINING_RECORD(entry, BlobUserMapping, ListEntry);
        if (m_blob_map_user_refs > 0)
        {
            m_blob_map_user_refs--;
        }
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s dropping map res_id=0x%x pid=%p process=%p user_va=%p size=0x%llx ref=%u\n",
                  __FUNCTION__,
                  m_Id,
                  map->ProcessId,
                  map->Process,
                  map->UserVa,
                  map->Size,
                  map->RefCount));
        if (map->UserVa)
        {
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("%s force unmap stale map res_id=0x%x user_va=%p pid=%p process=%p\n",
                     __FUNCTION__,
                      m_Id,
                      map->UserVa,
                      map->ProcessId,
                      map->Process));
            m_adapter->GetDxgkInterface()->DxgkCbUnmapMemory(
                m_adapter->GetDxgkInterface()->DeviceHandle, map->UserVa);
            map->UserVa = NULL;
        }
        if (map->Process)
        {
            ObDereferenceObject(map->Process);
            map->Process = NULL;
        }
        delete map;
    }
    m_blob_map_user_refs = 0;
    ExReleaseFastMutex(&m_blob_map_mutex);

    const bool resource_created = m_is_blob ? m_blob_created : (m_resource_created != 0);

    if (m_blob_kernel_va && m_blob_kernel_map_size)
    {
        MmUnmapIoSpace(m_blob_kernel_va, (SIZE_T)m_blob_kernel_map_size);
        m_blob_kernel_va = NULL;
        m_blob_kernel_map_size = 0;
    }

    if (resource_created && m_is_blob && m_blob_mapped)
    {
        m_adapter->ctrlQueue.ResourceUnmapBlob(m_Id);
    }

    if (m_blob_shmem_allocated)
    {
        m_adapter->FreeShmemRange(m_blob_offset, m_blob_size);
        m_blob_shmem_allocated = false;
    }

    if (resource_created)
    {
        DbgPrint(TRACE_LEVEL_VERBOSE, ("%s DestroyResource res_id=0x%x\n", __FUNCTION__, m_Id));
        m_adapter->ctrlQueue.DestroyResource(m_Id);
    }
    m_adapter->resourceIdr.PutId(m_Id);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void VioGpuAllocation::AttachBacking(MDL *pMDL, size_t pageCount, size_t pageOffset)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id=%d\n", __FUNCTION__, m_Id));

    m_pMDL = pMDL;
    m_pageCount = pageCount;
    m_pageOffset = pageOffset;
    ExAcquireFastMutex(&m_cpu_copy_mutex);
    m_cpu_copy_va = NULL;
    m_cpu_copy_map_size = 0;
    if (!m_is_blob && m_pMDL && m_pageCount)
    {
        PVOID base_va =
            MmGetSystemAddressForMdlSafe(m_pMDL,
                                         NormalPagePriority | MdlMappingNoExecute);
        if (base_va)
        {
            m_cpu_copy_va = (PUCHAR)base_va + (m_pageOffset * PAGE_SIZE);
            m_cpu_copy_map_size = (ULONGLONG)m_pageCount * PAGE_SIZE;
        }
        else
        {
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("%s MmGetSystemAddressForMdlSafe failed res_id=0x%x mdl=%p\n",
                      __FUNCTION__, m_Id, m_pMDL));
        }
    }
    ExReleaseFastMutex(&m_cpu_copy_mutex);

    GPU_MEM_ENTRY *ents = new (NonPagedPoolNx) GPU_MEM_ENTRY[pageCount];

    for (UINT i = 0; i < pageCount; i++)
    {
        ents[i].addr = MmGetMdlPfnArray(pMDL)[pageOffset + i] * PAGE_SIZE;
        ents[i].length = PAGE_SIZE;
        ents[i].padding = 0;
    }

    m_adapter->ctrlQueue.AttachBacking(m_Id, ents, (UINT)pageCount);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void VioGpuAllocation::DetachBacking()
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id=%d\n", __FUNCTION__, m_Id));

    ExAcquireFastMutex(&m_cpu_copy_mutex);
    m_cpu_copy_va = NULL;
    m_cpu_copy_map_size = 0;
    m_pMDL = NULL;
    m_pageCount = 0;
    m_pageOffset = 0;
    ExReleaseFastMutex(&m_cpu_copy_mutex);

    m_adapter->ctrlQueue.DetachBacking(m_Id);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

NTSTATUS VioGpuAllocation::MapForCpuCopy(ULONG ctx_id,
                                         PVOID *map_va,
                                         ULONGLONG *map_size,
                                         BOOLEAN *io_mapping)
{
    PAGED_CODE();

    if (!map_va || !map_size || !io_mapping)
    {
        return STATUS_INVALID_PARAMETER;
    }

    *map_va = NULL;
    *map_size = 0;
    *io_mapping = FALSE;

    if (m_is_blob)
    {
        NTSTATUS status = EnsureBlobCreatedAndWait(ctx_id);
        if (!NT_SUCCESS(status))
        {
            return status;
        }

        if (!(m_blob_flags & VIRTGPU_BLOB_FLAG_USE_MAPPABLE))
        {
            return STATUS_NOT_SUPPORTED;
        }

        const ULONGLONG map_len = ALIGN_UP_BY(m_blob_size, PAGE_SIZE);
        const ULONGLONG shmem_len = m_adapter->GetShmemLen();
        if (!map_len || m_blob_offset > shmem_len ||
            map_len > (shmem_len - m_blob_offset))
        {
            return STATUS_INVALID_PARAMETER;
        }

        ExAcquireFastMutex(&m_blob_map_mutex);
        if (!m_blob_mapped)
        {
            if (!m_adapter->ctrlQueue.ResourceMapBlob(m_Id, m_blob_offset,
                                                      &m_blob_map_info))
            {
                ExReleaseFastMutex(&m_blob_map_mutex);
                return STATUS_UNSUCCESSFUL;
            }
            m_blob_mapped = true;
        }

        if (!m_blob_kernel_va)
        {
            PHYSICAL_ADDRESS shmem_pa = {};
            if (!m_adapter->GetShmemCpuTranslatedAddress(&shmem_pa))
            {
                ExReleaseFastMutex(&m_blob_map_mutex);
                return STATUS_NOT_SUPPORTED;
            }
            shmem_pa.QuadPart += m_blob_offset;

            // Match the host and user mappings to avoid conflicting cache aliases.
            const MEMORY_CACHING_TYPE cache_type =
                VioGpuCacheTypeFromMapInfo(m_blob_map_info);
            DbgPrint(TRACE_LEVEL_VERBOSE,
                     ("%s kernel map res_id=0x%x map_info=0x%x cache_type=0x%x\n",
                      __FUNCTION__, m_Id, m_blob_map_info, cache_type));
            PVOID va = MmMapIoSpace(shmem_pa, (SIZE_T)map_len, cache_type);
            if (!va)
            {
                ExReleaseFastMutex(&m_blob_map_mutex);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            m_blob_kernel_va = va;
            m_blob_kernel_map_size = map_len;
        }

        *map_va = m_blob_kernel_va;
        *map_size = m_blob_kernel_map_size;
        *io_mapping = FALSE;
        ExReleaseFastMutex(&m_blob_map_mutex);
        return STATUS_SUCCESS;
    }

    ExAcquireFastMutex(&m_cpu_copy_mutex);
    if (!m_cpu_copy_va || !m_cpu_copy_map_size)
    {
        ExReleaseFastMutex(&m_cpu_copy_mutex);
        return STATUS_NOT_MAPPED_DATA;
    }

    *map_va = m_cpu_copy_va;
    *map_size = m_cpu_copy_map_size;
    *io_mapping = FALSE;
    ExReleaseFastMutex(&m_cpu_copy_mutex);
    return STATUS_SUCCESS;
}

void VioGpuAllocation::UnmapForCpuCopy(PVOID map_va,
                                       ULONGLONG map_size,
                                       BOOLEAN io_mapping)
{
    if (io_mapping && map_va && map_size)
    {
        MmUnmapIoSpace(map_va, (SIZE_T)map_size);
    }
}

void VioGpuAllocation::MarkBusy()
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s res_id=%d\n", __FUNCTION__, m_Id));

    InterlockedIncrement(&m_busy);
    KeClearEvent(&m_busyNotification);
}

void VioGpuAllocation::UnmarkBusy()
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s res_id=%d\n", __FUNCTION__, m_Id));

    if (InterlockedDecrement(&m_busy) == 0)
    {
        KeSetEvent(&m_busyNotification, IO_NO_INCREMENT, FALSE);
    }

    ASSERT(m_busy >= 0);
}

void VioGpuAllocation::FlushToScreen(UINT scan_id)
{
    UINT offset = m_is_blob ? GetScanoutOffset() : 0;
    FlushToScreen(scan_id, m_options.width, m_options.height, 0, 0,
                  m_options.format, GetStride(), offset);
}

void VioGpuAllocation::FlushToScreen(UINT scan_id,
                                     UINT width,
                                     UINT height,
                                     UINT x,
                                     UINT y,
                                     UINT format,
                                     UINT stride,
                                     UINT offset)
{
    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("---> %s res_id=%d scanout=%u rect=%ux%u+%u+%u format=%u stride=%u offset=%u\n",
              __FUNCTION__,
              m_Id,
              scan_id,
              width,
              height,
              x,
              y,
              format,
              stride,
              offset));

    if (!width)
    {
        width = m_options.width;
    }
    if (!height)
    {
        height = m_options.height;
    }
    if (!format)
    {
        format = m_options.format;
    }
    if (!stride)
    {
        stride = GetStride();
    }

    if (m_is_blob)
    {
        m_adapter->ctrlQueue.SetScanoutBlob(scan_id,
                                            m_Id,
                                            width,
                                            height,
                                            x,
                                            y,
                                            format,
                                            stride,
                                            offset);
    }
    else
    {
        m_adapter->ctrlQueue.SetScanout(scan_id, m_Id, width, height, x, y);
    }
    m_adapter->ctrlQueue.ResFlush(m_Id, width, height, x, y);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s res_id=%d\n", __FUNCTION__, m_Id));
}

PAGED_CODE_SEG_BEGIN

D3DDDIFORMAT VioGpuToD3DDDIColorFormat(ULONG format)
{
    PAGED_CODE();

    switch (format)
    {
        case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
        case VIRGL_FORMAT_B8G8R8A8_SRGB:
            return D3DDDIFMT_A8R8G8B8;
        case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
            return D3DDDIFMT_X8R8G8B8;
        case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
        case VIRGL_FORMAT_R8G8B8A8_SRGB:
            return D3DDDIFMT_A8B8G8R8;
        case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
            return D3DDDIFMT_X8B8G8R8;
    }
    DbgPrint(TRACE_LEVEL_ERROR, ("---> %s Unsupported color format %d\n", __FUNCTION__, format));
    return D3DDDIFMT_X8B8G8R8;
}

NTSTATUS VioGpuAllocation::GetStandardAllocationDriverData(DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *pStandardAllocation)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_ERROR, ("---> %s type=%d\n", __FUNCTION__, pStandardAllocation->StandardAllocationType));

    if (!pStandardAllocation->pResourcePrivateDriverData && !pStandardAllocation->pAllocationPrivateDriverData)
    {
        pStandardAllocation->ResourcePrivateDriverDataSize = sizeof(VIOGPU_CREATE_RESOURCE_EXCHANGE);
        pStandardAllocation->AllocationPrivateDriverDataSize = sizeof(VIOGPU_CREATE_ALLOCATION_EXCHANGE);
        return STATUS_SUCCESS;
    }

    VIOGPU_CREATE_ALLOCATION_EXCHANGE *allocationExchange = (VIOGPU_CREATE_ALLOCATION_EXCHANGE *)pStandardAllocation->pAllocationPrivateDriverData;

    allocationExchange->ResourceOptions.target = 2;
    allocationExchange->ResourceOptions.format = VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM;
    allocationExchange->ResourceOptions.bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW |
                                               VIRGL_BIND_DISPLAY_TARGET | VIRGL_BIND_SCANOUT;

    allocationExchange->ResourceOptions.width = 1024;
    allocationExchange->ResourceOptions.height = 768;
    allocationExchange->Stride = allocationExchange->ResourceOptions.width * 4;
    allocationExchange->ResourceOptions.depth = 1;

    allocationExchange->ResourceOptions.array_size = 1;
    allocationExchange->ResourceOptions.last_level = 0;
    allocationExchange->ResourceOptions.nr_samples = 0;
    allocationExchange->ResourceOptions.flags = 0;

    allocationExchange->BlobId = 0;
    allocationExchange->BlobMem = 0;
    allocationExchange->BlobFlags = 0;


    switch (pStandardAllocation->StandardAllocationType)
    {
        case D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE:
            {
                D3DKMDT_SHAREDPRIMARYSURFACEDATA *surfaceData = pStandardAllocation->pCreateSharedPrimarySurfaceData;
                //[in] UINT                           Width;
                //[in] UINT                           Height;
                //[in] D3DDDIFORMAT                   Format;

                allocationExchange->ResourceOptions.width = surfaceData->Width;
                allocationExchange->ResourceOptions.height = surfaceData->Height;
                allocationExchange->ResourceOptions.format = ColorFormat(surfaceData->Format);
                allocationExchange->Stride = surfaceData->Width * 4;
                allocationExchange->Size = (ULONGLONG)surfaceData->Width * (ULONGLONG)surfaceData->Height * 4;

                DbgPrint(TRACE_LEVEL_ERROR,
                         ("<--- %s shared primary surface: width=%d, height=%d, format=%d\n",
                          __FUNCTION__,
                          surfaceData->Width,
                          surfaceData->Height,
                          surfaceData->Format));
                return STATUS_SUCCESS;
            }

        case D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE:
            {
                D3DKMDT_SHADOWSURFACEDATA *surfaceData = pStandardAllocation->pCreateShadowSurfaceData;
                //[in] UINT                           Width;
                //[in] UINT                           Height;
                //[in] D3DDDIFORMAT                   Format;

                allocationExchange->ResourceOptions.width = surfaceData->Width;
                allocationExchange->ResourceOptions.height = surfaceData->Height;
                allocationExchange->ResourceOptions.format = ColorFormat(surfaceData->Format);
                allocationExchange->Stride = surfaceData->Width * 4;
                allocationExchange->Size = (ULONGLONG)surfaceData->Width * (ULONGLONG)surfaceData->Height * 4;

                allocationExchange->ResourceOptions.flags |= VIRGL_RESOURCE_FLAG_MAP_COHERENT;

                surfaceData->Pitch = surfaceData->Width * 4;
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("<--- %s shadow surface: width=%d, height=%d, format=%d\n",
                          __FUNCTION__,
                          surfaceData->Width,
                          surfaceData->Height,
                          surfaceData->Format));
                return STATUS_SUCCESS;
            }

        case D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE:
            {
                D3DKMDT_STAGINGSURFACEDATA *surfaceData = pStandardAllocation->pCreateStagingSurfaceData;
                //[in] UINT                           Width;
                //[in] UINT                           Height;
                //[in] D3DDDIFORMAT                   Format;

                allocationExchange->ResourceOptions.width = surfaceData->Width;
                allocationExchange->ResourceOptions.height = surfaceData->Height;
                allocationExchange->ResourceOptions.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
                allocationExchange->Stride = surfaceData->Width * 4;
                allocationExchange->Size = (ULONGLONG)surfaceData->Width * (ULONGLONG)surfaceData->Height * 4;

                allocationExchange->ResourceOptions.flags |= VIRGL_RESOURCE_FLAG_MAP_COHERENT;

                surfaceData->Pitch = surfaceData->Width * 4;
                DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s staging surface\n", __FUNCTION__));
                return STATUS_SUCCESS;
            }

        default:
            {
                DbgPrint(TRACE_LEVEL_FATAL, ("<--- Unknown standard allocation type \n"));
                return STATUS_NOT_SUPPORTED;
            }
    }
}

NTSTATUS VioGpuAllocation::DxgkCreateAllocation(VioGpuAdapter *adapter, DXGKARG_CREATEALLOCATION *pCreateAllocation)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    DXGK_ALLOCATIONINFO *allocationInfo = pCreateAllocation->pAllocationInfo;

    const SIZE_T legacyExchangeSize =
        FIELD_OFFSET(VIOGPU_CREATE_ALLOCATION_EXCHANGE, Stride);
    SIZE_T resourceExchangeSize = allocationInfo->PrivateDriverDataSize;

    if (max(resourceExchangeSize, pCreateAllocation->PrivateDriverDataSize) <
        legacyExchangeSize)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s private driver data is too small\n", __FUNCTION__));
        return STATUS_INVALID_PARAMETER;
    }

    VIOGPU_CREATE_ALLOCATION_EXCHANGE *resourceExchange = (VIOGPU_CREATE_ALLOCATION_EXCHANGE *)allocationInfo->pPrivateDriverData;
    ;
    if (pCreateAllocation->PrivateDriverDataSize > allocationInfo->PrivateDriverDataSize)
    {
        resourceExchange = (VIOGPU_CREATE_ALLOCATION_EXCHANGE *)pCreateAllocation->pPrivateDriverData;
        resourceExchangeSize = pCreateAllocation->PrivateDriverDataSize;
    }

    VIOGPU_CREATE_ALLOCATION_EXCHANGE localExchange = {};
    RtlCopyMemory(&localExchange,
                  resourceExchange,
                  min(resourceExchangeSize, sizeof(localExchange)));

    VioGpuAllocation *allocation = new (NonPagedPoolNx) VioGpuAllocation(adapter, &localExchange);
    if (!allocation->IsValid())
    {
        delete allocation;
        return STATUS_UNSUCCESSFUL;
    }
    const bool is_blob = allocation->IsBlob();
    if (is_blob && (!adapter->GetShmemLen()))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<--- %s blob allocation requires shared memory\n", __FUNCTION__));
        delete allocation;
        return STATUS_NOT_SUPPORTED;
    }

    const bool mappable_blob = is_blob &&
                               (allocation->GetBlobFlags() & VIRTGPU_BLOB_FLAG_USE_MAPPABLE);
    if (mappable_blob)
    {
        ULONGLONG alloc_size = ALIGN_UP_BY(localExchange.Size, PAGE_SIZE);
        ULONGLONG offset = 0;
        if (!adapter->AllocateShmemRange(alloc_size, PAGE_SIZE, &offset))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("<--- %s shmem alloc failed size=0x%llx shmem_len=0x%llx\n",
                      __FUNCTION__, alloc_size, adapter->GetShmemLen()));
            delete allocation;
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        allocation->m_blob_offset = offset;
        allocation->m_blob_shmem_allocated = true;
    }

    allocationInfo->hAllocation = allocation;

    if (pCreateAllocation->Flags.Resource)
    {
        // This driver does not keep separate resource state.
        // Reuse the allocation handle as the resource token.
        pCreateAllocation->hResource = reinterpret_cast<HANDLE>(allocation);
    }

    allocationInfo->Alignment = 0;
    allocationInfo->Size = (SIZE_T)localExchange.Size;
    allocationInfo->PitchAlignedSize = 0;
    allocationInfo->HintedBank.Value = 0;
    allocationInfo->AllocationPriority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
    UINT segment_id = is_blob ? 2 : 1;
    const UINT segment_mask = 1u << (segment_id - 1);
    allocationInfo->EvictionSegmentSet = segment_mask;
    allocationInfo->Flags.Value = 0;

    allocationInfo->PreferredSegment.Value = 0;
    allocationInfo->PreferredSegment.SegmentId0 = segment_id;
    allocationInfo->PreferredSegment.Direction0 = 0;

    allocationInfo->Flags.CpuVisible = TRUE;

    allocationInfo->HintedBank.Value = 0;
    allocationInfo->MaximumRenamingListLength = 0;
    allocationInfo->pAllocationUsageHint = NULL;
    allocationInfo->PhysicalAdapterIndex = 0;
    allocationInfo->PitchAlignedSize = 0;

    allocationInfo->SupportedReadSegmentSet = segment_mask;
    allocationInfo->SupportedWriteSegmentSet = segment_mask;

    if (segment_id == 2) {
        allocationInfo->Alignment = PAGE_SIZE;
        allocationInfo->Size = (SIZE_T)allocation->GetBlobSize();
        allocationInfo->EvictionSegmentSet = 0;
    }

    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("<--- %s res_id=0x%x size=%d\n", __FUNCTION__, allocation->GetId(), allocationInfo->Size));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAllocation::DescribeAllocation(DXGKARG_DESCRIBEALLOCATION *pDescribeAllocation)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d\n", __FUNCTION__, m_Id));

    pDescribeAllocation->Width = m_options.width;
    pDescribeAllocation->Height = m_options.height;
    pDescribeAllocation->PrivateDriverFormatAttribute = 0;

    pDescribeAllocation->Format = VioGpuToD3DDDIColorFormat(m_options.format);

    // this values are RANDOM
    pDescribeAllocation->MultisampleMethod.NumQualityLevels = 2;
    pDescribeAllocation->MultisampleMethod.NumSamples = 2;

    pDescribeAllocation->RefreshRate.Numerator = 148500000;
    pDescribeAllocation->RefreshRate.Denominator = 2475000;

    return STATUS_SUCCESS;
};

NTSTATUS VioGpuAllocation::MapApertureSegment(DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d\n", __FUNCTION__, m_Id));

    size_t pageCount = pBuildPagingBuffer->MapApertureSegment.NumberOfPages;
    size_t mdlPageOffset = pBuildPagingBuffer->MapApertureSegment.MdlOffset;

    MDL *pMdl = pBuildPagingBuffer->MapApertureSegment.pMdl;

    AttachBacking(pMdl, pageCount, mdlPageOffset);
    SetDxPhysicalAddress(pBuildPagingBuffer->MapApertureSegment.OffsetInPages * PAGE_SIZE);

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAllocation::UnmapApertureSegment(DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d\n", __FUNCTION__, m_Id));

    UNREFERENCED_PARAMETER(pBuildPagingBuffer);
    DetachBacking();
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAllocation::EscapeResourceInfo(VIOGPU_RES_INFO_REQ *resInfo)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d\n", __FUNCTION__, m_Id));

    resInfo->Id = m_Id;
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAllocation::EscapeResourceBusy(VIOGPU_RES_BUSY_REQ *resBusy)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d\n", __FUNCTION__, m_Id));

    while (resBusy->Wait && m_busy != 0)
    {
        KeWaitForSingleObject(&m_busyNotification, UserRequest, KernelMode, FALSE, NULL);

        LARGE_INTEGER wait;
        wait.QuadPart = -10LL;
        KeDelayExecutionThread(KernelMode, FALSE, &wait);
    }

    resBusy->IsBusy = m_busy != 0;

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAllocation::EscapeResourceSetScanoutBlob(VIOGPU_RES_SET_SCANOUT_BLOB_REQ *resScanout)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("<---> %s res_id=%d scanout=%u rect=%ux%u+%u+%u format=%u stride=%u offset=%u\n",
              __FUNCTION__,
              m_Id,
              resScanout ? resScanout->ScanoutId : 0,
              resScanout ? resScanout->Width : 0,
              resScanout ? resScanout->Height : 0,
              resScanout ? resScanout->X : 0,
              resScanout ? resScanout->Y : 0,
              resScanout ? resScanout->Format : 0,
              resScanout ? resScanout->Stride : 0,
              resScanout ? resScanout->Offset : 0));

    if (!resScanout)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (!m_is_blob)
    {
        return STATUS_NOT_SUPPORTED;
    }
    WaitForPendingBlobCreate();
    if (!m_blob_created)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s blob is not created res_id=0x%x status=0x%x\n",
                  __FUNCTION__, m_Id, m_blob_create_status));
        return NT_SUCCESS(m_blob_create_status) ? STATUS_UNSUCCESSFUL : m_blob_create_status;
    }

    FlushToScreen(resScanout->ScanoutId,
                  resScanout->Width,
                  resScanout->Height,
                  resScanout->X,
                  resScanout->Y,
                  resScanout->Format,
                  resScanout->Stride,
                  resScanout->Offset);
    return STATUS_SUCCESS;
}

VioGpuAllocation::BlobUserMapping *VioGpuAllocation::FindBlobMappingLocked(HANDLE process_id, PEPROCESS process)
{
    PAGED_CODE();

    for (PLIST_ENTRY entry = m_blob_map_list.Flink; entry != &m_blob_map_list; entry = entry->Flink)
    {
        BlobUserMapping *map = CONTAINING_RECORD(entry, BlobUserMapping, ListEntry);
        if (map->ProcessId == process_id)
        {
            if (process && map->Process && map->Process != process)
            {
                DbgPrint(TRACE_LEVEL_WARNING,
                         ("%s stale map for pid=%p: old_process=%p new_process=%p\n",
                          __FUNCTION__,
                          process_id,
                          map->Process,
                          process));
                continue;
            }
            return map;
        }
    }
    return NULL;
}

NTSTATUS VioGpuAllocation::EscapeResourceMapBlob(VIOGPU_RES_MAP_BLOB_REQ *resMap, VioGpuDevice *device)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=0x%x handle=0x%x\n", __FUNCTION__, m_Id, resMap ? resMap->ResHandle : 0));

    if (!resMap)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (!m_is_blob || !(m_blob_flags & VIRTGPU_BLOB_FLAG_USE_MAPPABLE))
    {
        return STATUS_NOT_SUPPORTED;
    }
    NTSTATUS blob_status = EnsureBlobCreatedAndWait(device ? device->GetId() : 0);
    if (!NT_SUCCESS(blob_status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s blob create failed status=0x%x res_id=0x%x\n",
                  __FUNCTION__, blob_status, m_Id));
        return blob_status;
    }

    const ULONGLONG requested_size = resMap->Size ? resMap->Size : m_blob_size;
    if (requested_size == 0 || requested_size > m_blob_size)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (resMap->Offset != 0 && resMap->Offset != m_blob_offset)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if ((m_blob_offset & (PAGE_SIZE - 1)) != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    const ULONGLONG map_len = ALIGN_UP_BY(requested_size, PAGE_SIZE);
    if (map_len == 0 || map_len > MAXULONG)
    {
        return STATUS_INVALID_PARAMETER;
    }

    PHYSICAL_ADDRESS shmem_pa = {};
    const ULONGLONG shmem_len = m_adapter->GetShmemLen();
    if (m_blob_offset > shmem_len || map_len > (shmem_len - m_blob_offset))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!m_adapter->GetShmemCpuTranslatedAddress(&shmem_pa))
    {
        return STATUS_NOT_SUPPORTED;
    }

    PEPROCESS target_process = NULL;
    HANDLE target_pid = NULL;
    VioGpuGetTargetProcess(device, &target_process, &target_pid);
    {
        PEPROCESS current_process = PsGetCurrentProcess();
        HANDLE current_pid = PsGetCurrentProcessId();
        DbgPrint(TRACE_LEVEL_VERBOSE,
                 ("%s target_pid=%p current_pid=%p target_process=%p current_process=%p\n",
                  __FUNCTION__,
                  target_pid,
                  current_pid,
                  target_process,
                  current_process));
    }

    ExAcquireFastMutex(&m_blob_map_mutex);
    PDXGKRNL_INTERFACE dxgk = m_adapter->GetDxgkInterface();
    BlobUserMapping *map = FindBlobMappingLocked(target_pid, target_process);
    if (map)
    {
        if (resMap->Size && resMap->Size != map->Size)
        {
            ExReleaseFastMutex(&m_blob_map_mutex);
            return STATUS_INVALID_PARAMETER;
        }
        map->RefCount++;
        m_blob_map_user_refs++;

        resMap->MapInfo = map->MapInfo;
        resMap->UserVa = (ULONGLONG)(ULONG_PTR)map->UserVa;
        resMap->Size = map->Size;
        resMap->Offset = m_blob_offset;

        ExReleaseFastMutex(&m_blob_map_mutex);
        return STATUS_SUCCESS;
    }

    MEMORY_CACHING_TYPE cache_type = MmNonCached;
    PVOID user_va = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (!m_blob_mapped)
    {
        if (!m_adapter->ctrlQueue.ResourceMapBlob(m_Id, m_blob_offset, &m_blob_map_info))
        {
            ExReleaseFastMutex(&m_blob_map_mutex);
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("<--- %s ResourceMapBlob failed res_id=0x%x offset=0x%llx\n",
                      __FUNCTION__, m_Id, m_blob_offset));
            return STATUS_UNSUCCESSFUL;
        }
        m_blob_mapped = true;
    }

    const MEMORY_CACHING_TYPE host_cache_type =
        VioGpuCacheTypeFromMapInfo(m_blob_map_info);

    /*
     * Honour the host-reported cache type.  The host reports write-combined,
     * which is fine for the writes the upload paths do but leaves every CPU
     * read uncached, and that is expensive: the UMD's draw-time index scan
     * measured ~142 ns per element, and a bulk copy of the same range reached
     * only ~21 MB/s.  Overriding this to MmCached made Sanctuary's D3D10 path
     * 5.4x faster (1.5 -> 8.1 fps) and left vertex/index data intact, but it
     * corrupted the display for every UMD client including DWM - striped,
     * partially stale scanlines, the signature of the guest mapping pages
     * write-back while the host maps them write-combined, which x86 does not
     * make coherent.  map_info is the host telling us the attribute it is
     * using; do not override it here.  Win back the read cost by not reading
     * this memory per draw instead (cache index bounds, or scan the frontend's
     * buffer_shadow).  Tried and reverted 2026-08-01.
     */
    cache_type = host_cache_type;

    PHYSICAL_ADDRESS map_pa = shmem_pa;
    map_pa.QuadPart += m_blob_offset;
    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("%s map_pa=0x%llx map_len=0x%llx cache_type=0x%x host_cache_type=0x%x shmem_pa=0x%llx shmem_len=0x%llx blob_offset=0x%llx\n",
              __FUNCTION__,
              map_pa.QuadPart,
              map_len,
              cache_type,
              host_cache_type,
              shmem_pa.QuadPart,
              shmem_len,
              m_blob_offset));
    const BOOLEAN in_io_space = FALSE;
    user_va = NULL;
    status = dxgk->DxgkCbMapMemory(dxgk->DeviceHandle,
                                   map_pa,
                                   (ULONG)map_len,
                                   in_io_space,
                                   TRUE,
                                   cache_type,
                                   &user_va);
    bool mapping_valid = NT_SUCCESS(status) && user_va;

#if DBG
    if (mapping_valid && target_process)
    {
        BOOLEAN attached = FALSE;
        if (target_process != PsGetCurrentProcess())
        {
            KeAttachProcess(reinterpret_cast<PRKPROCESS>(target_process));
            attached = TRUE;
        }

        BOOLEAN probe_ok = TRUE;
        __try
        {
            // Best-effort debug probe to ensure the mapped VA is readable.
            volatile const UCHAR first = *(volatile const UCHAR *)user_va;
            UNREFERENCED_PARAMETER(first);
            if (map_len > 1)
            {
                volatile const UCHAR last = *((volatile const UCHAR *)user_va + (map_len - 1));
                UNREFERENCED_PARAMETER(last);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            probe_ok = FALSE;
        }
        mapping_valid = probe_ok;
        if (!mapping_valid)
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("%s ERROR map probe failed user_va=%p map_len=0x%llx target_pid=%p target_process=%p\n",
                      __FUNCTION__,
                      user_va,
                      map_len,
                      target_pid,
                      target_process));
            VIOGPU_ASSERT_CHK(false);
        }

        if (attached)
        {
            KeDetachProcess();
        }
    }

    if (!mapping_valid && user_va)
    {
        BOOLEAN attached = FALSE;
        if (target_process && target_process != PsGetCurrentProcess())
        {
            KeAttachProcess(reinterpret_cast<PRKPROCESS>(target_process));
            attached = TRUE;
        }

        dxgk->DxgkCbUnmapMemory(dxgk->DeviceHandle, user_va);

        if (attached)
        {
            KeDetachProcess();
        }
    }
#endif

    if (!NT_SUCCESS(status) || !user_va || !mapping_valid)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s ERROR map failed res_id=0x%x status=0x%lx user_va=%p mapping_valid=%d map_pa=0x%llx map_len=0x%llx cache_type=0x%x\n",
                  __FUNCTION__,
                  m_Id,
                  status,
                  user_va,
                  mapping_valid ? 1 : 0,
                  map_pa.QuadPart,
                  map_len,
                  cache_type));
        if (m_blob_map_user_refs == 0 && m_blob_mapped && !m_blob_kernel_va)
        {
            m_adapter->ctrlQueue.ResourceUnmapBlob(m_Id);
            m_blob_mapped = false;
            m_blob_map_info = 0;
        }
        ExReleaseFastMutex(&m_blob_map_mutex);
        return (!NT_SUCCESS(status)) ? status : STATUS_UNSUCCESSFUL;
    }

    map = new (NonPagedPoolNx) BlobUserMapping();
    if (!map)
    {
        dxgk->DxgkCbUnmapMemory(dxgk->DeviceHandle, user_va);

        if (m_blob_map_user_refs == 0 && m_blob_mapped && !m_blob_kernel_va)
        {
            m_adapter->ctrlQueue.ResourceUnmapBlob(m_Id);
            m_blob_mapped = false;
            m_blob_map_info = 0;
        }
        ExReleaseFastMutex(&m_blob_map_mutex);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    map->Process = target_process;
    if (map->Process)
    {
        ObReferenceObject(map->Process);
    }
    map->ProcessId = target_pid;
    map->UserVa = user_va;
    map->Size = requested_size;
    map->MapInfo = m_blob_map_info;
    map->RefCount = 1;

    InsertTailList(&m_blob_map_list, &map->ListEntry);
    m_blob_map_user_refs++;

    resMap->MapInfo = map->MapInfo;
    resMap->UserVa = (ULONGLONG)(ULONG_PTR)map->UserVa;
    resMap->Size = map->Size;
    resMap->Offset = m_blob_offset;

    ExReleaseFastMutex(&m_blob_map_mutex);

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("%s res_id=0x%x handle=0x%x m_blob_offset=0x%llx cache_type=0x%x map_len=0x%llx\n",
              __FUNCTION__,
              m_Id,
              resMap->ResHandle,
               m_blob_offset,
              cache_type,
              map_len));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAllocation::EscapeResourceUnmapBlob(VIOGPU_RES_UNMAP_BLOB_REQ *resUnmap, VioGpuDevice *device)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=0x%x\n", __FUNCTION__, m_Id));

    if (!resUnmap)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (!m_is_blob || !(m_blob_flags & VIRTGPU_BLOB_FLAG_USE_MAPPABLE))
    {
        return STATUS_NOT_SUPPORTED;
    }
    PEPROCESS target_process = NULL;
    HANDLE target_pid = NULL;
    VioGpuGetTargetProcess(device, &target_process, &target_pid);

    ExAcquireFastMutex(&m_blob_map_mutex);
    BlobUserMapping *map = FindBlobMappingLocked(target_pid, target_process);
    if (!map || map->RefCount == 0)
    {
        ExReleaseFastMutex(&m_blob_map_mutex);
        return STATUS_INVALID_PARAMETER;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("%s unmap pid=%p process=%p user_va=%p ref=%u size=0x%llx\n",
              __FUNCTION__,
              target_pid,
              map->Process,
              map->UserVa,
              map->RefCount,
              map->Size));

    map->RefCount--;
    m_blob_map_user_refs--;

    if (map->RefCount == 0)
    {
        PVOID user_va = map->UserVa;
        RemoveEntryList(&map->ListEntry);
        DbgPrint(TRACE_LEVEL_VERBOSE, ("%s DxgkCbUnmapMemory res_id=0x%x user_va=%p\n", __FUNCTION__, m_Id, user_va));
        m_adapter->GetDxgkInterface()->DxgkCbUnmapMemory(
            m_adapter->GetDxgkInterface()->DeviceHandle, user_va);
        if (map->Process)
        {
            ObDereferenceObject(map->Process);
            map->Process = NULL;
        }
        delete map;
    }

    if (m_blob_map_user_refs == 0 && m_blob_mapped && !m_blob_kernel_va)
    {
        DbgPrint(TRACE_LEVEL_VERBOSE, ("%s ResourceUnmapBlob res_id=0x%x\n", __FUNCTION__, m_Id));
        m_adapter->ctrlQueue.ResourceUnmapBlob(m_Id);
        m_blob_mapped = false;
        m_blob_map_info = 0;

        if (!m_blob_shmem_allocated)
        {
            m_blob_offset = 0;
        }
    }

    ExReleaseFastMutex(&m_blob_map_mutex);

    return STATUS_SUCCESS;
}

PAGED_CODE_SEG_END
