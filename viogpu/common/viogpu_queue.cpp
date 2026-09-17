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

#include "viogpu_queue.h"
#include "baseobj.h"
#include "virgl_hw.h"
#if !DBG
#include "viogpu_queue.tmh"
#endif

static BOOLEAN BuildSGElement(VirtIOBufferDescriptor *sg, PVOID buf, ULONG size)
{
    if (size != 0 && MmIsAddressValid(buf))
    {
        sg->length = min(size, PAGE_SIZE);
        sg->physAddr = MmGetPhysicalAddress(buf);
        return TRUE;
    }
    return FALSE;
}

__declspec(noreturn) static void BugCheckCtrlQueueSubmitFailure(PGPU_VBUFFER buf, UINT ret)
{
    UINT commandType = 0;
    if (buf && buf->buf && buf->size >= sizeof(GPU_CTRL_HDR) && MmIsAddressValid(buf->buf))
    {
        commandType = reinterpret_cast<PGPU_CTRL_HDR>(buf->buf)->type;
    }

    DbgPrint(TRACE_LEVEL_FATAL,
             ("<--> %s permanent ctrlq submit failure buf=%p command_type=0x%x ret=0x%x -> bugcheck\n",
              __FUNCTION__,
              buf,
              commandType,
              ret));
    KeBugCheckEx(0x000000E2,
                 static_cast<ULONG_PTR>('QIVg'),
                 reinterpret_cast<ULONG_PTR>(buf),
                 static_cast<ULONG_PTR>(commandType),
                 static_cast<ULONG_PTR>(ret));
}

#define VIOGPU_CTRL_WAIT_LOG_INTERVAL_MS 5000

typedef struct _CTRL_QUEUE_WAIT_CTX
{
    PGPU_VBUFFER vbuf;
    UINT command_type;
    UINT context_id;
    UINT expected_response_type;
    ULONGLONG expected_fence_id;
    BOOLEAN validate_fence;
    KEVENT event;
    NTSTATUS status;
    volatile LONG refs;
} CTRL_QUEUE_WAIT_CTX, *PCTRL_QUEUE_WAIT_CTX;

static PCTRL_QUEUE_WAIT_CTX CtrlQueueAllocWaitCtx(void)
{
    PCTRL_QUEUE_WAIT_CTX ctx = new (NonPagedPoolNx) CTRL_QUEUE_WAIT_CTX();
    if (!ctx)
    {
        return NULL;
    }

    RtlZeroMemory(ctx, sizeof(*ctx));
    KeInitializeEvent(&ctx->event, NotificationEvent, FALSE);
    ctx->status = STATUS_PENDING;
    ctx->refs = 2;
    return ctx;
}

static void CtrlQueueSetWaitBuffer(PCTRL_QUEUE_WAIT_CTX ctx, PGPU_VBUFFER vbuf)
{
    ctx->vbuf = vbuf;
    if (vbuf && vbuf->buf && vbuf->size >= sizeof(GPU_CTRL_HDR) && MmIsAddressValid(vbuf->buf))
    {
        PGPU_CTRL_HDR hdr = reinterpret_cast<PGPU_CTRL_HDR>(vbuf->buf);
        ctx->command_type = hdr->type;
        ctx->context_id = hdr->ctx_id;
    }
}

static void CtrlQueueReleaseWaitCtx(PCTRL_QUEUE_WAIT_CTX ctx)
{
    if (ctx && InterlockedDecrement(&ctx->refs) == 0)
    {
        delete ctx;
    }
}

static void CtrlQueueCancelWaitCtx(PCTRL_QUEUE_WAIT_CTX ctx)
{
    CtrlQueueReleaseWaitCtx(ctx);
    CtrlQueueReleaseWaitCtx(ctx);
}

static void CtrlQueueWaitCompleteCB(void *ctx_void)
{
    PCTRL_QUEUE_WAIT_CTX ctx = reinterpret_cast<PCTRL_QUEUE_WAIT_CTX>(ctx_void);
    if (!ctx)
    {
        return;
    }

    ctx->status = STATUS_SUCCESS;
    if (!ctx->vbuf || !ctx->vbuf->resp_buf)
    {
        ctx->status = STATUS_UNSUCCESSFUL;
    }
    else
    {
        PGPU_CTRL_HDR resp = reinterpret_cast<PGPU_CTRL_HDR>(ctx->vbuf->resp_buf);
        if (!resp ||
            (ctx->expected_response_type && resp->type != ctx->expected_response_type) ||
            (!ctx->expected_response_type && resp->type >= VIRTIO_GPU_RESP_ERR_UNSPEC) ||
            (ctx->validate_fence &&
             (!(resp->flags & VIRTIO_GPU_FLAG_FENCE) ||
              resp->fence_id != ctx->expected_fence_id)))
        {
            ctx->status = STATUS_UNSUCCESSFUL;
        }
    }

    KeSetEvent(&ctx->event, IO_NO_INCREMENT, FALSE);
    CtrlQueueReleaseWaitCtx(ctx);
}

static NTSTATUS CtrlQueueWaitForCompletion(PCTRL_QUEUE_WAIT_CTX ctx)
{
    ULONG timeoutCount = 0;

    for (;;)
    {
        LARGE_INTEGER timeout = {};
        timeout.QuadPart = Int32x32To64(VIOGPU_CTRL_WAIT_LOG_INTERVAL_MS, -10000);

        NTSTATUS status = KeWaitForSingleObject(&ctx->event,
                                                Executive,
                                                KernelMode,
                                                FALSE,
                                                &timeout);
        if (status == STATUS_TIMEOUT)
        {
            timeoutCount++;
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("%s ctrlq request still pending after %lu x %u ms vbuf=%p command_type=0x%x ctx_id=%u\n",
                      __FUNCTION__,
                      timeoutCount,
                      VIOGPU_CTRL_WAIT_LOG_INTERVAL_MS,
                      ctx->vbuf,
                      ctx->command_type,
                      ctx->context_id));
            continue;
        }

        if (status != STATUS_SUCCESS)
        {
            DbgPrint(TRACE_LEVEL_FATAL,
                     ("%s permanent ctrlq wait failure status=0x%x vbuf=%p -> bugcheck\n",
                      __FUNCTION__,
                      status,
                      ctx->vbuf));
            KeBugCheckEx(0x000000E2,
                         static_cast<ULONG_PTR>('WIVg'),
                         reinterpret_cast<ULONG_PTR>(ctx->vbuf),
                         static_cast<ULONG_PTR>(status),
                         static_cast<ULONG_PTR>(timeoutCount));
        }

        status = ctx->status;
        CtrlQueueReleaseWaitCtx(ctx);
        return status;
    }
}

