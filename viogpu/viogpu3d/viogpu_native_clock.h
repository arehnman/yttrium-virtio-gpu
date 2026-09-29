/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */
/* Implementation included by viogpu_adapter.cpp. Sampling/transport happens in
 * the UMD at passive level. The cache read path never waits for host progress. */
#if VIOGPU_WDDM2
#pragma code_seg(push)
#pragma code_seg()

void VioGpuAdapter::ResetNativeClock(void)
{
    // DxgkDdiResetDevice can run at any IRQL, including a bugcheck. Do not take
    // a lock which an interrupted CPU could own. Observe this serial under the
    // cache lock on the next normal operation instead.
    InterlockedIncrement(&m_clockResetRequested);
}

void VioGpuAdapter::CheckNativeClockReset(void)
{
    const LONG requested = InterlockedCompareExchange(&m_clockResetRequested, 0, 0);
    if (requested != m_clockResetObserved)
    {
        VioGpuClockReset(&m_clockCache);
        m_clockResetObserved = requested;
    }
}

void VioGpuAdapter::ReleaseNativeClock(VioGpuDevice *owner)
{
    VIOGPU_CLOCK_PUBLISHER *publisher = owner->GetNativeClockPublisher();
    KIRQL irql;
    KeAcquireSpinLock(&m_clockLock, &irql);
    CheckNativeClockReset();
    VioGpuClockRelease(&m_clockCache, publisher);
    KeReleaseSpinLock(&m_clockLock, irql);
}

NTSTATUS VioGpuAdapter::ReadNativeClock(VIOGPU_CLOCK_SAMPLE *sample)
{
    if (!sample) return STATUS_INVALID_PARAMETER;
    KIRQL irql;
    LARGE_INTEGER frequency;
    KeAcquireSpinLock(&m_clockLock, &irql);
    CheckNativeClockReset();
    const LARGE_INTEGER now = KeQueryPerformanceCounter(&frequency);
    const int valid = VioGpuClockRead(&m_clockCache, (ULONGLONG)now.QuadPart,
                                    (ULONGLONG)frequency.QuadPart, sample);
    KeReleaseSpinLock(&m_clockLock, irql);
    return valid ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
}

// Although the escape enters at PASSIVE_LEVEL, its body must stay resident
// while holding the spin lock at DISPATCH_LEVEL.
NTSTATUS VioGpuAdapter::NativeClockEscape(const DXGKARG_ESCAPE *escape)
{
    PAGED_CODE();
    static_assert(sizeof(VIOGPU_CLOCK_REQUEST) == 72, "Clock escape ABI");
    const VIOGPU_ESCAPE *header = static_cast<const VIOGPU_ESCAPE *>(escape->pPrivateDriverData);
    if (header->DataLength != sizeof(VIOGPU_CLOCK_REQUEST) ||
        escape->PrivateDriverDataSize != sizeof(VIOGPU_ESCAPE) + sizeof(VIOGPU_CLOCK_REQUEST))
        return STATUS_INVALID_BUFFER_SIZE;
    VioGpuDevice *owner = static_cast<VioGpuDevice *>(escape->hDevice);
    if (!owner || owner->GetOwnerProcess() != PsGetCurrentProcess()) return STATUS_INVALID_PARAMETER;
    // VIOGPU_ESCAPE is packed: copy the payload into aligned storage first.
    VIOGPU_CLOCK_REQUEST request;
    RtlCopyMemory(&request, header + 1, sizeof(request));
    if (request.Version != VIOGPU_NATIVE_CLOCK_VERSION || request.Operation > VIOGPU_CLOCK_RELEASE || request.Result || request.Reserved)
        return STATUS_INVALID_PARAMETER;
    if (request.Operation == VIOGPU_CLOCK_PUBLISH && (!owner->IsContextCreated() || !owner->IsVenusContext()))
        return STATUS_INVALID_DEVICE_STATE;
    if (request.Operation != VIOGPU_CLOCK_PUBLISH)
    {
        const VIOGPU_CLOCK_SAMPLE empty = {};
        if (RtlCompareMemory(&request.Sample, &empty, sizeof(empty)) != sizeof(empty))
            return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS status = STATUS_SUCCESS;
    VIOGPU_CLOCK_PUBLISHER *publisher = owner->GetNativeClockPublisher();
    KIRQL irql;
    KeAcquireSpinLock(&m_clockLock, &irql);
    CheckNativeClockReset();
    if (request.Operation == VIOGPU_CLOCK_QUERY)
        request.Generation = m_clockCache.Generation;
    else if (request.Operation == VIOGPU_CLOCK_RELEASE)
        VioGpuClockRelease(&m_clockCache, publisher);
    else
    {
        LARGE_INTEGER frequency;
        const LARGE_INTEGER now = KeQueryPerformanceCounter(&frequency);
        switch (VioGpuClockPublish(&m_clockCache, publisher, &request, (ULONGLONG)now.QuadPart,
                                  (ULONGLONG)frequency.QuadPart))
        {
        case VIOGPU_CLOCK_OK: break;
        // D3D's EscapeCb can collapse STATUS_RETRY to E_FAIL. Preserve an
        // expected rejected sample in the payload, so the publisher can take
        // another sample without misclassifying it as transport/device failure.
        case VIOGPU_CLOCK_RETRY: request.Result = (unsigned int)STATUS_RETRY; break;
        case VIOGPU_CLOCK_FREQUENCY_CHANGED: status = STATUS_DEVICE_DATA_ERROR; break;
        default: status = STATUS_INVALID_PARAMETER; break;
        }
    }
    KeReleaseSpinLock(&m_clockLock, irql);
    if (NT_SUCCESS(status))
        RtlCopyMemory(static_cast<UCHAR *>(escape->pPrivateDriverData) + sizeof(VIOGPU_ESCAPE), &request, sizeof(request));
    return status;
}
#pragma code_seg(pop)
#endif
