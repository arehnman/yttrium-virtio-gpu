/*
 * Copyright 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 * 
 */

#pragma once

#include "helper.h"
#include "viogpu_dma_retirement.h"


class VioGpuAdapter;
class VioGpuDevice;
class VioGpuAllocation;
class VioGpuCommander;

class VioGpuCommand
{
  public:
    VioGpuCommand(VioGpuAdapter *adapter);
    ~VioGpuCommand();

    VIOGPU_DMA_RETIREMENT Retirement = {};
    void CancelBeforeRun() { InterlockedIncrement(&m_done); VioGpuCommandDone(); }
    void Run();

    void PrepareSubmit(const DXGKARG_SUBMITCOMMAND *pSubmitCommand);

    static void RunningCbDone(void *cmd);

    void VioGpuCommand::VioGpuCommandDone();

    void SetDmaBuf(char *pDmaBuffer)
    {
        m_pDmaBuffer = pDmaBuffer;
    }

    void SetPrivateDataSlot(VioGpuCommand **slot)
    {
        m_pPrivateDataSlot = slot;
    }

    NTSTATUS SetCpuCopyBlt(VioGpuAllocation *src,
                           VioGpuAllocation *dst,
                           const RECT *srcRect,
                           const RECT *dstRect,
                           const RECT *subRects,
                           UINT subRectCnt);
    NTSTATUS MapCpuCopyBlt(ULONG ctx_id);

    NTSTATUS AttachAllocations(DXGK_ALLOCATIONLIST *allocationList, UINT allocationListLength);

    UINT GetSubmissionFenceId() const
    {
        return m_FenceId;
    }

    UINT GetNodeOrdinal() const
    {
        return m_NodeOrdinal;
    }

    UINT GetEngineOrdinal() const
    {
        return m_EngineOrdinal;
    }

    ULONG GetContextId() const;
    HANDLE GetOwnerProcessId() const;

    BOOLEAN OnPacketCompletedFromIsr(UINT *fenceId, UINT *nodeOrdinal, UINT *engineOrdinal);

  private:
    VioGpuAdapter *m_pAdapter;
    VioGpuCommander *m_pCommander;
    VioGpuDevice *m_pContext;

    UINT m_FenceId;
    UINT m_NodeOrdinal;
    UINT m_EngineOrdinal;

    char *m_pDmaBuffer;
    char *m_pCommand;
    char *m_pEnd;
    LONG m_done = 0;
    LONG m_isrPendingPackets = 0;
    VioGpuCommand **m_pPrivateDataSlot = NULL;
    BOOLEAN m_expectedEmptySubmit = FALSE;
    BOOLEAN m_submitPaging = FALSE;
    UINT m_submitFlagsValue = 0;
    LONG m_dmaNotified = 0;
    BOOLEAN m_cpuCopyBlt = FALSE;
    BOOLEAN m_cpuCopyBltMapped = FALSE;
    RECT m_cpuCopySrcRect = {};
    RECT m_cpuCopyDstRect = {};
    // Dirty sub-rectangles (destination space). When m_cpuCopySubRectCnt == 0
    // the whole src/dst rect intersection is copied. When more than
    // VIOGPU_BLT_MAX_SUBRECTS are supplied they collapse to a single bounding
    // rect (index 0, count 1) so the storage stays bounded.
    static const UINT VIOGPU_BLT_MAX_SUBRECTS = 64;
    RECT m_cpuCopySubRects[VIOGPU_BLT_MAX_SUBRECTS] = {};
    UINT m_cpuCopySubRectCnt = 0;
    VioGpuAllocation *m_cpuCopySrc = NULL;
    VioGpuAllocation *m_cpuCopyDst = NULL;
    PVOID m_cpuCopySrcVa = NULL;
    PVOID m_cpuCopyDstVa = NULL;
    ULONGLONG m_cpuCopySrcMapSize = 0;
    ULONGLONG m_cpuCopyDstMapSize = 0;
    BOOLEAN m_cpuCopySrcIo = FALSE;
    BOOLEAN m_cpuCopyDstIo = FALSE;

    void ClearCpuCopyBltMappings();
    void ClearCpuCopyBlt();

    VioGpuAllocation **m_allocations;
    UINT m_allocationsLength;
};

class VioGpuCommander
{
  public:
    VioGpuCommander(VioGpuAdapter *pAdapter);

    NTSTATUS Patch(const DXGKARG_PATCH *pPatch);
    NTSTATUS SubmitCommand(const DXGKARG_SUBMITCOMMAND *pSubmitCommand);

  private:

    VioGpuAdapter *m_pAdapter;
};
