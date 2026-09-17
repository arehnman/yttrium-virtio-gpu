/*
 * Copyright 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 * 
 */

#include "viogpu_device.h"
#include "viogpu_adapter.h"
#include "baseobj.h"
#include "virgl_hw.h"

PAGED_CODE_SEG_BEGIN

VioGpuDevice::VioGpuDevice(VioGpuAdapter *pAdapter)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s\n", __FUNCTION__));

    m_pAdapter = pAdapter;
    m_owner_process = PsGetCurrentProcess();
    m_owner_pid = PsGetCurrentProcessId();
    ObReferenceObject(m_owner_process);
    m_id = pAdapter->ctxIdr.GetId();
    m_capset_id = 0;
    m_context_created = false;
}

VioGpuDevice::~VioGpuDevice()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s\n", __FUNCTION__));

    if (m_context_created)
    {
        m_pAdapter->ctrlQueue.DestroyCtx(m_id);
        m_context_created = false;
    }
    m_pAdapter->ctxIdr.PutId(m_id);
    if (m_owner_process)
    {
        ObDereferenceObject(m_owner_process);
        m_owner_process = NULL;
    }
}

static NTSTATUS VioGpuEmitPresentNop(DXGKARG_PRESENT *pPresent)
{
    if (!pPresent->pDmaBuffer)
    {
        return STATUS_SUCCESS;
    }

    if (pPresent->DmaSize < sizeof(VIOGPU_COMMAND_HDR))
    {
        return STATUS_INVALID_USER_BUFFER;
    }

    VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)pPresent->pDmaBuffer;
    cmd_hdr->type = VIOGPU_CMD_NOP;
    cmd_hdr->size = 0;
    pPresent->pDmaBuffer = (char *)pPresent->pDmaBuffer + sizeof(VIOGPU_COMMAND_HDR);

    return STATUS_SUCCESS;
}