//
// Submit a control-queue buffer and wait for the host's response using a
// heap-allocated, reference-counted wait context.
//
// The five-second interval is diagnostic only. A timeout does not cancel a
// virtqueue descriptor, so the waiter logs and continues waiting for the same
// request rather than abandoning it or submitting a duplicate.
//
//   STATUS_SUCCESS - host replied OK; the caller owns vbuf and must
//                    ReleaseBuffer() it after reading the response.
//   failure        - the queue rejected the buffer before submission or the
//                    host returned an error response; vbuf has been reclaimed.
//
NTSTATUS CtrlQueue::QueueSyncBuffer(PGPU_VBUFFER vbuf)
{
    PCTRL_QUEUE_WAIT_CTX wait_ctx = CtrlQueueAllocWaitCtx();
    if (!wait_ctx)
    {
        // Not queued yet, so reclaiming the buffer here is safe.
        ReleaseBuffer(vbuf);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    CtrlQueueSetWaitBuffer(wait_ctx, vbuf);
    vbuf->complete_cb = CtrlQueueWaitCompleteCB;
    vbuf->complete_ctx = wait_ctx;
    vbuf->auto_release = false; // caller owns vbuf on success

    UINT ret = QueueBuffer(vbuf);
    if (ret == (UINT)-1)
    {
        // Rejected before reaching the virtqueue: no completion will arrive.
        ReleaseBuffer(vbuf);
        CtrlQueueCancelWaitCtx(wait_ctx);
        return STATUS_UNSUCCESSFUL;
    }

    NTSTATUS status = CtrlQueueWaitForCompletion(wait_ctx);
    if (!NT_SUCCESS(status))
    {
        // The host returned the descriptor with an error response. The caller
        // will not inspect it, so reclaim the completed buffer here.
        ReleaseBuffer(vbuf);
    }
    return status;
}

VioGpuQueue::VioGpuQueue()
{
    m_pBuf = NULL;
    m_Index = (UINT)-1;
    m_pVIODevice = NULL;
    m_pVirtQueue = NULL;
    KeInitializeSpinLock(&m_SpinLock);
}

VioGpuQueue::~VioGpuQueue()
{
    Close();
}

void VioGpuQueue::Close(void)
{
    KIRQL SavedIrql;
    Lock(&SavedIrql);
    m_pVirtQueue = NULL;
    Unlock(SavedIrql);
}

BOOLEAN VioGpuQueue::Init(_In_ VirtIODevice *pVIODevice, _In_ struct virtqueue *pVirtQueue, _In_ UINT index)
{
    if ((pVIODevice == NULL) || (pVirtQueue == NULL))
    {
        return FALSE;
    }
    m_pVIODevice = pVIODevice;
    m_pVirtQueue = pVirtQueue;
    m_Index = index;
    EnableInterrupt();
    return TRUE;
}

_IRQL_requires_max_(DISPATCH_LEVEL) _IRQL_saves_global_(OldIrql, Irql) _IRQL_raises_(DISPATCH_LEVEL) void VioGpuQueue::
                                                                                                    Lock(KIRQL *Irql)
{
    KIRQL SavedIrql = KeGetCurrentIrql();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s at IRQL %d\n", __FUNCTION__, SavedIrql));

    if (SavedIrql < DISPATCH_LEVEL)
    {
        KeAcquireSpinLock(&m_SpinLock, &SavedIrql);
    }
    else if (SavedIrql == DISPATCH_LEVEL)
    {
        KeAcquireSpinLockAtDpcLevel(&m_SpinLock);
    }
    else
    {
        VioGpuDbgBreak();
    }
    *Irql = SavedIrql;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

_IRQL_requires_(DISPATCH_LEVEL) _IRQL_restores_global_(OldIrql, Irql) void VioGpuQueue::Unlock(KIRQL Irql)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s at IRQL %d\n", __FUNCTION__, Irql));

    if (Irql < DISPATCH_LEVEL)
    {
        KeReleaseSpinLock(&m_SpinLock, Irql);
    }
    else
    {
        KeReleaseSpinLockFromDpcLevel(&m_SpinLock);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

PAGED_CODE_SEG_BEGIN

UINT VioGpuQueue::QueryAllocation()
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    USHORT NumEntries;
    ULONG RingSize, HeapSize;

    NTSTATUS status = virtio_query_queue_allocation(m_pVIODevice, m_Index, &NumEntries, &RingSize, &HeapSize);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("[%s] virtio_query_queue_allocation(%d) failed with error %x\n", __FUNCTION__, m_Index, status));
        return 0;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return NumEntries;
}
PAGED_CODE_SEG_END

PAGED_CODE_SEG_BEGIN

BOOLEAN CtrlQueue::GetDisplayInfo(PGPU_VBUFFER buf, UINT id, PULONG xres, PULONG yres)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_RESP_DISP_INFO resp = (PGPU_RESP_DISP_INFO)buf->resp_buf;
    if (resp->hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO)
    {
        DbgPrint(TRACE_LEVEL_VERBOSE, (" %s type = %x: disabled\n", __FUNCTION__, resp->hdr.type));
        return FALSE;
    }
    if (resp->pmodes[id].enabled)
    {
        DbgPrint(TRACE_LEVEL_VERBOSE,
                 ("output %d: %dx%d+%d+%d\n",
                  id,
                  resp->pmodes[id].r.width,
                  resp->pmodes[id].r.height,
                  resp->pmodes[id].r.x,
                  resp->pmodes[id].r.y));
        *xres = resp->pmodes[id].r.width;
        *yres = resp->pmodes[id].r.height;
    }
    else
    {
        DbgPrint(TRACE_LEVEL_VERBOSE, ("output %d: disabled\n", id));
        return FALSE;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return TRUE;
}

BOOLEAN CtrlQueue::AskDisplayInfo(PGPU_VBUFFER *buf)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_CTRL_HDR cmd;
    PGPU_VBUFFER vbuf = NULL;
    PGPU_RESP_DISP_INFO resp_buf;

    if (buf)
    {
        *buf = NULL;
    }

    resp_buf = reinterpret_cast<PGPU_RESP_DISP_INFO>(new (NonPagedPoolNx) BYTE[sizeof(GPU_RESP_DISP_INFO)]);

    if (!resp_buf)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("---> %s Failed allocate %d bytes\n", __FUNCTION__, sizeof(GPU_RESP_DISP_INFO)));
        return FALSE;
    }

    cmd = (PGPU_CTRL_HDR)AllocCmdResp(&vbuf, sizeof(GPU_CTRL_HDR), resp_buf, sizeof(GPU_RESP_DISP_INFO));
    RtlZeroMemory(cmd, sizeof(GPU_CTRL_HDR));

    cmd->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    NTSTATUS status = QueueSyncBuffer(vbuf);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s display info request failed status=0x%x\n", __FUNCTION__, status));
        return FALSE;
    }

    *buf = vbuf;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return TRUE;
}

BOOLEAN CtrlQueue::AskEdidInfo(PGPU_VBUFFER *buf, UINT id)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_CMD_GET_EDID cmd;
    PGPU_VBUFFER vbuf = NULL;
    PGPU_RESP_EDID resp_buf;

    if (buf)
    {
        *buf = NULL;
    }

    resp_buf = reinterpret_cast<PGPU_RESP_EDID>(new (NonPagedPoolNx) BYTE[sizeof(GPU_RESP_EDID)]);

    if (!resp_buf)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("---> %s Failed allocate %d bytes\n", __FUNCTION__, sizeof(GPU_RESP_EDID)));
        return FALSE;
    }
    cmd = (PGPU_CMD_GET_EDID)AllocCmdResp(&vbuf, sizeof(GPU_CMD_GET_EDID), resp_buf, sizeof(GPU_RESP_EDID));
    RtlZeroMemory(cmd, sizeof(GPU_CMD_GET_EDID));

    cmd->hdr.type = VIRTIO_GPU_CMD_GET_EDID;
    cmd->scanout = id;

    NTSTATUS status = QueueSyncBuffer(vbuf);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s edid request failed for scanout=%u status=0x%x\n",
                  __FUNCTION__, id, status));
        return FALSE;
    }

    *buf = vbuf;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return TRUE;
}

BOOLEAN CtrlQueue::GetEdidInfo(PGPU_VBUFFER buf, UINT id, PBYTE edid)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    PGPU_CMD_GET_EDID cmd = (PGPU_CMD_GET_EDID)buf->buf;
    PGPU_RESP_EDID resp = (PGPU_RESP_EDID)buf->resp_buf;
    PUCHAR resp_edit = (PUCHAR)(resp->edid + (ULONGLONG)id * EDID_V1_BLOCK_SIZE);
    if (resp->hdr.type != VIRTIO_GPU_RESP_OK_EDID)
    {
        DbgPrint(TRACE_LEVEL_VERBOSE, (" %s type = %x: disabled\n", __FUNCTION__, resp->hdr.type));
        return FALSE;
    }
    if (cmd->scanout != id)
    {
        DbgPrint(TRACE_LEVEL_VERBOSE, (" %s invalid scaout = %x\n", __FUNCTION__, cmd->scanout));
        return FALSE;
    }

    RtlCopyMemory(edid, resp_edit, EDID_RAW_BLOCK_SIZE);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    return TRUE;
}

