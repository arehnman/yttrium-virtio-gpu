/*
 * Copyright 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 * 
 */

#include "viogpu_command.h"
#include "viogpu_device.h"
#include "viogpu_adapter.h"
#include "baseobj.h"
#include "virgl_hw.h"

#pragma code_seg(push)
#pragma code_seg()

// Bytes per pixel for the (32bpp) scanout formats the present path deals with.
// Kept in sync with VioGpuAllocation::GetStride().
static UINT VioGpuBltBytesPerPixel(UINT format)
{
    switch (format)
    {
        case VIRGL_FORMAT_B8G8R8A8_UNORM:
        case VIRGL_FORMAT_B8G8R8X8_UNORM:
        case VIRGL_FORMAT_B8G8R8A8_SRGB:
        case VIRGL_FORMAT_A8R8G8B8_UNORM:
        case VIRGL_FORMAT_X8R8G8B8_UNORM:
        case VIRGL_FORMAT_R8G8B8A8_UNORM:
        case VIRGL_FORMAT_X8B8G8R8_UNORM:
        case VIRGL_FORMAT_A8B8G8R8_UNORM:
        case VIRGL_FORMAT_R8G8B8X8_UNORM:
        case VIRGL_FORMAT_R8G8B8A8_SRGB:
            return 4;
        default:
            return 4;
    }
}

enum VIOGPU_BLT_PIXEL_LAYOUT
{
    VIOGPU_BLT_LAYOUT_UNKNOWN = 0,
    VIOGPU_BLT_LAYOUT_BGRA,
    VIOGPU_BLT_LAYOUT_ARGB,
    VIOGPU_BLT_LAYOUT_RGBA,
    VIOGPU_BLT_LAYOUT_ABGR,
};

enum VIOGPU_BLT_COPY_MODE
{
    VIOGPU_BLT_COPY_UNSUPPORTED = 0,
    VIOGPU_BLT_COPY_DIRECT,
    VIOGPU_BLT_COPY_SWAP_RB,
};

static VIOGPU_BLT_PIXEL_LAYOUT VioGpuBltPixelLayout(UINT format)
{
    switch (format)
    {
        case VIRGL_FORMAT_B8G8R8A8_UNORM:
        case VIRGL_FORMAT_B8G8R8X8_UNORM:
        case VIRGL_FORMAT_B8G8R8A8_SRGB:
            return VIOGPU_BLT_LAYOUT_BGRA;
        case VIRGL_FORMAT_A8R8G8B8_UNORM:
        case VIRGL_FORMAT_X8R8G8B8_UNORM:
            return VIOGPU_BLT_LAYOUT_ARGB;
        case VIRGL_FORMAT_R8G8B8A8_UNORM:
        case VIRGL_FORMAT_R8G8B8X8_UNORM:
        case VIRGL_FORMAT_R8G8B8A8_SRGB:
            return VIOGPU_BLT_LAYOUT_RGBA;
        case VIRGL_FORMAT_A8B8G8R8_UNORM:
        case VIRGL_FORMAT_X8B8G8R8_UNORM:
            return VIOGPU_BLT_LAYOUT_ABGR;
        default:
            return VIOGPU_BLT_LAYOUT_UNKNOWN;
    }
}

static VIOGPU_BLT_COPY_MODE VioGpuBltCopyMode(UINT src_format,
                                               UINT dst_format)
{
    if (src_format == dst_format)
    {
        return VIOGPU_BLT_COPY_DIRECT;
    }

    const VIOGPU_BLT_PIXEL_LAYOUT src_layout =
        VioGpuBltPixelLayout(src_format);
    const VIOGPU_BLT_PIXEL_LAYOUT dst_layout =
        VioGpuBltPixelLayout(dst_format);

    if (src_layout == VIOGPU_BLT_LAYOUT_UNKNOWN ||
        dst_layout == VIOGPU_BLT_LAYOUT_UNKNOWN)
    {
        return VIOGPU_BLT_COPY_UNSUPPORTED;
    }

    if (src_layout == dst_layout)
    {
        return VIOGPU_BLT_COPY_DIRECT;
    }

    if ((src_layout == VIOGPU_BLT_LAYOUT_RGBA &&
         dst_layout == VIOGPU_BLT_LAYOUT_BGRA) ||
        (src_layout == VIOGPU_BLT_LAYOUT_BGRA &&
         dst_layout == VIOGPU_BLT_LAYOUT_RGBA))
    {
        return VIOGPU_BLT_COPY_SWAP_RB;
    }

    return VIOGPU_BLT_COPY_UNSUPPORTED;
}

static __forceinline ULONG VioGpuBltSwapRedBlue(ULONG pixel)
{
    return (pixel & 0xff00ff00u) |
           ((pixel & 0x000000ffu) << 16) |
           ((pixel & 0x00ff0000u) >> 16);
}

__declspec(noreturn) static void BugCheckDmaQueueSubmitFailure(PVOID command,
                                                               UINT fenceId,
                                                               UINT nodeOrdinal,
                                                               UINT engineOrdinal,
                                                               UINT packetType,
                                                               UINT ret)
{
    DbgPrint(TRACE_LEVEL_FATAL,
             ("%s cmd=%p permanent ctrlq submit failure fence=%u node=%u engine=%u packet_type=%u ret=0x%x -> bugcheck\n",
              __FUNCTION__,
              command,
              fenceId,
              nodeOrdinal,
              engineOrdinal,
              packetType,
              ret));
    KeBugCheckEx(0x000000E2,
                 static_cast<ULONG_PTR>('QIVg'),
                 reinterpret_cast<ULONG_PTR>(command),
                 static_cast<ULONG_PTR>(fenceId),
                 static_cast<ULONG_PTR>(packetType));
}

VioGpuCommand::VioGpuCommand(VioGpuAdapter *adapter)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    m_pAdapter = adapter;
    m_pCommander = &adapter->commander;
    m_pContext = NULL;

    m_FenceId = 0;
    m_NodeOrdinal = 0;
    m_EngineOrdinal = 0;
    m_pDmaBuffer = NULL;
    m_pCommand = NULL;
    m_pEnd = NULL;

    m_allocations = NULL;
    m_allocationsLength = 0;
};

