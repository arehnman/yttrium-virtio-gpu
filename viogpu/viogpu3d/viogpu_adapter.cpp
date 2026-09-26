/*
 * Copyright 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 * 
 * Copyright (C) 2019-2020 Red Hat, Inc.
 *
 * Written By: Vadim Rozenfeld <vrozenfe@redhat.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "helper.h"
#include "driver.h"
#include "viogpu_adapter.h"
#include "baseobj.h"
#include "bitops.h"
#include "viogpum.h"
#include "viogpu_device.h"

typedef struct _VIOGPU_SUBMIT_ESCAPE_CTX
{
    void *cmd_buf;
} VIOGPU_SUBMIT_ESCAPE_CTX, *PVIOGPU_SUBMIT_ESCAPE_CTX;

static void SubmitEscapeCompleteCB(void *ctx)
{
    PVIOGPU_SUBMIT_ESCAPE_CTX submit_ctx = (PVIOGPU_SUBMIT_ESCAPE_CTX)ctx;
    if (submit_ctx)
    {
        delete submit_ctx;
    }
}

static UINT g_InstanceId = 0;

struct NOTIFY_CONTEXT
{
    DXGKRNL_INTERFACE *pDxgkInterface;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA *interrupt;
    BOOL triggerDpc;
};

static BOOLEAN NotifyInterruptSyncRoutine(PVOID ctxVoid)
{
    NOTIFY_CONTEXT *ctx = (NOTIFY_CONTEXT *)ctxVoid;
    if (!ctx || !ctx->pDxgkInterface || !ctx->interrupt)
    {
        return FALSE;
    }

    ctx->pDxgkInterface->DxgkCbNotifyInterrupt(ctx->pDxgkInterface->DeviceHandle, ctx->interrupt);
    if (ctx->triggerDpc)
    {
        ctx->pDxgkInterface->DxgkCbQueueDpc(ctx->pDxgkInterface->DeviceHandle);
    }

    return TRUE;
}

typedef struct _CTRLQUEUE_SYNCEXEC_CONTEXT
{
    VIOGPU_SYNC_EXEC_ROUTINE routine;
    void *routineCtx;
    BOOLEAN routineRet;
} CTRLQUEUE_SYNCEXEC_CONTEXT, *PCTRLQUEUE_SYNCEXEC_CONTEXT;

static __forceinline BOOLEAN IsFenceStrictlyNewer(UINT candidateFence, UINT lastFence)
{
    return static_cast<LONG>(candidateFence - lastFence) > 0;
}

static __forceinline BOOLEAN ShouldLogPreemptionSample(LONG count)
{
    return count <= 16 || (count & (count - 1)) == 0;
}

static BOOLEAN CtrlQueueSyncExecRoutine(PVOID ctxVoid)
{
    PCTRLQUEUE_SYNCEXEC_CONTEXT ctx = (PCTRLQUEUE_SYNCEXEC_CONTEXT)ctxVoid;
    ctx->routineRet = ctx->routine ? ctx->routine(ctx->routineCtx) : FALSE;
    return TRUE;
}

static BOOLEAN InterruptCloseBarrier(PVOID)
{
    return TRUE;
}

#define VIOGPU_WORK_THREAD_WAIT_LOG_INTERVAL_MS 5000

BOOLEAN VioGpuAdapter::ExecuteSynchronized(VIOGPU_SYNC_EXEC_ROUTINE routine, void *routineCtx)
{
    if (!routine)
    {
        return FALSE;
    }

    CTRLQUEUE_SYNCEXEC_CONTEXT syncCtx = {};
    syncCtx.routine = routine;
    syncCtx.routineCtx = routineCtx;
    syncCtx.routineRet = FALSE;

    BOOLEAN callbackRet = FALSE;
    const ULONG messageNumber = m_PciResources.IsMSIEnabled() ? 1 : 0;
    NTSTATUS status = m_DxgkInterface.DxgkCbSynchronizeExecution(m_DxgkInterface.DeviceHandle,
                                                                  CtrlQueueSyncExecRoutine,
                                                                  &syncCtx,
                                                                  messageNumber,
                                                                  &callbackRet);
    if (!NT_SUCCESS(status) || !callbackRet)
    {
        return FALSE;
    }

    return syncCtx.routineRet;
}

virtio_gpu_formats ColorFormat(UINT format)
{
    switch (format)
    {
        case D3DDDIFMT_A8R8G8B8:
            return VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
        case D3DDDIFMT_X8R8G8B8:
            return VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
        case D3DDDIFMT_A8B8G8R8:
            return VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM;
        case D3DDDIFMT_X8B8G8R8:
            return VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM;
    }
    DbgPrint(TRACE_LEVEL_ERROR, ("---> %s Unsupported color format %d\n", __FUNCTION__, format));
    return VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
}

PAGED_CODE_SEG_BEGIN

VioGpuAdapter::VioGpuAdapter(_In_ DEVICE_OBJECT *pPhysicalDeviceObject)
    : m_pPhysicalDevice(pPhysicalDeviceObject), m_MonitorPowerState(PowerDeviceD0), m_AdapterPowerState(PowerDeviceD0),
      commander(this), vidpn(this)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    *((UINT *)&m_Flags) = 0;
    RtlZeroMemory(&m_DxgkInterface, sizeof(m_DxgkInterface));
    RtlZeroMemory(&m_DeviceInfo, sizeof(m_DeviceInfo));
    RtlZeroMemory(&m_PointerShape, sizeof(m_PointerShape));
    m_VsyncInterruptEnabled = 1;
    KeInitializeSpinLock(&m_ctrlStageListLock);
    InitializeListHead(&m_ctrlStageReadyList);

    RtlZeroMemory(&m_VioDev, sizeof(m_VioDev));
    m_Id = g_InstanceId++;
    m_shmem_allocator.Init(0);
    m_PendingWorks = 0;
    m_InterruptsClosing = 1;
    RtlZeroMemory((void *)m_lastNotifiedFence, sizeof(m_lastNotifiedFence));
    RtlZeroMemory((void *)m_preemptSubmittedOutstanding, sizeof(m_preemptSubmittedOutstanding));
    RtlZeroMemory((void *)m_pendingPreemptionFence, sizeof(m_pendingPreemptionFence));
    m_preemptionRequestCount = 0;
    m_preemptionDeferredCount = 0;
    m_preemptionNotifyCount = 0;
    m_preemptionInvalidCount = 0;
    m_outOfOrderFenceDropCount = 0;
    KeInitializeEvent(&m_ConfigUpdateEvent, SynchronizationEvent, FALSE);
    m_bStopWorkThread = FALSE;
    m_pWorkThread = NULL;
    m_ResolutionEvent = NULL;
    m_ResolutionEventHandle = NULL;
    m_u32NumCapsets = 0;
    m_u32NumScanouts = 0;
    m_supportedCapsetIDs = 0;
#if VIOGPU_WDDM2
    m_pPageTableSegment = NULL;
    m_PageTableSegmentPA.QuadPart = 0;
    m_PageTableSegmentSize = 0;
    m_PageTableSegmentId = 0;
#endif

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VioGpuAdapter::~VioGpuAdapter(void)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    VioGpuAdapterClose();
    CloseResolutionEvent();
    HWClose();
#if VIOGPU_WDDM2
    FreePageTableSegment();
#endif
    m_Id = 0;
}

BOOLEAN VioGpuAdapter::CheckHardware()
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_GRAPHICS_DRIVER_MISMATCH;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PCI_COMMON_HEADER Header = {0};
    ULONG BytesRead;

    Status = m_DxgkInterface.DxgkCbReadDeviceSpace(m_DxgkInterface.DeviceHandle,
                                                   DXGK_WHICHSPACE_CONFIG,
                                                   &Header,
                                                   0,
                                                   sizeof(Header),
                                                   &BytesRead);

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("DxgkCbReadDeviceSpace failed with status 0x%X\n", Status));
        return FALSE;
    }
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("<--- %s VendorId = 0x%04X DeviceId = 0x%04X\n", __FUNCTION__, Header.VendorID, Header.DeviceID));
    if (Header.VendorID == REDHAT_PCI_VENDOR_ID && Header.DeviceID == 0x1050)
    {
        SetVgaDevice(Header.SubClass == PCI_SUBCLASS_VID_VGA_CTLR);
        return TRUE;
    }

    return FALSE;
}

#pragma warning(disable : 4702)
#if VIOGPU_WDDM2
/*
 * GpuMmu keeps page tables in a memory segment that VidMm can also map for the
 * CPU, and a virtio GPU has no VRAM to offer for one, so the driver reserves
 * contiguous system memory and describes it as a segment populated from system
 * memory.  16 MB holds the 128 KB root table plus roughly a hundred leaf
 * tables, which is several GB of mapped address space - far more than this
 * needs, and small enough to still be contiguously allocatable at start.
 */
#define VIOGPU_PAGE_TABLE_SEGMENT_SIZE (16 * 1024 * 1024)

NTSTATUS VioGpuAdapter::AllocatePageTableSegment(void)
{
    PAGED_CODE();

    if (m_pPageTableSegment != NULL)
    {
        return STATUS_SUCCESS;
    }

    PHYSICAL_ADDRESS Low;
    PHYSICAL_ADDRESS High;
    PHYSICAL_ADDRESS Boundary;

    Low.QuadPart = 0;
    High.QuadPart = MAXULONGLONG;
    Boundary.QuadPart = 0;

    m_pPageTableSegment = MmAllocateContiguousMemorySpecifyCache(VIOGPU_PAGE_TABLE_SEGMENT_SIZE,
                                                                 Low,
                                                                 High,
                                                                 Boundary,
                                                                 MmCached);

    if (m_pPageTableSegment == NULL)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s could not reserve %u bytes of contiguous memory for page tables\n",
                  __FUNCTION__, (UINT)VIOGPU_PAGE_TABLE_SEGMENT_SIZE));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(m_pPageTableSegment, VIOGPU_PAGE_TABLE_SEGMENT_SIZE);
    m_PageTableSegmentPA = MmGetPhysicalAddress(m_pPageTableSegment);
    m_PageTableSegmentSize = VIOGPU_PAGE_TABLE_SEGMENT_SIZE;

    /* Segment ids are one based and this segment follows the ones
     * QUERYSEGMENT already reports, so it can be named here rather than
     * depending on dxgkrnl asking for segments before page table levels. */
    CPciBar *pShmemBar = m_PciResources.GetPciBar(m_VioDev.shmem_bar);
    const bool bHasShmem = pShmemBar && m_VioDev.shmem_len;
    m_PageTableSegmentId = (bHasShmem ? 2 : 1) + 1;

    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("%s reserved %u bytes for page tables at physical %I64x\n",
              __FUNCTION__, (UINT)m_PageTableSegmentSize, m_PageTableSegmentPA.QuadPart));

    return STATUS_SUCCESS;
}

void VioGpuAdapter::FreePageTableSegment(void)
{
    PAGED_CODE();

    if (m_pPageTableSegment == NULL)
    {
        return;
    }

    MmFreeContiguousMemory(m_pPageTableSegment);
    m_pPageTableSegment = NULL;
    m_PageTableSegmentPA.QuadPart = 0;
    m_PageTableSegmentSize = 0;
    m_PageTableSegmentId = 0;
}
#endif

NTSTATUS VioGpuAdapter::StartDevice(_In_ DXGK_START_INFO *pDxgkStartInfo,
                                    _In_ DXGKRNL_INTERFACE *pDxgkInterface,
                                    _Out_ ULONG *pNumberOfViews,
                                    _Out_ ULONG *pNumberOfChildren)
{
    PAGED_CODE();

    NTSTATUS Status;
    VIOGPU_ASSERT(pDxgkStartInfo != NULL);
    VIOGPU_ASSERT(pDxgkInterface != NULL);
    VIOGPU_ASSERT(pNumberOfViews != NULL);
    VIOGPU_ASSERT(pNumberOfChildren != NULL);
    RtlCopyMemory(&m_DxgkInterface, pDxgkInterface, sizeof(m_DxgkInterface));

    Status = m_DxgkInterface.DxgkCbGetDeviceInformation(m_DxgkInterface.DeviceHandle, &m_DeviceInfo);
    if (!NT_SUCCESS(Status))
    {
        VIOGPU_LOG_ASSERTION1("DxgkCbGetDeviceInformation failed with status 0x%X\n", Status);
        return Status;
    }

    if (!CheckHardware())
    {
        Status = STATUS_NO_MEMORY;
        DbgPrint(TRACE_LEVEL_ERROR, ("StartDevice failed to allocate memory\n"));
        return Status;
    }

    Status = GetRegisterInfo();
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("GetRegisterInfo failed with status 0x%X\n", Status));
    }

    Status = HWInit(m_DeviceInfo.TranslatedResourceList);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("HWInit failed with status 0x%X\n", Status));
        return Status;
    }

#if VIOGPU_WDDM2
    /* Reserve the page table backing before any segment is reported: under
     * GpuMmu there is nowhere else for VidMm to keep page tables, so failing
     * here is worth saying out loud rather than starting without a segment
     * and letting VidMm fail silently later. */
    Status = AllocatePageTableSegment();
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("AllocatePageTableSegment failed with status 0x%X\n", Status));
        return Status;
    }
#endif

    Status = SetRegisterInfo(GetInstanceId(), 0);
    if (!NT_SUCCESS(Status))
    {
        VIOGPU_LOG_ASSERTION1("RegisterHWInfo failed with status 0x%X\n", Status);
        return Status;
    }

    Status = vidpn.Start(pNumberOfViews, pNumberOfChildren);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("VioGpuaVidPN::Start failed with status 0x%X\n", Status));
        VioGpuDbgBreak();
        return STATUS_UNSUCCESSFUL;
    }

    m_Flags.DriverStarted = TRUE;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAdapter::StopDevice(VOID)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));
    m_Flags.DriverStarted = FALSE;
    vidpn.StopVsyncTimer();
    StopWorkThread();
    vidpn.Stop();
    VioGpuAdapterClose();
#if VIOGPU_WDDM2
    FreePageTableSegment();
#endif
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAdapter::DispatchIoRequest(_In_ ULONG VidPnSourceId, _In_ VIDEO_REQUEST_PACKET *pVideoRequestPacket)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(VidPnSourceId);
    UNREFERENCED_PARAMETER(pVideoRequestPacket);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s\n", __FUNCTION__));

    return STATUS_SUCCESS;
}

PCHAR
DbgDevicePowerString(__in DEVICE_POWER_STATE Type)
{
    PAGED_CODE();

    switch (Type)
    {
        case PowerDeviceUnspecified:
            return "PowerDeviceUnspecified";
        case PowerDeviceD0:
            return "PowerDeviceD0";
        case PowerDeviceD1:
            return "PowerDeviceD1";
        case PowerDeviceD2:
            return "PowerDeviceD2";
        case PowerDeviceD3:
            return "PowerDeviceD3";
        case PowerDeviceMaximum:
            return "PowerDeviceMaximum";
        default:
            return "UnKnown Device Power State";
    }
}