BOOLEAN CtrlQueue::AskCapsetInfo(PGPU_VBUFFER *buf, ULONG idx)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_CMD_GET_CASPSET_INFO cmd;
    PGPU_VBUFFER vbuf = NULL;
    PGPU_RESP_CAPSET_INFO resp_buf;

    if (buf)
    {
        *buf = NULL;
    }

    resp_buf = reinterpret_cast<PGPU_RESP_CAPSET_INFO>(new (NonPagedPoolNx) BYTE[sizeof(GPU_RESP_CAPSET_INFO)]);

    if (!resp_buf)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("---> %s Failed allocate %d bytes\n", __FUNCTION__, sizeof(GPU_RESP_CAPSET_INFO)));
        return FALSE;
    }
    cmd = (PGPU_CMD_GET_CASPSET_INFO)AllocCmdResp(&vbuf,
                                                  sizeof(GPU_CMD_GET_CAPSET_INFO),
                                                  resp_buf,
                                                  sizeof(GPU_RESP_CAPSET_INFO));
    RtlZeroMemory(cmd, sizeof(GPU_CMD_GET_CAPSET_INFO));

    cmd->hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
    cmd->capset_index = idx;
    cmd->padding = 0;

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("%s queue GET_CAPSET_INFO index=%lu cmd_type=0x%x\n",
              __FUNCTION__,
              idx,
              cmd->hdr.type));

    NTSTATUS status = QueueSyncBuffer(vbuf);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s capset info request failed for index=%lu status=0x%x\n",
                  __FUNCTION__, idx, status));
        return FALSE;
    }

    PGPU_RESP_CAPSET_INFO resp = reinterpret_cast<PGPU_RESP_CAPSET_INFO>(vbuf->resp_buf);
    if (!resp)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("%s null response buffer for capset index=%lu\n", __FUNCTION__, idx));
        ReleaseBuffer(vbuf);
        return FALSE;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("%s capset index=%lu resp_type=0x%x capset_id=%lu max_ver=%lu max_size=%lu\n",
              __FUNCTION__,
              idx,
              resp->hdr.type,
              resp->capset_id,
              resp->capset_max_version,
              resp->capset_max_size));

    *buf = vbuf;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return TRUE;
}

BOOLEAN CtrlQueue::AskCapset(PGPU_VBUFFER *buf, ULONG capset_id, ULONG capset_size, ULONG capset_version)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_CMD_GET_CASPSET cmd;
    PGPU_VBUFFER vbuf = NULL;
    PGPU_RESP_CAPSET resp_buf;
    int resp_size = sizeof(GPU_RESP_CAPSET) + capset_size;

    if (buf)
    {
        *buf = NULL;
    }

    resp_buf = reinterpret_cast<PGPU_RESP_CAPSET>(new (NonPagedPoolNx) BYTE[resp_size]);

    if (!resp_buf)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("---> %s Failed allocate %d bytes\n", __FUNCTION__, resp_size));
        return FALSE;
    }
    cmd = (PGPU_CMD_GET_CASPSET)AllocCmdResp(&vbuf, sizeof(GPU_CMD_GET_CAPSET), resp_buf, resp_size);
    RtlZeroMemory(cmd, sizeof(GPU_CMD_GET_CAPSET));

    cmd->hdr.type = VIRTIO_GPU_CMD_GET_CAPSET;
    cmd->capset_id = capset_id;
    cmd->capset_version = capset_version;

    NTSTATUS status = QueueSyncBuffer(vbuf);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s capset request failed for id=%lu status=0x%x\n",
                  __FUNCTION__, capset_id, status));
        return FALSE;
    }

    *buf = vbuf;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return TRUE;
}

void CtrlQueue::CreateResource(UINT res_id, UINT format, UINT width, UINT height)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_RES_CREATE_2D cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_RES_CREATE_2D)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd->resource_id = res_id;
    cmd->format = format;
    cmd->width = width;
    cmd->height = height;

    // FIXME!!! if
    QueueBufferFenced(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void CtrlQueue::CreateResource3D(UINT res_id, VIOGPU_RESOURCE_OPTIONS *options)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_CMD_RES_CREATE_3D cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_CMD_RES_CREATE_3D)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    cmd->res_id = res_id;

    cmd->target = options->target;
    cmd->format = options->format;
    cmd->bind = options->bind;
    cmd->width = options->width;
    cmd->height = options->height;
    cmd->depth = options->depth;
    cmd->array_size = options->array_size;
    cmd->last_level = options->last_level;
    cmd->nr_samples = options->nr_samples;

    // FIXME!!! if
    QueueBufferFenced(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

NTSTATUS CtrlQueue::CreateResourceBlob(UINT res_id,
                                       ULONGLONG size,
                                       ULONG blob_mem,
                                       ULONG blob_flags,
                                       ULONGLONG blob_id,
                                       UINT ctx_id,
                                       PGPU_MEM_ENTRY ents,
                                       UINT nents,
                                       void (*complete_cb)(void *),
                                       void *complete_ctx)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id = 0x%x\n", __FUNCTION__, res_id));

    PGPU_RES_CREATE_BLOB cmd;
    PGPU_VBUFFER vbuf;
    PGPU_CTRL_HDR resp_buf;

    cmd = (PGPU_RES_CREATE_BLOB)AllocCmdResp(&vbuf, sizeof(*cmd), NULL, sizeof(GPU_CTRL_HDR));
    if (!cmd || !vbuf)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(cmd, sizeof(*cmd));
    resp_buf = (PGPU_CTRL_HDR)vbuf->resp_buf;
    if (resp_buf)
    {
        RtlZeroMemory(resp_buf, sizeof(GPU_CTRL_HDR));
    }

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    cmd->hdr.ctx_id = ctx_id;
    cmd->resource_id = res_id;
    cmd->blob_mem = blob_mem;
    cmd->blob_flags = blob_flags;
    cmd->nr_entries = nents;
    cmd->blob_id = blob_id;
    cmd->size = size;

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("---> %s hdr.type=0x%x ctx_id=0x%x res_id=0x%x blob_mem=0x%x blob_flags=0x%x nr_entries=0x%x blob_id=0x%llx size=0x%llx\n",
              __FUNCTION__, cmd->hdr.type, cmd->hdr.ctx_id, cmd->resource_id, cmd->blob_mem, cmd->blob_flags,
              cmd->nr_entries, cmd->blob_id, cmd->size));

    if (ents && nents)
    {
        vbuf->data_buf = ents;
        vbuf->data_size = sizeof(*ents) * nents;
    }

    if (complete_cb) {
        if (complete_ctx) {
            PVIOGPU_COMPLETE_CTX ctx = (PVIOGPU_COMPLETE_CTX)complete_ctx;
            ctx->vbuf = vbuf;
        }
        vbuf->complete_cb = complete_cb;
        vbuf->complete_ctx = complete_ctx;
    }

    QueueBufferFenced(vbuf);
    return STATUS_SUCCESS;
}

BOOLEAN CtrlQueue::ResourceMapBlob(UINT res_id, ULONGLONG offset, ULONG *map_info)
{
    PAGED_CODE();

    if (!map_info)
    {
        return FALSE;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id = 0x%x offset = 0x%llx\n", __FUNCTION__, res_id, offset));

    PGPU_RES_MAP_BLOB cmd;
    PGPU_VBUFFER vbuf = NULL;
    PGPU_RESP_MAP_INFO resp_buf;

    resp_buf = reinterpret_cast<PGPU_RESP_MAP_INFO>(new (NonPagedPoolNx) BYTE[sizeof(GPU_RESP_MAP_INFO)]);
    if (!resp_buf)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("---> %s ERROR Failed allocate %d bytes\n", __FUNCTION__, sizeof(GPU_RESP_MAP_INFO)));
        return FALSE;
    }

    cmd = (PGPU_RES_MAP_BLOB)AllocCmdResp(&vbuf, sizeof(GPU_RES_MAP_BLOB), resp_buf, sizeof(GPU_RESP_MAP_INFO));
    RtlZeroMemory(cmd, sizeof(GPU_RES_MAP_BLOB));

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    cmd->resource_id = res_id;
    cmd->offset = offset;

    NTSTATUS status = QueueSyncBuffer(vbuf);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("---> %s ERROR map blob failed res_id=0x%x status=0x%x\n", __FUNCTION__, res_id, status));
        return FALSE;
    }

    BOOLEAN ret = FALSE;
    if (resp_buf->hdr.type != VIRTIO_GPU_RESP_OK_MAP_INFO)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("---> %s ERROR invalid response type 0x%x\n", __FUNCTION__, resp_buf->hdr.type));
    }
    else
    {
        *map_info = resp_buf->map_info;
        ret = TRUE;
    }

    ReleaseBuffer(vbuf);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s res_id = 0x%x\n", __FUNCTION__, res_id));
    return ret;
}