VioGpuCommand::~VioGpuCommand()
{
    // A command can be destroyed by a direct `delete` on an error/early-exit
    // path (e.g. a failed Blt Present) without going through VioGpuCommandDone().
    // If it was stashed in a DMA-buffer private-data slot, clear the slot so a
    // later SubmitCommand cannot pull this freed pointer out of it (UAF).
    // CompareExchange so the slot is only NULLed if it still points at us.
    if (m_pPrivateDataSlot)
    {
        InterlockedCompareExchangePointer((PVOID volatile *)m_pPrivateDataSlot, NULL, this);
        m_pPrivateDataSlot = NULL;
    }
}

static BOOLEAN IsExpectedEmptySubmit(const DXGKARG_SUBMITCOMMAND *pSubmitCommand)
{
    if (pSubmitCommand->DmaBufferSubmissionEndOffset <
        pSubmitCommand->DmaBufferSubmissionStartOffset)
    {
        return FALSE;
    }

    const BOOLEAN emptyDmaRange =
        pSubmitCommand->DmaBufferSubmissionEndOffset ==
        pSubmitCommand->DmaBufferSubmissionStartOffset;

    if (!emptyDmaRange)
    {
        return FALSE;
    }

    if (pSubmitCommand->Flags.Flip || pSubmitCommand->Flags.FlipWithNoWait)
    {
        return TRUE;
    }

    if (pSubmitCommand->Flags.Paging ||
        pSubmitCommand->Flags.NullRendering)
    {
        return TRUE;
    }

#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WIN8)
    if (pSubmitCommand->Flags.ContextSwitch)
    {
        return TRUE;
    }
#endif

    return FALSE;
}

void VioGpuCommand::PrepareSubmit(const DXGKARG_SUBMITCOMMAND *pSubmitCommand)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    // The base reference is taken in Run(); reset the retirement/notification
    // counters for this submission.
    InterlockedExchange(&m_done, 0);
    InterlockedExchange(&m_isrPendingPackets, 0);
    InterlockedExchange(&m_dmaNotified, 0);

    m_FenceId = pSubmitCommand->SubmissionFenceId;
    m_EngineOrdinal = pSubmitCommand->EngineOrdinal;
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WIN8)
    m_NodeOrdinal = pSubmitCommand->NodeOrdinal;
#else
    m_NodeOrdinal = 0;
#endif
    m_submitFlagsValue = pSubmitCommand->Flags.Value;
    m_submitPaging = pSubmitCommand->Flags.Paging ? TRUE : FALSE;
    m_expectedEmptySubmit = IsExpectedEmptySubmit(pSubmitCommand);
    if (m_pDmaBuffer)
    {
        m_pCommand = (char *)m_pDmaBuffer + pSubmitCommand->DmaBufferSubmissionStartOffset;
        m_pEnd = (char *)m_pDmaBuffer + pSubmitCommand->DmaBufferSubmissionEndOffset;
    }
    else
    {
        m_pCommand = NULL;
        m_pEnd = NULL;
    }
    m_pContext = reinterpret_cast<VioGpuDevice *>(pSubmitCommand->hContext);

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("%s cmd=%p fence=%u node=%u engine=%u hContext=%p ctx_id=%u dma=%p start=0x%x end=0x%x\n",
              __FUNCTION__,
              this,
              m_FenceId,
              m_NodeOrdinal,
              m_EngineOrdinal,
              m_pContext,
              m_pContext ? m_pContext->GetId() : 0,
              m_pDmaBuffer,
              pSubmitCommand->DmaBufferSubmissionStartOffset,
              pSubmitCommand->DmaBufferSubmissionEndOffset));
}

void VioGpuCommand::ClearCpuCopyBltMappings()
{
    if (m_cpuCopySrc && m_cpuCopySrcVa)
    {
        m_cpuCopySrc->UnmapForCpuCopy(m_cpuCopySrcVa,
                                      m_cpuCopySrcMapSize,
                                      m_cpuCopySrcIo);
    }
    if (m_cpuCopyDst && m_cpuCopyDstVa)
    {
        m_cpuCopyDst->UnmapForCpuCopy(m_cpuCopyDstVa,
                                      m_cpuCopyDstMapSize,
                                      m_cpuCopyDstIo);
    }

    m_cpuCopyBltMapped = FALSE;
    m_cpuCopySrcVa = NULL;
    m_cpuCopyDstVa = NULL;
    m_cpuCopySrcMapSize = 0;
    m_cpuCopyDstMapSize = 0;
    m_cpuCopySrcIo = FALSE;
    m_cpuCopyDstIo = FALSE;
}

void VioGpuCommand::ClearCpuCopyBlt()
{
    ClearCpuCopyBltMappings();
    m_cpuCopyBlt = FALSE;
    m_cpuCopySrc = NULL;
    m_cpuCopyDst = NULL;
    m_cpuCopySubRectCnt = 0;
    RtlZeroMemory(&m_cpuCopySrcRect, sizeof(m_cpuCopySrcRect));
    RtlZeroMemory(&m_cpuCopyDstRect, sizeof(m_cpuCopyDstRect));
    RtlZeroMemory(m_cpuCopySubRects, sizeof(m_cpuCopySubRects));
}