PCHAR
DbgPowerActionString(__in POWER_ACTION Type)
{
    PAGED_CODE();

    switch (Type)
    {
        case PowerActionNone:
            return "PowerActionNone";
        case PowerActionReserved:
            return "PowerActionReserved";
        case PowerActionSleep:
            return "PowerActionSleep";
        case PowerActionHibernate:
            return "PowerActionHibernate";
        case PowerActionShutdown:
            return "PowerActionShutdown";
        case PowerActionShutdownReset:
            return "PowerActionShutdownReset";
        case PowerActionShutdownOff:
            return "PowerActionShutdownOff";
        case PowerActionWarmEject:
            return "PowerActionWarmEject";
        default:
            return "UnKnown Device Power State";
    }
}

NTSTATUS VioGpuAdapter::SetPowerState(_In_ ULONG HardwareUid,
                                      _In_ DEVICE_POWER_STATE DevicePowerState,
                                      _In_ POWER_ACTION ActionType)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_FATAL,
             ("---> %s HardwareUid = 0x%x ActionType = %s DevicePowerState = %s AdapterPowerState = %s\n",
              __FUNCTION__,
              HardwareUid,
              DbgPowerActionString(ActionType),
              DbgDevicePowerString(DevicePowerState),
              DbgDevicePowerString(m_AdapterPowerState)));

    if (HardwareUid == DISPLAY_ADAPTER_HW_ID)
    {
        NTSTATUS status = STATUS_SUCCESS;

        if (DevicePowerState == PowerDeviceD0)
        {
            status = vidpn.AcquirePostDisplayOwnership();
            if (!NT_SUCCESS(status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("%s AcquirePostDisplayOwnership failed: 0x%x\n", __FUNCTION__, status));
                return status;
            }

            if (m_AdapterPowerState == PowerDeviceD3)
            {
                DXGKARG_SETVIDPNSOURCEVISIBILITY Visibility;
                Visibility.VidPnSourceId = D3DDDI_ID_ALL;
                Visibility.Visible = FALSE;
                status = vidpn.SetVidPnSourceVisibility(&Visibility);
                if (!NT_SUCCESS(status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s SetVidPnSourceVisibility failed: 0x%x\n", __FUNCTION__, status));
                    return status;
                }
            }
        }

        switch (DevicePowerState)
        {
            case PowerDeviceUnspecified:
            case PowerDeviceD0:
                {
                    status = VioGpuAdapterInit();
                    if (NT_SUCCESS(status))
                    {
                        status = StartWorkThread();
                        if (NT_SUCCESS(status))
                        {
                            vidpn.StartVsyncTimer();
                        }
                        else
                        {
                            VioGpuAdapterClose();
                        }
                    }
                }
                break;
            case PowerDeviceD1:
            case PowerDeviceD2:
            case PowerDeviceD3:
                {
                    vidpn.StopVsyncTimer();
                    StopWorkThread();
                    vidpn.Powerdown();
                    VioGpuAdapterClose();
                }
                break;
            default:
                return STATUS_INVALID_PARAMETER;
        }

        if (!NT_SUCCESS(status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("%s failed to switch to %s: 0x%x\n",
                      __FUNCTION__,
                      DbgDevicePowerString(DevicePowerState),
                      status));
            return status;
        }

        m_AdapterPowerState = DevicePowerState;
        return STATUS_SUCCESS;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
VioGpuAdapter::QueryChildRelations(_Out_writes_bytes_(ChildRelationsSize) DXGK_CHILD_DESCRIPTOR *pChildRelations,
                                   _In_ ULONG ChildRelationsSize)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    VIOGPU_ASSERT(pChildRelations != NULL);

    ULONG ChildRelationsCount = (ChildRelationsSize / sizeof(DXGK_CHILD_DESCRIPTOR)) - 1;
    VIOGPU_ASSERT(ChildRelationsCount <= MAX_CHILDREN);

    for (UINT ChildIndex = 0; ChildIndex < ChildRelationsCount; ++ChildIndex)
    {
        pChildRelations[ChildIndex].ChildDeviceType = TypeVideoOutput;
        pChildRelations[ChildIndex].ChildCapabilities.HpdAwareness = IsVgaDevice() ? HpdAwarenessAlwaysConnected
                                                                                   : HpdAwarenessInterruptible;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = IsVgaDevice() ? D3DKMDT_VOT_INTERNAL
                                                                                                           : D3DKMDT_VOT_HD15;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
        pChildRelations[ChildIndex].AcpiUid = 0;
        pChildRelations[ChildIndex].ChildUid = ChildIndex;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAdapter::QueryChildStatus(_Inout_ DXGK_CHILD_STATUS *pChildStatus, _In_ BOOLEAN NonDestructiveOnly)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    UNREFERENCED_PARAMETER(NonDestructiveOnly);
    VIOGPU_ASSERT(pChildStatus != NULL);
    VIOGPU_ASSERT(pChildStatus->ChildUid < MAX_CHILDREN);

    switch (pChildStatus->Type)
    {
        case StatusConnection:
            {
                pChildStatus->HotPlug.Connected = IsDriverActive();
                return STATUS_SUCCESS;
            }

        case StatusRotation:
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("Child status being queried for StatusRotation even though D3DKMDT_MOA_NONE was reported"));
                return STATUS_INVALID_PARAMETER;
            }

        default:
            {
                DbgPrint(TRACE_LEVEL_WARNING, ("Unknown pChildStatus->Type (0x%I64x) requested.", pChildStatus->Type));
                return STATUS_NOT_SUPPORTED;
            }
    }
}

NTSTATUS VioGpuAdapter::QueryDeviceDescriptor(_In_ ULONG ChildUid, _Inout_ DXGK_DEVICE_DESCRIPTOR *pDeviceDescriptor)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pDeviceDescriptor != NULL);
    VIOGPU_ASSERT(ChildUid < MAX_CHILDREN);
    PBYTE edid = vidpn.GetEdidData(ChildUid);

    if (!edid)
    {
        return STATUS_GRAPHICS_CHILD_DESCRIPTOR_NOT_SUPPORTED;
    }
    else if (pDeviceDescriptor->DescriptorOffset < EDID_RAW_BLOCK_SIZE)
    {
        ULONG len = min(pDeviceDescriptor->DescriptorLength,
                        (EDID_RAW_BLOCK_SIZE - pDeviceDescriptor->DescriptorOffset));
        RtlCopyMemory(pDeviceDescriptor->DescriptorBuffer, (edid + pDeviceDescriptor->DescriptorOffset), len);
        pDeviceDescriptor->DescriptorLength = len;
        return STATUS_SUCCESS;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
}

NTSTATUS VioGpuAdapter::QueryAdapterInfo(_In_ CONST DXGKARG_QUERYADAPTERINFO *pQueryAdapterInfo)
{
    PAGED_CODE();

    VIOGPU_ASSERT(pQueryAdapterInfo != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    switch (pQueryAdapterInfo->Type)
    {
        case DXGKQAITYPE_UMDRIVERPRIVATE:
            {
                if (pQueryAdapterInfo->OutputDataSize < sizeof(VIOGPU_ADAPTERINFO))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pQueryAdapterInfo->OutputDataSize (0x%u) is smaller than sizeof(VIOGPU_ADAPTERINFO) "
                              "(0x%u)\n",
                              pQueryAdapterInfo->OutputDataSize,
                              sizeof(VIOGPU_ADAPTERINFO)));
                    return STATUS_BUFFER_TOO_SMALL;
                }
                VIOGPU_ADAPTERINFO *info = (VIOGPU_ADAPTERINFO *)pQueryAdapterInfo->pOutputData;
                info->IamVioGPU = VIOGPU_IAM;
                info->Flags.Supports3d =
                   virtio_is_feature_enabled(m_u64HostFeatures, VIRTIO_GPU_F_VIRGL);
                /* Driver implements the capset query fix; gate it on 3D */
                info->Flags.has_capset_query_fix = info->Flags.Supports3d;
                info->Flags.has_context_init =
                   virtio_is_feature_enabled(m_u64GuestFeatures, VIRTIO_GPU_F_CONTEXT_INIT);
                info->Flags.has_host_visible =
                   (m_VioDev.shmem_len && m_PciResources.GetPciBar(m_VioDev.shmem_bar));
                info->Flags.has_resource_assign_uuid =
                   virtio_is_feature_enabled(m_u64HostFeatures, VIRTIO_GPU_F_RESOURCE_UUID);
                info->Flags.has_resource_blob =
                   virtio_is_feature_enabled(m_u64HostFeatures, VIRTIO_GPU_F_RESOURCE_BLOB);
                info->Flags.Reserved = 0;
                info->Flags.requires_explicit_residency = VIOGPU_WDDM2 ? 1 : 0;
                info->SupportedCapsetIDs = m_supportedCapsetIDs;
                return STATUS_SUCCESS;
            }
        case DXGKQAITYPE_DRIVERCAPS:
            {
                if (!pQueryAdapterInfo->OutputDataSize)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pQueryAdapterInfo->OutputDataSize (0x%u) is smaller than sizeof(DXGK_DRIVERCAPS) "
                              "(0x%u)\n",
                              pQueryAdapterInfo->OutputDataSize,
                              sizeof(DXGK_DRIVERCAPS)));
                    return STATUS_BUFFER_TOO_SMALL;
                }

                DXGK_DRIVERCAPS *pDriverCaps = (DXGK_DRIVERCAPS *)pQueryAdapterInfo->pOutputData;
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("InterruptMessageNumber = %d, WDDMVersion = %d\n",
                          pDriverCaps->InterruptMessageNumber,
                          pDriverCaps->WDDMVersion));
                RtlZeroMemory(pDriverCaps, pQueryAdapterInfo->OutputDataSize /*sizeof(DXGK_DRIVERCAPS)*/);
                /*
                 * D3D12 needs WDDM 2.0.  Everything the D3D12 runtime asks of
                 * a user mode driver already works on 1.3 - it loads the
                 * driver, negotiates the DDI, creates the device and accepts
                 * every capability - and then fails creating a paging queue,
                 * which 1.3 has no notion of.  Declaring 2.0 is the first step
                 * in finding out what dxgkrnl actually requires beyond that;
                 * it is not on its own a claim that this driver implements the
                 * 2.0 memory model.
                 */
#if VIOGPU_WDDM2
                pDriverCaps->WDDMVersion = DXGKDDI_WDDMv2;
#else
                pDriverCaps->WDDMVersion = DXGKDDI_WDDMv1_3;
#endif

                /*
                 * Declaring the version alone is not enough: dxgkrnl still
                 * refuses D3DKMTCreatePagingQueue with STATUS_NOT_IMPLEMENTED
                 * because these caps are zero, and a WDDM 2.0 driver has to
                 * say how the GPU addresses memory.
                 *
                 * IoMmu rather than GpuMmu, because there is no GPU here whose
                 * page tables we could own - the device reaches guest memory
                 * through the platform, so system physical addressing is what
                 * is actually happening, and it spares the driver a page table
                 * implementation it would only be pretending to have.
                 */
                /* WDDM 2.0 experiment, off: see git history. */
#if VIOGPU_WDDM2
                pDriverCaps->MemoryManagementCaps.VirtualAddressingSupported = 1;
                pDriverCaps->MemoryManagementCaps.GpuMmuSupported = 1;
