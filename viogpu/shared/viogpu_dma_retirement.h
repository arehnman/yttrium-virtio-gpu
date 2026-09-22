/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */
#pragma once

/* Caller serializes all access at the control-queue interrupt level. Entries
 * live in DMA commands; the held final packet owns a command reference until
 * the ISR stages it for DPC retirement. No allocation or free occurs here. */
typedef struct _VIOGPU_DMA_RETIREMENT
{
    struct _VIOGPU_DMA_RETIREMENT *Next;
    void *Packet;
    unsigned Length;
    unsigned Failed;
} VIOGPU_DMA_RETIREMENT;

typedef struct _VIOGPU_DMA_RETIREMENT_QUEUE
{
    VIOGPU_DMA_RETIREMENT *Head, *Tail;
} VIOGPU_DMA_RETIREMENT_QUEUE;

static inline void VioGpuDmaRetirementPush(VIOGPU_DMA_RETIREMENT_QUEUE *queue,
                                         VIOGPU_DMA_RETIREMENT *entry)
{
    entry->Next = 0;
    entry->Packet = 0;
    entry->Length = 0;
    entry->Failed = 0;
    if (queue->Tail) queue->Tail->Next = entry;
    else queue->Head = entry;
    queue->Tail = entry;
}

static inline VIOGPU_DMA_RETIREMENT *VioGpuDmaRetirementPopReady(VIOGPU_DMA_RETIREMENT_QUEUE *queue)
{
    VIOGPU_DMA_RETIREMENT *entry = queue->Head;
    if (!entry || !entry->Packet || entry->Failed) return 0;
    queue->Head = entry->Next;
    if (!queue->Head) queue->Tail = 0;
    entry->Next = 0;
    return entry;
}