void CtrlQueue::ResourceUnmapBlob(UINT res_id)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id = 0x%x\n", __FUNCTION__, res_id));

    PGPU_RES_UNMAP_BLOB cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_RES_UNMAP_BLOB)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB;
    cmd->resource_id = res_id;

    QueueBufferFenced(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s res_id = 0x%x\n", __FUNCTION__, res_id));
}

void CtrlQueue::CreateCtx(UINT ctx_id, UINT context_init)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_CMD_CTX_CREATE cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_CMD_CTX_CREATE)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    cmd->hdr.ctx_id = ctx_id;
    cmd->nlen = 0;
    cmd->context_init = context_init;

    // FIXME!!! if
    QueueBuffer(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void CtrlQueue::DestroyCtx(UINT ctx_id)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_CMD_CTX_DESTROY cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_CMD_CTX_DESTROY)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    cmd->hdr.ctx_id = ctx_id;

    // FIXME!!! if
    QueueBuffer(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

PAGED_CODE_SEG_END

UINT CtrlQueue::ResFlush(UINT res_id,
                         UINT width,
                         UINT height,
                         UINT x,
                         UINT y,
                         void (*complete_cb)(void *),
                         void *complete_ctx)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    PGPU_RES_FLUSH cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_RES_FLUSH)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd->resource_id = res_id;
    cmd->r.width = width;
    cmd->r.height = height;
    cmd->r.x = x;
    cmd->r.y = y;

    vbuf->complete_cb = complete_cb;
    vbuf->complete_ctx = complete_ctx;

    UINT ret = QueueBufferFenced(vbuf);
    if (ret)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s failed to queue flush res=%u rect=%ux%u+%u+%u ret=%u\n",
                  __FUNCTION__,
                  res_id,
                  width,
                  height,
                  x,
                  y,
                  ret));
        ReleaseBuffer(vbuf);
        return ret;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return ret;
}

PAGED_CODE_SEG_BEGIN

void CtrlQueue::TransferToHost2D(UINT res_id, ULONG offset, UINT width, UINT height, UINT x, UINT y)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    PGPU_RES_TRANSF_TO_HOST_2D cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_RES_TRANSF_TO_HOST_2D)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd->resource_id = res_id;
    cmd->offset = offset;
    cmd->r.width = width;
    cmd->r.height = height;
    cmd->r.x = x;
    cmd->r.y = y;

    UINT ret = QueueBufferFenced(vbuf);
    if (ret)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s failed to queue transfer2d res=%u offset=%lu rect=%ux%u+%u+%u ret=%u\n",
                  __FUNCTION__,
                  res_id,
                  offset,
                  width,
                  height,
                  x,
                  y,
                  ret));
        ReleaseBuffer(vbuf);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void CtrlQueue::TransferToHost3D(UINT res_id, GPU_BOX *box)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    PGPU_CMD_TRANSFER_HOST_3D cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_CMD_TRANSFER_HOST_3D)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    cmd->resource_id = res_id;
    cmd->offset = 0;
    cmd->level = 0;
    cmd->stride = 0;
    cmd->layer_stride = 0;

    memcpy(&cmd->box, box, sizeof(GPU_BOX));

    UINT ret = QueueBufferFenced(vbuf);
    if (ret)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s failed to queue transfer3d res=%u box=%ux%ux%u@%u,%u,%u ret=%u\n",
                  __FUNCTION__,
                  res_id,
                  box->width,
                  box->height,
                  box->depth,
                  box->x,
                  box->y,
                  box->z,
                  ret));
        ReleaseBuffer(vbuf);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void CtrlQueue::AttachBacking(UINT res_id, PGPU_MEM_ENTRY ents, UINT nents)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_RES_ATTACH_BACKING cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_RES_ATTACH_BACKING)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd->resource_id = res_id;
    cmd->nr_entries = nents;

    vbuf->data_buf = ents;
    vbuf->data_size = sizeof(*ents) * nents;

    QueueBufferFenced(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void CtrlQueue::CtxResource(bool attach, UINT ctx_id, UINT res_id)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_CMD_CTX_RESOURCE cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_CMD_CTX_RESOURCE)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = attach ? VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE : VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE;
    cmd->hdr.ctx_id = ctx_id;
    cmd->resource_id = res_id;

    QueueBuffer(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

NTSTATUS CtrlQueue::CtxResourceWait(UINT ctx_id, UINT res_id)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("---> %s ctx_id=0x%x res_id=0x%x\n",
              __FUNCTION__, ctx_id, res_id));

    PCTRL_QUEUE_WAIT_CTX wait_ctx = CtrlQueueAllocWaitCtx();
    if (!wait_ctx)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    PGPU_CMD_CTX_RESOURCE cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_CMD_CTX_RESOURCE)AllocCmd(&vbuf, sizeof(*cmd));
    if (!cmd || !vbuf)
    {
        CtrlQueueCancelWaitCtx(wait_ctx);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(cmd, sizeof(*cmd));

    /*
     * OpenAllocation may have queued its attach before the lazily-created
     * Venus context existed.  Re-issue the real attach here.  The current
     * proxy treats duplicate attaches as idempotent.
     *
     * INFO_RING_IDX routes the fence through the target renderer context.
     * On the Venus proxy, IMPORT_RESOURCE and the ring-0 SUBMIT_FENCE are then
     * sent on the same socket and processed in order.  A plain FENCE would use
     * QEMU's unrelated legacy context-0 fence.
     */
    cmd->hdr.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
    cmd->hdr.ctx_id = ctx_id;
    cmd->hdr.flags = VIRTIO_GPU_FLAG_FENCE | VIRTIO_GPU_FLAG_INFO_RING_IDX;
    cmd->hdr.fence_id = InterlockedIncrement(&m_FenceIdr);
    cmd->hdr.ring_idx = 0;
    cmd->resource_id = res_id;

    CtrlQueueSetWaitBuffer(wait_ctx, vbuf);
    wait_ctx->expected_response_type = VIRTIO_GPU_RESP_OK_NODATA;
    wait_ctx->expected_fence_id = cmd->hdr.fence_id;
    wait_ctx->validate_fence = TRUE;
    vbuf->complete_cb = CtrlQueueWaitCompleteCB;
    vbuf->complete_ctx = wait_ctx;

    UINT ret = QueueBuffer(vbuf);
    if (ret)
    {
        ReleaseBuffer(vbuf);
        CtrlQueueCancelWaitCtx(wait_ctx);
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s failed to queue context wait ctx_id=0x%x res_id=0x%x ret=%u\n",
                  __FUNCTION__, ctx_id, res_id, ret));
        return STATUS_UNSUCCESSFUL;
    }

    NTSTATUS status = CtrlQueueWaitForCompletion(wait_ctx);
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("<--- %s ctx_id=0x%x res_id=0x%x status=0x%x\n",
              __FUNCTION__, ctx_id, res_id, status));
    return status;
}

PAGED_CODE_SEG_END