#endif
                pDriverCaps->HighestAcceptableAddress.QuadPart = (ULONG64)-1;

                /*
                 * Zeroed PresentationCaps declares neither GDI hardware
                 * acceleration nor CDD/DWM interop, leaving win32k no driver
                 * path for windowed blt-model presents; it falls back to its
                 * own software engine, where win32kfull!vDirectStretch32
                 * accounts for ~99% of a DXGI_SWAP_EFFECT_DISCARD present.
                 * Interop is the accurate declaration for us - we accelerate
                 * no GDI operations.  The GDI blt acceleration bits
                 * (SupportAllBltRops, SupportMirrorStretchBlt,
                 * SupportMonoStretchBltModes) stay unset: they promise GDI
                 * hardware DDIs this driver does not implement.
                 */
                pDriverCaps->PresentationCaps.DriverSupportsCddDwmInterop = 1;

                /* Blt surface pitch alignment is (1 << AlignmentShift) bytes;
                 * the DDI requires >= 2, so zero was out of spec. */
                pDriverCaps->PresentationCaps.AlignmentShift = 2;

                /* Max blt texture is 2 ^ (shift + DXGK_TEXTURE_SIZE_SHIFT):
                 * 2048 at shift 0, 4096 at shift 1. */
                pDriverCaps->PresentationCaps.MaxTextureWidthShift = 1;
                pDriverCaps->PresentationCaps.MaxTextureHeightShift = 1;

                pDriverCaps->FlipCaps.FlipOnVSyncMmIo = TRUE;

                pDriverCaps->MaxQueuedFlipOnVSync = 1;

                /* FlipIndependent required on WDDM 1.3 */
                pDriverCaps->FlipCaps.FlipIndependent = 1;

                pDriverCaps->MemoryManagementCaps.SectionBackedPrimary = TRUE;

                pDriverCaps->SupportDirectFlip = 1;
                pDriverCaps->SchedulingCaps.MultiEngineAware = 1;
                pDriverCaps->SchedulingCaps.PreemptionAware = 1;
                pDriverCaps->PreemptionCaps.GraphicsPreemptionGranularity =
                    D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
                pDriverCaps->PreemptionCaps.ComputePreemptionGranularity =
                    D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;

                pDriverCaps->GpuEngineTopology.NbAsymetricProcessingNodes =
                    VIOGPU_EXECUTION_NODE_COUNT;

                pDriverCaps->SupportSmoothRotation = FALSE;
                pDriverCaps->SupportNonVGA = IsVgaDevice();

                // Disable pointer on viogpu3d for now
                // if (IsPointerEnabled()) {
                //    pDriverCaps->MaxPointerWidth = POINTER_SIZE;
                //    pDriverCaps->MaxPointerHeight = POINTER_SIZE;
                //    pDriverCaps->PointerCaps.Value = 0;
                //    pDriverCaps->PointerCaps.Color = 1;
                //}

                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s Driver caps return\n", __FUNCTION__));
                return STATUS_SUCCESS;
            }
        case DXGKQAITYPE_QUERYSEGMENT3:
            {
                if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT3))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pQueryAdapterInfo->OutputDataSize (0x%u) is smaller than sizeof(DXGK_QUERYSEGMENTOUT) "
                              "(0x%u)\n",
                              pQueryAdapterInfo->OutputDataSize,
                              sizeof(DXGK_QUERYSEGMENTOUT)));
                    return STATUS_BUFFER_TOO_SMALL;
                }

                DbgPrint(TRACE_LEVEL_ERROR, ("QUERY SEG\n"));
                DXGK_QUERYSEGMENTOUT3 *pSegmentInfo = (DXGK_QUERYSEGMENTOUT3 *)pQueryAdapterInfo->pOutputData;
                ULONGLONG shmem_len = m_VioDev.shmem_len;
                CPciBar *shmem_bar = m_PciResources.GetPciBar(m_VioDev.shmem_bar);
                const bool has_shmem = shmem_bar && shmem_len;
                if (!pSegmentInfo[0].pSegmentDescriptor)
                {
                    pSegmentInfo->NbSegment = has_shmem ? 2 : 1;
                }
                else
                {
                    const UINT segment_count = has_shmem ? 2 : 1;
                    DXGK_SEGMENTDESCRIPTOR3 *pSegmentDesc = pSegmentInfo->pSegmentDescriptor;
                    memset(&pSegmentDesc[0], 0, sizeof(pSegmentDesc[0]) * segment_count);

                    pSegmentInfo->PagingBufferPrivateDataSize = 0;

                    /* keep paging buffers in segment 1 */
                    pSegmentInfo->PagingBufferSegmentId = 1;
                    pSegmentInfo->PagingBufferSize = 10 * PAGE_SIZE;

                    /* Segment 1: framebuffer/aperture */
                    ULONGLONG segment1_base = 0xC0000000;
                    if (has_shmem)
                    {
                        ULONGLONG min_base = ALIGN_UP_BY(shmem_len, PAGE_SIZE);
                        if (segment1_base < min_base)
                        {
                            segment1_base = min_base;
                        }
                    }
                    pSegmentDesc[0].BaseAddress.QuadPart = segment1_base;
                    pSegmentDesc[0].Flags.Aperture = TRUE;
                    pSegmentDesc[0].Flags.CacheCoherent = TRUE;
                    pSegmentDesc[0].Flags.CpuVisible = FALSE;
                    pSegmentDesc[0].Size = 256 * 1024 * 4096;
                    pSegmentDesc[0].CommitLimit = 256 * 1024 * 4096;
                    pSegmentDesc[0].Flags.DirectFlip = TRUE;

                    if (has_shmem) 
                    {
                        //Segment 2: BAR-backed shared memory (CPU-visible)
                        pSegmentDesc[1].BaseAddress.QuadPart = 0;

                        pSegmentDesc[1].Flags.Aperture = TRUE;
                        //pSegmentDesc[1].Flags.Aperture = FALSE;

                        /* Declaring this segment coherent made no measurable
                         * difference to the windowed blt-model present cost
                         * (tried 2026-07-31); the CPU mapping attribute comes
                         * from the per-allocation flags, not from here. */
                        pSegmentDesc[1].Flags.CacheCoherent = FALSE;
                        pSegmentDesc[1].Flags.CpuVisible = TRUE;
                        pSegmentDesc[1].Flags.DirectFlip = FALSE;

                        PHYSICAL_ADDRESS shmem_pa = shmem_bar->GetPA();
                        shmem_pa.QuadPart += m_VioDev.shmem_offset;
                        pSegmentDesc[1].CpuTranslatedAddress = shmem_pa;
                        pSegmentDesc[1].Size = (SIZE_T)shmem_len;
                        pSegmentDesc[1].CommitLimit = (SIZE_T)shmem_len;
                    }

                    for (UINT i=0; i<segment_count; i++) 
                    {
                        DbgPrint(TRACE_LEVEL_VERBOSE, ("%s pSegmentDesc[%d].BaseAddress=%llx\n", __FUNCTION__, i, pSegmentDesc[i].BaseAddress.QuadPart));
                        DbgPrint(TRACE_LEVEL_VERBOSE, ("%s pSegmentDesc[%d].CpuTranslatedAddress=%llx\n", __FUNCTION__, i, pSegmentDesc[i].CpuTranslatedAddress.QuadPart));
                        DbgPrint(TRACE_LEVEL_VERBOSE, ("%s pSegmentDesc[%d].Size=%zx\n", __FUNCTION__, i, pSegmentDesc[i].Size));
                        DbgPrint(TRACE_LEVEL_VERBOSE, ("%s pSegmentDesc[%d].Flags: Aperture=%u CpuVisible=%u CacheCoherent=%u DirectFlip=%u\n", __FUNCTION__, i, pSegmentDesc[i].Flags.Aperture, pSegmentDesc[i].Flags.CpuVisible, pSegmentDesc[i].Flags.CacheCoherent, pSegmentDesc[i].Flags.DirectFlip));
                    }
                }
                
                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s Requested segments\n", __FUNCTION__));
                return STATUS_SUCCESS;
            }

#if VIOGPU_WDDM2
        case DXGKQAITYPE_QUERYSEGMENT4:
            {
                /*
                 * The WDDM 2.0 form of QUERYSEGMENT3, and the query dxgkrnl
                 * refused to proceed without once the 2.0 contract was
                 * declared - it asked three times and got STATUS_NOT_SUPPORTED
                 * each time, and dropped the adapter as a render device.
                 *
                 * The segments are the same ones QUERYSEGMENT3 reports; only
                 * the shape of the answer differs.  Descriptors are addressed
                 * by the caller's stride rather than as a typed array,
                 * so that the structure can grow without breaking callers.
                 */
                if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT4))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s OutputDataSize (%u) smaller than DXGK_QUERYSEGMENTOUT4 (%u)\n",
                              __FUNCTION__,
                              pQueryAdapterInfo->OutputDataSize,
                              (UINT)sizeof(DXGK_QUERYSEGMENTOUT4)));
                    return STATUS_BUFFER_TOO_SMALL;
                }

                DXGK_QUERYSEGMENTOUT4 *pSegmentInfo =
                    (DXGK_QUERYSEGMENTOUT4 *)pQueryAdapterInfo->pOutputData;
                ULONGLONG shmem_len = m_VioDev.shmem_len;
                CPciBar *shmem_bar = m_PciResources.GetPciBar(m_VioDev.shmem_bar);
                const bool has_shmem = shmem_bar && shmem_len;
                const UINT base_segment_count = has_shmem ? 2 : 1;
                /* One more segment holds page tables when GpuMmu is in play. */
                const UINT segment_count =
                    base_segment_count + (m_pPageTableSegment ? 1 : 0);

                if (pSegmentInfo->NbSegment == 0)
                {
                    /* Only NbSegment is valid on the counting pass. */
                    pSegmentInfo->NbSegment = segment_count;
                    return STATUS_SUCCESS;
                }

                if (pSegmentInfo->NbSegment != segment_count || !pSegmentInfo->pSegmentDescriptor)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s QUERYSEGMENT4 invalid fill buffer: count %u, expected %u, descriptors %p\n",
                              __FUNCTION__, pSegmentInfo->NbSegment, segment_count,
                              pSegmentInfo->pSegmentDescriptor));
                    return STATUS_INVALID_PARAMETER;
                }

                /* Require the WDDM 2.0 fields, independent of newer fields
                 * appended to the descriptor in the build's WDK. */
                const SIZE_T required_descriptor_size =
                    FIELD_OFFSET(DXGK_SEGMENTDESCRIPTOR4, VprReserveSize) + sizeof(UINT);
                const SIZE_T stride = pSegmentInfo->SegmentDescriptorStride;
                if (stride < required_descriptor_size || stride > MAXSIZE_T / segment_count)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s QUERYSEGMENT4 invalid descriptor stride %Iu: minimum %Iu, count %u\n",
                              __FUNCTION__, stride, required_descriptor_size, segment_count));
                    return STATUS_INVALID_PARAMETER;
                }

                BYTE *pDescBytes = pSegmentInfo->pSegmentDescriptor;

                RtlZeroMemory(pDescBytes, stride * segment_count);

                pSegmentInfo->PagingBufferPrivateDataSize = 0;
                /*
                 * The paging buffer stays in segment 1, the aperture segment.
                 * Moving it into the page table memory segment made
                 * VIDMM_GLOBAL::InitDmaPools fail with
                 * STATUS_INVALID_PARAMETER, which took the adapter down:
                 * an aperture segment is where VidMm expects to map a paging
                 * buffer, and it does exactly that under WDDM 1.3 with the
                 * two MAP_APERTURE_SEGMENT calls right after segments are
                 * reported.
                 */
                pSegmentInfo->PagingBufferSegmentId = 1;
                pSegmentInfo->PagingBufferSize = 10 * PAGE_SIZE;

                /* Segment 1: framebuffer/aperture, as QUERYSEGMENT3 reports it. */
                DXGK_SEGMENTDESCRIPTOR4 *pSeg0 = (DXGK_SEGMENTDESCRIPTOR4 *)pDescBytes;
                ULONGLONG segment1_base = 0xC0000000;
                if (has_shmem)
                {
                    ULONGLONG min_base = ALIGN_UP_BY(shmem_len, PAGE_SIZE);
                    if (segment1_base < min_base)
                    {
                        segment1_base = min_base;
                    }
                }
                pSeg0->BaseAddress.QuadPart = segment1_base;
                pSeg0->Flags.Aperture = TRUE;
                pSeg0->Flags.CacheCoherent = TRUE;
                pSeg0->Flags.CpuVisible = FALSE;
                pSeg0->Flags.DirectFlip = TRUE;
                pSeg0->Size = 256 * 1024 * 4096;
                pSeg0->CommitLimit = 256 * 1024 * 4096;

                if (has_shmem)
                {
                    /* Segment 2: BAR-backed shared memory, CPU visible. */
                    DXGK_SEGMENTDESCRIPTOR4 *pSeg1 =
                        (DXGK_SEGMENTDESCRIPTOR4 *)(pDescBytes + stride);
                    PHYSICAL_ADDRESS shmem_pa = shmem_bar->GetPA();

                    shmem_pa.QuadPart += m_VioDev.shmem_offset;

                    pSeg1->BaseAddress.QuadPart = 0;
                    pSeg1->Flags.Aperture = TRUE;
                    pSeg1->Flags.CacheCoherent = FALSE;
                    pSeg1->Flags.CpuVisible = TRUE;
                    pSeg1->Flags.DirectFlip = FALSE;
                    pSeg1->CpuTranslatedAddress = shmem_pa;
                    pSeg1->Size = (SIZE_T)shmem_len;
                    pSeg1->CommitLimit = (SIZE_T)shmem_len;
                }

                if (m_pPageTableSegment)
                {
                    /*
                     * The page table segment: a real memory segment, not an
                     * aperture, because VidMm allocates page tables out of it
                     * and maps them for the CPU to write.  Its backing is the
                     * contiguous system memory reserved at start, which is
                     * what PopulatedFromSystemMemory describes.
                     *
                     * Segment ids are one based, so this one follows the
                     * segments already reported above.
                     */
                    DXGK_SEGMENTDESCRIPTOR4 *pSegPt =
                        (DXGK_SEGMENTDESCRIPTOR4 *)(pDescBytes + stride * base_segment_count);

                    m_PageTableSegmentId = base_segment_count + 1;

                    pSegPt->BaseAddress.QuadPart = 0;
                    pSegPt->Flags.Aperture = FALSE;
                    pSegPt->Flags.PopulatedFromSystemMemory = TRUE;
                    pSegPt->Flags.CpuVisible = TRUE;
                    pSegPt->Flags.CacheCoherent = TRUE;
                    pSegPt->Flags.DirectFlip = FALSE;
                    pSegPt->CpuTranslatedAddress = m_PageTableSegmentPA;
                    pSegPt->Size = m_PageTableSegmentSize;
                    /* Wholly system memory, so the system memory portion runs
                     * to the end; CommitLimit is documented as applying to
                     * aperture segments only and stays zero here. */
                    pSegPt->SystemMemoryEndAddress = m_PageTableSegmentSize;

                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s page table segment is id %u, %u bytes at physical %I64x\n",
                              __FUNCTION__, m_PageTableSegmentId,
                              (UINT)m_PageTableSegmentSize, m_PageTableSegmentPA.QuadPart));
                }

                DbgPrint(TRACE_LEVEL_ERROR,
                         ("%s filled %u segments at stride %u, output %u bytes\n",
                          __FUNCTION__, segment_count, (UINT)stride,
                          pQueryAdapterInfo->OutputDataSize));
                return STATUS_SUCCESS;
            }

        case DXGKQAITYPE_PHYSICALADAPTERCAPS:
            {
                /*
                 * Keep paging on its own logical node. Windows 10's
                 * dxgmms2 10.0.19041.6456 single-engine selection fast path
                 * only visits node 0, overlooking the OS's additional software
                 * synchronization node. DWM fence packets then accumulate and
                 * StopDevice hangs in VidSchFlushAdapter before calling us.
                 * Two execution nodes select the general scheduler path, which
                 * drains that software node as well. This changes scheduling
                 * topology, not the shared virtio transport or completion
                 * rules; it does not claim a separate hardware copy engine.
                 */
                /*
                 * dxgkrnl sizes this buffer for the DDI version the driver
                 * declared, not for the one the driver was compiled against.
                 * Declaring WDDM 2.0 gets 20 bytes - two WORDs, the handle and
                 * the flags - while the 3.2 structure in the WDK is 32, having
                 * gained VPRPagingNode and VirtualCopyNodeIndex since.
                 * Demanding the larger size refused a query dxgkrnl requires,
                 * and the adapter was torn down immediately afterwards.
                 *
                 * So require only as far as Flags, and write only that much.
                 */
                const UINT required = FIELD_OFFSET(DXGK_PHYSICALADAPTERCAPS, Flags) +
                                      sizeof(DXGK_PHYSICALADAPTERFLAGS);

                if (pQueryAdapterInfo->OutputDataSize < required)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s OutputDataSize (%u) smaller than the %u bytes "
                              "this query needs\n",
                              __FUNCTION__, pQueryAdapterInfo->OutputDataSize, required));
                    return STATUS_BUFFER_TOO_SMALL;
                }

                DXGK_PHYSICALADAPTERCAPS *pCaps =
                    (DXGK_PHYSICALADAPTERCAPS *)pQueryAdapterInfo->pOutputData;

                pCaps->NumExecutionNodes = VIOGPU_EXECUTION_NODE_COUNT;
                pCaps->PagingNodeIndex = VIOGPU_PAGING_NODE;

                /*
                 * Not the driver's own object, and not something dxgkrnl fills
                 * in: this is dxgkrnl's device handle, the one handed to the
                 * driver as DXGKRNL_INTERFACE::DeviceHandle at StartDevice,
                 * echoed back so it can tie this physical adapter to that
                 * device.  Leaving it zero is why its per physical adapter
                 * records held a null object for every index, and why VidMm
                 * died dereferencing one in ReadPhysicalAdapterConfiguration.
                 */
                pCaps->DxgkPhysicalAdapterHandle = m_DxgkInterface.DeviceHandle;

                /*
                 * These flags are per physical adapter and have to agree with
                 * the MemoryManagementCaps declared above.  Zeroing them said
                 * this adapter supports neither IoMmu nor GpuMmu addressing,
                 * while the driver capabilities said IoMmu - and VidMm then
                 * had no adapter to configure, which is the null pointer it
                 * died on in ReadPhysicalAdapterConfiguration.
                 */
                RtlZeroMemory(&pCaps->Flags, sizeof(pCaps->Flags));
                pCaps->Flags.GpuMmuSupported = 1;

                DbgPrint(TRACE_LEVEL_ERROR,
                         ("%s execution nodes=%u render=%u paging=%u\n", __FUNCTION__,
                          VIOGPU_EXECUTION_NODE_COUNT, VIOGPU_RENDER_NODE, VIOGPU_PAGING_NODE));
                return STATUS_SUCCESS;
            }

