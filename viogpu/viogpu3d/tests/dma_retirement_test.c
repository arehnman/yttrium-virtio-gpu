/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */
/* Standalone host test, without WDK dependencies. From an MSVC developer shell:
 * cl /nologo /W4 /WX /std:c11 /RTC1 /UNDEBUG dma_retirement_test.c
 * dma_retirement_test.exe
 */
#ifdef NDEBUG
#error This test requires assertions.
#endif
#include <assert.h>
#include <stdio.h>
#include "../../shared/viogpu_dma_retirement.h"

enum { count = 6 };
static unsigned permutations;

static void check_order(const unsigned *order)
{
    VIOGPU_DMA_RETIREMENT_QUEUE queue = {0};
    VIOGPU_DMA_RETIREMENT entries[count] = {0};
    unsigned packets[count] = {0}, next = 0;
    for (unsigned i = 0; i < count; ++i)
        VioGpuDmaRetirementPush(&queue, &entries[i]);
    for (unsigned i = 0; i < count; ++i) {
        unsigned completed = order[i];
        entries[completed].Packet = &packets[completed];
        entries[completed].Length = completed + 1;
        VIOGPU_DMA_RETIREMENT *ready;
        while ((ready = VioGpuDmaRetirementPopReady(&queue)) != 0) {
            assert(ready == &entries[next]);
            assert(ready->Packet == &packets[next] && ready->Length == next + 1);
            assert(!ready->Next);
            ++next;
        }
        assert(next == count || queue.Head == &entries[next]);
    }
    assert(next == count && !queue.Head && !queue.Tail);
    /* Draining and reusing the queue must reset the tail, readiness and links. */
    VioGpuDmaRetirementPush(&queue, &entries[0]);
    assert(!entries[0].Packet && !entries[0].Length && !entries[0].Next);
    assert(!VioGpuDmaRetirementPopReady(&queue));
    entries[0].Packet = &packets[0];
    assert(VioGpuDmaRetirementPopReady(&queue) == &entries[0]);
    assert(!queue.Head && !queue.Tail);
    ++permutations;
}

static void permute(unsigned *order, unsigned n)
{
    if (n == count) { check_order(order); return; }
    for (unsigned i = n; i < count; ++i) {
        unsigned tmp = order[n]; order[n] = order[i]; order[i] = tmp;
        permute(order, n + 1);
        tmp = order[n]; order[n] = order[i]; order[i] = tmp;
    }
}

int main(void)
{
    unsigned order[count] = {0, 1, 2, 3, 4, 5};
    permute(order, 0);
    assert(permutations == 720);
    VIOGPU_DMA_RETIREMENT_QUEUE queue = {0};
    VIOGPU_DMA_RETIREMENT entries[2] = {0};
    VioGpuDmaRetirementPush(&queue, &entries[0]);
    VioGpuDmaRetirementPush(&queue, &entries[1]);
    entries[0].Packet = &entries[0];
    entries[0].Failed = 1;
    entries[1].Packet = &entries[1];
    assert(!VioGpuDmaRetirementPopReady(&queue));
    assert(queue.Head == &entries[0] && queue.Tail == &entries[1]);
    puts("DMA retirement: PASS (all 720 completion orders; held packets retire in submission order)");
    return 0;
}
