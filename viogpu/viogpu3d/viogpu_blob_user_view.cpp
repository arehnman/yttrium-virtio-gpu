/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */

#include <ntifs.h>
#include "viogpu_blob_user_view.h"

struct VIOGPU_BLOB_USER_PROCESS
{
    LIST_ENTRY Entry;
    PEPROCESS Process;
    ULONG References; /* Registry spinlock; each view and callback hold one. */
    KMUTEX Mutex;
    LIST_ENTRY Views;
    BOOLEAN Exiting; /* Owner mutex, together with Views and view MM fields. */
};

static const ULONG s_blob_user_process_tag = 'pBgV';
static KSPIN_LOCK s_blob_user_process_lock;
static LIST_ENTRY s_blob_user_processes;

static VIOGPU_BLOB_USER_PROCESS *VioGpuFindBlobUserProcessLocked(PEPROCESS process)
{
    for (PLIST_ENTRY entry = s_blob_user_processes.Flink;
         entry != &s_blob_user_processes; entry = entry->Flink)
    {
        VIOGPU_BLOB_USER_PROCESS *owner =
            CONTAINING_RECORD(entry, VIOGPU_BLOB_USER_PROCESS, Entry);
        if (owner->Process == process)
            return owner;
    }
    return NULL;
}

static VIOGPU_BLOB_USER_PROCESS *VioGpuReferenceBlobUserProcess(PEPROCESS process)
{
    KIRQL old_irql;
    KeAcquireSpinLock(&s_blob_user_process_lock, &old_irql);
    VIOGPU_BLOB_USER_PROCESS *owner = VioGpuFindBlobUserProcessLocked(process);
    if (owner)
        owner->References++;
    KeReleaseSpinLock(&s_blob_user_process_lock, old_irql);
    return owner;
}

static void VioGpuDereferenceBlobUserProcess(VIOGPU_BLOB_USER_PROCESS *owner)
{
    KIRQL old_irql;
    KeAcquireSpinLock(&s_blob_user_process_lock, &old_irql);
    ASSERT(owner->References != 0);
    const BOOLEAN destroy = --owner->References == 0;
    if (destroy)
        RemoveEntryList(&owner->Entry);
    KeReleaseSpinLock(&s_blob_user_process_lock, old_irql);
    if (destroy)
    {
        ASSERT(IsListEmpty(&owner->Views));
        ObDereferenceObject(owner->Process);
        ExFreePoolWithTag(owner, s_blob_user_process_tag);
    }
}

static VIOGPU_BLOB_USER_PROCESS *VioGpuGetBlobUserProcess(PEPROCESS process)
{
    VIOGPU_BLOB_USER_PROCESS *owner = VioGpuReferenceBlobUserProcess(process);
    if (owner)
        return owner;

    VIOGPU_BLOB_USER_PROCESS *candidate = static_cast<VIOGPU_BLOB_USER_PROCESS *>(
        ExAllocatePoolUninitialized(NonPagedPoolNx, sizeof(*candidate), s_blob_user_process_tag));
    if (!candidate)
        return NULL;
    RtlZeroMemory(candidate, sizeof(*candidate));
    candidate->Process = process;
    candidate->References = 1;
    KeInitializeMutex(&candidate->Mutex, 0);
    InitializeListHead(&candidate->Views);
    ObReferenceObject(process);

    /* Another mapping thread may have registered the same owner meanwhile. */
    KIRQL old_irql;
    KeAcquireSpinLock(&s_blob_user_process_lock, &old_irql);
    owner = VioGpuFindBlobUserProcessLocked(process);
    if (owner)
        owner->References++;
    else
        InsertTailList(&s_blob_user_processes, &candidate->Entry);
    KeReleaseSpinLock(&s_blob_user_process_lock, old_irql);
    if (owner)
    {
        ObDereferenceObject(process);
        ExFreePoolWithTag(candidate, s_blob_user_process_tag);
        return owner;
    }
    return candidate;
}

/* Only the owner's mutex is held here. Never acquire an allocation mutex,
 * send host commands, or wait for GPU work while holding it. Exit must wait
 * for an already-entered foreign unmap before deleting this address space;
 * a process object reference alone does not provide that protection. */
static void VioGpuReleaseBlobUserViewLocked(VIOGPU_BLOB_USER_VIEW *view)
{
    if (view->UserVa)
    {
        KAPC_STATE apc = {};
        PEPROCESS process = view->Owner->Process;
        const BOOLEAN attached = process != PsGetCurrentProcess();
        if (attached)
            KeStackAttachProcess(reinterpret_cast<PRKPROCESS>(process), &apc);
        __try
        {
            MmUnmapLockedPages(view->UserVa, view->Mdl);
        }
        __finally
        {
            if (attached)
                KeUnstackDetachProcess(&apc);
        }
        view->UserVa = NULL;
    }
    if (view->Mdl)
    {
        IoFreeMdl(view->Mdl);
        view->Mdl = NULL;
    }
    if (view->Registered)
    {
        RemoveEntryList(&view->Entry);
        view->Registered = FALSE;
    }
}