#endif
        case DXGKQAITYPE_HISTORYBUFFERPRECISION:
            {
                /*
                 * Timestamp width for the scheduler's history buffers.  There
                 * is no hardware counter behind a virtio device, so report the
                 * full 64 bits of the software timestamps the driver already
                 * keeps.
                 */
                if (pQueryAdapterInfo->OutputDataSize <
                    sizeof(DXGKARG_HISTORYBUFFERPRECISION) * VIOGPU_EXECUTION_NODE_COUNT)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                DXGKARG_HISTORYBUFFERPRECISION *pPrecision =
                    (DXGKARG_HISTORYBUFFERPRECISION *)pQueryAdapterInfo->pOutputData;

                // The response contains one entry per execution node.
                for (UINT node = 0; node < VIOGPU_EXECUTION_NODE_COUNT; ++node)
                    pPrecision[node].PrecisionBits = 64;

                DbgPrint(TRACE_LEVEL_ERROR,
                         ("%s history buffer precision 64 bits for %u nodes\n",
                          __FUNCTION__, VIOGPU_EXECUTION_NODE_COUNT));
                return STATUS_SUCCESS;
            }

#if VIOGPU_WDDM2
        case DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION:
            {
                /*
                 * None of the optional display capabilities are present: no
                 * secure display, no virtual modes.  Zero is the answer, but
                 * it has to be given rather than refused.
                 */
                if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_DISPLAY_DRIVERCAPS_EXTENSION))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                RtlZeroMemory(pQueryAdapterInfo->pOutputData,
                              sizeof(DXGK_DISPLAY_DRIVERCAPS_EXTENSION));

                DbgPrint(TRACE_LEVEL_ERROR,
                         ("%s display caps extension, none supported\n", __FUNCTION__));
                return STATUS_SUCCESS;
            }

        case DXGKQAITYPE_GPUMMUCAPS:
            {
                /*
                 * Declaring GpuMmu rather than IoMmu is what got dxgkrnl
                 * talking again: with IoMmu it asked for nothing further and
                 * failed silently inside VidMm, while with GpuMmu it names
                 * each thing it still wants.  That makes this the path worth
                 * following, even though a virtio device has no page tables of
                 * its own - the answers here describe an address space dxgkrnl
                 * can reason about, and what it asks for next is the point.
                 *
                 * Two levels of 4KB pages over a 32 bit address space, updated
                 * by the CPU, because there is no GPU engine that could walk or
                 * update a page table here.
                 *
                 * 32 bits rather than the 40 this first claimed: VidMm sizes a
                 * page tracking structure from the address space, four bytes
                 * per 4KB page, so 40 bits asked it for a gigabyte in one
                 * allocation.  The ETW trace shows it attempting exactly
                 * 0x40000042 bytes, failing, and destroying the VaAllocator it
                 * had just created, which took the adapter down with it.  At
                 * 32 bits the same structure is 4MB, and 4GB of address space
                 * is far more than this driver has any use for yet.
                 */
                if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_GPUMMUCAPS))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s GPUMMUCAPS wants %u bytes, got %u\n", __FUNCTION__,
                              (UINT)sizeof(DXGK_GPUMMUCAPS), pQueryAdapterInfo->OutputDataSize));
                    return STATUS_BUFFER_TOO_SMALL;
                }

                DXGK_GPUMMUCAPS *pMmu = (DXGK_GPUMMUCAPS *)pQueryAdapterInfo->pOutputData;

                RtlZeroMemory(pMmu, sizeof(*pMmu));
                pMmu->CacheCoherentMemorySupported = 1;
                pMmu->PageTableUpdateMode = DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
                pMmu->VirtualAddressBitCount = 32;
                pMmu->PageTableLevelCount = VIOGPU_PAGE_TABLE_LEVEL_COUNT;
                pMmu->LeafPageTableSizeFor64KPagesInBytes = 0;

                DbgPrint(TRACE_LEVEL_ERROR,
                         ("%s gpummu caps: %u va bits, %u levels\n", __FUNCTION__,
                          pMmu->VirtualAddressBitCount, pMmu->PageTableLevelCount));
                return STATUS_SUCCESS;
            }

        case DXGKQAITYPE_PAGETABLELEVELDESC:
            {
                /*
                 * Describes one level of the page table hierarchy GPUMMUCAPS
                 * announced.  Two levels over 32 bits of address space, on 4KB
                 * pages: the page offset takes 12 bits, leaving 20 bits to
                 * split, so 10 index bits per level and a page table of 2^10
                 * driver-format entries - one 16KB table per level, each leaf
                 * covering 4MB.
                 *
                 * Level zero is the leaf.  Page tables live in the memory
                 * segment reserved at start, because VidMm allocates them from
                 * it and maps them for the CPU to write - an aperture segment
                 * cannot back them, and neither of the two this driver already
                 * reports is both a memory segment and CPU visible.
                 */
                const DXGK_QUERYPAGETABLELEVELDESCIN *pIn =
                    (const DXGK_QUERYPAGETABLELEVELDESCIN *)pQueryAdapterInfo->pInputData;

                if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_PAGE_TABLE_LEVEL_DESC))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                DXGK_PAGE_TABLE_LEVEL_DESC *pLevel =
                    (DXGK_PAGE_TABLE_LEVEL_DESC *)pQueryAdapterInfo->pOutputData;

                RtlZeroMemory(pLevel, sizeof(*pLevel));
                pLevel->PageTableIndexBitCount = VIOGPU_PAGE_TABLE_INDEX_BITS;
                pLevel->PageTableSegmentId = m_PageTableSegmentId;
                pLevel->PagingProcessPageTableSegmentId = m_PageTableSegmentId;
                pLevel->PageTableSizeInBytes =
                    VIOGPU_PAGE_TABLE_ENTRY_COUNT * sizeof(VIOGPU_PAGE_TABLE_ENTRY);
                pLevel->PageTableAlignmentInBytes = 0;

                DbgPrint(TRACE_LEVEL_ERROR,
                         ("%s page table level %u: %u index bits, %u bytes, segment %u\n",
                          __FUNCTION__, pIn ? pIn->LevelIndex : 0,
                          pLevel->PageTableIndexBitCount, pLevel->PageTableSizeInBytes,
                          pLevel->PageTableSegmentId));
                return STATUS_SUCCESS;
            }

#endif
        default:
            {
                /*
                 * ERROR rather than VERBOSE, because which queries go
                 * unanswered is exactly the question at the moment and the
                 * default trace level filters VERBOSE out.  D3D12 needs the
                 * WDDM 2.0 memory model queries - QUERYSEGMENT4 (11),
                 * GPUMMUCAPS (13), PAGETABLELEVELDESC (14) - and this driver
                 * answers none of them.
                 */
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("<--- %s unanswered query type %d\n", __FUNCTION__, pQueryAdapterInfo->Type));
                return STATUS_NOT_SUPPORTED;
            }
    }
}

bool VioGpuAdapter::AllocateShmemRange(ULONGLONG size, ULONGLONG alignment, ULONGLONG *offset)
{
    PAGED_CODE();

    return m_shmem_allocator.Allocate(size, alignment, offset);
}

void VioGpuAdapter::FreeShmemRange(ULONGLONG offset, ULONGLONG size)
{
    PAGED_CODE();

    m_shmem_allocator.Free(offset, size);
}

// Keep SEH in a separate function so Escape can use scoped references.
static NTSTATUS VioGpuCopyCapsetToUser(void *destination, const void *source, ULONG size)
{
    PAGED_CODE();

    __try
    {
        memcpy(destination, source, size);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("Failed to copy"));
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAdapter::Escape(_In_ CONST DXGKARG_ESCAPE *pEscape)
{
    PAGED_CODE();

    VIOGPU_ASSERT(pEscape != NULL);

    DbgPrint(TRACE_LEVEL_INFORMATION, ("<---> %s Flags = %d\n", __FUNCTION__, pEscape->Flags.Value));
    PAGED_CODE();
    PVIOGPU_ESCAPE pVioGpuEscape = (PVIOGPU_ESCAPE)pEscape->pPrivateDriverData;
    NTSTATUS status = STATUS_SUCCESS;
    UNREFERENCED_PARAMETER(pVioGpuEscape);

    UINT size = pEscape->PrivateDriverDataSize;
    const UINT requiredSize = sizeof(VIOGPU_ESCAPE);
    if (size < requiredSize)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s buffer too small %u, should be at least %u\n",
                  __FUNCTION__,
                  pEscape->PrivateDriverDataSize,
                  requiredSize));
        return STATUS_INVALID_BUFFER_SIZE;
    }

    switch (pVioGpuEscape->Type)
    {
        case VIOGPU_QUERY_TIMELINE_SUBMIT:
            if (pVioGpuEscape->DataLength < sizeof(UINT))
                return STATUS_INVALID_BUFFER_SIZE;
#if VIOGPU_WDDM2
            pVioGpuEscape->Id = VIOGPU_TIMELINE_SUBMIT_VERSION;
            break;
#else
            return STATUS_NOT_SUPPORTED;
#endif
        case VIOGPU_GET_DEVICE_ID:
            {
                CreateResolutionEvent();
                size = sizeof(ULONG);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                pVioGpuEscape->Id = m_Id;
                break;
            }
        case VIOGPU_GET_CUSTOM_RESOLUTION:
            {
                size = sizeof(VIOGPU_DISP_MODE);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                vidpn.EscapeCustomResoulution(&pVioGpuEscape->Resolution);
                break;
            }
        case VIOGPU_GET_CAPS:
            {
                size = sizeof(VIOGPU_CAPSET_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }

                if (!(m_supportedCapsetIDs & (1ull << pVioGpuEscape->Capset.CapsetId)))
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s capset id is not supported\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER_1;
                }
                CAPSET_INFO *pCapsetInfo = &m_capsetInfos[pVioGpuEscape->Capset.CapsetId];
                if (pCapsetInfo->max_version < pVioGpuEscape->Capset.Version)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s capset version is too low\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER_2;
                };

                PGPU_VBUFFER vbuf = 0;

                /* ARE 2025-08-30 Spice server v0.16.0 does not return Capset if the display is not visible */

                status = ctrlQueue.AskCapset(&vbuf,
                                             pVioGpuEscape->Capset.CapsetId,
                                             pCapsetInfo->max_size,
                                             pVioGpuEscape->Capset.Version);
                if (!status) 
                {
                    return STATUS_INTERNAL_ERROR;
                }

                UCHAR *buf = ((PGPU_RESP_CAPSET)vbuf->resp_buf)->capset_data;
                ULONG to_copy = min(pVioGpuEscape->Capset.Size, pCapsetInfo->max_size);
                UCHAR *userCapset = (UCHAR *)(ULONG_PTR)pVioGpuEscape->Capset.Capset;
                status = VioGpuCopyCapsetToUser(userCapset, buf, to_copy);
                ctrlQueue.ReleaseBuffer(vbuf);

                break;
            }
        case VIOGPU_RES_INFO:
            {
                size = sizeof(VIOGPU_RES_INFO_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                VioGpuAllocationReference allocationReference(this, pVioGpuEscape->ResourceInfo.ResHandle);
                VioGpuAllocation *allocation = allocationReference.Get();
                if (allocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s ivalid handle\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER;
                }

                status = allocation->EscapeResourceInfo(&pVioGpuEscape->ResourceInfo);

                break;
            }
        case VIOGPU_RES_ATTACH_WAIT:
            {
                size = sizeof(VIOGPU_RES_ATTACH_WAIT_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }

                VioGpuAllocationReference allocationReference(this, pVioGpuEscape->ResourceAttachWait.ResHandle);
                VioGpuAllocation *allocation = allocationReference.Get();
                if (allocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s invalid attach-wait handle\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER;
                }

                VioGpuDevice *device = reinterpret_cast<VioGpuDevice *>(pEscape->hDevice);
                if (device == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s attach-wait without device context\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER;
                }
                if (!virtio_is_feature_enabled(m_u64GuestFeatures, VIRTIO_GPU_F_CONTEXT_INIT))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s attach-wait requires negotiated CONTEXT_INIT\n", __FUNCTION__));
                    return STATUS_NOT_SUPPORTED;
                }
                if (!device->IsContextCreated())
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s attach-wait before context creation ctx_id=0x%x\n",
                              __FUNCTION__, device->GetId()));
                    return STATUS_DEVICE_NOT_READY;
                }
                if (!device->IsVenusContext())
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s attach-wait requires a Venus context ctx_id=0x%x capset=0x%x\n",
                              __FUNCTION__, device->GetId(), device->GetCapsetId()));
                    return STATUS_NOT_SUPPORTED;
                }

                if (allocation->IsBlob())
                {
                    status = allocation->EnsureBlobCreatedAndWait(device->GetId());
                    if (!NT_SUCCESS(status))
                    {
                        DbgPrint(TRACE_LEVEL_ERROR,
                                 ("%s attach-wait blob create failed status=0x%x res_id=0x%x ctx_id=0x%x\n",
                                  __FUNCTION__, status, allocation->GetId(), device->GetId()));
                        return status;
                    }
                }

                pVioGpuEscape->ResourceAttachWait.Id = allocation->GetId();
                status = ctrlQueue.CtxResourceWait(device->GetId(), allocation->GetId());
                if (!NT_SUCCESS(status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s attach-wait failed status=0x%x res_id=0x%x ctx_id=0x%x\n",
                              __FUNCTION__, status, allocation->GetId(), device->GetId()));
                }
                break;
            }
        case VIOGPU_RES_BUSY:
            {
                size = sizeof(VIOGPU_RES_BUSY_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                VioGpuAllocationReference allocationReference(this, pVioGpuEscape->ResourceBusy.ResHandle);
                VioGpuAllocation *allocation = allocationReference.Get();
                if (allocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s ivalid handle\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER;
                }
                status = allocation->EscapeResourceBusy(&pVioGpuEscape->ResourceBusy);

                break;
            }
        case VIOGPU_RES_MAP_BLOB:
            {
                size = sizeof(VIOGPU_RES_MAP_BLOB_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                VioGpuAllocationReference allocationReference(this, pVioGpuEscape->ResourceMapBlob.ResHandle);
                VioGpuAllocation *allocation = allocationReference.Get();
                if (allocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s invalid handle\n", __FUNCTION__));
                    return STATUS_INVALID_HANDLE;
                }

                VioGpuDevice *device = reinterpret_cast<VioGpuDevice *>(pEscape->hDevice);
                status = allocation->EscapeResourceMapBlob(&pVioGpuEscape->ResourceMapBlob, device);
                break;
            }
        case VIOGPU_RES_UNMAP_BLOB:
            {
                size = sizeof(VIOGPU_RES_UNMAP_BLOB_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                VioGpuAllocationReference allocationReference(this, pVioGpuEscape->ResourceUnmapBlob.ResHandle);
                VioGpuAllocation *allocation = allocationReference.Get();
                if (allocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s invalid handle\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER;
                }

                VioGpuDevice *device = reinterpret_cast<VioGpuDevice *>(pEscape->hDevice);
                status = allocation->EscapeResourceUnmapBlob(&pVioGpuEscape->ResourceUnmapBlob, device);
                break;
            }
        case VIOGPU_RES_SET_SCANOUT_BLOB:
            {
                size = sizeof(VIOGPU_RES_SET_SCANOUT_BLOB_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                VioGpuAllocationReference allocationReference(this, pVioGpuEscape->ResourceSetScanoutBlob.ResHandle);
                VioGpuAllocation *allocation = allocationReference.Get();
                if (allocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s invalid handle\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER;
                }

                status = allocation->EscapeResourceSetScanoutBlob(&pVioGpuEscape->ResourceSetScanoutBlob);
                break;
            }
        case VIOGPU_CTX_INIT:
            {
                size = sizeof(VIOGPU_CTX_INIT_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                VioGpuDevice *context = reinterpret_cast<VioGpuDevice *>(pEscape->hDevice);
                if (context == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s no hDdevice(context) supplied\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER;
                }
                context->Init(&pVioGpuEscape->CtxInit);
                break;
            }
        case VIOGPU_SUBMIT_CMD:
            {
                size = sizeof(VIOGPU_SUBMIT_CMD_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }

                const UINT total_size = sizeof(VIOGPU_ESCAPE) + pVioGpuEscape->DataLength;
                if (pEscape->PrivateDriverDataSize < total_size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s escape size too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pEscape->PrivateDriverDataSize,
                              total_size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }

                PUINT8 payload = (PUINT8)(pVioGpuEscape + 1);
                VIOGPU_SUBMIT_CMD_REQ *req = (VIOGPU_SUBMIT_CMD_REQ *)payload;
                const UINT needed = (UINT)(sizeof(*req) + req->CmdSize);
                if (pVioGpuEscape->DataLength < needed)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s escape payload too small %d, need %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              needed));
                    return STATUS_INVALID_BUFFER_SIZE;
                }

                VioGpuDevice *device = reinterpret_cast<VioGpuDevice *>(pEscape->hDevice);
                if (!device)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s NULL device\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER;
                }

                if (req->CmdType == VIOGPU_CMD_SUBMIT)
                {
                    if (req->CmdSize == 0)
                    {
                        DbgPrint(TRACE_LEVEL_ERROR, ("%s invalid cmd_size=0\n", __FUNCTION__));
                        return STATUS_INVALID_PARAMETER;
                    }

                    PUINT8 cmd_copy = new (NonPagedPoolNx) BYTE[req->CmdSize];
                    if (!cmd_copy)
                    {
                        return STATUS_INSUFFICIENT_RESOURCES;
                    }
                    RtlCopyMemory(cmd_copy, payload + sizeof(*req), req->CmdSize);

                    PVIOGPU_SUBMIT_ESCAPE_CTX submit_ctx =
                        new (NonPagedPoolNx) VIOGPU_SUBMIT_ESCAPE_CTX();
                    if (!submit_ctx)
                    {
                        delete[] cmd_copy;
                        return STATUS_INSUFFICIENT_RESOURCES;
                    }
                    submit_ctx->cmd_buf = cmd_copy;

                    ctrlQueue.SubmitCommand(cmd_copy,
                                            req->CmdSize,
                                            device->GetId(),
                                            SubmitEscapeCompleteCB,
                                            submit_ctx);
                }
                else
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s unsupported cmd_type=0x%x\n",
                              __FUNCTION__, req->CmdType));
                    return STATUS_INVALID_PARAMETER;
                }
                break;
            }

        default:
            DbgPrint(TRACE_LEVEL_ERROR, ("%s: invalid Escape type 0x%x\n", __FUNCTION__, pVioGpuEscape->Type));
            status = STATUS_INVALID_PARAMETER;
    }

    return status;
}