NTSTATUS VioGpuCommand::SetCpuCopyBlt(VioGpuAllocation *src,
                                      VioGpuAllocation *dst,
                                      const RECT *srcRect,
                                      const RECT *dstRect,
                                      const RECT *subRects,
                                      UINT subRectCnt)
{
    ClearCpuCopyBlt();

    if (!src || !dst || !srcRect || !dstRect)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!src->IsBlob() || dst->IsBlob())
    {
        return STATUS_NOT_SUPPORTED;
    }

    m_cpuCopyBlt = TRUE;
    m_cpuCopySrc = src;
    m_cpuCopyDst = dst;
    m_cpuCopySrcRect = *srcRect;
    m_cpuCopyDstRect = *dstRect;

    // Capture the dirty sub-rectangles so only changed pixels are copied later
    // (the copy runs from Run() at DISPATCH_LEVEL, long after pPresent is gone).
    // The list is valid only during Present(), so snapshot it here.  If it does
    // not fit, collapse to its bounding box - still far smaller than the whole
    // blt rect for a localized update, and storage stays bounded.
    m_cpuCopySubRectCnt = 0;
    if (subRects && subRectCnt)
    {
        if (subRectCnt <= VIOGPU_BLT_MAX_SUBRECTS)
        {
            RtlCopyMemory(m_cpuCopySubRects, subRects,
                          subRectCnt * sizeof(RECT));
            m_cpuCopySubRectCnt = subRectCnt;
        }
        else
        {
            RECT cover = subRects[0];
            for (UINT i = 1; i < subRectCnt; i++)
            {
                cover.left = min(cover.left, subRects[i].left);
                cover.top = min(cover.top, subRects[i].top);
                cover.right = max(cover.right, subRects[i].right);
                cover.bottom = max(cover.bottom, subRects[i].bottom);
            }
            m_cpuCopySubRects[0] = cover;
            m_cpuCopySubRectCnt = 1;
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("%s staged Venus BLT present src_res=0x%x dst_res=0x%x subrects=%u stored=%u src_rect=%ld,%ld-%ld,%ld dst_rect=%ld,%ld-%ld,%ld\n",
              __FUNCTION__,
              src->GetId(),
              dst->GetId(),
              subRectCnt,
              m_cpuCopySubRectCnt,
              srcRect->left,
              srcRect->top,
              srcRect->right,
              srcRect->bottom,
              dstRect->left,
              dstRect->top,
              dstRect->right,
              dstRect->bottom));

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuCommand::MapCpuCopyBlt(ULONG ctx_id)
{
    if (!m_cpuCopyBlt)
    {
        return STATUS_NOT_FOUND;
    }

    // Present may pre-patch resident allocations, in which case VidMm can
    // submit without calling Patch. If backing moves before submission,
    // however, VidMm calls Patch again with the new residency. The old CPU
    // VA snapshot is not a backing lease: always reacquire it here, even
    // when Present or an earlier Patch already mapped this command.
    // Invalidate first so a failed refresh cannot reuse an obsolete VA.
    ClearCpuCopyBltMappings();

    if (!m_cpuCopySrc || !m_cpuCopyDst)
    {
        return STATUS_INVALID_PARAMETER;
    }

    PVOID src_va = NULL;
    PVOID dst_va = NULL;
    ULONGLONG src_map_size = 0;
    ULONGLONG dst_map_size = 0;
    BOOLEAN src_io = FALSE;
    BOOLEAN dst_io = FALSE;

    NTSTATUS status = m_cpuCopySrc->MapForCpuCopy(ctx_id, &src_va,
                                                  &src_map_size, &src_io);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s source map failed status=0x%x src_res=0x%x ctx=%u\n",
                  __FUNCTION__, status, m_cpuCopySrc->GetId(), ctx_id));
        return status;
    }

    status = m_cpuCopyDst->MapForCpuCopy(ctx_id, &dst_va,
                                         &dst_map_size, &dst_io);
    if (!NT_SUCCESS(status))
    {
        m_cpuCopySrc->UnmapForCpuCopy(src_va, src_map_size, src_io);
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s destination map failed status=0x%x dst_res=0x%x ctx=%u\n",
                  __FUNCTION__, status, m_cpuCopyDst->GetId(), ctx_id));
        return status;
    }

    m_cpuCopySrcVa = src_va;
    m_cpuCopyDstVa = dst_va;
    m_cpuCopySrcMapSize = src_map_size;
    m_cpuCopyDstMapSize = dst_map_size;
    m_cpuCopySrcIo = src_io;
    m_cpuCopyDstIo = dst_io;
    m_cpuCopyBltMapped = TRUE;

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("%s mapped Venus BLT present ctx=%u src_res=0x%x dst_res=0x%x src_va=%p dst_va=%p src_size=0x%llx dst_size=0x%llx src_io=%u dst_io=%u\n",
              __FUNCTION__, ctx_id, m_cpuCopySrc->GetId(), m_cpuCopyDst->GetId(),
              src_va, dst_va, src_map_size, dst_map_size,
              src_io ? 1 : 0, dst_io ? 1 : 0));

    return STATUS_SUCCESS;
}

// Copy one destination-space region for a CPU blt, mapping it back to source
// space with the (dx,dy) blt offset and validating every access against the
// mapped sizes.  Degenerate or out-of-bounds regions are skipped (returns 0)
// rather than failing the whole present, so a single bad sub-rect cannot drop
// an entire frame.  Returns the number of bytes copied.
static ULONGLONG VioGpuBltCopyRegion(VioGpuCommand *cmd,
                                     VioGpuAllocation *src,
                                     VioGpuAllocation *dst,
                                     PUCHAR src_va, ULONGLONG src_map_size,
                                     PUCHAR dst_va, ULONGLONG dst_map_size,
                                     const RECT *clampRect,
                                     LONG dx, LONG dy,
                                     UINT bpp,
                                     UINT src_stride, UINT dst_stride,
                                     VIOGPU_BLT_COPY_MODE copy_mode,
                                     const RECT *region)
{
    // Clip the requested region to the destination blt rectangle.
    LONG rl = max(region->left, clampRect->left);
    LONG rt = max(region->top, clampRect->top);
    LONG rr = min(region->right, clampRect->right);
    LONG rb = min(region->bottom, clampRect->bottom);
    if (rr <= rl || rb <= rt)
    {
        return 0;
    }

    const LONG sl = rl + dx;
    const LONG st = rt + dy;
    if (rl < 0 || rt < 0 || sl < 0 || st < 0)
    {
        return 0;
    }

    // Clip the copy extent to both surfaces (no stretch).
    LONG w = rr - rl;
    LONG h = rb - rt;
    if ((LONG)dst->GetWidth() - rl < w)
    {
        w = (LONG)dst->GetWidth() - rl;
    }
    if ((LONG)src->GetWidth() - sl < w)
    {
        w = (LONG)src->GetWidth() - sl;
    }
    if ((LONG)dst->GetHeight() - rt < h)
    {
        h = (LONG)dst->GetHeight() - rt;
    }
    if ((LONG)src->GetHeight() - st < h)
    {
        h = (LONG)src->GetHeight() - st;
    }
    if (w <= 0 || h <= 0)
    {
        return 0;
    }

    const SIZE_T row_bytes = (SIZE_T)w * bpp;
    if (row_bytes > src_stride || row_bytes > dst_stride)
    {
        return 0;
    }

    const ULONGLONG src_offset =
        (ULONGLONG)st * src_stride + (ULONGLONG)sl * bpp;
    const ULONGLONG dst_offset =
        (ULONGLONG)rt * dst_stride + (ULONGLONG)rl * bpp;
    const ULONGLONG src_required =
        src_offset + (ULONGLONG)(h - 1) * src_stride + row_bytes;
    const ULONGLONG dst_required =
        dst_offset + (ULONGLONG)(h - 1) * dst_stride + row_bytes;
    if (src_required > src_map_size || dst_required > dst_map_size)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s bounds skip src_res=0x%x dst_res=0x%x src_required=0x%llx src_size=0x%llx dst_required=0x%llx dst_size=0x%llx row=0x%llx height=%ld fence=%u\n",
                  __FUNCTION__, src->GetId(), dst->GetId(),
                  src_required, src_map_size, dst_required, dst_map_size,
                  (ULONGLONG)row_bytes, h, cmd->GetSubmissionFenceId()));
        return 0;
    }

    PUCHAR s = src_va + src_offset;
    PUCHAR d = dst_va + dst_offset;
    for (LONG y = 0; y < h; y++)
    {
        PUCHAR src_row = s + (ULONGLONG)y * src_stride;
        PUCHAR dst_row = d + (ULONGLONG)y * dst_stride;

        if (copy_mode == VIOGPU_BLT_COPY_DIRECT)
        {
            RtlCopyMemory(dst_row, src_row, row_bytes);
        }
        else
        {
            const ULONG *src_pixel = (const ULONG *)src_row;
            ULONG *dst_pixel = (ULONG *)dst_row;
            for (LONG x = 0; x < w; x++)
            {
                dst_pixel[x] = VioGpuBltSwapRedBlue(src_pixel[x]);
            }
        }
    }

    return (ULONGLONG)row_bytes * h;
}