UINT CtrlQueue::SubmitCommand(void *cmdbuf,
                              ULONG size,
                              ULONG ctx_id,
                              void (*complete_cb)(void *),
                              void *complete_ctx)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_CMD_SUBMIT cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_CMD_SUBMIT)AllocCmd(&vbuf, sizeof(*cmd));
    if (!cmd || !vbuf)
    {
        delete[] reinterpret_cast<PBYTE>(cmdbuf);
        return (UINT)-1;
    }
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    cmd->size = size;

    cmd->hdr.ctx_id = ctx_id;

    vbuf->data_buf = cmdbuf;
    vbuf->data_size = size;

    vbuf->complete_cb = complete_cb;
    vbuf->complete_ctx = complete_ctx;

    UINT ret = QueueBufferFenced(vbuf);
    if (ret)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s failed to queue submit ctx_id=%lu size=%lu ret=%u\n",
                  __FUNCTION__,
                  ctx_id,
                  size,
                  ret));
        ReleaseBuffer(vbuf);
        return ret;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return ret;
}

UINT CtrlQueue::SubmitNop(void (*complete_cb)(void *), void *complete_ctx, BOOLEAN fenced)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_CMD_SUBMIT cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_CMD_SUBMIT)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;

    vbuf->complete_cb = complete_cb;
    vbuf->complete_ctx = complete_ctx;

    UINT ret = fenced ? QueueBufferFenced(vbuf) : QueueBuffer(vbuf);
    if (ret)
    {
        ReleaseBuffer(vbuf);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return ret;
}


UINT CtrlQueue::TransferHostCmd(bool to_host,
                                ULONG ctx_id,
                                VIOGPU_TRANSFER_CMD *options,
                                void (*complete_cb)(void *),
                                void *complete_ctx)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_CMD_TRANSFER_HOST_3D cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_CMD_TRANSFER_HOST_3D)AllocCmd(&vbuf, sizeof(*cmd));
    if (!cmd || !vbuf)
    {
        return (UINT)-1;
    }
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = to_host ? VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D : VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;

    cmd->resource_id = options->res_id;

    cmd->box.x = options->box.x;
    cmd->box.y = options->box.y;
    cmd->box.z = options->box.z;
    cmd->box.width = options->box.width;
    cmd->box.height = options->box.height;
    cmd->box.depth = options->box.depth;

    cmd->offset = options->offset;
    cmd->level = options->level;
    cmd->stride = options->stride;
    cmd->layer_stride = options->layer_stride;

    cmd->hdr.ctx_id = ctx_id;

    vbuf->complete_cb = complete_cb;
    vbuf->complete_ctx = complete_ctx;

    UINT ret = QueueBufferFenced(vbuf);
    if (ret)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s failed to queue %s ctx_id=%lu res_id=%u ret=%u\n",
                  __FUNCTION__,
                  to_host ? "transfer_to_host" : "transfer_from_host",
                  ctx_id,
                  options->res_id,
                  ret));
        ReleaseBuffer(vbuf);
        return ret;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return ret;
}

void CtrlQueue::DestroyResource(UINT res_id)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_RES_UNREF cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_RES_UNREF)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    cmd->resource_id = res_id;

    QueueBuffer(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void CtrlQueue::DetachBacking(UINT res_id)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_RES_DETACH_BACKING cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_RES_DETACH_BACKING)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    cmd->resource_id = res_id;

    QueueBufferFenced(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

PVOID CtrlQueue::AllocCmdResp(PGPU_VBUFFER *buf, int cmd_sz, PVOID resp_buf, int resp_sz)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_VBUFFER vbuf;
    vbuf = m_pBuf->GetBuf(cmd_sz, resp_sz, resp_buf);
    ASSERT(vbuf);
    *buf = vbuf;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return vbuf ? vbuf->buf : NULL;
}

PVOID CtrlQueue::AllocCmd(PGPU_VBUFFER *buf, int sz)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (buf == NULL || sz == 0)
    {
        return NULL;
    }

    PGPU_VBUFFER vbuf = m_pBuf->GetBuf(sz, sizeof(GPU_CTRL_HDR), NULL);
    ASSERT(vbuf);
    *buf = vbuf;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s  vbuf = %p\n", __FUNCTION__, vbuf));

    return vbuf ? vbuf->buf : NULL;
}

void CtrlQueue::SetScanout(UINT scan_id, UINT res_id, UINT width, UINT height, UINT x, UINT y)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_SET_SCANOUT cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_SET_SCANOUT)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd->resource_id = res_id;
    cmd->scanout_id = scan_id;
    cmd->r.width = width;
    cmd->r.height = height;
    cmd->r.x = x;
    cmd->r.y = y;

    UINT ret = QueueBuffer(vbuf);
    if (ret)
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("%s failed to queue set_scanout scan=%u res=%u rect=%ux%u+%u+%u ret=%u\n",
                  __FUNCTION__,
                  scan_id,
                  res_id,
                  width,
                  height,
                  x,
                  y,
                  ret));
        BugCheckCtrlQueueSubmitFailure(vbuf, ret);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void CtrlQueue::SetScanoutBlob(UINT scan_id,
                               UINT res_id,
                               UINT width,
                               UINT height,
                               UINT x,
                               UINT y,
                               UINT format,
                               UINT stride,
                               UINT offset)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s scan_id=0x%x res_id=0x%x\n", __FUNCTION__, scan_id, res_id));

    PGPU_SET_SCANOUT_BLOB cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_SET_SCANOUT_BLOB)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT_BLOB;
    cmd->resource_id = res_id;
    cmd->scanout_id = scan_id;
    cmd->r.width = width;
    cmd->r.height = height;
    cmd->r.x = x;
    cmd->r.y = y;
    cmd->width = width;
    cmd->height = height;
    switch (format)
    {
        case VIRGL_FORMAT_B8G8R8A8_SRGB:
            cmd->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
            break;
        case VIRGL_FORMAT_R8G8B8A8_SRGB:
            cmd->format = VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM;
            break;
        default:
            cmd->format = format;
            break;
    }
    cmd->strides[0] = stride;
    cmd->offsets[0] = offset;

    UINT ret = QueueBuffer(vbuf);
    if (ret)
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("%s failed to queue set_scanout_blob scan=%u res=%u rect=%ux%u+%u+%u format=%u stride=%u offset=%u ret=%u\n",
                  __FUNCTION__,
                  scan_id,
                  res_id,
                  width,
                  height,
                  x,
                  y,
                  format,
                  stride,
                  offset,
                  ret));
        BugCheckCtrlQueueSubmitFailure(vbuf, ret);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

#define SGLIST_SIZE 256
static const UINT VIRTQUEUE_NO_SPACE = static_cast<UINT>(-28); // -ENOSPC

static BOOLEAN ValidateCtrlQueueBuffer(PGPU_VBUFFER buf)
{
    if (!buf)
    {
        return FALSE;
    }

    if (buf->size > PAGE_SIZE)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<--> %s size is too big %d\n", __FUNCTION__, buf->size));
        return FALSE;
    }

    if (buf->resp_size > PAGE_SIZE)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<--> %s resp_size is too big %d\n", __FUNCTION__, buf->resp_size));
        return FALSE;
    }

    const UINT dataSgCount = BYTES_TO_PAGES(buf->data_size);
    const UINT cmdSgCount = buf->size ? 1 : 0;
    const UINT respSgCount = buf->resp_size ? 1 : 0;
    if ((cmdSgCount + dataSgCount + respSgCount) > SGLIST_SIZE)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<--> %s no more sg element spots left cmd=%u data=%u resp=%u\n",
                  __FUNCTION__,
                  cmdSgCount,
                  dataSgCount,
                  respSgCount));
        return FALSE;
    }

    return TRUE;
}