NTSTATUS VioGpuAdapter::QueryInterface(_In_ CONST PQUERY_INTERFACE pQueryInterface)
{
    PAGED_CODE();

    VIOGPU_ASSERT(pQueryInterface != NULL);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s Version = %d\n", __FUNCTION__, pQueryInterface->Version));

    return STATUS_NOT_SUPPORTED;
}

NTSTATUS VioGpuAdapter::StopDeviceAndReleasePostDisplayOwnership(_In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                                 _Out_ DXGK_DISPLAY_INFORMATION *pDisplayInfo)
{
    PAGED_CODE();

    VIOGPU_ASSERT(TargetId < MAX_CHILDREN);
    // FIXME!!!
    if (m_MonitorPowerState > PowerDeviceD0)
    {
        SetPowerState(TargetId, PowerDeviceD0, PowerActionNone);
    }
    vidpn.ReleasePostDisplayOwnership(TargetId, pDisplayInfo);
    return StopDevice();
}

PAGED_CODE_SEG_END

//
// Non-Paged Code
//
#pragma code_seg(push)
#pragma code_seg()

BOOLEAN VioGpuAdapter::ShouldNotifyDmaFence(UINT fenceId,
                                            UINT nodeOrdinal,
                                            UINT engineOrdinal,
                                            ULONG ctxId,
                                            HANDLE ownerPid)
{
    if (nodeOrdinal >= kMaxTrackedNodes || engineOrdinal >= kMaxTrackedEngines)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s fence=%u node=%u engine=%u out of tracked range; notify without monotonic check ctx_id=%u owner_pid=%p\n",
                  __FUNCTION__,
                  fenceId,
                  nodeOrdinal,
                  engineOrdinal,
                  ctxId,
                  ownerPid));
        return TRUE;
    }

    volatile LONG *slot = &m_lastNotifiedFence[nodeOrdinal][engineOrdinal];
    LONG observed = InterlockedExchangeAdd(slot, 0);
    UINT lastFence = static_cast<UINT>(observed);

    while (lastFence == 0 || IsFenceStrictlyNewer(fenceId, lastFence))
    {
        LONG previous = InterlockedCompareExchange(slot, static_cast<LONG>(fenceId), observed);
        if (previous == observed)
        {
            return TRUE;
        }

        observed = previous;
        lastFence = static_cast<UINT>(observed);
    }

    LONG dropCount = InterlockedIncrement(&m_outOfOrderFenceDropCount);
    DbgPrint(TRACE_LEVEL_WARNING,
             ("%s stale/duplicate DMA completion observed (filtered) fence=%u last_notified=%u node=%u engine=%u ctx_id=%u owner_pid=%p count=%ld\n",
              __FUNCTION__,
              fenceId,
              lastFence,
              nodeOrdinal,
              engineOrdinal,
              ctxId,
              ownerPid,
              dropCount));
    return FALSE;
}

UINT VioGpuAdapter::GetLastNotifiedFence(UINT nodeOrdinal, UINT engineOrdinal)
{
    if (nodeOrdinal >= kMaxTrackedNodes || engineOrdinal >= kMaxTrackedEngines)
    {
        return 0;
    }

    return static_cast<UINT>(
        InterlockedCompareExchange(&m_lastNotifiedFence[nodeOrdinal][engineOrdinal], 0, 0));
}

void VioGpuAdapter::NotifyDmaPreempted(UINT preemptionFenceId,
                                       UINT lastCompletedFenceId,
                                       UINT nodeOrdinal,
                                       UINT engineOrdinal,
                                       BOOLEAN fromIsr,
                                       PCSTR reason)
{
    DXGKARGCB_NOTIFY_INTERRUPT_DATA interrupt = {};
    interrupt.InterruptType = DXGK_INTERRUPT_DMA_PREEMPTED;
    interrupt.DmaPreempted.PreemptionFenceId = preemptionFenceId;
    interrupt.DmaPreempted.LastCompletedFenceId = lastCompletedFenceId;
    interrupt.DmaPreempted.NodeOrdinal = nodeOrdinal;
    interrupt.DmaPreempted.EngineOrdinal = engineOrdinal;

    LONG notifyCount = InterlockedIncrement(&m_preemptionNotifyCount);
    if (ShouldLogPreemptionSample(notifyCount))
    {
        DbgPrint(TRACE_LEVEL_VERBOSE,
                 ("%s preemption_notify[%ld] reason=%s preempt_fence=%u last_completed=%u node=%u engine=%u from_isr=%u\n",
                  __FUNCTION__,
                  notifyCount,
                  reason ? reason : "unknown",
                  preemptionFenceId,
                  lastCompletedFenceId,
                  nodeOrdinal,
                  engineOrdinal,
                  fromIsr));
    }

    if (fromIsr)
    {
        m_DxgkInterface.DxgkCbNotifyInterrupt(m_DxgkInterface.DeviceHandle, &interrupt);
        return;
    }

    NOTIFY_CONTEXT notify = {};
    notify.pDxgkInterface = &m_DxgkInterface;
    notify.interrupt = &interrupt;
    notify.triggerDpc = TRUE;

    BOOLEAN callbackRet = FALSE;
    NTSTATUS status = m_DxgkInterface.DxgkCbSynchronizeExecution(m_DxgkInterface.DeviceHandle,
                                                                  NotifyInterruptSyncRoutine,
                                                                  &notify,
                                                                  0,
                                                                  &callbackRet);
    if (!NT_SUCCESS(status) || !callbackRet)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s preemption_notify_sync_failed status=0x%x callback=%u preempt_fence=%u node=%u engine=%u\n",
                  __FUNCTION__,
                  status,
                  callbackRet,
                  preemptionFenceId,
                  nodeOrdinal,
                  engineOrdinal));
        m_DxgkInterface.DxgkCbNotifyInterrupt(m_DxgkInterface.DeviceHandle, &interrupt);
        m_DxgkInterface.DxgkCbQueueDpc(m_DxgkInterface.DeviceHandle);
    }
}

void VioGpuAdapter::NotifyPendingPreemptionIfDrained(UINT nodeOrdinal,
                                                     UINT engineOrdinal,
                                                     BOOLEAN fromIsr,
                                                     PCSTR reason)
{
    if (nodeOrdinal >= kMaxTrackedNodes || engineOrdinal >= kMaxTrackedEngines)
    {
        return;
    }

    LONG outstanding = InterlockedCompareExchange(&m_preemptSubmittedOutstanding[nodeOrdinal][engineOrdinal], 0, 0);
    if (outstanding != 0)
    {
        return;
    }

    LONG pendingFence = InterlockedExchange(&m_pendingPreemptionFence[nodeOrdinal][engineOrdinal], 0);
    if (pendingFence == 0)
    {
        return;
    }

    NotifyDmaPreempted(static_cast<UINT>(pendingFence),
                       GetLastNotifiedFence(nodeOrdinal, engineOrdinal),
                       nodeOrdinal,
                       engineOrdinal,
                       fromIsr,
                       reason);
}

NTSTATUS VioGpuAdapter::PreemptCommand(_In_ CONST DXGKARG_PREEMPTCOMMAND *pPreemptCommand)
{
    if (!pPreemptCommand)
    {
        InterlockedIncrement(&m_preemptionInvalidCount);
        return STATUS_INVALID_PARAMETER;
    }

    UINT nodeOrdinal = pPreemptCommand->NodeOrdinal;
    UINT engineOrdinal = pPreemptCommand->EngineOrdinal;
    UINT preemptionFenceId = pPreemptCommand->PreemptionFenceId;

    if (nodeOrdinal >= kMaxTrackedNodes || engineOrdinal >= kMaxTrackedEngines)
    {
        LONG invalidCount = InterlockedIncrement(&m_preemptionInvalidCount);
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s invalid_preemption[%ld] preempt_fence=%u node=%u engine=%u flags=0x%x\n",
                  __FUNCTION__,
                  invalidCount,
                  preemptionFenceId,
                  nodeOrdinal,
                  engineOrdinal,
                  pPreemptCommand->Flags.Value));
        return STATUS_INVALID_PARAMETER;
    }

    LONG requestCount = InterlockedIncrement(&m_preemptionRequestCount);
    LONG outstanding = InterlockedCompareExchange(&m_preemptSubmittedOutstanding[nodeOrdinal][engineOrdinal], 0, 0);
    if (outstanding > 0)
    {
        LONG previousFence =
            InterlockedExchange(&m_pendingPreemptionFence[nodeOrdinal][engineOrdinal],
                                static_cast<LONG>(preemptionFenceId));
        LONG deferredCount = InterlockedIncrement(&m_preemptionDeferredCount);
        if (ShouldLogPreemptionSample(deferredCount))
        {
            DbgPrint(TRACE_LEVEL_VERBOSE,
                     ("%s preemption_deferred[%ld] request=%ld preempt_fence=%u previous_pending=%ld "
                      "outstanding=%ld last_completed=%u node=%u engine=%u flags=0x%x\n",
                      __FUNCTION__,
                      deferredCount,
                      requestCount,
                      preemptionFenceId,
                      previousFence,
                      outstanding,
                      GetLastNotifiedFence(nodeOrdinal, engineOrdinal),
                      nodeOrdinal,
                      engineOrdinal,
                      pPreemptCommand->Flags.Value));
        }

        NotifyPendingPreemptionIfDrained(nodeOrdinal, engineOrdinal, FALSE, "preempt_race_drained");
        return STATUS_SUCCESS;
    }

    if (ShouldLogPreemptionSample(requestCount))
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s preemption_idle request=%ld preempt_fence=%u last_completed=%u node=%u engine=%u flags=0x%x\n",
                  __FUNCTION__,
                  requestCount,
                  preemptionFenceId,
                  GetLastNotifiedFence(nodeOrdinal, engineOrdinal),
                  nodeOrdinal,
                  engineOrdinal,
                  pPreemptCommand->Flags.Value));
    }
    NotifyDmaPreempted(preemptionFenceId,
                       GetLastNotifiedFence(nodeOrdinal, engineOrdinal),
                       nodeOrdinal,
                       engineOrdinal,
                       FALSE,
                       "preempt_idle");

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAdapter::QueryCurrentFence(_Inout_ DXGKARG_QUERYCURRENTFENCE *pCurrentFence)
{
    if (!pCurrentFence)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (pCurrentFence->NodeOrdinal >= kMaxTrackedNodes ||
        pCurrentFence->EngineOrdinal >= kMaxTrackedEngines)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s invalid_query node=%u engine=%u\n",
                  __FUNCTION__,
                  pCurrentFence->NodeOrdinal,
                  pCurrentFence->EngineOrdinal));
        return STATUS_INVALID_PARAMETER;
    }

    pCurrentFence->CurrentFence = GetLastNotifiedFence(pCurrentFence->NodeOrdinal,
                                                       pCurrentFence->EngineOrdinal);
    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("%s node=%u engine=%u current_fence=%u\n",
              __FUNCTION__,
              pCurrentFence->NodeOrdinal,
              pCurrentFence->EngineOrdinal,
              pCurrentFence->CurrentFence));

    return STATUS_SUCCESS;
}