static NTSTATUS VioGpuCpuCopyBltPresent(VioGpuCommand *cmd,
                                        VioGpuAllocation *src,
                                        VioGpuAllocation *dst,
                                        const RECT *srcRect,
                                        const RECT *dstRect,
                                        PVOID src_va,
                                        ULONGLONG src_map_size,
                                        PVOID dst_va,
                                        ULONGLONG dst_map_size,
                                        const RECT *subRects,
                                        UINT subRectCnt)
{
    if (!cmd || !src || !dst || !srcRect || !dstRect || !src_va || !dst_va)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!src->IsBlob() || dst->IsBlob())
    {
        return STATUS_NOT_SUPPORTED;
    }

    const LONG src_width = srcRect->right - srcRect->left;
    const LONG src_height = srcRect->bottom - srcRect->top;
    const LONG dst_width = dstRect->right - dstRect->left;
    const LONG dst_height = dstRect->bottom - dstRect->top;
    if (srcRect->left < 0 || srcRect->top < 0 ||
        dstRect->left < 0 || dstRect->top < 0 ||
        src_width <= 0 || src_height <= 0 ||
        dst_width <= 0 || dst_height <= 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    const UINT bpp = VioGpuBltBytesPerPixel(dst->GetFormat());
    const UINT src_stride = src->GetStride();
    const UINT dst_stride = dst->GetStride();
    const VIOGPU_BLT_COPY_MODE copy_mode =
        VioGpuBltCopyMode(src->GetFormat(), dst->GetFormat());
    const LONG dx = srcRect->left - dstRect->left;
    const LONG dy = srcRect->top - dstRect->top;

    if (copy_mode == VIOGPU_BLT_COPY_UNSUPPORTED)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s WARNING Venus/Yttrium BLT conversion unavailable owner=viogpu3d reason=unsupported_format_pair src_res=0x%x dst_res=0x%x src_format=%u dst_format=%u\n",
                  __FUNCTION__, src->GetId(), dst->GetId(),
                  src->GetFormat(), dst->GetFormat()));
        return STATUS_NOT_SUPPORTED;
    }

    // Copy or convert each dirty region directly into the destination so
    // conversion adds no extra pass.
    LARGE_INTEGER frequency = {};
    const LARGE_INTEGER start = KeQueryPerformanceCounter(&frequency);
    ULONGLONG bytes_copied = 0;
    UINT regions_copied = 0;
    if (subRectCnt == 0)
    {
        bytes_copied += VioGpuBltCopyRegion(cmd, src, dst, (PUCHAR)src_va,
                                            src_map_size, (PUCHAR)dst_va,
                                            dst_map_size, dstRect, dx, dy, bpp,
                                            src_stride, dst_stride, copy_mode,
                                            dstRect);
        regions_copied = bytes_copied ? 1 : 0;
    }
    else
    {
        for (UINT i = 0; i < subRectCnt; i++)
        {
            ULONGLONG n = VioGpuBltCopyRegion(cmd, src, dst, (PUCHAR)src_va,
                                              src_map_size, (PUCHAR)dst_va,
                                              dst_map_size, dstRect, dx, dy, bpp,
                                              src_stride, dst_stride, copy_mode,
                                              &subRects[i]);
            if (n)
            {
                bytes_copied += n;
                regions_copied++;
            }
        }
    }
    const LARGE_INTEGER end = KeQueryPerformanceCounter(NULL);
    ULONGLONG duration_us = 0;
    if (frequency.QuadPart > 0 && end.QuadPart >= start.QuadPart)
    {
        duration_us =
            ((ULONGLONG)(end.QuadPart - start.QuadPart) * 1000000ull) /
            (ULONGLONG)frequency.QuadPart;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("%s copied Venus BLT present fence=%u src_res=0x%x dst_res=0x%x src_format=%u dst_format=%u copy_mode=%u subrects=%u regions=%u bytes=0x%llx src_stride=%u dst_stride=%u duration_us=%llu\n",
              __FUNCTION__, cmd->GetSubmissionFenceId(), src->GetId(),
              dst->GetId(), src->GetFormat(), dst->GetFormat(), copy_mode,
              subRectCnt, regions_copied, bytes_copied, src_stride, dst_stride,
              duration_us));

    return regions_copied ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