NTSTATUS VioGpuDevice::Init(VIOGPU_CTX_INIT_REQ *pOptions)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s", __FUNCTION__));

    UINT context_init = 0;

    m_capset_id = pOptions->CapsetID;
    context_init |= m_capset_id;

    if (m_context_created)
    {
        m_pAdapter->ctrlQueue.DestroyCtx(m_id);
        m_context_created = false;
    }
    m_pAdapter->ctrlQueue.CreateCtx(m_id, context_init);
    m_context_created = true;

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDevice::GenerateBltPresent(DXGKARG_PRESENT *pPresent, VioGpuAllocation *src, VioGpuAllocation *dst)
{
    if (!pPresent->pDmaBuffer ||
        !pPresent->pDstSubRects ||
        pPresent->SubRectCnt == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    UCHAR *dmaBuf = (UCHAR *)pPresent->pDmaBuffer;

    // Calculate rect covering all SubRectx
    RECT coverRect = pPresent->pDstSubRects[0];
    for (UINT i = 1; i < pPresent->SubRectCnt; i++)
    {
        coverRect.top = min(coverRect.top, pPresent->pDstSubRects[i].top);
        coverRect.left = min(coverRect.left, pPresent->pDstSubRects[i].left);
        coverRect.right = max(coverRect.right, pPresent->pDstSubRects[i].right);
        coverRect.bottom = max(coverRect.bottom, pPresent->pDstSubRects[i].bottom);
    }

    INT dx = pPresent->SrcRect.left - pPresent->DstRect.left;
    INT dy = pPresent->SrcRect.top - pPresent->DstRect.top;

    // If source requires coherency (staging or shadow surface) then emit transfer
    if (src->IsCoherent())
    {
        VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)dmaBuf;
        cmd_hdr->type = VIOGPU_CMD_TRANSFER_TO_HOST;
        cmd_hdr->size = sizeof(VIOGPU_TRANSFER_CMD);
        dmaBuf += sizeof(VIOGPU_COMMAND_HDR);

        VIOGPU_TRANSFER_CMD *cmdBody = (VIOGPU_TRANSFER_CMD *)dmaBuf;
        dmaBuf += sizeof(VIOGPU_TRANSFER_CMD);

        cmdBody->res_id = src->GetId();

        cmdBody->box.x = coverRect.left + dx;
        cmdBody->box.y = coverRect.top + dy;
        cmdBody->box.z = 0;
        cmdBody->box.width = coverRect.right - coverRect.left;
        cmdBody->box.height = coverRect.bottom - coverRect.top;
        cmdBody->box.depth = 1;

        cmdBody->layer_stride = 0;
        cmdBody->stride = 0;
        cmdBody->level = 0;
        cmdBody->offset = 0;
    }

    {
        UINT sizeOfOneRect = 4 * (VIRGL_CMD_RESOURCE_COPY_REGION_SIZE + 1);
        if (pPresent->DmaSize < 0x100 + sizeOfOneRect)
        {
            return STATUS_INVALID_USER_BUFFER;
        }

        // TODO: Support MultiPassOffset
        UINT rectCnt = min(pPresent->SubRectCnt, (pPresent->DmaSize - 0x100) / sizeOfOneRect);

        VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)dmaBuf;
        cmd_hdr->type = VIOGPU_CMD_SUBMIT;
        cmd_hdr->size = rectCnt * sizeOfOneRect;
        dmaBuf += sizeof(VIOGPU_COMMAND_HDR);

        for (UINT i = 0; i < rectCnt; i++)
        {
            UINT *cmdBody = (UINT *)dmaBuf;
            dmaBuf += sizeOfOneRect;

            RECT rect = pPresent->pDstSubRects[i];

            cmdBody[0] = VIRGL_CMD0(VIRGL_CCMD_RESOURCE_COPY_REGION, 0, VIRGL_CMD_RESOURCE_COPY_REGION_SIZE);
            cmdBody[1] = dst->GetId();
            cmdBody[2] = 0;
            cmdBody[3] = rect.left;
            cmdBody[4] = rect.top;
            cmdBody[5] = 0;

            cmdBody[6] = src->GetId();
            cmdBody[7] = 0;
            cmdBody[8] = rect.left + dx;
            cmdBody[9] = rect.top + dy;
            cmdBody[10] = 0;
            cmdBody[11] = rect.right - rect.left;
            cmdBody[12] = rect.bottom - rect.top;
            cmdBody[13] = 1;
        }
    }

    if (dst->IsCoherent())
    {
        VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)dmaBuf;
        cmd_hdr->type = VIOGPU_CMD_TRANSFER_FROM_HOST;
        cmd_hdr->size = sizeof(VIOGPU_TRANSFER_CMD);
        dmaBuf += sizeof(VIOGPU_COMMAND_HDR);

        VIOGPU_TRANSFER_CMD *cmdBody = (VIOGPU_TRANSFER_CMD *)dmaBuf;
        dmaBuf += sizeof(VIOGPU_TRANSFER_CMD);

        cmdBody->res_id = dst->GetId();

        cmdBody->box.x = coverRect.left;
        cmdBody->box.y = coverRect.top;
        cmdBody->box.z = 0;
        cmdBody->box.width = coverRect.right - coverRect.left;
        cmdBody->box.height = coverRect.bottom - coverRect.top;
        cmdBody->box.depth = 1;

        cmdBody->layer_stride = 0;
        cmdBody->stride = 0;
        cmdBody->level = 0;
        cmdBody->offset = 0;
    }

    pPresent->pDmaBuffer = dmaBuf;

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDevice::Present(_Inout_ DXGKARG_PRESENT *pPresent)
{
    PAGED_CODE();

    DXGK_ALLOCATIONLIST *dxgk_src =
        pPresent->pAllocationList ? &pPresent->pAllocationList[DXGK_PRESENT_SOURCE_INDEX] : NULL;
    DXGK_ALLOCATIONLIST *dxgk_dst =
        pPresent->pAllocationList ? &pPresent->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX] : NULL;

    VioGpuAllocation *vensrc = NULL;
    VioGpuAllocation *vendst = NULL;

    if (dxgk_src && dxgk_src->hDeviceSpecificAllocation != NULL)
    {
        vensrc = reinterpret_cast<VioGpuDeviceAllocation *>(dxgk_src->hDeviceSpecificAllocation)->GetAllocation();
    }

    if (dxgk_dst != NULL && dxgk_dst->hDeviceSpecificAllocation != NULL)
    {
        vendst = reinterpret_cast<VioGpuDeviceAllocation *>(dxgk_dst->hDeviceSpecificAllocation)->GetAllocation();
    }

    if (pPresent->Flags.Flip || pPresent->Flags.FlipWithNoWait)
    {
        if (pPresent->pDmaBuffer &&
            pPresent->DmaSize != 0)
        {
            const UINT flipCommandSize = sizeof(VIOGPU_COMMAND_HDR) + sizeof(VIOGPU_PRESENT_FLIP_CMD);
            if (pPresent->DmaSize < flipCommandSize)
            {
                return STATUS_INVALID_USER_BUFFER;
            }

            if (!dxgk_src || dxgk_src->hDeviceSpecificAllocation == NULL)
            {
                return STATUS_INVALID_PARAMETER;
            }

            VioGpuAllocation *src =
                reinterpret_cast<VioGpuDeviceAllocation *>(dxgk_src->hDeviceSpecificAllocation)->GetAllocation();
            if (!src)
            {
                return STATUS_INVALID_PARAMETER;
            }

            VioGpuCommand *cmd = new (NonPagedPoolNx) VioGpuCommand(m_pAdapter);
            if (!cmd)
            {
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            if (pPresent->pDmaBufferPrivateData &&
                pPresent->DmaBufferPrivateDataSize >= sizeof(VioGpuCommand *))
            {
                VioGpuCommand **privateData = (VioGpuCommand **)pPresent->pDmaBufferPrivateData;
                // Tripwire: the slot must be clean before we stash the command. A
                // non-NULL slot would mean a reused/offset DMA buffer carried a stale
                // command -> the pDmaBufferPrivateData handoff would be unsafe.
                ASSERT(*privateData == NULL);
                *privateData = cmd;
                cmd->SetPrivateDataSlot(privateData);
            }
            else
            {
                delete cmd;
                return STATUS_INVALID_PARAMETER;
            }

            cmd->SetDmaBuf((char *)pPresent->pDmaBuffer);

            VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)pPresent->pDmaBuffer;
            cmd_hdr->type = VIOGPU_CMD_PRESENT_FLIP;
            cmd_hdr->size = sizeof(VIOGPU_PRESENT_FLIP_CMD);

            VIOGPU_PRESENT_FLIP_CMD *flipCmd = (VIOGPU_PRESENT_FLIP_CMD *)(cmd_hdr + 1);
            flipCmd->scan_id = 0;
            flipCmd->res_id = src->GetId();
            flipCmd->width = src->GetWidth();
            flipCmd->height = src->GetHeight();
            flipCmd->x = 0;
            flipCmd->y = 0;
            flipCmd->is_blob = src->IsBlob() ? 1 : 0;
            flipCmd->format = src->GetFormat();
            flipCmd->stride = src->GetStride();
            flipCmd->offset = src->GetScanoutOffset();

            pPresent->pDmaBuffer = (char *)pPresent->pDmaBuffer + flipCommandSize;
        }
        return STATUS_SUCCESS;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("<---> %s Flags=%s %s %s %s %s %s %s %s)\n",
              __FUNCTION__,
              pPresent->Flags.Blt ? "Blt" : "",
              pPresent->Flags.ColorFill ? "ColorFill" : "",
              pPresent->Flags.Flip ? "Flip" : "",
              pPresent->Flags.FlipWithNoWait ? "FlipWithNoWait" : "",
              pPresent->Flags.SrcColorKey ? "SrcColorKey" : "",
              pPresent->Flags.DstColorKey ? "DstColorKey" : "",
              pPresent->Flags.LinearToSrgb ? "LinearToSrgb" : "",
              pPresent->Flags.Rotate ? "Rotate" : ""));

    VioGpuCommand *cmd = new (NonPagedPoolNx) VioGpuCommand(m_pAdapter);
    if (!cmd)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (pPresent->pDmaBufferPrivateData &&
        pPresent->DmaBufferPrivateDataSize >= sizeof(VioGpuCommand *))
    {
        VioGpuCommand **privateData = (VioGpuCommand **)pPresent->pDmaBufferPrivateData;
        // Tripwire: the slot must be clean before we stash the command. A non-NULL
        // slot would mean a reused/offset DMA buffer carried a stale command ->
        // the pDmaBufferPrivateData handoff would be unsafe.
        ASSERT(*privateData == NULL);
        *privateData = cmd;
        cmd->SetPrivateDataSlot(privateData);
    }
    else
    {
        delete cmd;
        return STATUS_INVALID_PARAMETER;
    }

    if (pPresent->pDmaBuffer)
    {
        cmd->SetDmaBuf((char *)pPresent->pDmaBuffer);
    }

    VioGpuAllocation *src = NULL;
    VioGpuAllocation *dst = NULL;

    if (dxgk_src && dxgk_src->hDeviceSpecificAllocation != NULL)
    {
        src = reinterpret_cast<VioGpuDeviceAllocation *>(dxgk_src->hDeviceSpecificAllocation)->GetAllocation();
        if (pPresent->pDmaBuffer)
        {
            pPresent->pPatchLocationListOut->AllocationIndex = DXGK_PRESENT_SOURCE_INDEX;
            pPresent->pPatchLocationListOut->AllocationOffset = 0;
            pPresent->pPatchLocationListOut->DriverId = 1;
            pPresent->pPatchLocationListOut->SlotId = 1;
            pPresent->pPatchLocationListOut->PatchOffset = 0;
            pPresent->pPatchLocationListOut->SplitOffset = 0;

            pPresent->pPatchLocationListOut += 1;
        }
    }

    if (dxgk_dst != NULL && dxgk_dst->hDeviceSpecificAllocation != NULL)
    {
        dst = reinterpret_cast<VioGpuDeviceAllocation *>(dxgk_dst->hDeviceSpecificAllocation)->GetAllocation();
        if (pPresent->pDmaBuffer)
        {
            pPresent->pPatchLocationListOut->AllocationIndex = DXGK_PRESENT_DESTINATION_INDEX;
            pPresent->pPatchLocationListOut->AllocationOffset = 0;
            pPresent->pPatchLocationListOut->DriverId = 2;
            pPresent->pPatchLocationListOut->SlotId = 2;
            pPresent->pPatchLocationListOut->PatchOffset = 0;
            pPresent->pPatchLocationListOut->SplitOffset = 0;

            pPresent->pPatchLocationListOut += 1;
        }
    }

    if (pPresent->Flags.Blt)
    {
        if (IsVenusContext())
        {
            NTSTATUS stageStatus =
                cmd->SetCpuCopyBlt(src, dst, &pPresent->SrcRect,
                                   &pPresent->DstRect,
                                   pPresent->pDstSubRects,
                                   pPresent->SubRectCnt);
            if (!NT_SUCCESS(stageStatus))
            {
                DbgPrint(TRACE_LEVEL_WARNING,
                         ("%s Venus/Yttrium BLT CPU-copy setup failed status=0x%x ctx=0x%x src_res=0x%x dst_res=0x%x\n",
                          __FUNCTION__,
                          stageStatus,
                          m_id,
                          src ? src->GetId() : 0,
                          dst ? dst->GetId() : 0));
                delete cmd;
                return stageStatus;
            }
            else if (src && dst &&
                     ((dxgk_src->SegmentId != 0 && dxgk_dst->SegmentId != 0) ||
                      (src->HasCpuCopyBacking() && dst->HasCpuCopyBacking())))
            {
                // Nonzero SegmentId is VidMm's pre-patched residency
                // contract: Patch may be skipped even if the blob's CPU
                // mapping has not been established by an earlier Present.
                NTSTATUS mapStatus = cmd->MapCpuCopyBlt(m_id);
                if (!NT_SUCCESS(mapStatus) && mapStatus != STATUS_NOT_FOUND)
                {
                    DbgPrint(TRACE_LEVEL_WARNING,
                             ("%s Venus/Yttrium BLT CPU-copy resident map failed status=0x%x ctx=0x%x src_res=0x%x dst_res=0x%x\n",
                              __FUNCTION__,
                              mapStatus,
                              m_id,
                              src ? src->GetId() : 0,
                              dst ? dst->GetId() : 0));
                    // A pre-patched Present may bypass Patch entirely.
                    // Do not accept it unless its CPU mapping is usable.
                    delete cmd;
                    return mapStatus;
                }
            }
            NTSTATUS status = VioGpuEmitPresentNop(pPresent);
            if (!NT_SUCCESS(status))
            {
                delete cmd;
            }
            return status;
        }

        if (pPresent->pDmaBuffer && dst && src)
        {
            NTSTATUS status = GenerateBltPresent(pPresent, src, dst);
            if (!NT_SUCCESS(status))
            {
                delete cmd;
            }
            return status;
        }
    }
    else
    {
        NTSTATUS status = VioGpuEmitPresentNop(pPresent);
        if (!NT_SUCCESS(status))
        {
            return status;
        }

        DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s Unsupported PRESENT\n", __FUNCTION__));
    }

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDevice::Render(DXGKARG_RENDER *pRender)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    char *pDmaBufStart = (char *)pRender->pDmaBuffer;

    if (!pRender->pDmaBufferPrivateData ||
        pRender->DmaBufferPrivateDataSize < sizeof(VioGpuCommand *))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (pRender->CommandLength && !pRender->pCommand)
    {
        return STATUS_INVALID_USER_BUFFER;
    }

    if (pRender->CommandLength && !pRender->pDmaBuffer)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (pRender->PatchLocationListInSize > pRender->PatchLocationListOutSize)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (pRender->PatchLocationListInSize &&
        (!pRender->pPatchLocationListIn || !pRender->pPatchLocationListOut))
    {
        return STATUS_INVALID_USER_BUFFER;
    }

    if (pRender->MultipassOffset != 0)
    {
        return STATUS_INVALID_USER_BUFFER;
    }

    __try
    {
        pRender->PatchLocationListOutSize = pRender->PatchLocationListInSize;
        D3DDDI_PATCHLOCATIONLIST *patchLocationListOut = pRender->pPatchLocationListOut;
        for (UINT i = 0; i < pRender->PatchLocationListOutSize; i++)
        {
            patchLocationListOut->AllocationIndex = pRender->pPatchLocationListIn[i].AllocationIndex;
            patchLocationListOut->AllocationOffset = 0;
            patchLocationListOut->PatchOffset = 0;
            patchLocationListOut->SplitOffset = 0;
            patchLocationListOut->SlotId = i;

            patchLocationListOut++;
        }

        unsigned char *dmaBuf = (unsigned char *)pRender->pDmaBuffer;
        unsigned char *cmdBuf = (unsigned char *)pRender->pCommand;
        unsigned char *endBuf = cmdBuf + pRender->CommandLength;
        UINT dmaBytesLeft = pRender->DmaSize;
        while (cmdBuf < endBuf)
        {
            if (cmdBuf + sizeof(VIOGPU_COMMAND_HDR) > endBuf)
            {
                return STATUS_INVALID_USER_BUFFER;
            }

            VIOGPU_COMMAND_HDR cmdHdr;
            memcpy(&cmdHdr, cmdBuf, sizeof(cmdHdr));

            if (cmdHdr.size > (UINT)(endBuf - cmdBuf) - sizeof(VIOGPU_COMMAND_HDR))
            {
                return STATUS_INVALID_USER_BUFFER;
            }

            UINT commandBytes = sizeof(VIOGPU_COMMAND_HDR) + cmdHdr.size;
            if (commandBytes > dmaBytesLeft)
            {
                return STATUS_INVALID_USER_BUFFER;
            }

            memcpy(dmaBuf, cmdBuf, commandBytes);
            dmaBuf += commandBytes;
            cmdBuf += commandBytes;
            dmaBytesLeft -= commandBytes;
        }
        pRender->pPatchLocationListOut = patchLocationListOut;
        pRender->pDmaBuffer = dmaBuf;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("<---> %s Usermode copy exception", __FUNCTION__));
        return STATUS_INVALID_PARAMETER;
    }

    VioGpuCommand *cmd = new (NonPagedPoolNx) VioGpuCommand(m_pAdapter);
    if (!cmd)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (pRender->pDmaBufferPrivateData)
    {
        VioGpuCommand **privateData = (VioGpuCommand **)pRender->pDmaBufferPrivateData;
        // Tripwire: the slot must be clean before we stash the command. A non-NULL
        // slot would mean a reused/offset DMA buffer carried a stale command ->
        // the pDmaBufferPrivateData handoff would be unsafe.
        ASSERT(*privateData == NULL);
        *privateData = cmd;
        cmd->SetPrivateDataSlot(privateData);
    }
    cmd->SetDmaBuf(pDmaBufStart);
    NTSTATUS attachStatus = cmd->AttachAllocations(pRender->pAllocationList, pRender->AllocationListSize);
    if (!NT_SUCCESS(attachStatus))
    {
        if (pRender->pDmaBufferPrivateData)
        {
            VioGpuCommand **privateData = (VioGpuCommand **)pRender->pDmaBufferPrivateData;
            if (*privateData == cmd)
            {
                *privateData = NULL;
            }
        }
        cmd->SetPrivateDataSlot(NULL);
        delete cmd;
        return attachStatus;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    return STATUS_SUCCESS;
};