void VioGpuAdapter::RecordDmaSubmittedForPreemption(UINT fenceId,
                                                    UINT nodeOrdinal,
                                                    UINT engineOrdinal,
                                                    ULONG ctxId,
                                                    HANDLE ownerPid)
{
    if (nodeOrdinal >= kMaxTrackedNodes || engineOrdinal >= kMaxTrackedEngines)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s skipped_out_of_range fence=%u node=%u engine=%u ctx_id=%u owner_pid=%p\n",
                  __FUNCTION__,
                  fenceId,
                  nodeOrdinal,
                  engineOrdinal,
                  ctxId,
                  ownerPid));
        return;
    }

    LONG outstanding = InterlockedIncrement(&m_preemptSubmittedOutstanding[nodeOrdinal][engineOrdinal]);
    LONG pendingFence = InterlockedCompareExchange(&m_pendingPreemptionFence[nodeOrdinal][engineOrdinal], 0, 0);
    if (pendingFence != 0)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s submit_while_preempt_pending fence=%u pending_preempt=%ld outstanding=%ld "
                  "node=%u engine=%u ctx_id=%u owner_pid=%p\n",
                  __FUNCTION__,
                  fenceId,
                  pendingFence,
                  outstanding,
                  nodeOrdinal,
                  engineOrdinal,
                  ctxId,
                  ownerPid));
    }
}

void VioGpuAdapter::RecordDmaCompletionForPreemptionFromIsr(UINT fenceId,
                                                            UINT nodeOrdinal,
                                                            UINT engineOrdinal,
                                                            ULONG ctxId,
                                                            HANDLE ownerPid)
{
    if (nodeOrdinal >= kMaxTrackedNodes || engineOrdinal >= kMaxTrackedEngines)
    {
        return;
    }

    LONG outstanding = InterlockedDecrement(&m_preemptSubmittedOutstanding[nodeOrdinal][engineOrdinal]);
    if (outstanding < 0)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s outstanding_underflow fence=%u node=%u engine=%u ctx_id=%u owner_pid=%p\n",
                  __FUNCTION__,
                  fenceId,
                  nodeOrdinal,
                  engineOrdinal,
                  ctxId,
                  ownerPid));
        InterlockedExchange(&m_preemptSubmittedOutstanding[nodeOrdinal][engineOrdinal], 0);
        outstanding = 0;
    }

    if (outstanding == 0)
    {
        NotifyPendingPreemptionIfDrained(nodeOrdinal, engineOrdinal, TRUE, "dma_drained");
    }
}

struct VIOGPU_TRACK_DMA_CONTEXT
{
    VIOGPU_DMA_RETIREMENT_QUEUE *queue;
    VIOGPU_DMA_RETIREMENT *entry;
};

static BOOLEAN TrackDmaSubmissionSynchronized(void *opaque)
{
    VIOGPU_TRACK_DMA_CONTEXT *ctx = static_cast<VIOGPU_TRACK_DMA_CONTEXT *>(opaque);
    VioGpuDmaRetirementPush(ctx->queue, ctx->entry);
    return TRUE;
}

BOOLEAN VioGpuAdapter::TrackDmaSubmission(VioGpuCommand *command)
{
    const UINT node = command->GetNodeOrdinal();
    const UINT engine = command->GetEngineOrdinal();
    if (node >= kMaxTrackedNodes || engine >= kMaxTrackedEngines)
        return FALSE;
    VIOGPU_TRACK_DMA_CONTEXT ctx = { &m_dmaRetirement[node][engine], &command->Retirement };
    return ExecuteSynchronized(TrackDmaSubmissionSynchronized, &ctx);
}

VOID VioGpuAdapter::CtrlStagePushFromIsr(PGPU_VBUFFER buf, UINT len)
{
    buf->isr_stage_len = len;
    ExInterlockedInsertTailList(&m_ctrlStageReadyList, &buf->isr_stage_entry, &m_ctrlStageListLock);
}

BOOLEAN VioGpuAdapter::CtrlStagePopForDpc(PGPU_VBUFFER *buf, UINT *len)
{
    PLIST_ENTRY entryList = ExInterlockedRemoveHeadList(&m_ctrlStageReadyList, &m_ctrlStageListLock);
    if (entryList == NULL)
    {
        return FALSE;
    }

    PGPU_VBUFFER stagedBuf = CONTAINING_RECORD(entryList, GPU_VBUFFER, isr_stage_entry);
    *buf = stagedBuf;
    *len = stagedBuf->isr_stage_len;
    stagedBuf->isr_stage_len = 0;
    return TRUE;
}

VOID VioGpuAdapter::ProcessCtrlQueueBuffer(PGPU_VBUFFER pvbuf, UINT len)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s ctrlQueue pvbuf = %p len = %d\n", __FUNCTION__, pvbuf, len));

    PGPU_CTRL_HDR pcmd = (PGPU_CTRL_HDR)pvbuf->buf;
    PGPU_CTRL_HDR resp = (PGPU_CTRL_HDR)pvbuf->resp_buf;

    if (resp == NULL)
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("!!!!! Command failed resp_buf == NULL\n"));
    }
    else if (resp->type >= VIRTIO_GPU_RESP_ERR_UNSPEC)
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("!!!!! Command failed resp->type=%x pcmd->type=%x\n", resp->type, pcmd->type));
    }
    else if (resp->type == VIRTIO_GPU_RESP_OK_NODATA)
    {
        DbgPrint(TRACE_LEVEL_INFORMATION,
                 ("<--- %s fence_id=%llu cmd_type=%lu\n",
                  __FUNCTION__,
                  (ULONGLONG)resp->fence_id,
                  pcmd->type));
    }
    else
    {
        DbgPrint(TRACE_LEVEL_VERBOSE,
                 ("<--- %s type = %xlu flags = %lx fence_id = %llx ctx_id = %lx cmd_type = %lx\n",
                  __FUNCTION__,
                  resp->type,
                  resp->flags,
                  resp->fence_id,
                  resp->ctx_id,
                  pcmd->type));
    }

    const bool auto_release = pvbuf->auto_release;
    if (pvbuf->complete_cb != NULL)
    {
        pvbuf->complete_cb(pvbuf->complete_ctx);
    }
    if (auto_release)
    {
        ctrlQueue.ReleaseBuffer(pvbuf);
    }
}

VOID VioGpuAdapter::DpcRoutine(VOID)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    PGPU_VBUFFER pvbuf = NULL;
    UINT len = 0;
    ULONG reason;

    while ((reason = InterlockedExchange((PLONG)&m_PendingWorks, 0)) != 0)
    {
        if ((reason & ISR_REASON_DISPLAY))
        {
            while (CtrlStagePopForDpc(&pvbuf, &len))
            {
                ProcessCtrlQueueBuffer(pvbuf, len);
            }
        }
        if ((reason & ISR_REASON_CURSOR))
        {
            while ((pvbuf = m_CursorQueue.DequeueCursor(&len)) != NULL)
            {
                DbgPrint(TRACE_LEVEL_VERBOSE,
                         ("---> %s m_CursorQueue pvbuf = %p len = %u\n", __FUNCTION__, pvbuf, len));
                m_CursorQueue.ReleaseBuffer(pvbuf);
            };
        }
        if (reason & ISR_REASON_CHANGE)
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("---> %s ConfigChanged\n", __FUNCTION__));
            KeSetEvent(&m_ConfigUpdateEvent, IO_NO_INCREMENT, FALSE);
        }
        // In ISR-staging mode, dequeue happens in ISR, so flush pending
        // control commands here after descriptor space has been returned.
        ctrlQueue.Flush();
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    m_DxgkInterface.DxgkCbNotifyDpc((HANDLE)m_DxgkInterface.DeviceHandle);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VOID VioGpuAdapter::ResetDevice(VOID)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));
}

#pragma code_seg(pop) // End Non-Paged Code

PAGED_CODE_SEG_BEGIN
NTSTATUS VioGpuAdapter::WriteRegistryString(_In_ HANDLE DevInstRegKeyHandle,
                                            _In_ PCWSTR pszwValueName,
                                            _In_ PCSTR pszValue)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    ANSI_STRING AnsiStrValue;
    UNICODE_STRING UnicodeStrValue;
    UNICODE_STRING UnicodeStrValueName;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    RtlInitUnicodeString(&UnicodeStrValueName, pszwValueName);

    RtlInitAnsiString(&AnsiStrValue, pszValue);
    Status = RtlAnsiStringToUnicodeString(&UnicodeStrValue, &AnsiStrValue, TRUE);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("RtlAnsiStringToUnicodeString failed with Status: 0x%X\n", Status));
        return Status;
    }

    Status = ZwSetValueKey(DevInstRegKeyHandle,
                           &UnicodeStrValueName,
                           0,
                           REG_SZ,
                           UnicodeStrValue.Buffer,
                           UnicodeStrValue.MaximumLength);

    RtlFreeUnicodeString(&UnicodeStrValue);

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("ZwSetValueKey failed with Status: 0x%X\n", Status));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuAdapter::WriteRegistryDWORD(_In_ HANDLE DevInstRegKeyHandle,
                                           _In_ PCWSTR pszwValueName,
                                           _In_ PDWORD pdwValue)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    UNICODE_STRING UnicodeStrValueName;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    RtlInitUnicodeString(&UnicodeStrValueName, pszwValueName);

    Status = ZwSetValueKey(DevInstRegKeyHandle, &UnicodeStrValueName, 0, REG_DWORD, pdwValue, sizeof(DWORD));

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("ZwSetValueKey failed with Status: 0x%X\n", Status));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuAdapter::ReadRegistryDWORD(_In_ HANDLE DevInstRegKeyHandle,
                                          _In_ PCWSTR pszwValueName,
                                          _Inout_ PDWORD pdwValue)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    UNICODE_STRING UnicodeStrValueName;
    ULONG ulRes;
    UCHAR Buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(DWORD)];
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    RtlInitUnicodeString(&UnicodeStrValueName, pszwValueName);

    Status = ZwQueryValueKey(DevInstRegKeyHandle,
                             &UnicodeStrValueName,
                             KeyValuePartialInformation,
                             Buf,
                             sizeof(Buf),
                             &ulRes);

    if (Status == STATUS_SUCCESS)
    {
        if (((PKEY_VALUE_PARTIAL_INFORMATION)Buf)->Type == REG_DWORD &&
            (((PKEY_VALUE_PARTIAL_INFORMATION)Buf)->DataLength == sizeof(DWORD)))
        {
            *pdwValue = *((PDWORD) & (((PKEY_VALUE_PARTIAL_INFORMATION)Buf)->Data));
        }
        else
        {
            Status = STATUS_INVALID_PARAMETER;
            VioGpuDbgBreak();
        }
    }

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("ZwQueryValueKey failed with Status: 0x%X\n", Status));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuAdapter::SetRegisterInfo(_In_ ULONG Id, _In_ DWORD MemSize)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PCSTR StrHWInfoChipType = "QEMU VIRTIO GPU";
    PCSTR StrHWInfoDacType = "VIRTIO GPU";
    PCSTR StrHWInfoAdapterString = "VIRTIO GPU";
    PCSTR StrHWInfoBiosString = "SEABIOS VIRTIO GPU";

    HANDLE DevInstRegKeyHandle;
    Status = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &DevInstRegKeyHandle);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("IoOpenDeviceRegistryKey failed for PDO: 0x%p, Status: 0x%X", m_pPhysicalDevice, Status));
        return Status;
    }

    do
    {
        Status = WriteRegistryString(DevInstRegKeyHandle, L"HardwareInformation.ChipType", StrHWInfoChipType);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryString failed for ChipType with Status: 0x%X", Status));
            break;
        }

        Status = WriteRegistryString(DevInstRegKeyHandle, L"HardwareInformation.DacType", StrHWInfoDacType);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryString failed DacType with Status: 0x%X", Status));
            break;
        }

        Status = WriteRegistryString(DevInstRegKeyHandle, L"HardwareInformation.AdapterString", StrHWInfoAdapterString);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryString failed for AdapterString with Status: 0x%X", Status));
            break;
        }

        Status = WriteRegistryString(DevInstRegKeyHandle, L"HardwareInformation.BiosString", StrHWInfoBiosString);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryString failed for BiosString with Status: 0x%X", Status));
            break;
        }

        DWORD MemorySize = MemSize;
        Status = WriteRegistryDWORD(DevInstRegKeyHandle, L"HardwareInformation.MemorySize", &MemorySize);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryDWORD failed for MemorySize with Status: 0x%X", Status));
            break;
        }

        DWORD DeviceId = Id;
        Status = WriteRegistryDWORD(DevInstRegKeyHandle, L"VioGpuAdapterID", &DeviceId);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryDWORD failed for VioGpuAdapterID with Status: 0x%X", Status));
        }
    } while (0);

    ZwClose(DevInstRegKeyHandle);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuAdapter::GetRegisterInfo(void)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    HANDLE DevInstRegKeyHandle;
    Status = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_READ, &DevInstRegKeyHandle);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("IoOpenDeviceRegistryKey failed for PDO: 0x%p, Status: 0x%X", m_pPhysicalDevice, Status));
        return Status;
    }

    DWORD value = 0;
    Status = ReadRegistryDWORD(DevInstRegKeyHandle, L"HWCursor", &value);
    if (NT_SUCCESS(Status))
    {
        SetPointerEnabled(!!value);
    }

    value = 0;
    Status = ReadRegistryDWORD(DevInstRegKeyHandle, L"FlexResolution", &value);
    if (NT_SUCCESS(Status))
    {
        SetFlexResolution(!!value);
    }

    value = 0;
    Status = ReadRegistryDWORD(DevInstRegKeyHandle, L"UsePhysicalMemory", &value);
    if (NT_SUCCESS(Status))
    {
        SetUsePhysicalMemory(!!value);
    }

    ZwClose(DevInstRegKeyHandle);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}
PAGED_CODE_SEG_END

PAGED_CODE_SEG_BEGIN

