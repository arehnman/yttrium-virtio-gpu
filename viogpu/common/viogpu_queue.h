/*
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

#pragma once
#include "viogpu.h"

#pragma pack(1)
typedef struct virtio_gpu_config
{
    u32 events_read;
    u32 events_clear;
    u32 num_scanouts;
    u32 num_capsets;
} GPU_CONFIG, *PGPU_CONFIG;
#pragma pack()

// #pragma pack(1)
typedef struct virtio_gpu_vbuffer
{
    char *buf;
    int size;

    void *data_buf;
    u32 data_size;

    char *resp_buf;
    int resp_size;
    LIST_ENTRY list_entry;
    LIST_ENTRY ctrl_queue_entry;
    LIST_ENTRY isr_stage_entry;
    UINT isr_stage_len;

    void (*complete_cb)(void *ctx);
    void *complete_ctx;

    bool auto_release;
} GPU_VBUFFER, *PGPU_VBUFFER;

typedef struct viogpu_complete_ctx
{
    PGPU_VBUFFER vbuf;
    void (*user_cb)(void *ctx);
    void *user_ctx;
    void *owner;
} VIOGPU_COMPLETE_CTX, *PVIOGPU_COMPLETE_CTX;
// #pragma pack()

typedef BOOLEAN (*VIOGPU_SYNC_EXEC_ROUTINE)(void *ctx);

class IVioGpuQueueSync
{
  public:
    virtual ~IVioGpuQueueSync() {}
    virtual BOOLEAN ExecuteSynchronized(VIOGPU_SYNC_EXEC_ROUTINE routine, void *routineCtx) = 0;
};

#define MAX_INLINE_CMD_SIZE  96
#define MAX_INLINE_RESP_SIZE 24
#define VBUFFER_SIZE         (sizeof(GPU_VBUFFER) + MAX_INLINE_CMD_SIZE + MAX_INLINE_RESP_SIZE)

class VioGpuBuf
{
  public:
    VioGpuBuf();
    ~VioGpuBuf();
    PGPU_VBUFFER GetBuf(_In_ int size, _In_ int resp_size, _In_opt_ void *resp_buf);
    void FreeBuf(_In_ PGPU_VBUFFER pbuf);
    BOOLEAN Init(_In_ UINT cnt);

  private:
    void Close(void);

  private:
    LIST_ENTRY m_FreeBufs;
    LIST_ENTRY m_InUseBufs;
    KSPIN_LOCK m_SpinLock;
    UINT m_uCount;
    UINT m_uCountMin = 0;
};

class VioGpuMemSegment
{
  public:
    VioGpuMemSegment(void);
    ~VioGpuMemSegment(void);
    SIZE_T GetSize(void)
    {
        return m_Size;
    }
    PVOID GetVirtualAddress(void)
    {
        return m_pVAddr;
    }
    PHYSICAL_ADDRESS GetPhysicalAddress(void);
    PSCATTER_GATHER_LIST GetSGList(void)
    {
        return m_pSGList;
    }
    BOOLEAN Init(_In_ UINT size, _In_opt_ PPHYSICAL_ADDRESS pPAddr);
    BOOLEAN IsSystemMemory(void)
    {
        return m_bSystemMemory;
    }
    void Close(void);

  private:
    BOOLEAN m_bSystemMemory;
    BOOLEAN m_bMapped;
    PSCATTER_GATHER_LIST m_pSGList;
    PVOID m_pVAddr;
    PMDL m_pMdl;
    SIZE_T m_Size;
};

class VioGpuObj
{
  public:
    VioGpuObj(void);
    ~VioGpuObj(void);
    void SetId(_In_ UINT id)
    {
        m_uiHwRes = id;
    }
    UINT GetId(void)
    {
        return m_uiHwRes;
    }
    BOOLEAN Init(_In_ UINT size, VioGpuMemSegment *pSegment);
    SIZE_T GetSize(void)
    {
        return m_Size;
    }
    PSCATTER_GATHER_LIST GetSGList(void)
    {
        return m_pSegment ? m_pSegment->GetSGList() : NULL;
    }
    PHYSICAL_ADDRESS GetPhysicalAddress(void)
    {
        PHYSICAL_ADDRESS pa = {0};
        return m_pSegment ? m_pSegment->GetPhysicalAddress() : pa;
    }
    PVOID GetVirtualAddress(void)
    {
        return m_pSegment ? m_pSegment->GetVirtualAddress() : NULL;
    }

  private:
    UINT m_uiHwRes;
    SIZE_T m_Size;
    VioGpuMemSegment *m_pSegment;
};

class VioGpuQueue
{
  public:
    VioGpuQueue();
    ~VioGpuQueue();
    BOOLEAN Init(_In_ VirtIODevice *pVIODevice, _In_ struct virtqueue *pVirtQueue, _In_ UINT index);
    void Close(void);
    int AddBuf(_In_ struct VirtIOBufferDescriptor sg[],
               _In_ UINT out_num,
               _In_ UINT in_num,
               _In_ void *data,
               _In_opt_ void *va_indirect,
               _In_ ULONGLONG phys_indirect)
    {
        return m_pVirtQueue ? virtqueue_add_buf(m_pVirtQueue, sg, out_num, in_num, data, va_indirect, phys_indirect)
                            : 0;
    }
    void *GetBuf(_Out_ UINT *len)
    {
        if (m_pVirtQueue)
        {
            return virtqueue_get_buf(m_pVirtQueue, len);
        }
        *len = 0;
        return NULL;
    }
    void Kick()
    {
        if (m_pVirtQueue)
        {
            //
            // virtqueue_kick() asks the ring whether the host actually wants a
            // notification before writing the notify register.  Every MMIO
            // write out of a guest is a VM exit, so an unconditional kick
            // costs roughly a microsecond whether or not anyone is listening.
            //
            // This used to be virtqueue_kick_always().  That was harmless when
            // viogpu only carried modeset, cursor and the occasional resource
            // op - a few kicks a second.  Once the same queue is used to
            // submit 3D command streams it becomes about 3200 kicks per frame
            // against 11 batches, and the exits measured 4.8 ms per frame,
            // 22% of the frame, second only to the driver's own CPU time.
            //
            // Every other driver in this tree - netkvm, viostor, vioscsi,
            // balloon, viorng, vioserial, viosock, viofs - already kicks this
            // way; viogpu was the only caller of the unconditional form.
            //
            // Spelled out with the two inlines rather than virtqueue_kick(),
            // which is a real function in VirtIOPCICommon.c - a file this
            // driver does not build.  These two are inline in VirtIO.h and
            // are what virtqueue_kick() does anyway.
            //
            if (virtqueue_kick_prepare(m_pVirtQueue))
            {
                virtqueue_kick_always(m_pVirtQueue);
            }
        }
    }
    bool EnableInterrupt(void)
    {
        return m_pVirtQueue ? virtqueue_enable_cb(m_pVirtQueue) : false;
    }
    VOID DisableInterrupt(void)
    {
        if (m_pVirtQueue)
        {
            virtqueue_disable_cb(m_pVirtQueue);
        }
    }
    UINT QueryAllocation();
    void SetGpuBuf(_In_ VioGpuBuf *pbuf)
    {
        m_pBuf = pbuf;
    }
    void ReleaseBuffer(PGPU_VBUFFER buf);

  protected:
    _IRQL_requires_max_(DISPATCH_LEVEL) _IRQL_saves_global_(OldIrql,
                                                            Irql) _IRQL_raises_(DISPATCH_LEVEL) void Lock(KIRQL *Irql);
    _IRQL_requires_(DISPATCH_LEVEL) _IRQL_restores_global_(OldIrql, Irql) void Unlock(KIRQL Irql);

  private:
    struct virtqueue *m_pVirtQueue;
    VirtIODevice *m_pVIODevice;
    UINT m_Index;
    KSPIN_LOCK m_SpinLock;

  protected:
    VioGpuBuf *m_pBuf;
};

class CtrlQueue : public VioGpuQueue
{
  public:
    CtrlQueue() : VioGpuQueue()
    {
        m_FenceIdr = 0;
        InitializeListHead(&m_CtrlQueueList);
        KeInitializeSpinLock(&m_CtrlQueueSpinLock);
        m_CtrlQueueFlushInProgress = 0;
        m_CtrlQueueFlushRequested = 0;
        m_CtrlQueueFullRequeues = 0;
    };

    PVOID AllocCmd(PGPU_VBUFFER *buf, int sz);
    PVOID AllocCmdResp(PGPU_VBUFFER *buf, int cmd_sz, PVOID resp_buf, int resp_sz);

    UINT QueueBufferFenced(PGPU_VBUFFER vbuf);
    PGPU_VBUFFER DequeueBuffer(_Out_ UINT *len);
    PGPU_VBUFFER DequeueBufferFromIsr(_Out_ UINT *len);
    void SetSynchronizeExecution(IVioGpuQueueSync *syncExec)
    {
        m_SyncExec = syncExec;
    }
    UINT Flush();

    void CreateResource(UINT res_id, UINT format, UINT width, UINT height);
    void CreateResource3D(UINT res_id, VIOGPU_RESOURCE_OPTIONS *options);

    NTSTATUS CreateResourceBlob(UINT res_id,
                                ULONGLONG size,
                                ULONG blob_mem,
                                ULONG blob_flags,
                                ULONGLONG blob_id,
                                UINT ctx_id,
                                PGPU_MEM_ENTRY ents,
                                UINT nents,
                                void (*complete_cb)(void *),
                                void *complete_ctx);
    BOOLEAN ResourceMapBlob(UINT res_id, ULONGLONG offset, ULONG *map_info);
    void ResourceUnmapBlob(UINT res_id);
    void DestroyResource(UINT id);
    void CtxResource(bool attach, UINT ctx_id, UINT res_id);
    NTSTATUS CtxResourceWait(UINT ctx_id, UINT res_id);

    UINT SubmitCommand(void *cmdbuf,
                       ULONG size,
                       ULONG ctx_id,
                       void (*complete_cb)(void *),
                       void *complete_ctx,
                       UCHAR ringIndex = 0);
    UINT TransferHostCmd(bool to_host,
                         ULONG res_id,
                         VIOGPU_TRANSFER_CMD *options,
                         void (*complete_cb)(void *),
                         void *complete_ctx);

    void SetScanout(UINT scan_id, UINT res_id, UINT width, UINT height, UINT x, UINT y);
    void SetScanoutBlob(UINT scan_id,
                        UINT res_id,
                        UINT width,
                        UINT height,
                        UINT x,
                        UINT y,
                        UINT format,
                        UINT stride,
                        UINT offset);
    UINT ResFlush(UINT res_id,
                  UINT width,
                  UINT height,
                  UINT x,
                  UINT y,
                  void (*complete_cb)(void *) = NULL,
                  void *complete_ctx = NULL);
    UINT SubmitNop(void (*complete_cb)(void *), void *complete_ctx, BOOLEAN fenced = FALSE);
    void TransferToHost2D(UINT res_id, ULONG offset, UINT width, UINT height, UINT x, UINT y);
    void TransferToHost3D(UINT res_id, GPU_BOX *box);

    void AttachBacking(UINT res_id, PGPU_MEM_ENTRY ents, UINT nents);
    void DetachBacking(UINT id);

    BOOLEAN GetDisplayInfo(PGPU_VBUFFER buf, UINT id, PULONG xres, PULONG yres);
    BOOLEAN AskDisplayInfo(PGPU_VBUFFER *buf);
    BOOLEAN AskEdidInfo(PGPU_VBUFFER *buf, UINT id);
    BOOLEAN GetEdidInfo(PGPU_VBUFFER buf, UINT id, PBYTE edid);
    BOOLEAN AskCapsetInfo(PGPU_VBUFFER *buf, ULONG idx);
    BOOLEAN AskCapset(PGPU_VBUFFER *buf, ULONG capset_id, ULONG capset_size, ULONG capset_version);

    void CreateCtx(UINT ctx_id, UINT context_init);
    void DestroyCtx(UINT ctx_id);

  private:
    UINT AddBufferSerialized(VirtIOBufferDescriptor *sg,
                             UINT outcnt,
                             UINT incnt,
                             PGPU_VBUFFER buf,
                             BOOLEAN kickOnSuccess);
    UINT SubmitBuffer(PGPU_VBUFFER buf);
    UINT QueueBuffer(PGPU_VBUFFER buf);
    // Submit vbuf and wait for the host reply. Five-second waits are repeated
    // only to log stalled requests; a submitted request is never abandoned.
    // STATUS_SUCCESS => caller owns vbuf; failure => vbuf was reclaimed.
    NTSTATUS QueueSyncBuffer(PGPU_VBUFFER vbuf);

    volatile LONG m_FenceIdr;
    LIST_ENTRY m_CtrlQueueList;
    KSPIN_LOCK m_CtrlQueueSpinLock;
    volatile LONG m_CtrlQueueFlushInProgress;
    volatile LONG m_CtrlQueueFlushRequested;
    // Count of transient "control vq full" re-stagings. Non-zero here means the
    // queue is applying backpressure (packets staged, drained by a later Flush);
    // it is NOT an error. Watch this to confirm how often the vq runs dry.
    volatile LONG m_CtrlQueueFullRequeues;
    IVioGpuQueueSync *m_SyncExec = NULL;
};

class CrsrQueue : public VioGpuQueue
{
  public:
    PVOID AllocCursor(PGPU_VBUFFER *buf);
    UINT QueueCursor(PGPU_VBUFFER buf);
    PGPU_VBUFFER DequeueCursor(_Out_ UINT *len);
};