NTSTATUS VioGpuDevice::OpenAllocation(_In_ CONST DXGKARG_OPENALLOCATION *pOpenAllocation)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    NTSTATUS status = STATUS_SUCCESS;

    for (UINT i = 0; i < pOpenAllocation->NumAllocations; i++)
    {
        DXGK_OPENALLOCATIONINFO *openAllocationInfo = &pOpenAllocation->pOpenAllocation[i];
        VioGpuAllocation *allocation = m_pAdapter->AllocationFromHandle(openAllocationInfo->hAllocation);

        if (!allocation)
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("%s missing allocation handle=%p\n", __FUNCTION__, openAllocationInfo->hAllocation));
            status = STATUS_INVALID_HANDLE;
            goto fail;
        }

        if (allocation->IsBlob())
        {
            status = allocation->EnsureBlobCreatedAndWait(GetId());
            if (!NT_SUCCESS(status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("%s blob create failed status=0x%x res_id=0x%x\n",
                          __FUNCTION__, status, allocation->GetId()));
                goto fail;
            }
        }

        VioGpuDeviceAllocation *devAlloc = new (NonPagedPoolNx) VioGpuDeviceAllocation(this, allocation);
        if (!devAlloc)
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("%s failed to create device allocation\n", __FUNCTION__));
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto fail;
        }

        openAllocationInfo->hDeviceSpecificAllocation = devAlloc;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;