static BOOLEAN BuildCtrlQueueSGList(PGPU_VBUFFER buf,
                                    VirtIOBufferDescriptor *sg,
                                    UINT sgSize,
                                    UINT *outcnt,
                                    UINT *incnt)
{
    UINT sgleft = sgSize;

    *outcnt = 0;
    *incnt = 0;

    if (!ValidateCtrlQueueBuffer(buf))
    {
        return FALSE;
    }

    if (buf->size)
    {
        if (!BuildSGElement(&sg[*outcnt + *incnt], (PVOID)buf->buf, buf->size))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("<--> %s failed to build command sg element\n", __FUNCTION__));
            return FALSE;
        }
        (*outcnt)++;
        sgleft--;
    }

    if (buf->data_size)
    {
        ULONG data_size = buf->data_size;
        PVOID data_buf = (PVOID)buf->data_buf;
        while (data_size)
        {
            if (sgleft == 0)
            {
                DbgPrint(TRACE_LEVEL_ERROR, ("<--> %s no more sg element spots left %u\n", __FUNCTION__, *outcnt));
                return FALSE;
            }

            const UINT sgIndex = *outcnt + *incnt;
            if (!BuildSGElement(&sg[sgIndex], data_buf, data_size))
            {
                DbgPrint(TRACE_LEVEL_ERROR, ("<--> %s failed to build data sg element\n", __FUNCTION__));
                return FALSE;
            }

            data_buf = (PVOID)((LONG_PTR)data_buf + sg[sgIndex].length);
            data_size -= sg[sgIndex].length;
            (*outcnt)++;
            sgleft--;
        }
    }

    if (buf->resp_size)
    {
        if (sgleft == 0)
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("<--> %s no more sg element spots left for response\n", __FUNCTION__));
            return FALSE;
        }

        if (!BuildSGElement(&sg[*outcnt + *incnt], (PVOID)buf->resp_buf, buf->resp_size))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("<--> %s failed to build response sg element\n", __FUNCTION__));
            return FALSE;
        }
        (*incnt)++;
        sgleft--;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s sgleft %d\n", __FUNCTION__, sgleft));

    return TRUE;
}

typedef struct _CTRLQUEUE_SYNC_ADD_CTX
{
    CtrlQueue *queue;
    VirtIOBufferDescriptor *sg;
    UINT outcnt;
    UINT incnt;
    PGPU_VBUFFER buf;
    BOOLEAN kickOnSuccess;
    UINT ret;
} CTRLQUEUE_SYNC_ADD_CTX, *PCTRLQUEUE_SYNC_ADD_CTX;

static BOOLEAN CtrlQueueSyncAddRoutine(void *ctxVoid)
{
    PCTRLQUEUE_SYNC_ADD_CTX ctx = (PCTRLQUEUE_SYNC_ADD_CTX)ctxVoid;
    ctx->ret = ctx->queue->AddBuf(ctx->sg, ctx->outcnt, ctx->incnt, ctx->buf, NULL, 0);
    if (ctx->kickOnSuccess && ctx->ret == 0)
    {
        ctx->queue->Kick();
    }
    return TRUE;
}

UINT CtrlQueue::AddBufferSerialized(VirtIOBufferDescriptor *sg,
                                    UINT outcnt,
                                    UINT incnt,
                                    PGPU_VBUFFER buf,
                                    BOOLEAN kickOnSuccess)
{
    // viogpu3d installs a queue sync provider so add/get are serialized with ISR.
    // viogpudo does not, so keep the local lock fallback for compatibility.
    if (m_SyncExec)
    {
        CTRLQUEUE_SYNC_ADD_CTX syncCtx = {};
        syncCtx.queue = this;
        syncCtx.sg = sg;
        syncCtx.outcnt = outcnt;
        syncCtx.incnt = incnt;
        syncCtx.buf = buf;
        syncCtx.kickOnSuccess = kickOnSuccess;
        syncCtx.ret = (UINT)-1;

        if (!m_SyncExec->ExecuteSynchronized(CtrlQueueSyncAddRoutine, &syncCtx))
        {
            return (UINT)-1;
        }

        return syncCtx.ret;
    }

    KIRQL SavedIrql;
    UINT ret = 0;
    Lock(&SavedIrql);
    ret = AddBuf(sg, outcnt, incnt, buf, NULL, 0);
    if (kickOnSuccess && ret == 0)
    {
        Kick();
    }
    Unlock(SavedIrql);
    return ret;
}

UINT CtrlQueue::SubmitBuffer(PGPU_VBUFFER buf)
{
    VirtIOBufferDescriptor sg[SGLIST_SIZE];
    UINT outcnt = 0;
    UINT incnt = 0;

    if (!BuildCtrlQueueSGList(buf, &sg[0], SGLIST_SIZE, &outcnt, &incnt))
    {
        return (UINT)-1;
    }

    return AddBufferSerialized(&sg[0], outcnt, incnt, buf, TRUE);
}

UINT CtrlQueue::Flush()
{
    UINT ret = 0;

    if (InterlockedCompareExchange(&m_CtrlQueueFlushInProgress, 1, 0) != 0)
    {
        return 0;
    }

    for (;;)
    {
        InterlockedExchange(&m_CtrlQueueFlushRequested, 0);

        PLIST_ENTRY entry;
        while ((entry = ExInterlockedRemoveHeadList(&m_CtrlQueueList, &m_CtrlQueueSpinLock)) != NULL)
        {
            PGPU_VBUFFER queuedBuf = CONTAINING_RECORD(entry, GPU_VBUFFER, ctrl_queue_entry);

            ret = SubmitBuffer(queuedBuf);
            if (ret == 0)
            {
                continue;
            }

            if (ret != VIRTQUEUE_NO_SPACE)
            {
                // Only descriptor exhaustion is retryable. SG construction,
                // synchronized execution, and unexpected virtqueue failures are
                // fatal invariants. QueueBuffer() already transferred ownership,
                // so neither completion nor recycling is truthful here.
                BugCheckCtrlQueueSubmitFailure(queuedBuf, ret);
            }

            // Transient: the control virtqueue is out of descriptors (-ENOSPC).
            // This is NOT a failure of this buffer. It stays staged (re-inserted
            // at the head so submission order is preserved) and is drained by a
            // later Flush() -- e.g. from the completion DPC once the host returns
            // descriptors. The caller must never observe this as its buffer
            // failing, or it will release a buffer the queue still owns and the
            // retry will resubmit/free it a second time (data_buf UAF -> host
            // reads a stale/other submit payload).
            LONG requeues = InterlockedIncrement(&m_CtrlQueueFullRequeues);
            DbgPrint(TRACE_LEVEL_INFORMATION,
                     ("<--> %s ctrl vq full (ret=0x%x) re-staging buf=%p total_requeues=%ld\n",
                      __FUNCTION__, ret, queuedBuf, requeues));

            ExInterlockedInsertHeadList(&m_CtrlQueueList, &queuedBuf->ctrl_queue_entry, &m_CtrlQueueSpinLock);
            InterlockedExchange(&m_CtrlQueueFlushInProgress, 0);
            return ret;
        }

        InterlockedExchange(&m_CtrlQueueFlushInProgress, 0);
        // QueueBuffer may have enqueued while this flusher was exiting and
        // observed m_CtrlQueueFlushInProgress still set.
        if (InterlockedCompareExchange(&m_CtrlQueueFlushRequested, 0, 0) == 0)
        {
            return ret;
        }

        if (InterlockedCompareExchange(&m_CtrlQueueFlushInProgress, 1, 0) != 0)
        {
            return ret;
        }
    }
}

UINT CtrlQueue::QueueBuffer(PGPU_VBUFFER buf)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (!ValidateCtrlQueueBuffer(buf))
    {
        // Rejected before staging: nothing was queued, so the caller still owns
        // the buffer and is responsible for releasing it. This is the ONLY
        // caller-visible failure.
        return (UINT)-1;
    }

    ExInterlockedInsertTailList(&m_CtrlQueueList, &buf->ctrl_queue_entry, &m_CtrlQueueSpinLock);
    InterlockedExchange(&m_CtrlQueueFlushRequested, 1);

    // The buffer is now staged and owned by the queue. Flush() drains as much as
    // the control virtqueue currently accepts; whatever it cannot submit right
    // now (vq full) stays staged and is drained by a later Flush(). Flush()'s
    // return describes the *flush attempt*, not this buffer -- a non-zero
    // "not drained yet" must NOT be reported to the caller as this buffer
    // failing, or the caller releases a buffer the queue still owns and the
    // retry submits/frees it a second time. The enqueue itself cannot fail
    // (list insert), so report success.
    Flush();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return 0;
}