static UINT CountDmaCompletionPackets(char *command, char *end, UINT fenceId, BOOLEAN *valid)
{
    UINT packets = 0;
    char *cursor = command;

    *valid = TRUE;

    while (cursor < end)
    {
        if (cursor + sizeof(VIOGPU_COMMAND_HDR) > end)
        {
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("%s fence_id=%u truncated command header: cmd=%p end=%p\n",
                      __FUNCTION__,
                      fenceId,
                      cursor,
                      end));
            *valid = FALSE;
            return 0;
        }

        VIOGPU_COMMAND_HDR *cmdHdr = (VIOGPU_COMMAND_HDR *)cursor;
        cursor += sizeof(VIOGPU_COMMAND_HDR);

        if (cursor + cmdHdr->size > end)
        {
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("%s fence_id=%u invalid command size=%u cmd=%p end=%p\n",
                      __FUNCTION__,
                      fenceId,
                      cmdHdr->size,
                      cursor,
                      end));
            *valid = FALSE;
            return 0;
        }

        cursor += cmdHdr->size;

        switch (cmdHdr->type)
        {
            case VIOGPU_CMD_NOP:
            case VIOGPU_CMD_SUBMIT:
            case VIOGPU_CMD_TRANSFER_TO_HOST:
            case VIOGPU_CMD_TRANSFER_FROM_HOST:
                packets++;
                break;

            case VIOGPU_CMD_PRESENT_FLIP:
                if (cmdHdr->size < sizeof(VIOGPU_PRESENT_FLIP_CMD))
                {
                    DbgPrint(TRACE_LEVEL_WARNING,
                             ("%s fence_id=%u invalid present flip size=%u\n",
                              __FUNCTION__,
                              fenceId,
                              cmdHdr->size));
                    *valid = FALSE;
                    return 0;
                }
                packets++;
                break;

            default:
                DbgPrint(TRACE_LEVEL_WARNING,
                         ("%s fence_id=%u unsupported command type=%u size=%u\n",
                          __FUNCTION__, fenceId, cmdHdr->type, cmdHdr->size));
                *valid = FALSE;
                return 0;
        }
    }

    return packets;
}