fail:
    for (UINT j = 0; j < pOpenAllocation->NumAllocations; j++)
    {
        DXGK_OPENALLOCATIONINFO *info = &pOpenAllocation->pOpenAllocation[j];
        if (info->hDeviceSpecificAllocation)
        {
            delete reinterpret_cast<VioGpuDeviceAllocation *>(info->hDeviceSpecificAllocation);
            info->hDeviceSpecificAllocation = NULL;
        }
    }
    DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s failed\n", __FUNCTION__));
    return status;
}

CtrlQueue *VioGpuDevice::GetCtrlQueue()
{
    PAGED_CODE();

    return &m_pAdapter->ctrlQueue;
}

VioGpuDeviceAllocation::VioGpuDeviceAllocation(VioGpuDevice *device, VioGpuAllocation *allocation)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("<---> %s res_id=%d ctx_id=%d\n", __FUNCTION__, allocation->GetId(), device->GetId()));

    m_pAllocation = allocation;
    m_pDevice = device;
    m_attached = false;

    if (m_pAllocation->IsBlob() || !m_pDevice->IsVenusContext())
    {
        m_pDevice->GetCtrlQueue()->CtxResource(true, m_pDevice->GetId(), m_pAllocation->GetId());
        m_attached = true;
    }
}

VioGpuDeviceAllocation::~VioGpuDeviceAllocation()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("<---> %s res_id=%d ctx_id=%d\n", __FUNCTION__, m_pAllocation->GetId(), m_pDevice->GetId()));

    if (m_attached)
    {
        m_pDevice->GetCtrlQueue()->CtxResource(false, m_pDevice->GetId(), m_pAllocation->GetId());
        m_attached = false;
    }
}

VioGpuAllocation *VioGpuDeviceAllocation::GetAllocation()
{
    PAGED_CODE();

    return m_pAllocation;
}

PAGED_CODE_SEG_END