NTSTATUS VioGpuAdapter::VioGpuAdapterInit()
{
    PAGED_CODE();
    NTSTATUS status = STATUS_SUCCESS;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (IsHardwareInit())
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("Already Initialized\n"));
        VioGpuDbgBreak();
        return status;
    }
    status = VirtIoDeviceInit();
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize virtio device, error %x\n", status));
        VioGpuDbgBreak();
        return status;
    }

    m_shmem_allocator.Init(m_VioDev.shmem_len);

    m_u64HostFeatures = virtio_get_features(&m_VioDev);
    m_u64GuestFeatures = 0;
    do
    {
        struct virtqueue *vqs[2];
        if (!AckFeature(VIRTIO_GPU_F_VIRGL))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("VioGpu3D cannot start because virgl is not enabled\n"));
            status = STATUS_NOT_SUPPORTED;
            break;
        }
        if (!AckFeature(VIRTIO_GPU_F_CONTEXT_INIT))
        {
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("VIRTIO_GPU_F_CONTEXT_INIT not supported; Venus contexts are unavailable\n"));
        }
        if (!AckFeature(VIRTIO_GPU_F_RESOURCE_BLOB))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("VIRTIO_GPU_F_RESOURCE_BLOB not supported by host\n"));
        }
        if (!AckFeature(VIRTIO_F_VERSION_1))
        {
            status = STATUS_UNSUCCESSFUL;
            break;
        }
        // Ctrl-queue buffers are currently described with CPU physical
        // addresses. Do not negotiate ACCESS_PLATFORM until those buffers use
        // DMA-remapping-safe addresses. /akre
        #if (NTDDI_VERSION >= NTDDI_WIN10)
        //AckFeature(VIRTIO_F_ACCESS_PLATFORM);
        #endif


        if (!AckFeature(VIRTIO_F_VERSION_1))
        {
            status = STATUS_UNSUCCESSFUL;
            break;
        }

        status = virtio_set_features(&m_VioDev, m_u64GuestFeatures);
        if (!NT_SUCCESS(status))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("%s virtio_set_features failed with %x\n", __FUNCTION__, status));
            VioGpuDbgBreak();
            break;
        }

        status = virtio_find_queues(&m_VioDev, 2, vqs);
        if (!NT_SUCCESS(status))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("virtio_find_queues failed with error %x\n", status));
            VioGpuDbgBreak();
            break;
        }

        if (!ctrlQueue.Init(&m_VioDev, vqs[0], 0) || !m_CursorQueue.Init(&m_VioDev, vqs[1], 1))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize virtio queues\n"));
            status = STATUS_INSUFFICIENT_RESOURCES;
            VioGpuDbgBreak();
            break;
        }

        ctrlQueue.SetSynchronizeExecution(this);

        virtio_get_config(&m_VioDev,
                          FIELD_OFFSET(GPU_CONFIG, num_scanouts),
                          &m_u32NumScanouts,
                          sizeof(m_u32NumScanouts));

        virtio_get_config(&m_VioDev, FIELD_OFFSET(GPU_CONFIG, num_capsets), &m_u32NumCapsets, sizeof(m_u32NumCapsets));
    } while (0);
    if (status == STATUS_SUCCESS)
    {
        virtio_device_ready(&m_VioDev);
        InterlockedExchange(&m_InterruptsClosing, 0);
        SetHardwareInit(TRUE);
    }
    else
    {
        virtio_add_status(&m_VioDev, VIRTIO_CONFIG_S_FAILED);
        VioGpuDbgBreak();
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return status;
}

void VioGpuAdapter::VioGpuAdapterClose()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_FATAL, ("---> %s\n", __FUNCTION__));
    vidpn.StopVsyncTimer();
    StopWorkThread();

    if (IsHardwareInit())
    {
        InterlockedExchange(&m_InterruptsClosing, 1);
        SynchronizeInterruptsForClose();
        ctrlQueue.DisableInterrupt();
        m_CursorQueue.DisableInterrupt();
        KeFlushQueuedDpcs();
        SetHardwareInit(FALSE);
        InterlockedExchange((PLONG)&m_PendingWorks, 0);
        KeClearEvent(&m_ConfigUpdateEvent);
        virtio_device_reset(&m_VioDev);
        for (UINT node = 0; node < kMaxTrackedNodes; ++node)
        {
            for (UINT engine = 0; engine < kMaxTrackedEngines; ++engine)
            {
                VIOGPU_DMA_RETIREMENT_QUEUE *queue = &m_dmaRetirement[node][engine];
                while (queue->Head)
                {
                    VIOGPU_DMA_RETIREMENT *entry = queue->Head;
                    queue->Head = entry->Next;
                    entry->Next = NULL;
                    if (entry->Packet)
                    {
                        PGPU_VBUFFER packet = static_cast<PGPU_VBUFFER>(entry->Packet);
                        entry->Packet = NULL;
                        // Teardown releases ownership, never reports successful DMA.
                        packet->complete_cb(packet->complete_ctx);
                        ctrlQueue.ReleaseBuffer(packet);
                    }
                }
                queue->Tail = NULL;
            }
        }
        virtio_delete_queues(&m_VioDev);
        ctrlQueue.Close();
        m_CursorQueue.Close();
        virtio_device_shutdown(&m_VioDev);
    }
    m_shmem_allocator.Reset();
    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s\n", __FUNCTION__));
}

void VioGpuAdapter::SynchronizeInterruptsForClose(void)
{
    PAGED_CODE();

    const ULONG messageCount = m_PciResources.IsMSIEnabled() ? 3 : 1;
    for (ULONG messageNumber = 0; messageNumber < messageCount; messageNumber++)
    {
        BOOLEAN callbackRet = FALSE;
        NTSTATUS status = m_DxgkInterface.DxgkCbSynchronizeExecution(m_DxgkInterface.DeviceHandle,
                                                                      InterruptCloseBarrier,
                                                                      NULL,
                                                                      messageNumber,
                                                                      &callbackRet);
        if (!NT_SUCCESS(status) || !callbackRet)
        {
            DbgPrint(TRACE_LEVEL_FATAL,
                     ("%s failed message=%lu status=0x%x callback=%u -> bugcheck\n",
                      __FUNCTION__,
                      messageNumber,
                      status,
                      callbackRet));
            KeBugCheckEx(0x000000E2,
                         static_cast<ULONG_PTR>('SIVg'),
                         static_cast<ULONG_PTR>(messageNumber),
                         static_cast<ULONG_PTR>(status),
                         static_cast<ULONG_PTR>(callbackRet));
        }
    }
}

BOOLEAN VioGpuAdapter::AckFeature(UINT64 Feature)
{
    PAGED_CODE();

    if (virtio_is_feature_enabled(m_u64HostFeatures, Feature))
    {
        virtio_feature_enable(m_u64GuestFeatures, Feature);
        return TRUE;
    }
    return FALSE;
}

NTSTATUS VioGpuAdapter::VirtIoDeviceInit()
{
    PAGED_CODE();

    return virtio_device_initialize(&m_VioDev,
                                    &VioGpuSystemOps,
                                    reinterpret_cast<IVioGpuPCI *>(this),
                                    m_PciResources.IsMSIEnabled());
}

VOID VioGpuAdapter::CreateResolutionEvent(VOID)
{
    PAGED_CODE();

    if (m_ResolutionEvent != NULL && m_ResolutionEventHandle != NULL)
    {
        return;
    }
    DECLARE_UNICODE_STRING_SIZE(DeviceNumber, 10);
    DECLARE_UNICODE_STRING_SIZE(EventName, 256);

    RtlIntegerToUnicodeString(m_Id, 10, &DeviceNumber);
    NTSTATUS status = RtlUnicodeStringPrintf(&EventName,
                                             L"%ws%ws%ws",
                                             BASE_NAMED_OBJECTS,
                                             RESOLUTION_EVENT_NAME,
                                             DeviceNumber.Buffer);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("RtlUnicodeStringPrintf failed 0x%x\n", status));
        return;
    }
    m_ResolutionEvent = IoCreateNotificationEvent(&EventName, &m_ResolutionEventHandle);
    if (m_ResolutionEvent == NULL)
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("<--> %s\n", __FUNCTION__));
        return;
    }
    KeClearEvent(m_ResolutionEvent);
    ObReferenceObject(m_ResolutionEvent);
}

VOID VioGpuAdapter::NotifyResolutionEvent(VOID)
{
    PAGED_CODE();

    if (m_ResolutionEvent != NULL)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("NotifyResolutionEvent\n"));
        KeSetEvent(m_ResolutionEvent, IO_NO_INCREMENT, FALSE);
        KeClearEvent(m_ResolutionEvent);
    }
}

VOID VioGpuAdapter::CloseResolutionEvent(VOID)
{
    PAGED_CODE();

    if (m_ResolutionEventHandle != NULL)
    {
        ZwClose(m_ResolutionEventHandle);
        m_ResolutionEventHandle = NULL;
    }

    if (m_ResolutionEvent != NULL)
    {
        ObDereferenceObject(m_ResolutionEvent);
        m_ResolutionEvent = NULL;
    }
}

NTSTATUS VioGpuAdapter::StartWorkThread(void)
{
    PAGED_CODE();

    if (m_pWorkThread != NULL)
    {
        return STATUS_SUCCESS;
    }

    m_bStopWorkThread = FALSE;

    HANDLE threadHandle = NULL;
    NTSTATUS status = PsCreateSystemThread(&threadHandle,
                                           (ACCESS_MASK)0,
                                           NULL,
                                           (HANDLE)0,
                                           NULL,
                                           VioGpuAdapter::ThreadWork,
                                           this);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s failed to create system thread, status 0x%x\n", __FUNCTION__, status));
        return status;
    }

    PETHREAD workThread = NULL;
    status = ObReferenceObjectByHandle(threadHandle,
                                       THREAD_ALL_ACCESS,
                                       NULL,
                                       KernelMode,
                                       reinterpret_cast<PVOID *>(&workThread),
                                       NULL);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("%s failed to reference system thread, status 0x%x -> bugcheck\n", __FUNCTION__, status));
        KeBugCheckEx(0x000000E2,
                     static_cast<ULONG_PTR>('RIVg'),
                     reinterpret_cast<ULONG_PTR>(threadHandle),
                     static_cast<ULONG_PTR>(status),
                     0);
    }

    ZwClose(threadHandle);
    m_pWorkThread = workThread;
    return STATUS_SUCCESS;
}

void VioGpuAdapter::StopWorkThread(void)
{
    PAGED_CODE();

    PETHREAD workThread = m_pWorkThread;
    if (workThread == NULL)
    {
        m_bStopWorkThread = TRUE;
        return;
    }

    m_bStopWorkThread = TRUE;
    KeSetEvent(&m_ConfigUpdateEvent, IO_NO_INCREMENT, FALSE);

    ULONG timeoutCount = 0;
    for (;;)
    {
        LARGE_INTEGER timeout = {};
        timeout.QuadPart = Int32x32To64(VIOGPU_WORK_THREAD_WAIT_LOG_INTERVAL_MS, -10000);
        NTSTATUS status = KeWaitForSingleObject(workThread,
                                                Executive,
                                                KernelMode,
                                                FALSE,
                                                &timeout);
        if (status == STATUS_TIMEOUT)
        {
            timeoutCount++;
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("%s worker still stopping after %lu x %u ms thread=%p\n",
                      __FUNCTION__,
                      timeoutCount,
                      VIOGPU_WORK_THREAD_WAIT_LOG_INTERVAL_MS,
                      workThread));
            continue;
        }

        if (status != STATUS_SUCCESS)
        {
            DbgPrint(TRACE_LEVEL_FATAL,
                     ("%s permanent worker wait failure status=0x%x thread=%p -> bugcheck\n",
                      __FUNCTION__,
                      status,
                      workThread));
            KeBugCheckEx(0x000000E2,
                         static_cast<ULONG_PTR>('TIVg'),
                         reinterpret_cast<ULONG_PTR>(workThread),
                         static_cast<ULONG_PTR>(status),
                         static_cast<ULONG_PTR>(timeoutCount));
        }
        break;
    }

    m_pWorkThread = NULL;
    ObDereferenceObject(workThread);
}

NTSTATUS VioGpuAdapter::HWInit(PCM_RESOURCE_LIST pResList)
{
    PAGED_CODE();

    NTSTATUS status = STATUS_SUCCESS;
    DbgPrint(TRACE_LEVEL_INFORMATION, ("---> %s\n", __FUNCTION__));
    UINT size = 0;
    do
    {
        if (!m_PciResources.Init(GetDxgkInterface(), pResList))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("Incomplete resources\n"));
            status = STATUS_INSUFFICIENT_RESOURCES;
            VioGpuDbgBreak();
            break;
        }

        status = VioGpuAdapterInit();
        if (!NT_SUCCESS(status))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("%s Failed initialize adapter %x\n", __FUNCTION__, status));
            VioGpuDbgBreak();
            break;
        }

        size = ctrlQueue.QueryAllocation() + m_CursorQueue.QueryAllocation();
        DbgPrint(TRACE_LEVEL_FATAL, ("%s size %d\n", __FUNCTION__, size));
        ASSERT(size);

        if (!m_GpuBuf.Init(size))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize buffers\n"));
            status = STATUS_INSUFFICIENT_RESOURCES;
            VioGpuDbgBreak();
            break;
        }

        ctrlQueue.SetGpuBuf(&m_GpuBuf);
        m_CursorQueue.SetGpuBuf(&m_GpuBuf);

        if (!resourceIdr.Init(1))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize id generator\n"));
            status = STATUS_INSUFFICIENT_RESOURCES;
            VioGpuDbgBreak();
            break;
        }

        if (!ctxIdr.Init(1))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize id generator\n"));
            status = STATUS_INSUFFICIENT_RESOURCES;
            VioGpuDbgBreak();
            break;
        }

        // Capset queries are synchronous. A stalled host is reported by the
        // control-queue wait diagnostics without abandoning the request.
        m_supportedCapsetIDs = 0;
        NegotiateCapsets();

    } while (0);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    PHYSICAL_ADDRESS fb_pa = m_PciResources.GetPciBar(0)->GetPA();
    UINT fb_size = (UINT)m_PciResources.GetPciBar(0)->GetSize();

    // FIXME
#if NTDDI_VERSION > NTDDI_WINBLUE
    UINT req_size = 0x1000000;
#else
    UINT req_size = 0x800000;
#endif

    if (!IsUsePhysicalMemory() || fb_pa.QuadPart == 0 || fb_size < req_size)
    {
        fb_pa.QuadPart = 0LL;
        fb_size = max(req_size, fb_size);
    }

    if (!frameSegment.Init(fb_size, &fb_pa))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s failed to allocate FB memory segment\n", __FUNCTION__));
        status = STATUS_INSUFFICIENT_RESOURCES;
        VioGpuDbgBreak();
        VioGpuAdapterClose();
        return status;
    }

    status = StartWorkThread();
    if (!NT_SUCCESS(status))
    {
        frameSegment.Close();
        VioGpuAdapterClose();
        return status;
    }

    return status;
}

NTSTATUS VioGpuAdapter::HWClose(void)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_INFORMATION, ("---> %s\n", __FUNCTION__));
    StopWorkThread();

    frameSegment.Close();

    DbgPrint(TRACE_LEVEL_INFORMATION, ("<--- %s\n", __FUNCTION__));

    return STATUS_SUCCESS;
}