void VioGpuCommand::Run()
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    // Base reference for this submission. Each packet submitted in the loop
    // below takes its own reference (InterlockedIncrement); the matching ISR
    // completions and the final VioGpuCommandDone() at the end of Run() retire
    // them, and the last decrement to zero frees the command.
    InterlockedIncrement(&m_done);

    if (m_cpuCopyBlt && m_cpuCopyBltMapped)
    {
        NTSTATUS copyStatus =
            VioGpuCpuCopyBltPresent(this, m_cpuCopySrc,
                                    m_cpuCopyDst, &m_cpuCopySrcRect,
                                    &m_cpuCopyDstRect,
                                    m_cpuCopySrcVa,
                                    m_cpuCopySrcMapSize,
                                    m_cpuCopyDstVa,
                                    m_cpuCopyDstMapSize,
                                    m_cpuCopySubRects,
                                    m_cpuCopySubRectCnt);
        if (!NT_SUCCESS(copyStatus))
        {
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("%s Venus/Yttrium BLT CPU-copy failed in SubmitCommand status=0x%x fence=%u src_res=0x%x dst_res=0x%x\n",
                      __FUNCTION__,
                      copyStatus,
                      m_FenceId,
                      m_cpuCopySrc ? m_cpuCopySrc->GetId() : 0,
                      m_cpuCopyDst ? m_cpuCopyDst->GetId() : 0));
        }
        ClearCpuCopyBlt();
    }
    else if (m_cpuCopyBlt)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s Venus/Yttrium BLT CPU-copy reached SubmitCommand without mapped pointers fence=%u src_res=0x%x dst_res=0x%x\n",
                  __FUNCTION__,
                  m_FenceId,
                  m_cpuCopySrc ? m_cpuCopySrc->GetId() : 0,
                  m_cpuCopyDst ? m_cpuCopyDst->GetId() : 0));
        ClearCpuCopyBlt();
    }

    if (!m_pCommand || !m_pEnd || m_pCommand >= m_pEnd)
    {
        if (m_expectedEmptySubmit)
        {
            DbgPrint(TRACE_LEVEL_VERBOSE,
                     ("%s cmd=%p empty dma submit (fence-only) fence=%u node=%u engine=%u submit_flags=0x%x paging=%u hContext=%p ctx_id=%u owner_pid=%p cmd_ptr=%p end_ptr=%p\n",
                      __FUNCTION__,
                      this,
                      m_FenceId,
                      m_NodeOrdinal,
                      m_EngineOrdinal,
                      m_submitFlagsValue,
                      m_submitPaging ? 1 : 0,
                      m_pContext,
                      m_pContext ? m_pContext->GetId() : 0,
                      m_pContext ? m_pContext->GetOwnerProcessId() : 0,
                      m_pCommand,
                      m_pEnd));
        }
        else
        {
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("%s cmd=%p WARNING unexpected empty dma buffer fence=%u node=%u engine=%u submit_flags=0x%x paging=%u hContext=%p ctx_id=%u owner_pid=%p cmd_ptr=%p end_ptr=%p\n",
                      __FUNCTION__,
                      this,
                      m_FenceId,
                      m_NodeOrdinal,
                      m_EngineOrdinal,
                      m_submitFlagsValue,
                      m_submitPaging ? 1 : 0,
                      m_pContext,
                      m_pContext ? m_pContext->GetId() : 0,
                      m_pContext ? m_pContext->GetOwnerProcessId() : 0,
                      m_pCommand,
                      m_pEnd));
        }

        /* Keep fence completion ordering consistent with non-empty submits.
         * Immediate software completion can overtake older pending fences and
         * trigger VIDEO_SCHEDULER_INTERNAL_ERROR(0x119, Arg1=1).
         */
        InterlockedExchange(&m_isrPendingPackets, 1);
        InterlockedIncrement(&m_done);
        UINT ret = m_pAdapter->ctrlQueue.SubmitNop(VioGpuCommand::RunningCbDone, this, TRUE /* fenced */);
        if (ret)
        {
            BugCheckDmaQueueSubmitFailure(this,
                                           m_FenceId,
                                           m_NodeOrdinal,
                                           m_EngineOrdinal,
                                           VIOGPU_CMD_NOP,
                                           ret);
        }

        VioGpuCommand::VioGpuCommandDone();
        return;
    }

    BOOLEAN validCommandBuffer = TRUE;
    UINT pendingPackets = CountDmaCompletionPackets(m_pCommand, m_pEnd, m_FenceId, &validCommandBuffer);
    if (!validCommandBuffer)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s cmd=%p fence=%u invalid DMA command stream, completing with fenced NOP\n",
                  __FUNCTION__,
                  this,
                  m_FenceId));

        InterlockedExchange(&m_isrPendingPackets, 1);
        InterlockedIncrement(&m_done);
        UINT ret = m_pAdapter->ctrlQueue.SubmitNop(VioGpuCommand::RunningCbDone, this, TRUE /* fenced */);
        if (ret)
        {
            BugCheckDmaQueueSubmitFailure(this,
                                           m_FenceId,
                                           m_NodeOrdinal,
                                           m_EngineOrdinal,
                                           VIOGPU_CMD_NOP,
                                           ret);
        }

        VioGpuCommand::VioGpuCommandDone();
        return;
    }

    InterlockedExchange(&m_isrPendingPackets, (LONG)pendingPackets);

    while (m_pCommand < m_pEnd)
    {
        if (m_pCommand + sizeof(VIOGPU_COMMAND_HDR) > m_pEnd)
        {
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("%s fence_id=%u truncated command header: cmd=%p end=%p\n",
                      __FUNCTION__, m_FenceId, m_pCommand, m_pEnd));
            break;
        }
        VIOGPU_COMMAND_HDR *cmdHdr = (VIOGPU_COMMAND_HDR *)m_pCommand;
        m_pCommand += sizeof(VIOGPU_COMMAND_HDR);

        void *cmdBody = m_pCommand;
        if (m_pCommand + cmdHdr->size > m_pEnd)
        {
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("%s fence_id=%u invalid command size=%u cmd=%p end=%p\n",
                      __FUNCTION__, m_FenceId, cmdHdr->size, m_pCommand, m_pEnd));
            break;
        }
        m_pCommand += cmdHdr->size;
        DbgPrint(TRACE_LEVEL_VERBOSE, ("%s fence_id=%d running command=%d", __FUNCTION__, m_FenceId, cmdHdr->type));

        switch (cmdHdr->type)
        {
            case VIOGPU_CMD_NOP:
                {
                    InterlockedIncrement(&m_done);

                    UINT ret = m_pAdapter->ctrlQueue.SubmitNop(VioGpuCommand::RunningCbDone,
                                                                this,
                                                                TRUE /* fenced */);
                    if (ret)
                    {
                        BugCheckDmaQueueSubmitFailure(this,
                                                       m_FenceId,
                                                       m_NodeOrdinal,
                                                       m_EngineOrdinal,
                                                       cmdHdr->type,
                                                       ret);
                    }
                    break;
                }

            case VIOGPU_CMD_SUBMIT:
                {
                    InterlockedIncrement(&m_done);

                    PBYTE submitCmd = NULL;
                    if (cmdHdr->size > 0)
                    {
                        submitCmd = new (NonPagedPoolNx) BYTE[cmdHdr->size];
                        if (!submitCmd)
                        {
                            DbgPrint(TRACE_LEVEL_FATAL,
                                     ("%s fence_id=%u OOM allocating submit buffer (size=%u) -> bugcheck\n",
                                      __FUNCTION__,
                                      m_FenceId,
                                      cmdHdr->size));
                            KeBugCheckEx(0x000000E2,
                                         static_cast<ULONG_PTR>('OIVg'),
                                         static_cast<ULONG_PTR>(m_FenceId),
                                         static_cast<ULONG_PTR>(cmdHdr->size),
                                         reinterpret_cast<ULONG_PTR>(this));
                        }
                        RtlCopyMemory(submitCmd, cmdBody, cmdHdr->size);
                    }

                    UINT ret = m_pAdapter->ctrlQueue.SubmitCommand(submitCmd,
                                                                    cmdHdr->size,
                                                                    m_pContext->GetId(),
                                                                    VioGpuCommand::RunningCbDone,
                                                                    this);
                    if (ret)
                    {
                        BugCheckDmaQueueSubmitFailure(this,
                                                       m_FenceId,
                                                       m_NodeOrdinal,
                                                       m_EngineOrdinal,
                                                       cmdHdr->type,
                                                       ret);
                    }
                    break;
                }

            case VIOGPU_CMD_TRANSFER_TO_HOST:
            case VIOGPU_CMD_TRANSFER_FROM_HOST:
                {
                    InterlockedIncrement(&m_done);

                    VIOGPU_TRANSFER_CMD *transferCmd = (VIOGPU_TRANSFER_CMD *)cmdBody;

                    UINT ret = m_pAdapter->ctrlQueue.TransferHostCmd(cmdHdr->type == VIOGPU_CMD_TRANSFER_TO_HOST,
                                                                      m_pContext->GetId(),
                                                                      transferCmd,
                                                                      VioGpuCommand::RunningCbDone,
                                                                      this);
                    if (ret)
                    {
                        BugCheckDmaQueueSubmitFailure(this,
                                                       m_FenceId,
                                                       m_NodeOrdinal,
                                                       m_EngineOrdinal,
                                                       cmdHdr->type,
                                                       ret);
                    }
                    break;
                }

            case VIOGPU_CMD_PRESENT_FLIP:
                {
                    if (cmdHdr->size < sizeof(VIOGPU_PRESENT_FLIP_CMD))
                    {
                        DbgPrint(TRACE_LEVEL_WARNING,
                                 ("%s fence_id=%u invalid present flip size=%u\n",
                                  __FUNCTION__,
                                  m_FenceId,
                                  cmdHdr->size));
                        break;
                    }

                    VIOGPU_PRESENT_FLIP_CMD *flipCmd = (VIOGPU_PRESENT_FLIP_CMD *)cmdBody;

                    InterlockedIncrement(&m_done);

                    if (flipCmd->is_blob)
                    {
                        m_pAdapter->ctrlQueue.SetScanoutBlob(flipCmd->scan_id,
                                                             flipCmd->res_id,
                                                             flipCmd->width,
                                                             flipCmd->height,
                                                             flipCmd->x,
                                                             flipCmd->y,
                                                             flipCmd->format,
                                                             flipCmd->stride,
                                                             flipCmd->offset);
                    }
                    else
                    {
                        m_pAdapter->ctrlQueue.SetScanout(flipCmd->scan_id,
                                                         flipCmd->res_id,
                                                         flipCmd->width,
                                                         flipCmd->height,
                                                         flipCmd->x,
                                                         flipCmd->y);
                    }
                    UINT ret = m_pAdapter->ctrlQueue.ResFlush(flipCmd->res_id,
                                                              flipCmd->width,
                                                              flipCmd->height,
                                                              flipCmd->x,
                                                              flipCmd->y,
                                                              VioGpuCommand::RunningCbDone,
                                                              this);
                    if (ret)
                    {
                        BugCheckDmaQueueSubmitFailure(this,
                                                       m_FenceId,
                                                       m_NodeOrdinal,
                                                       m_EngineOrdinal,
                                                       cmdHdr->type,
                                                       ret);
                    }
                    break;
                }

            default:
                {
                    DbgPrint(TRACE_LEVEL_WARNING,
                             ("%s fence_id=%u unsupported command type=%u size=%u\n",
                              __FUNCTION__, m_FenceId, cmdHdr->type, cmdHdr->size));
                    ASSERT(0);
                    break;
                }
        }
    }

    VioGpuCommand::VioGpuCommandDone();
}