static void VioGpuBlobProcessNotify(HANDLE parent_id, HANDLE process_id, BOOLEAN create)
{
    UNREFERENCED_PARAMETER(parent_id);
    UNREFERENCED_PARAMETER(process_id);
    if (create)
        return;
    /* The documented legacy notification runs in the last exiting thread,
     * before its process address space is deleted. No host/graphics work is
     * performed here; leave allocation references to normal DXG cleanup. */
    PEPROCESS process = PsGetCurrentProcess();
    ASSERT(PsGetProcessId(process) == process_id);
    VIOGPU_BLOB_USER_PROCESS *owner = VioGpuReferenceBlobUserProcess(process);
    if (!owner)
        return;
    /* Registry lookup never holds its spinlock across this gate or MM work.
     * Only this process's in-flight MM operations can delay its exit. */
    KeWaitForSingleObject(&owner->Mutex, Executive, KernelMode, FALSE, NULL);
    owner->Exiting = TRUE;
    while (!IsListEmpty(&owner->Views))
    {
        VIOGPU_BLOB_USER_VIEW *view =
            CONTAINING_RECORD(owner->Views.Flink, VIOGPU_BLOB_USER_VIEW, Entry);
        VioGpuReleaseBlobUserViewLocked(view);
    }
    KeReleaseMutex(&owner->Mutex, FALSE);
    VioGpuDereferenceBlobUserProcess(owner);
}

NTSTATUS VioGpuInitializeBlobUserViews()
{
    KeInitializeSpinLock(&s_blob_user_process_lock);
    InitializeListHead(&s_blob_user_processes);
    return PsSetCreateProcessNotifyRoutine(VioGpuBlobProcessNotify, FALSE);
}

void VioGpuShutdownBlobUserViews()
{
    /* Removal waits for already-entered notifications before returning. All
     * allocations must have been released before the graphics driver unloads. */
    const NTSTATUS status = PsSetCreateProcessNotifyRoutine(VioGpuBlobProcessNotify, TRUE);
    ASSERT(NT_SUCCESS(status));
    UNREFERENCED_PARAMETER(status);
    ASSERT(IsListEmpty(&s_blob_user_processes));
}

NTSTATUS VioGpuMapBlobUserView(VIOGPU_BLOB_USER_VIEW *view, PEPROCESS owner,
                             PHYSICAL_ADDRESS pa, SIZE_T length,
                             MEMORY_CACHING_TYPE cache_type)
{
    PAGED_CODE();
    /* Escapes create views only in their owning process. This also prevents
     * publication from another process after the owner's exit notification. */
    if (!owner || owner != PsGetCurrentProcess() ||
        PsGetCurrentProcessId() != PsGetProcessId(owner))
        return STATUS_INVALID_DEVICE_STATE;
    if (!view || view->Owner || view->Mdl || view->UserVa || view->Registered || !length ||
        length > MAXULONG || (length & (PAGE_SIZE - 1)) ||
        (pa.QuadPart & (PAGE_SIZE - 1)) || pa.QuadPart < 0 ||
        (ULONGLONG)pa.QuadPart > (ULONGLONG)MAXLONGLONG - length)
        return STATUS_INVALID_PARAMETER;

    MM_PHYSICAL_ADDRESS_LIST range = {};
    range.PhysicalAddress = pa;
    range.NumberOfBytes = length;
    PMDL mdl = NULL;
    NTSTATUS status = MmAllocateMdlForIoSpace(&range, 1, &mdl);
    if (!NT_SUCCESS(status))
        return status;

    VIOGPU_BLOB_USER_PROCESS *process = VioGpuGetBlobUserProcess(owner);
    if (!process)
    {
        IoFreeMdl(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    PVOID user_va = NULL;
    KeWaitForSingleObject(&process->Mutex, Executive, KernelMode, FALSE, NULL);
    if (process->Exiting)
        status = STATUS_PROCESS_IS_TERMINATING;
    else
    {
        __try
        {
            user_va = MmMapLockedPagesSpecifyCache(mdl, UserMode, cache_type, NULL,
                                                  FALSE, NormalPagePriority);
            if (!user_va)
                status = STATUS_INSUFFICIENT_RESOURCES;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            status = GetExceptionCode();
        }
    }
    if (NT_SUCCESS(status) && !user_va)
        status = STATUS_INSUFFICIENT_RESOURCES;
    if (NT_SUCCESS(status))
    {
        view->Owner = process;
        view->Mdl = mdl;
        view->UserVa = user_va;
        InsertTailList(&process->Views, &view->Entry);
        view->Registered = TRUE;
    }
    KeReleaseMutex(&process->Mutex, FALSE);
    if (!NT_SUCCESS(status))
    {
        IoFreeMdl(mdl);
        VioGpuDereferenceBlobUserProcess(process);
    }
    return status;
}

void VioGpuUnmapBlobUserView(VIOGPU_BLOB_USER_VIEW *view)
{
    PAGED_CODE();
    if (!view || !view->Owner)
        return;
    VIOGPU_BLOB_USER_PROCESS *owner = view->Owner;
    KeWaitForSingleObject(&owner->Mutex, Executive, KernelMode, FALSE, NULL);
    VioGpuReleaseBlobUserViewLocked(view);
    view->Owner = NULL;
    KeReleaseMutex(&owner->Mutex, FALSE);
    VioGpuDereferenceBlobUserProcess(owner);
}