BOOLEAN FindUpdateRect(_In_ ULONG NumMoves,
                       _In_ D3DKMT_MOVE_RECT *pMoves,
                       _In_ ULONG NumDirtyRects,
                       _In_ PRECT pDirtyRect,
                       _In_ D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation,
                       _Out_ PRECT pUpdateRect)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Rotation);
    BOOLEAN updated = FALSE;

    if (pUpdateRect == NULL)
    {
        return FALSE;
    }

    if (NumMoves == 0 && NumDirtyRects == 0)
    {
        pUpdateRect->bottom = 0;
        pUpdateRect->left = 0;
        pUpdateRect->right = 0;
        pUpdateRect->top = 0;
    }

    for (ULONG i = 0; i < NumMoves; i++)
    {
        PRECT pRect = &pMoves[i].DestRect;
        if (!updated)
        {
            *pUpdateRect = *pRect;
            updated = TRUE;
        }
        else
        {
            pUpdateRect->bottom = max(pRect->bottom, pUpdateRect->bottom);
            pUpdateRect->left = min(pRect->left, pUpdateRect->left);
            pUpdateRect->right = max(pRect->right, pUpdateRect->right);
            pUpdateRect->top = min(pRect->top, pUpdateRect->top);
        }
    }
    for (ULONG i = 0; i < NumDirtyRects; i++)
    {
        PRECT pRect = &pDirtyRect[i];
        if (!updated)
        {
            *pUpdateRect = *pRect;
            updated = TRUE;
        }
        else
        {
            pUpdateRect->bottom = max(pRect->bottom, pUpdateRect->bottom);
            pUpdateRect->left = min(pRect->left, pUpdateRect->left);
            pUpdateRect->right = max(pRect->right, pUpdateRect->right);
            pUpdateRect->top = min(pRect->top, pUpdateRect->top);
        }
    }
    if (Rotation == D3DKMDT_VPPR_ROTATE90 || Rotation == D3DKMDT_VPPR_ROTATE270)
    {
    }
    return updated;
}

NTSTATUS VioGpuAdapter::UpdateChildStatus(BOOLEAN connect)
{
    PAGED_CODE();
    NTSTATUS Status(STATUS_SUCCESS);
    DXGK_CHILD_STATUS ChildStatus;
    PDXGKRNL_INTERFACE pDXGKInterface(GetDxgkInterface());

    RtlZeroMemory(&ChildStatus, sizeof(ChildStatus));

    ChildStatus.Type = StatusConnection;
    ChildStatus.ChildUid = 0;
    ChildStatus.HotPlug.Connected = connect;
    Status = pDXGKInterface->DxgkCbIndicateChildStatus(pDXGKInterface->DeviceHandle, &ChildStatus);
    if (Status != STATUS_SUCCESS)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<--- %s DxgkCbIndicateChildStatus failed with status %x\n ", __FUNCTION__, Status));
    }
    return Status;
}

PAGED_CODE_SEG_END

BOOLEAN VioGpuAdapter::InterruptRoutine(_In_ ULONG MessageNumber)
{
    if (!IsHardwareInit() || InterlockedCompareExchange(&m_InterruptsClosing, 0, 0) != 0)
    {
        return FALSE;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s MessageNumber = %d\n", __FUNCTION__, MessageNumber));
    BOOLEAN serviced = TRUE;
    ULONG intReason = 0;
    // return FALSE;
    if (m_PciResources.IsMSIEnabled())
    {
        switch (MessageNumber)
        {
            case 0:
                intReason = ISR_REASON_CHANGE;
                break;
            case 1:
                intReason = ISR_REASON_DISPLAY;
                break;
            case 2:
                intReason = ISR_REASON_CURSOR;
                break;
            default:
                serviced = FALSE;
                DbgPrint(TRACE_LEVEL_FATAL,
                         ("---> %s Unknown Interrupt Reason MessageNumber%d\n", __FUNCTION__, MessageNumber));
        }
    }
    else
    {
        UNREFERENCED_PARAMETER(MessageNumber);
        UCHAR isrstat = virtio_read_isr_status(&m_VioDev);

        switch (isrstat)
        {
            case 1:
                intReason = (ISR_REASON_DISPLAY | ISR_REASON_CURSOR);
                break;
            case 3:
                intReason = ISR_REASON_CHANGE;
                break;
            default:
                serviced = FALSE;
        }
    }

    if (serviced)
    {
        if (intReason & ISR_REASON_DISPLAY)
        {
            UINT len = 0;
            PGPU_VBUFFER pvbuf = NULL;

            while ((pvbuf = ctrlQueue.DequeueBufferFromIsr(&len)) != NULL)
            {
                if (pvbuf->complete_cb == VioGpuCommand::RunningCbDone && pvbuf->complete_ctx != NULL)
                {
                    VioGpuCommand *cmd = reinterpret_cast<VioGpuCommand *>(pvbuf->complete_ctx);
                    const PGPU_CTRL_HDR request = reinterpret_cast<PGPU_CTRL_HDR>(pvbuf->buf);
                    const PGPU_CTRL_HDR response = reinterpret_cast<PGPU_CTRL_HDR>(pvbuf->resp_buf);
                    if ((request->flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) &&
                        (!response || len < sizeof(*response) ||
                         response->type != VIRTIO_GPU_RESP_OK_NODATA ||
                         !(response->flags & VIRTIO_GPU_FLAG_FENCE) ||
                         response->fence_id != request->fence_id))
                    {
                        // An error response is not GPU completion. Retain this
                        // DMA and later engine retirements for the scheduler's
                        // existing timeout/reset path; never signal success.
                        cmd->Retirement.Failed = 1;
                        DbgPrint(TRACE_LEVEL_ERROR,
                                 ("%s GPU timeline response failed owner=viogpu3d fence=%u len=%u response=%#x; DMA retained for timeout/reset\n",
                                  __FUNCTION__, cmd->GetSubmissionFenceId(), len,
                                  response && len >= sizeof(*response) ? response->type : 0));
                    }
                    UINT fenceId = 0;
                    UINT nodeOrdinal = 0;
                    UINT engineOrdinal = 0;
                    if (cmd->OnPacketCompletedFromIsr(&fenceId, &nodeOrdinal, &engineOrdinal))
                    {
                        // Hold the final packet reference until every older DMA
                        // on this engine has completed. A CPU/legacy timeline
                        // response must never overtake a pending GPU timeline.
                        cmd->Retirement.Packet = pvbuf;
                        cmd->Retirement.Length = len;
                        VIOGPU_DMA_RETIREMENT_QUEUE *queue = &m_dmaRetirement[nodeOrdinal][engineOrdinal];
                        VIOGPU_DMA_RETIREMENT *ready;
                        while ((ready = VioGpuDmaRetirementPopReady(queue)) != NULL)
                        {
                            PGPU_VBUFFER packet = static_cast<PGPU_VBUFFER>(ready->Packet);
                            VioGpuCommand *retired = static_cast<VioGpuCommand *>(packet->complete_ctx);
                            const UINT retiredFence = retired->GetSubmissionFenceId();
                            const ULONG ctxId = retired->GetContextId();
                            const HANDLE ownerPid = retired->GetOwnerProcessId();
                            if (ShouldNotifyDmaFence(retiredFence, nodeOrdinal, engineOrdinal, ctxId, ownerPid))
                            {
                                DXGKARGCB_NOTIFY_INTERRUPT_DATA interrupt = {};
                                interrupt.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
                                interrupt.DmaCompleted.SubmissionFenceId = retiredFence;
                                interrupt.DmaCompleted.NodeOrdinal = nodeOrdinal;
                                interrupt.DmaCompleted.EngineOrdinal = engineOrdinal;
                                m_DxgkInterface.DxgkCbNotifyInterrupt(m_DxgkInterface.DeviceHandle, &interrupt);
                            }
                            RecordDmaCompletionForPreemptionFromIsr(retiredFence, nodeOrdinal, engineOrdinal, ctxId, ownerPid);
                            const UINT packetLength = ready->Length;
                            ready->Packet = NULL;
                            CtrlStagePushFromIsr(packet, packetLength);
                        }
                        continue; // Final packet was staged above or retained.
                    }
                }

                CtrlStagePushFromIsr(pvbuf, len);
            }
        }
        if (IsVsyncInterruptEnabled())
        {
            if (InterlockedExchange(&vidpn.m_vsync, 0))
            {
                PHYSICAL_ADDRESS sourceAddress = {};
                vidpn.DequeueSourceAddress(&sourceAddress);
                DXGKARGCB_NOTIFY_INTERRUPT_DATA interrupt = {};
                interrupt.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC;
                interrupt.CrtcVsync.VidPnTargetId = 0;
                interrupt.CrtcVsync.PhysicalAddress = sourceAddress;
                m_DxgkInterface.DxgkCbNotifyInterrupt(m_DxgkInterface.DeviceHandle, &interrupt);
            }
        }
        InterlockedOr((PLONG)&m_PendingWorks, intReason);
        m_DxgkInterface.DxgkCbQueueDpc(m_DxgkInterface.DeviceHandle);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return serviced;
}

void VioGpuAdapter::ThreadWork(_In_ PVOID Context)
{
    VioGpuAdapter *pdev = reinterpret_cast<VioGpuAdapter *>(Context);
    pdev->ThreadWorkRoutine();
}

void VioGpuAdapter::ThreadWorkRoutine(void)
{
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);

    for (;;)
    {
        KeWaitForSingleObject(&m_ConfigUpdateEvent, Executive, KernelMode, FALSE, NULL);

        if (m_bStopWorkThread)
        {
            PsTerminateSystemThread(STATUS_SUCCESS);
            break;
        }

        ConfigChanged();
        NotifyResolutionEvent();
    }
}

void VioGpuAdapter::NegotiateCapsets(void)
{
    PAGED_CODE();

    for (UINT32 i = 0; i < m_u32NumCapsets; i++)
    {
        PGPU_VBUFFER vbuf = NULL;

        DbgPrint(TRACE_LEVEL_VERBOSE,
                 ("%s querying capset info index=%d/%d\n", __FUNCTION__, i, m_u32NumCapsets));

        if (!ctrlQueue.AskCapsetInfo(&vbuf, i))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("%s AskCapsetInfo failed for index %d\n", __FUNCTION__, i));
            continue;
        }

        PGPU_RESP_CAPSET_INFO resp = (PGPU_RESP_CAPSET_INFO)vbuf->resp_buf;
        ULONG capset_id = resp->capset_id;
        if (capset_id == 0 || capset_id > VIRTIO_GPU_MAX_CAPSET_ID)
        {
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("%s invalid capset response index=%d id=%d resp_type=0x%x\n",
                      __FUNCTION__,
                      i,
                      capset_id,
                      resp->hdr.type));
            ctrlQueue.ReleaseBuffer(vbuf);
            continue; // Invalid capset id, capsets ids are in range from 1 to 63 per specification
        }
        m_capsetInfos[capset_id].id = capset_id;
        m_capsetInfos[capset_id].max_size = resp->capset_max_size;
        m_capsetInfos[capset_id].max_version = resp->capset_max_version;
        m_supportedCapsetIDs |= 1ull << capset_id;
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("CAPSET INFO %d    id: %d; version: %d; size: %d\n",
                  i,
                  capset_id,
                  resp->capset_max_size,
                  resp->capset_max_version));
        ctrlQueue.ReleaseBuffer(vbuf);
    }

    if (m_supportedCapsetIDs == 0)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s no capsets negotiated (num_capsets=%d), continuing with limited functionality\n",
                  __FUNCTION__,
                  m_u32NumCapsets));
    }
}

void VioGpuAdapter::ConfigChanged(void)
{
    DbgPrint(TRACE_LEVEL_FATAL, ("<--> %s\n", __FUNCTION__));
    UINT32 events_read, events_clear = 0;
    virtio_get_config(&m_VioDev, FIELD_OFFSET(GPU_CONFIG, events_read), &events_read, sizeof(m_u32NumScanouts));
    if (events_read & VIRTIO_GPU_EVENT_DISPLAY)
    {
        vidpn.GetDisplayInfo();
        events_clear |= VIRTIO_GPU_EVENT_DISPLAY;
        virtio_set_config(&m_VioDev, FIELD_OFFSET(GPU_CONFIG, events_clear), &events_clear, sizeof(m_u32NumScanouts));
        //        UpdateChildStatus(FALSE);
        //        ProcessEdid();
        UpdateChildStatus(TRUE);
    }
}

bool VioGpuAdapter::GetShmemCpuTranslatedAddress(PHYSICAL_ADDRESS *out_pa)
{
    PAGED_CODE();

    if (!out_pa)
    {
        return false;
    }

    ULONGLONG shmem_len = m_VioDev.shmem_len;
    CPciBar *shmem_bar = m_PciResources.GetPciBar(m_VioDev.shmem_bar);
    if (!shmem_bar || shmem_len == 0)
    {
        return false;
    }

    PHYSICAL_ADDRESS shmem_pa = shmem_bar->GetPA();
    shmem_pa.QuadPart += m_VioDev.shmem_offset;
    *out_pa = shmem_pa;

    return true;
}

VioGpuAllocationReference::VioGpuAllocationReference(VioGpuAdapter *adapter, D3DKMT_HANDLE handle)
    : m_adapter(adapter), m_allocation(NULL)
#if VIOGPU_WDDM2
    , m_releaseHandle(NULL)
#endif
{
    PAGED_CODE();

    DXGKARGCB_GETHANDLEDATA getHandleData = {};
    getHandleData.hObject = handle;
    getHandleData.Type = DXGK_HANDLE_ALLOCATION;
    getHandleData.Flags.Value = 0;
    PDXGKRNL_INTERFACE dxgkInterface = m_adapter->GetDxgkInterface();

#if VIOGPU_WDDM2
    // WDDM 2.0 requires the reference-taking callback for allocation data.
    if (!dxgkInterface->DxgkCbAcquireHandleData || !dxgkInterface->DxgkCbReleaseHandleData)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s missing WDDM 2.0 allocation reference callbacks adapter=%p handle=0x%x\n",
                  __FUNCTION__, adapter, handle));
        return;
    }
    m_allocation = reinterpret_cast<VioGpuAllocation *>(
        dxgkInterface->DxgkCbAcquireHandleData(&getHandleData, &m_releaseHandle));
#else
    // The explicitly selected WDDM 1.3 build uses its original callback contract.
    m_allocation = reinterpret_cast<VioGpuAllocation *>(
        dxgkInterface->DxgkCbGetHandleData(&getHandleData));
#endif
}

VioGpuAllocationReference::~VioGpuAllocationReference()
{
    PAGED_CODE();

#if VIOGPU_WDDM2
    if (m_releaseHandle)
    {
        DXGKARGCB_RELEASEHANDLEDATA releaseHandleData = {};
        releaseHandleData.ReleaseHandle = m_releaseHandle;
        releaseHandleData.Type = DXGK_HANDLE_ALLOCATION;
        m_adapter->GetDxgkInterface()->DxgkCbReleaseHandleData(releaseHandleData);
    }
#endif
}

VioGpuResource *VioGpuAdapter::ResourceFromHandle(D3DKMT_HANDLE handle)
{
    DXGKARGCB_GETHANDLEDATA getHandleData;
    getHandleData.hObject = handle;
    getHandleData.Type = DXGK_HANDLE_RESOURCE;
    getHandleData.Flags.DeviceSpecific = 0;
    return reinterpret_cast<VioGpuResource *>(m_DxgkInterface.DxgkCbGetHandleData(&getHandleData));
}
