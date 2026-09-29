/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */
#pragma once
typedef unsigned long long VIOGPU_CLOCK_UINT64;

/* Additive escape: this fixed-size payload follows VIOGPU_ESCAPE, outside its
 * union. Do not change the size of that existing Mesa/viogpu ABI. Version 3
 * uses Venus DEVICE ticks shifted left by 64 - timestampValidBits, and scales
 * GpuFrequency by the same factor. Native rollover is then uint64 rollover.
 * Retryable sample rejection is returned separately from transport status. */
#define VIOGPU_NATIVE_CLOCK 0x302u
#define VIOGPU_NATIVE_CLOCK_VERSION 3u
#define VIOGPU_CLOCK_QUERY 0u
#define VIOGPU_CLOCK_PUBLISH 1u
#define VIOGPU_CLOCK_RELEASE 2u

#pragma pack(push, 8)
typedef struct _VIOGPU_CLOCK_SAMPLE {
    VIOGPU_CLOCK_UINT64 GpuTimestamp, GpuFrequency;
    VIOGPU_CLOCK_UINT64 CpuBefore, CpuAfter, CpuFrequency;
    VIOGPU_CLOCK_UINT64 NativeDeviationNs;
} VIOGPU_CLOCK_SAMPLE;

typedef struct _VIOGPU_CLOCK_REQUEST {
    unsigned int Version, Operation;
    VIOGPU_CLOCK_UINT64 Generation;
    VIOGPU_CLOCK_SAMPLE Sample;
    unsigned int Result, Reserved; /* Output NTSTATUS; input fields must be zero. */
} VIOGPU_CLOCK_REQUEST;
#pragma pack(pop)

/* These are validity bounds, not accuracy claims. Guest transport uncertainty
 * is the complete QPC bracket; NativeDeviationNs alone is never sufficient.
 * The UMD must refresh well inside the expiry time and report failures. */
#define VIOGPU_CLOCK_MAX_BRACKET_DIVISOR 100u /* 10 ms */
#define VIOGPU_CLOCK_MAX_AGE_DIVISOR 10u      /* 100 ms */
#define VIOGPU_CLOCK_MAX_NATIVE_DEVIATION_NS 1000000u

/* Nonpaged storage, serialized externally by the adapter's clock spin lock.
 * A device registers once per generation. The latest physical GPU clock sample
 * remains usable while any publisher lives, even if its original source exits. */
typedef struct _VIOGPU_CLOCK_PUBLISHER {
    VIOGPU_CLOCK_UINT64 Generation;
} VIOGPU_CLOCK_PUBLISHER;

typedef struct _VIOGPU_CLOCK_CACHE {
    VIOGPU_CLOCK_UINT64 Generation;
    VIOGPU_CLOCK_SAMPLE Sample;
    unsigned int Publishers;
} VIOGPU_CLOCK_CACHE;

enum VIOGPU_CLOCK_RESULT {
    VIOGPU_CLOCK_OK, VIOGPU_CLOCK_INVALID, VIOGPU_CLOCK_RETRY,
    VIOGPU_CLOCK_FREQUENCY_CHANGED
};

static inline void VioGpuClockReset(VIOGPU_CLOCK_CACHE *cache)
{
    VIOGPU_CLOCK_UINT64 generation = cache->Generation + 1;
    VIOGPU_CLOCK_CACHE empty = {0};
    *cache = empty;
    cache->Generation = generation ? generation : 1;
}

static inline void VioGpuClockRelease(VIOGPU_CLOCK_CACHE *cache, VIOGPU_CLOCK_PUBLISHER *publisher)
{
    if (publisher->Generation == cache->Generation && cache->Publishers) --cache->Publishers;
    publisher->Generation = 0;
    /* Keep the frequency invariant until an adapter reset, even between UMDs. */
}

static inline int VioGpuClockFresh(const VIOGPU_CLOCK_SAMPLE *sample, VIOGPU_CLOCK_UINT64 now, VIOGPU_CLOCK_UINT64 cpuFrequency)
{
    return cpuFrequency && sample->CpuFrequency == cpuFrequency && now >= sample->CpuAfter &&
           now - sample->CpuAfter <= cpuFrequency / VIOGPU_CLOCK_MAX_AGE_DIVISOR;
}

static inline enum VIOGPU_CLOCK_RESULT VioGpuClockPublish(VIOGPU_CLOCK_CACHE *cache,
    VIOGPU_CLOCK_PUBLISHER *publisher, const VIOGPU_CLOCK_REQUEST *request, VIOGPU_CLOCK_UINT64 now, VIOGPU_CLOCK_UINT64 cpuFrequency)
{
    const VIOGPU_CLOCK_SAMPLE *sample = &request->Sample;
    if (!publisher || request->Version != VIOGPU_NATIVE_CLOCK_VERSION || request->Operation != VIOGPU_CLOCK_PUBLISH ||
        !sample->GpuFrequency || !cpuFrequency || sample->CpuFrequency != cpuFrequency ||
        sample->CpuBefore > sample->CpuAfter || sample->CpuAfter > now)
        return VIOGPU_CLOCK_INVALID;
    if (request->Generation != cache->Generation || !VioGpuClockFresh(sample, now, cpuFrequency) ||
        sample->CpuAfter - sample->CpuBefore > cpuFrequency / VIOGPU_CLOCK_MAX_BRACKET_DIVISOR ||
        sample->NativeDeviationNs > VIOGPU_CLOCK_MAX_NATIVE_DEVIATION_NS)
        return VIOGPU_CLOCK_RETRY;
    if (cache->Sample.GpuFrequency && sample->GpuFrequency != cache->Sample.GpuFrequency)
        return VIOGPU_CLOCK_FREQUENCY_CHANGED;
    if (cache->Sample.GpuFrequency) {
        if (sample->CpuBefore < cache->Sample.CpuBefore || sample->CpuAfter < cache->Sample.CpuAfter)
            return VIOGPU_CLOCK_RETRY;
        /* Compare modulo 2^64 only while the previous sample is fresh.
         * A representable frequency implies a half-wrap longer than 0.5 s,
         * safely beyond the 100 ms freshness and 10 ms bracket bounds. After
         * a long publisher gap, QPC orders samples; an arbitrary number of
         * native wraps may have elapsed and GPU ordering is ambiguous. */
        if (VioGpuClockFresh(&cache->Sample, now, cpuFrequency) &&
            sample->GpuTimestamp - cache->Sample.GpuTimestamp > (~(VIOGPU_CLOCK_UINT64)0 >> 1))
            return VIOGPU_CLOCK_RETRY;
    }
    if (publisher->Generation != cache->Generation) {
        if (cache->Publishers == ~0u) return VIOGPU_CLOCK_INVALID;
        ++cache->Publishers;
        publisher->Generation = cache->Generation;
    }
    cache->Sample = *sample;
    return VIOGPU_CLOCK_OK;
}

static inline int VioGpuClockRead(const VIOGPU_CLOCK_CACHE *cache, VIOGPU_CLOCK_UINT64 now,
    VIOGPU_CLOCK_UINT64 cpuFrequency, VIOGPU_CLOCK_SAMPLE *sample)
{
    VIOGPU_CLOCK_SAMPLE empty = {0};
    *sample = empty;
    if (!cache->Publishers || !VioGpuClockFresh(&cache->Sample, now, cpuFrequency)) return 0;
    *sample = cache->Sample;
    return 1;
}