ULONG VioGpuCommand::GetContextId() const
{
    return m_pContext ? m_pContext->GetId() : 0;
}

HANDLE VioGpuCommand::GetOwnerProcessId() const
{
    return m_pContext ? m_pContext->GetOwnerProcessId() : NULL;
}

#pragma code_seg(pop)
PAGED_CODE_SEG_BEGIN

NTSTATUS VioGpuCommand::AttachAllocations(DXGK_ALLOCATIONLIST *allocationList, UINT allocationListLength)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    if (allocationListLength == 0)
    {
        m_allocations = NULL;
        m_allocationsLength = 0;
        return STATUS_SUCCESS;
    }

    if (!allocationList)
    {
        return STATUS_INVALID_PARAMETER;
    }

    m_allocations = new (NonPagedPoolNx) VioGpuAllocation *[allocationListLength];
    if (!m_allocations)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s failed to allocate allocation array (count=%u)\n",
                  __FUNCTION__,
                  allocationListLength));
        m_allocationsLength = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    m_allocationsLength = allocationListLength;
    for (UINT i = 0; i < allocationListLength; i++)
    {
        VioGpuDeviceAllocation *deviceAllocation = reinterpret_cast<VioGpuDeviceAllocation *>(allocationList[i].hDeviceSpecificAllocation);
        if (deviceAllocation)
        {
            m_allocations[i] = deviceAllocation->GetAllocation();
            m_allocations[i]->MarkBusy();
        }
        else
        {
            m_allocations[i] = NULL;
        }
    }

    return STATUS_SUCCESS;
}

PAGED_CODE_SEG_END

#pragma code_seg(push)
#pragma code_seg()

void VioGpuCommand::RunningCbDone(void *cmd)
{
    ((VioGpuCommand *)cmd)->VioGpuCommandDone();
}

BOOLEAN VioGpuCommand::OnPacketCompletedFromIsr(UINT *fenceId, UINT *nodeOrdinal, UINT *engineOrdinal)
{
    LONG pending = InterlockedDecrement(&m_isrPendingPackets);
    if (pending > 0)
    {
        return FALSE;
    }

    if (pending < 0)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s cmd=%p WARNING m_isrPendingPackets underflow fence_id=%u\n",
                  __FUNCTION__,
                  this,
                  m_FenceId));
        InterlockedExchange(&m_isrPendingPackets, 0);
        return FALSE;
    }

    if (InterlockedCompareExchange(&m_dmaNotified, 1, 0) != 0)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s cmd=%p duplicate DMA completion suppressed fence_id=%u node=%u engine=%u ctx_id=%u owner_pid=%p\n",
                  __FUNCTION__,
                  this,
                  m_FenceId,
                  m_NodeOrdinal,
                  m_EngineOrdinal,
                  GetContextId(),
                  GetOwnerProcessId()));
        return FALSE;
    }

    if (fenceId)
    {
        *fenceId = m_FenceId;
    }
    if (nodeOrdinal)
    {
        *nodeOrdinal = m_NodeOrdinal;
    }
    if (engineOrdinal)
    {
        *engineOrdinal = m_EngineOrdinal;
    }

    return TRUE;
}

void VioGpuCommand::VioGpuCommandDone()
{
    if (InterlockedDecrement(&m_done))
    {
        return;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("%s cmd=%p finished fence_id=%d\n", __FUNCTION__, this, m_FenceId));

    if (m_allocations)
    {
        for (UINT i = 0; i < m_allocationsLength; i++)
        {
            if (m_allocations[i])
            {
                m_allocations[i]->UnmarkBusy();
            }
        }
        delete[] m_allocations;
    }

    // DMA completion notify happens from ISR during ctrl queue staging.

    if (m_pPrivateDataSlot)
    {
        InterlockedCompareExchangePointer((PVOID volatile *)m_pPrivateDataSlot, NULL, this);
        m_pPrivateDataSlot = NULL;
    }

    ClearCpuCopyBlt();

    delete this;
}

#pragma code_seg(pop)

#pragma code_seg(push)
#pragma code_seg()

VioGpuCommander::VioGpuCommander(VioGpuAdapter *pAdapter)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    m_pAdapter = pAdapter;
}