UINT CtrlQueue::QueueBufferFenced(PGPU_VBUFFER vbuf)
{
    UINT ret;
    PGPU_CTRL_HDR hdr = (PGPU_CTRL_HDR)vbuf->buf;
    hdr->fence_id = InterlockedIncrement(&m_FenceIdr);
    hdr->flags |= VIRTIO_GPU_FLAG_FENCE;

    ret = QueueBuffer(vbuf);

    return ret;
}

/* DequeueBuffer is only used for the viogpudo driver */
PGPU_VBUFFER CtrlQueue::DequeueBuffer(_Out_ UINT *len)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_VBUFFER buf = NULL;
    KIRQL SavedIrql;
    Lock(&SavedIrql);
    buf = (PGPU_VBUFFER)GetBuf(len);
    Unlock(SavedIrql);
    if (buf == NULL)
    {
        *len = 0;
    }

    Flush();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return buf;
}

PGPU_VBUFFER CtrlQueue::DequeueBufferFromIsr(_Out_ UINT *len)
{
    PGPU_VBUFFER buf = (PGPU_VBUFFER)GetBuf(len);
    if (buf == NULL)
    {
        *len = 0;
    }

    return buf;
}

void VioGpuQueue::ReleaseBuffer(PGPU_VBUFFER buf)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (buf == NULL)
    {
        return;
    }

    m_pBuf->FreeBuf(buf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN VioGpuBuf::Init(_In_ UINT cnt)
{
    KIRQL OldIrql;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    ASSERT(KeGetCurrentIrql() < DISPATCH_LEVEL);
    m_uCountMin = cnt;

    for (UINT i = 0; i < cnt; ++i)
    {
        PGPU_VBUFFER pvbuf = reinterpret_cast<PGPU_VBUFFER>(new (NonPagedPoolNx) BYTE[VBUFFER_SIZE]);
        // FIXME
        RtlZeroMemory(pvbuf, VBUFFER_SIZE);
        if (pvbuf)
        {
            KeAcquireSpinLock(&m_SpinLock, &OldIrql);
            InsertTailList(&m_FreeBufs, &pvbuf->list_entry);
            ++m_uCount;
            KeReleaseSpinLock(&m_SpinLock, OldIrql);
        }
    }
    ASSERT(m_uCount == cnt);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return (m_uCount > 0);
}

void VioGpuBuf::Close(void)
{
    KIRQL OldIrql;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    ASSERT(KeGetCurrentIrql() < DISPATCH_LEVEL);

    KeAcquireSpinLock(&m_SpinLock, &OldIrql);
    while (!IsListEmpty(&m_InUseBufs))
    {
        LIST_ENTRY *pListItem = RemoveHeadList(&m_InUseBufs);
        if (pListItem)
        {
            PGPU_VBUFFER pvbuf = CONTAINING_RECORD(pListItem, GPU_VBUFFER, list_entry);
            ASSERT(pvbuf);

            if (pvbuf->resp_size > MAX_INLINE_RESP_SIZE)
            {
                delete[] reinterpret_cast<PBYTE>(pvbuf->resp_buf);
                pvbuf->resp_buf = NULL;
                pvbuf->resp_size = 0;
            }

            if (pvbuf->data_buf)
            {
                delete[] reinterpret_cast<PBYTE>(pvbuf->data_buf);
                pvbuf->data_buf = NULL;
                pvbuf->data_size = 0;
            }

            delete[] reinterpret_cast<PBYTE>(pvbuf);
            --m_uCount;
        }
    }

    while (!IsListEmpty(&m_FreeBufs))
    {
        LIST_ENTRY *pListItem = RemoveHeadList(&m_FreeBufs);
        if (pListItem)
        {
            PGPU_VBUFFER pbuf = CONTAINING_RECORD(pListItem, GPU_VBUFFER, list_entry);
            ASSERT(pbuf);

            if (pbuf->resp_buf && pbuf->resp_size > MAX_INLINE_RESP_SIZE)
            {
                delete[] reinterpret_cast<PBYTE>(pbuf->resp_buf);
                pbuf->resp_buf = NULL;
                pbuf->resp_size = 0;
            }

            if (pbuf->data_buf)
            {
                delete[] reinterpret_cast<PBYTE>(pbuf->data_buf);
                pbuf->data_buf = NULL;
                pbuf->data_size = 0;
            }

            delete[] reinterpret_cast<PBYTE>(pbuf);
            --m_uCount;
        }
    }
    KeReleaseSpinLock(&m_SpinLock, OldIrql);

    ASSERT(m_uCount == 0);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

PGPU_VBUFFER VioGpuBuf::GetBuf(_In_ int size, _In_ int resp_size, _In_opt_ void *resp_buf)
{

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_VBUFFER pbuf = NULL;
    PLIST_ENTRY pListItem = NULL;
    KIRQL SavedIrql = KeGetCurrentIrql();

    if (SavedIrql < DISPATCH_LEVEL)
    {
        KeAcquireSpinLock(&m_SpinLock, &SavedIrql);
    }
    else if (SavedIrql == DISPATCH_LEVEL)
    {
        KeAcquireSpinLockAtDpcLevel(&m_SpinLock);
    }
    else
    {
        VioGpuDbgBreak();
    }

    if (IsListEmpty(&m_FreeBufs))
    {
        pbuf = reinterpret_cast<PGPU_VBUFFER>(new (NonPagedPoolNx) BYTE[VBUFFER_SIZE]);
        ++m_uCount;
    }
    else
    {
        pListItem = RemoveHeadList(&m_FreeBufs);
        pbuf = CONTAINING_RECORD(pListItem, GPU_VBUFFER, list_entry);
    }

    ASSERT(pbuf);
    memset(pbuf, 0, VBUFFER_SIZE);
    ASSERT(size <= MAX_INLINE_CMD_SIZE);

    pbuf->buf = (char *)((ULONG_PTR)pbuf + sizeof(*pbuf));
    pbuf->size = size;
    pbuf->auto_release = true;

    pbuf->resp_size = resp_size;
    if (resp_size <= MAX_INLINE_RESP_SIZE)
    {
        ASSERT(resp_buf == NULL);
        pbuf->resp_buf = (char *)((ULONG_PTR)pbuf->buf + size);
    }
    else
    {
        pbuf->resp_buf = (char *)resp_buf;
    }
    ASSERT(pbuf->resp_buf);
    InsertTailList(&m_InUseBufs, &pbuf->list_entry);

    if (SavedIrql < DISPATCH_LEVEL)
    {
        KeReleaseSpinLock(&m_SpinLock, SavedIrql);
    }
    else if (SavedIrql == DISPATCH_LEVEL)
    {
        KeReleaseSpinLockFromDpcLevel(&m_SpinLock);
    }
    else
    {
        VioGpuDbgBreak();
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s buf = %p\n", __FUNCTION__, pbuf));

    return pbuf;
}

void VioGpuBuf::FreeBuf(_In_ PGPU_VBUFFER pbuf)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s buf = %p\n", __FUNCTION__, pbuf));

    KIRQL SavedIrql = KeGetCurrentIrql();
    if (SavedIrql < DISPATCH_LEVEL)
    {
        KeAcquireSpinLock(&m_SpinLock, &SavedIrql);
    }
    else if (SavedIrql == DISPATCH_LEVEL)
    {
        KeAcquireSpinLockAtDpcLevel(&m_SpinLock);
    }
    else
    {
        VioGpuDbgBreak();
    }

    RemoveEntryList(&pbuf->list_entry);

    if (pbuf->resp_buf && pbuf->resp_size > MAX_INLINE_RESP_SIZE)
    {
        delete[] reinterpret_cast<PBYTE>(pbuf->resp_buf);
        pbuf->resp_buf = NULL;
        pbuf->resp_size = 0;
    }

    if (pbuf->data_buf)
    {
        delete[] reinterpret_cast<PBYTE>(pbuf->data_buf);
        pbuf->data_buf = NULL;
        pbuf->data_size = 0;
    }

    if (m_uCount > m_uCountMin)
    {
        delete[] reinterpret_cast<PBYTE>(pbuf);
        --m_uCount;
    }
    else
    {
        InsertTailList(&m_FreeBufs, &pbuf->list_entry);
    }

    if (SavedIrql < DISPATCH_LEVEL)
    {
        KeReleaseSpinLock(&m_SpinLock, SavedIrql);
    }
    else if (SavedIrql == DISPATCH_LEVEL)
    {
        KeReleaseSpinLockFromDpcLevel(&m_SpinLock);
    }
    else
    {
        VioGpuDbgBreak();
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

PAGED_CODE_SEG_BEGIN
VioGpuBuf::VioGpuBuf()
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    InitializeListHead(&m_FreeBufs);
    InitializeListHead(&m_InUseBufs);
    KeInitializeSpinLock(&m_SpinLock);
    m_uCount = 0;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VioGpuBuf::~VioGpuBuf()
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_FATAL, ("---> %s 0x%p\n", __FUNCTION__, this));

    Close();

    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s\n", __FUNCTION__));
}

VioGpuMemSegment::VioGpuMemSegment(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    m_pSGList = NULL;
    m_pVAddr = NULL;
    m_pMdl = NULL;
    m_bSystemMemory = FALSE;
    m_bMapped = FALSE;
    m_Size = 0;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VioGpuMemSegment::~VioGpuMemSegment(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    Close();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN VioGpuMemSegment::Init(_In_ UINT size, _In_opt_ PPHYSICAL_ADDRESS pPAddr)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    ASSERT(size);
    PVOID buf = NULL;
    UINT pages = BYTES_TO_PAGES(size);
    UINT sglsize = sizeof(SCATTER_GATHER_LIST) + (sizeof(SCATTER_GATHER_ELEMENT) * pages);
    size = pages * PAGE_SIZE;

    if ((pPAddr == NULL) || pPAddr->QuadPart == 0LL)
    {
        m_pVAddr = new (NonPagedPoolNx) BYTE[size];

        if (!m_pVAddr)
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("%s insufficient resources to allocate %x bytes\n", __FUNCTION__, size));
            return FALSE;
        }
        RtlZeroMemory(m_pVAddr, size);
        m_bSystemMemory = TRUE;
    }
    else
    {
        NTSTATUS Status = MapFrameBuffer(*pPAddr, size, &m_pVAddr);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s MapFrameBuffer failed with Status: 0x%X\n", __FUNCTION__, Status));
            return FALSE;
        }
        m_bMapped = TRUE;
    }

    m_pMdl = IoAllocateMdl(m_pVAddr, size, FALSE, FALSE, NULL);
    if (!m_pMdl)
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s insufficient resources to allocate MDLs\n", __FUNCTION__));
        return FALSE;
    }
    if (m_bSystemMemory == TRUE)
    {
        __try
        {
            MmProbeAndLockPages(m_pMdl, KernelMode, IoWriteAccess);
        }
#pragma prefast(suppress : __WARNING_EXCEPTIONEXECUTEHANDLER, "try/except is only able to protect against user-mode errors and these are the only errors we try to catch here");
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("%s Failed to lock pages with error %x\n", __FUNCTION__, GetExceptionCode()));
            IoFreeMdl(m_pMdl);
            return FALSE;
        }
    }
    m_pSGList = reinterpret_cast<PSCATTER_GATHER_LIST>(new (NonPagedPoolNx) BYTE[sglsize]);
    m_pSGList->NumberOfElements = 0;
    m_pSGList->Reserved = 0;
    //       m_pSAddr = reinterpret_cast<BYTE*>
    //    (MmGetSystemAddressForMdlSafe(m_pMdl, NormalPagePriority | MdlMappingNoExecute));

    RtlZeroMemory(m_pSGList, sglsize);
    buf = PAGE_ALIGN(m_pVAddr);

    for (UINT i = 0; i < pages; ++i)
    {
        PHYSICAL_ADDRESS pa = {0};
        ASSERT(MmIsAddressValid(buf));
        pa = MmGetPhysicalAddress(buf);
        if (pa.QuadPart == 0LL)
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("%s Invalid PA buf = %p element %d\n", __FUNCTION__, buf, i));
            break;
        }
        m_pSGList->Elements[i].Address = pa;
        m_pSGList->Elements[i].Length = PAGE_SIZE;
        buf = (PVOID)((LONG_PTR)(buf) + PAGE_SIZE);
        m_pSGList->NumberOfElements++;
    }
    m_Size = size;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return TRUE;
}

