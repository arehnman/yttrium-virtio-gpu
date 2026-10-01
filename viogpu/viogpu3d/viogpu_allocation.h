/*
 * Copyright 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 * 
 */

#pragma once
#include "helper.h"
#include "virgl_hw.h"
#include "viogpum.h"
#include "viogpu_blob_user_view.h"

class VioGpuAdapter;
class VioGpuDevice;

class VioGpuResource
{
  public:
  private:
};

class VioGpuAllocation
{
  public:
    VioGpuAllocation(VioGpuAdapter *adapter, VIOGPU_CREATE_ALLOCATION_EXCHANGE *exchange);
    ~VioGpuAllocation(void);

    UINT GetId(void)
    {
        return m_Id;
    }

    UINT GetWidth(void) const
    {
        return m_options.width;
    }

    UINT GetHeight(void) const
    {
        return m_options.height;
    }
    UINT GetFormat(void) const
    {
        return m_options.format;
    }
    UINT GetStride(void) const;
    UINT GetScanoutOffset(void) const
    {
        return m_scanout_offset;
    }

    void MarkBusy();
    void UnmarkBusy();

    void SetDxPhysicalAddress(size_t DxPhysicalAddress)
    {
        m_DxPhysicalAddress = DxPhysicalAddress;
    };

    size_t GetDxPhysicalAddress()
    {
        return m_DxPhysicalAddress;
    };

    BOOL IsCoherent()
    {
        return (m_options.flags & VIRGL_RESOURCE_FLAG_MAP_COHERENT) != 0;
    }

    bool IsValid() const
    {
        return m_valid;
    }
    bool IsBlob() const
    {
        return m_is_blob;
    }
    ULONGLONG GetBlobSize() const
    {
        return m_blob_size;
    }
    ULONG GetBlobFlags() const
    {
        return m_blob_flags;
    }
    bool IsBlobCreated() const
    {
        return m_blob_created;
    }
    bool HasCpuCopyBacking() const
    {
        return m_is_blob ? m_blob_kernel_va != NULL : m_cpu_copy_va != NULL;
    }
    bool IsBlobPending() const
    {
        return (m_blob_create_state == 1);
    }

    void EnsureBlobCreated(ULONG ctx_id);
    NTSTATUS EnsureBlobCreatedAndWait(ULONG ctx_id);
    NTSTATUS CreateBlobResource(UINT ctx_id,
                                void (*complete_cb)(void *) = NULL,
                                void *complete_ctx = NULL);

    void AttachBacking(MDL *pMdl, size_t pageCount, size_t pageOffset);
    void DetachBacking();

    void FlushToScreen(UINT scan_id);
    void FlushToScreen(UINT scan_id,
                       UINT width,
                       UINT height,
                       UINT x,
                       UINT y,
                       UINT format,
                       UINT stride,
                       UINT offset);

    static NTSTATUS GetStandardAllocationDriverData(DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *pStandardAllocation);
    static NTSTATUS DxgkCreateAllocation(VioGpuAdapter *adapter, DXGKARG_CREATEALLOCATION *pCreateAllocation);

    NTSTATUS DescribeAllocation(DXGKARG_DESCRIBEALLOCATION *pDescribeAllocation);
    NTSTATUS MapApertureSegment(DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer);
    NTSTATUS UnmapApertureSegment(DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer);

    NTSTATUS EscapeResourceInfo(VIOGPU_RES_INFO_REQ *resInfo);
    NTSTATUS EscapeResourceBusy(VIOGPU_RES_BUSY_REQ *resBusy);
    NTSTATUS EscapeResourceMapBlob(VIOGPU_RES_MAP_BLOB_REQ *resMap, VioGpuDevice *device);
    NTSTATUS EscapeResourceUnmapBlob(VIOGPU_RES_UNMAP_BLOB_REQ *resUnmap, VioGpuDevice *device);
    NTSTATUS EscapeResourceSetScanoutBlob(VIOGPU_RES_SET_SCANOUT_BLOB_REQ *resScanout);
    NTSTATUS MapForCpuCopy(ULONG ctx_id, PVOID *map_va, ULONGLONG *map_size, BOOLEAN *io_mapping);
    void UnmapForCpuCopy(PVOID map_va, ULONGLONG map_size, BOOLEAN io_mapping);

  private:
    static void BlobCreateCompleteCB(void *ctx_void);
    void WaitForPendingBlobCreate();
    struct BlobUserMapping
    {
        LIST_ENTRY ListEntry;
        PEPROCESS Process;
        HANDLE ProcessId;
        VIOGPU_BLOB_USER_VIEW View;
        /* Immutable while this allocation record is published. The registry
         * clears View.UserVa at exit; keep the response address separate. */
        PVOID UserVa;
        ULONGLONG Size;
        ULONG MapInfo;
        ULONG RefCount;
    };

    BlobUserMapping *FindBlobMappingLocked(HANDLE process_id, PEPROCESS process);

    VioGpuAdapter *m_adapter;

    VIOGPU_RESOURCE_OPTIONS m_options;
    UINT m_Id;
    bool m_is_blob;
    bool m_valid;
    ULONG m_stride;
    ULONG m_scanout_offset;
    ULONGLONG m_blob_size;
    ULONGLONG m_blob_id;
    ULONG m_blob_mem;
    ULONG m_blob_flags;
    ULONGLONG m_blob_offset;
    bool m_blob_shmem_allocated;
    ULONG m_blob_map_info;
    bool m_blob_mapped;
    PVOID m_blob_kernel_va;
    ULONGLONG m_blob_kernel_map_size;
    bool m_blob_created;
    NTSTATUS m_blob_create_status;
    volatile LONG m_blob_create_state;
    KEVENT m_blob_create_complete_event;
    EX_RUNDOWN_REF m_blob_create_rundown;
    KMUTEX m_blob_map_mutex;
    LIST_ENTRY m_blob_map_list;
    ULONG m_blob_map_user_refs;
    volatile LONG m_resource_created;

    MDL *m_pMDL;
    size_t m_pageCount;
    size_t m_pageOffset;
    FAST_MUTEX m_cpu_copy_mutex;
    PVOID m_cpu_copy_va;
    ULONGLONG m_cpu_copy_map_size;

    size_t m_DxPhysicalAddress;

    KEVENT m_busyNotification;
    volatile LONG m_busy;
};