NTSTATUS VioGpuCommander::Patch(const DXGKARG_PATCH *pPatch)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s \n", __FUNCTION__));

    if (!pPatch)
    {
        return STATUS_INVALID_PARAMETER;
    }

    VioGpuCommand *cmd = NULL;
    if (pPatch->pDmaBufferPrivateData &&
        pPatch->DmaBufferPrivateDataSize >= sizeof(VioGpuCommand *))
    {
        VioGpuCommand **slot =
            (VioGpuCommand **)pPatch->pDmaBufferPrivateData;
        cmd = *slot;
    }

    if (!cmd)
    {
        return STATUS_SUCCESS;
    }

    VioGpuDevice *context = reinterpret_cast<VioGpuDevice *>(pPatch->hContext);
    ULONG ctx_id = context ? context->GetId() : 0;
    NTSTATUS status = cmd->MapCpuCopyBlt(ctx_id);
    if (status != STATUS_NOT_FOUND && !NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s ERROR owner=viogpu3d reason=cpu_blt_mapping_refresh_failed status=0x%x ctx=%u fence=%u priv=%p\n",
                  __FUNCTION__,
                  status,
                  ctx_id,
                  pPatch->SubmissionFenceId,
                  pPatch->pDmaBufferPrivateData));
        // WDDM treats any Patch failure as fatal (bugcheck 0x119/3).
        // Fail closed: this is not recoverable error reporting, and success
        // would retire an invalid CPU BLT as a successful NOP.
        return status;
    }

    return STATUS_SUCCESS;
}

PAGED_CODE_SEG_END

#pragma code_seg(push)
#pragma code_seg()
NTSTATUS VioGpuCommander::SubmitCommand(const DXGKARG_SUBMITCOMMAND *pSubmitCommand)
{
    VIOGPU_ASSERT(pSubmitCommand != NULL);

    // Tripwire for the pDmaBufferPrivateData handoff: it is sound only while each
    // DMA buffer carries a single submission starting at offset 0. A non-zero
    // start means the scheduler packed or split submissions into one buffer, which
    // would make the per-buffer private-data slot ambiguous.
    ASSERT(pSubmitCommand->DmaBufferSubmissionStartOffset == 0);

#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WIN8)
    UINT nodeOrdinal = pSubmitCommand->NodeOrdinal;
#else
    UINT nodeOrdinal = 0;
#endif

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("---> %s fence_id=%u node=%u engine=%u hContext=%p start=0x%x end=0x%x priv=%p\n",
              __FUNCTION__,
              pSubmitCommand->SubmissionFenceId,
              nodeOrdinal,
              pSubmitCommand->EngineOrdinal,
              pSubmitCommand->hContext,
              pSubmitCommand->DmaBufferSubmissionStartOffset,
              pSubmitCommand->DmaBufferSubmissionEndOffset,
              pSubmitCommand->pDmaBufferPrivateData));

    VioGpuCommand *cmd = NULL;
    VioGpuCommand **privSlot = NULL;
    if (pSubmitCommand->pDmaBufferPrivateData)
    {
        privSlot = (VioGpuCommand **)pSubmitCommand->pDmaBufferPrivateData;
        cmd = reinterpret_cast<VioGpuCommand *>(
            InterlockedExchangePointer((PVOID volatile *)privSlot, NULL));
        if (cmd)
        {
            // This command object is consumed by this SubmitCommand invocation.
            // Prevent slot-based reuse across concurrent/duplicate submissions.
            cmd->SetPrivateDataSlot(NULL);
        }
        else
        {
            if (IsExpectedEmptySubmit(pSubmitCommand))
            {
                DbgPrint(TRACE_LEVEL_VERBOSE,
                         ("%s private slot empty for fence-only submit fence_id=%u node=%u engine=%u flags=0x%x hContext=%p priv_slot=%p\n",
                          __FUNCTION__,
                          pSubmitCommand->SubmissionFenceId,
                          nodeOrdinal,
                          pSubmitCommand->EngineOrdinal,
                          pSubmitCommand->Flags.Value,
                          pSubmitCommand->hContext,
                          privSlot));
            }
            else
            {
                DbgPrint(TRACE_LEVEL_WARNING,
                         ("%s private slot already empty fence_id=%u node=%u engine=%u flags=0x%x hContext=%p start=0x%x end=0x%x priv_slot=%p\n",
                          __FUNCTION__,
                          pSubmitCommand->SubmissionFenceId,
                          nodeOrdinal,
                          pSubmitCommand->EngineOrdinal,
                          pSubmitCommand->Flags.Value,
                          pSubmitCommand->hContext,
                          pSubmitCommand->DmaBufferSubmissionStartOffset,
                          pSubmitCommand->DmaBufferSubmissionEndOffset,
                          privSlot));
            }
        }
    }

    if (!cmd)
    {
        cmd = new (NonPagedPoolNx) VioGpuCommand(m_pAdapter);
        if (!cmd)
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("%s failed to allocate command object fence_id=%u hContext=%p\n",
                      __FUNCTION__,
                      pSubmitCommand->SubmissionFenceId,
                      pSubmitCommand->hContext));
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        DbgPrint(TRACE_LEVEL_VERBOSE,
                 ("%s created command object cmd=%p fence_id=%u hContext=%p submit_flags=0x%x start=0x%x end=0x%x priv=%p\n",
                  __FUNCTION__,
                  cmd,
                  pSubmitCommand->SubmissionFenceId,
                  pSubmitCommand->hContext,
                  pSubmitCommand->Flags.Value,
                  pSubmitCommand->DmaBufferSubmissionStartOffset,
                  pSubmitCommand->DmaBufferSubmissionEndOffset,
                  pSubmitCommand->pDmaBufferPrivateData));
    }
    else
    {
        DbgPrint(TRACE_LEVEL_VERBOSE,
                 ("%s consumed private command cmd=%p fence_id=%u hContext=%p priv_slot=%p\n",
                  __FUNCTION__,
                  cmd,
                  pSubmitCommand->SubmissionFenceId,
                  pSubmitCommand->hContext,
                  privSlot));
    }

    cmd->PrepareSubmit(pSubmitCommand);
    m_pAdapter->RecordDmaSubmittedForPreemption(cmd->GetSubmissionFenceId(),
                                                cmd->GetNodeOrdinal(),
                                                cmd->GetEngineOrdinal(),
                                                cmd->GetContextId(),
                                                cmd->GetOwnerProcessId());

    cmd->Run();

    return STATUS_SUCCESS;
}

#pragma code_seg(pop)