PHYSICAL_ADDRESS VioGpuMemSegment::GetPhysicalAddress(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PHYSICAL_ADDRESS pa = {0};
    if (m_pVAddr && MmIsAddressValid(m_pVAddr))
    {
        pa = MmGetPhysicalAddress(m_pVAddr);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return pa;
}

void VioGpuMemSegment::Close(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (m_pMdl)
    {
        if (m_bSystemMemory)
        {
            MmUnlockPages(m_pMdl);
        }
        IoFreeMdl(m_pMdl);
        m_pMdl = NULL;
    }

    if (m_bSystemMemory)
    {
        delete[] m_pVAddr;
    }
    else
    {
        UnmapFrameBuffer(m_pVAddr, (ULONG)m_Size);
        m_bMapped = FALSE;
    }
    m_pVAddr = NULL;

    delete[] reinterpret_cast<PBYTE>(m_pSGList);
    m_pSGList = NULL;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VioGpuObj::VioGpuObj(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    m_uiHwRes = 0;
    m_pSegment = NULL;
    m_Size = 0;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VioGpuObj::~VioGpuObj(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN VioGpuObj::Init(_In_ UINT size, VioGpuMemSegment *pSegment)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s requested size = %d\n", __FUNCTION__, size));

    ASSERT(size);
    ASSERT(pSegment);
    UINT pages = BYTES_TO_PAGES(size);
    size = pages * PAGE_SIZE;
    if (!pSegment || size > pSegment->GetSize())
    {
        // NB: log pSegment, not m_pSegment -- m_pSegment is still NULL here (it
        // is assigned below), so dereferencing it would bugcheck.
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("<--- %s segment size too small = %Iu (%u)\n",
                  __FUNCTION__,
                  pSegment ? pSegment->GetSize() : 0,
                  size));
        return FALSE;
    }
    m_pSegment = pSegment;
    m_Size = size;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s size = %Iu\n", __FUNCTION__, m_Size));
    return TRUE;
}

PVOID CrsrQueue::AllocCursor(PGPU_VBUFFER *buf)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_VBUFFER vbuf;
    vbuf = m_pBuf->GetBuf(sizeof(GPU_UPDATE_CURSOR), 0, NULL);
    ASSERT(vbuf);
    *buf = vbuf;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s  vbuf = %p\n", __FUNCTION__, vbuf));

    return vbuf ? vbuf->buf : NULL;
}

PAGED_CODE_SEG_END

UINT CrsrQueue::QueueCursor(PGPU_VBUFFER buf)
{
    //    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    UINT res = 0;
    KIRQL SavedIrql;

    VirtIOBufferDescriptor sg[1];
    int outcnt = 0;
    UINT ret = 0;

    ASSERT(buf->size <= PAGE_SIZE);
    if (BuildSGElement(&sg[outcnt], (PVOID)buf->buf, buf->size))
    {
        outcnt++;
    }

    ASSERT(outcnt);
    Lock(&SavedIrql);
    ret = AddBuf(&sg[0], outcnt, 0, buf, NULL, 0);
    Kick();
    Unlock(SavedIrql);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s vbuf = %p outcnt = %d, ret = %d\n", __FUNCTION__, buf, outcnt, ret));
    return res;
}

PGPU_VBUFFER CrsrQueue::DequeueCursor(_Out_ UINT *len)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_VBUFFER buf = NULL;
    KIRQL SavedIrql;
    Lock(&SavedIrql);
    buf = (PGPU_VBUFFER)GetBuf(len);
    Unlock(SavedIrql);
    if (buf == NULL)
    {
        *len = 0;
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s buf %p len = %u\n", __FUNCTION__, buf, *len));
    return buf;
}
