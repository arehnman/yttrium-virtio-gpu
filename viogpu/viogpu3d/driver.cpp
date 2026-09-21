/*
 * Copyright 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0 
 *
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

#include "driver.h"
#include "helper.h"
#include "baseobj.h"
#include "viogpu_adapter.h"
#include "viogpu_device.h"
#include "driver.tmh"

#if DBG
#undef DbgPrint
#define DbgPrint(Level, MSG) VIOGPU_DEBUG_PRINT(Level, MSG)
#endif

static PDRIVER_OBJECT g_VioGpu3DDriverObject;

#pragma code_seg(push)
#pragma code_seg("INIT")

int nDebugLevel;
int virtioDebugLevel;
int bDebugPrint;
int bBreakAlways;

tDebugPrintFunc VirtioDebugPrintProc;

#ifdef DBG
void InitializeDebugPrints(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    bDebugPrint = 0;
    virtioDebugLevel = 0;
    nDebugLevel = TRACE_LEVEL_NONE;
    bBreakAlways = 0;

    bDebugPrint = 1;
    virtioDebugLevel = 0x5;
    bBreakAlways = 1;
    nDebugLevel = TRACE_LEVEL_WARNING;
#if defined(COM_DEBUG)
    VirtioDebugPrintProc = DebugPrintFuncSerial;
#elif defined(PRINT_DEBUG)
    VirtioDebugPrintProc = DebugPrintFuncKdPrint;
#endif
}
#endif

#include <ntddk.h>
#include "viogpu_device.h"

#pragma code_seg(push)
#pragma code_seg("PAGE")
extern "C" NTSTATUS DriverEntry(_In_ DRIVER_OBJECT *pDriverObject, _In_ UNICODE_STRING *pRegistryPath)
{
    PAGED_CODE();
    g_VioGpu3DDriverObject = pDriverObject;
    VIOGPU_TRACE_INIT(pDriverObject, pRegistryPath);
    WPP_INIT_TRACING(pDriverObject, pRegistryPath);
    DbgPrint(TRACE_LEVEL_FATAL, ("---> VIOGPU FULL build on on %s %s\n", __DATE__, __TIME__));
    DRIVER_INITIALIZATION_DATA InitialData = {0};

    /*
     * This, not the WDDMVersion capability, is what selects the DDI contract
     * dxgkrnl holds the driver to.  While it said WDDM1_3 the 2.0 paths were
     * never taken at all: no WDDM 2.0 entry point was called, no WDDM 2.0
     * adapter query was asked, and D3DKMTCreatePagingQueue simply returned
     * STATUS_NOT_IMPLEMENTED, which is what D3D12 needs.
     */
#if VIOGPU_WDDM2
    InitialData.Version = DXGKDDI_INTERFACE_VERSION_WDDM2_0;
#else
    InitialData.Version = DXGKDDI_INTERFACE_VERSION_WDDM1_3;
#endif

    InitialData.DxgkDdiAddDevice = VioGpu3DAddDevice;
    InitialData.DxgkDdiStartDevice = VioGpu3DStartDevice;
    InitialData.DxgkDdiStopDevice = VioGpu3DStopDevice;
    InitialData.DxgkDdiRemoveDevice = VioGpu3DRemoveDevice;

    InitialData.DxgkDdiDispatchIoRequest = VioGpu3DDispatchIoRequest;
    InitialData.DxgkDdiInterruptRoutine = VioGpu3DInterruptRoutine;
    InitialData.DxgkDdiDpcRoutine = VioGpu3DDpcRoutine;

    InitialData.DxgkDdiQueryChildRelations = VioGpu3DQueryChildRelations;
    InitialData.DxgkDdiQueryChildStatus = VioGpu3DQueryChildStatus;
    InitialData.DxgkDdiQueryDeviceDescriptor = VioGpu3DQueryDeviceDescriptor;
    InitialData.DxgkDdiSetPowerState = VioGpu3DSetPowerState;
    InitialData.DxgkDdiResetDevice = VioGpu3DResetDevice;
    InitialData.DxgkDdiUnload = VioGpu3DUnload;

    InitialData.DxgkDdiQueryAdapterInfo = VioGpu3DQueryAdapterInfo;
    InitialData.DxgkDdiEscape = VioGpu3DEscape;
    InitialData.DxgkDdiCreateAllocation = VioGpu3DCreateAllocation;
    InitialData.DxgkDdiOpenAllocation = VioGpu3DOpenAllocation;
    InitialData.DxgkDdiCloseAllocation = VioGpu3DCloseAllocation;
    InitialData.DxgkDdiDescribeAllocation = VioGpu3DDescribeAllocation;
    InitialData.DxgkDdiDestroyAllocation = VioGpu3DDestroyAllocation;
    InitialData.DxgkDdiGetStandardAllocationDriverData = VioGpu3DGetStandardAllocationDriverData;
    InitialData.DxgkDdiBuildPagingBuffer = VioGpu3DBuildPagingBuffer;

    InitialData.DxgkDdiCreateContext = VioGpu3DDdiCreateContext;
    InitialData.DxgkDdiDestroyContext = VioGpu3DDdiDestroyContext;

    InitialData.DxgkDdiPresent = VioGpu3DPresent;
    InitialData.DxgkDdiRender = VioGpu3DRender;
    InitialData.DxgkDdiPatch = VioGpu3DPatch;
    InitialData.DxgkDdiSubmitCommand = VioGpu3DSubmitCommand;

    InitialData.DxgkDdiSetPointerPosition = VioGpu3DSetPointerPosition;
    InitialData.DxgkDdiSetPointerShape = VioGpu3DSetPointerShape;
    InitialData.DxgkDdiIsSupportedVidPn = VioGpu3DIsSupportedVidPn;
    InitialData.DxgkDdiRecommendFunctionalVidPn = VioGpu3DRecommendFunctionalVidPn;
    InitialData.DxgkDdiEnumVidPnCofuncModality = VioGpu3DEnumVidPnCofuncModality;
    InitialData.DxgkDdiSetVidPnSourceVisibility = VioGpu3DSetVidPnSourceVisibility;
    InitialData.DxgkDdiCommitVidPn = VioGpu3DCommitVidPn;
    InitialData.DxgkDdiUpdateActiveVidPnPresentPath = VioGpu3DUpdateActiveVidPnPresentPath;
    InitialData.DxgkDdiSetVidPnSourceAddress = VioGpu3DSetVidPnSourceAddress;
    InitialData.DxgkDdiRecommendMonitorModes = VioGpu3DRecommendMonitorModes;
    InitialData.DxgkDdiQueryVidPnHWCapability = VioGpu3DQueryVidPnHWCapability;
    InitialData.DxgkDdiSystemDisplayEnable = VioGpu3DSystemDisplayEnable;
    InitialData.DxgkDdiSystemDisplayWrite = VioGpu3DSystemDisplayWrite;

    InitialData.DxgkDdiStopDeviceAndReleasePostDisplayOwnership = VioGpu3DStopDeviceAndReleasePostDisplayOwnership;

    InitialData.DxgkDdiCreateDevice = VioGpu3DCreateDevice;
    InitialData.DxgkDdiDestroyDevice = VioGpu3DDestroyDevice;

    InitialData.DxgkDdiPreemptCommand = VioGpu3DDdiPreemptCommand;
    InitialData.DxgkDdiResetFromTimeout = VioGpu3DDdiResetFromTimeout;
    InitialData.DxgkDdiRestartFromTimeout = VioGpu3DDdiRestartFromTimeout;
    InitialData.DxgkDdiCollectDbgInfo = VioGpu3DDdiCollectDbgInfo;
    InitialData.DxgkDdiQueryCurrentFence = VioGpu3DDdiQueryCurrentFence;

    InitialData.DxgkDdiQueryEngineStatus = VioGpu3DDdiQueryEngineStatus;
    InitialData.DxgkDdiResetEngine = VioGpu3DDdiResetEngine;
    InitialData.DxgkDdiCancelCommand = VioGpu3DDdiCancelCommand;

    InitialData.DxgkDdiGetNodeMetadata = VioGpu3DDdiGetNodeMetadata;

#if VIOGPU_WDDM2
    /* WDDM 2.0: present so dxgkrnl has something to call.  See their
     * definitions - they exist to find out what it actually requires. */
    InitialData.DxgkDdiCreateProcess = VioGpu3DDdiCreateProcess;
    InitialData.DxgkDdiDestroyProcess = VioGpu3DDdiDestroyProcess;
    InitialData.DxgkDdiSubmitCommandVirtual = VioGpu3DDdiSubmitCommandVirtual;

    // Keep each callback's exact DDI signature, including on x86.
    InitialData.DxgkDdiRenderGdi = VioGpu3DDdiRenderGdi;
    /* Required at DDI 0x5008 and above; a null here fails the whole adapter
     * with STATUS_INVALID_PARAMETER and watchdog error 1DD6. */
    InitialData.DxgkDdiCalibrateGpuClock = VioGpu3DDdiCalibrateGpuClock;
    InitialData.DxgkDdiSetRootPageTable = VioGpu3DDdiSetRootPageTable;
    InitialData.DxgkDdiGetRootPageTableSize = VioGpu3DDdiGetRootPageTableSize;
    InitialData.DxgkDdiMapCpuHostAperture = VioGpu3DDdiMapCpuHostAperture;
    InitialData.DxgkDdiUnmapCpuHostAperture = VioGpu3DDdiUnmapCpuHostAperture;
    InitialData.DxgkDdiSetStablePowerState = VioGpu3DDdiSetStablePowerState;
#endif
    InitialData.DxgkDdiControlInterrupt = VioGpu3DDdiControlInterrupt;
    InitialData.DxgkDdiGetScanLine = VioGpu3DDdiGetScanLine;

    NTSTATUS Status = DxgkInitialize(pDriverObject, pRegistryPath, &InitialData);

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("DxgkInitialize failed with Status: 0x%X\n", Status));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}
// END: Init Code
#pragma code_seg(pop)

#pragma code_seg(push)
#pragma code_seg("PAGE")

//
// PnP DDIs
//

VOID VioGpu3DUnload(VOID)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_INFORMATION, ("<--> %s\n", __FUNCTION__));
    VIOGPU_TRACE_CLEANUP(NULL);
    if (g_VioGpu3DDriverObject)
    {
        WPP_CLEANUP(g_VioGpu3DDriverObject);
        g_VioGpu3DDriverObject = NULL;
    }
}

NTSTATUS
VioGpu3DAddDevice(_In_ DEVICE_OBJECT *pPhysicalDeviceObject, _Outptr_ PVOID *ppDeviceContext)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    if ((pPhysicalDeviceObject == NULL) || (ppDeviceContext == NULL))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("One of pPhysicalDeviceObject (%p), ppDeviceContext (%p) is NULL",
                  pPhysicalDeviceObject,
                  ppDeviceContext));
        return STATUS_INVALID_PARAMETER;
    }
    *ppDeviceContext = NULL;

    VioGpuAdapter *pAdapter = new (NonPagedPoolNx) VioGpuAdapter(pPhysicalDeviceObject);
    if (pAdapter == NULL)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("pAdapter failed to be allocated"));
        return STATUS_NO_MEMORY;
    }

    *ppDeviceContext = pAdapter;

    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s ppDeviceContext = %p\n", __FUNCTION__, pAdapter));
    return STATUS_SUCCESS;
}

NTSTATUS
VioGpu3DRemoveDevice(_In_ VOID *pDeviceContext)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_FATAL, ("---> %s 0x%p\n", __FUNCTION__, pDeviceContext));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(pDeviceContext);

    if (pAdapter)
    {
        delete pAdapter;
    }

    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS
VioGpu3DStartDevice(_In_ VOID *pDeviceContext,
                    _In_ DXGK_START_INFO *pDxgkStartInfo,
                    _In_ DXGKRNL_INTERFACE *pDxgkInterface,
                    _Out_ ULONG *pNumberOfViews,
                    _Out_ ULONG *pNumberOfChildren)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(pDeviceContext != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(pDeviceContext);
    return pAdapter->StartDevice(pDxgkStartInfo, pDxgkInterface, pNumberOfViews, pNumberOfChildren);
}

NTSTATUS
VioGpu3DStopDevice(_In_ VOID *pDeviceContext)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(pDeviceContext != NULL);
    DbgPrint(TRACE_LEVEL_INFORMATION, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(pDeviceContext);
    return pAdapter->StopDevice();
}

NTSTATUS
VioGpu3DDispatchIoRequest(_In_ VOID *pDeviceContext,
                          _In_ ULONG VidPnSourceId,
                          _In_ VIDEO_REQUEST_PACKET *pVideoRequestPacket)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(pDeviceContext != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(pDeviceContext);
    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VioGpuAdapter (0x%I64x) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->DispatchIoRequest(VidPnSourceId, pVideoRequestPacket);
}

NTSTATUS
VioGpu3DSetPowerState(_In_ VOID *pDeviceContext,
                      _In_ ULONG HardwareUid,
                      _In_ DEVICE_POWER_STATE DevicePowerState,
                      _In_ POWER_ACTION ActionType)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(pDeviceContext != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(pDeviceContext);
    if (!pAdapter->IsDriverActive())
    {
        return STATUS_SUCCESS;
    }
    return pAdapter->SetPowerState(HardwareUid, DevicePowerState, ActionType);
}

NTSTATUS
VioGpu3DQueryChildRelations(_In_ VOID *pDeviceContext,
                            _Out_writes_bytes_(ChildRelationsSize) DXGK_CHILD_DESCRIPTOR *pChildRelations,
                            _In_ ULONG ChildRelationsSize)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(pDeviceContext != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(pDeviceContext);
    return pAdapter->QueryChildRelations(pChildRelations, ChildRelationsSize);
}

NTSTATUS
VioGpu3DQueryChildStatus(_In_ VOID *pDeviceContext,
                         _Inout_ DXGK_CHILD_STATUS *pChildStatus,
                         _In_ BOOLEAN NonDestructiveOnly)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(pDeviceContext != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(pDeviceContext);
    return pAdapter->QueryChildStatus(pChildStatus, NonDestructiveOnly);
}

NTSTATUS
VioGpu3DQueryDeviceDescriptor(_In_ VOID *pDeviceContext,
                              _In_ ULONG ChildUid,
                              _Inout_ DXGK_DEVICE_DESCRIPTOR *pDeviceDescriptor)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(pDeviceContext != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(pDeviceContext);
    if (!pAdapter->IsDriverActive())
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("VIOGPU (%p) is being called when not active!", pAdapter));
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->QueryDeviceDescriptor(ChildUid, pDeviceDescriptor);
}

NTSTATUS
APIENTRY
VioGpu3DQueryAdapterInfo(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_QUERYADAPTERINFO *pQueryAdapterInfo)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);
    return pAdapter->QueryAdapterInfo(pQueryAdapterInfo);
}

#if VIOGPU_WDDM2
// These entry points remain unsupported. Use separate, correctly typed
// callbacks so their names identify the missing operation in diagnostics.

NTSTATUS
APIENTRY
VioGpu3DDdiRenderGdi(_In_ CONST HANDLE hContext, _Inout_ DXGKARG_RENDERGDI *pRenderGdi)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hContext);
    UNREFERENCED_PARAMETER(pRenderGdi);
    DbgPrint(TRACE_LEVEL_ERROR, ("%s unsupported operation owner=viogpu3d\n", __FUNCTION__));
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
APIENTRY
VioGpu3DDdiMapCpuHostAperture(_In_ CONST HANDLE hAdapter,
                            _In_ CONST DXGKARG_MAPCPUHOSTAPERTURE *pArgs)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint(TRACE_LEVEL_ERROR, ("%s unsupported operation owner=viogpu3d\n", __FUNCTION__));
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
APIENTRY
VioGpu3DDdiUnmapCpuHostAperture(_In_ CONST HANDLE hAdapter,
                              _In_ CONST DXGKARG_UNMAPCPUHOSTAPERTURE *pArgs)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint(TRACE_LEVEL_ERROR, ("%s unsupported operation owner=viogpu3d\n", __FUNCTION__));
    return STATUS_NOT_IMPLEMENTED;
}

VOID
APIENTRY
VioGpu3DDdiSetStablePowerState(_In_ CONST HANDLE hAdapter,
                             _In_ CONST DXGKARG_SETSTABLEPOWERSTATE *pArgs)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint(TRACE_LEVEL_ERROR, ("%s unsupported operation owner=viogpu3d\n", __FUNCTION__));
}

/*
 * Called only when GPUMMUCAPS::PageTableLevelCount is two, which is what this
 * driver reports, so the shared stub was never right here: the argument is
 * in/out and the return value carries the size.  Round the requested entry
 * count up to a whole page of driver-format entries, because the root table lives
 * in a memory segment and has to be a whole number of that segment's pages.
 */
SIZE_T
APIENTRY
VioGpu3DDdiGetRootPageTableSize(_In_ CONST HANDLE hAdapter,
                                _Inout_ DXGKARG_GETROOTPAGETABLESIZE *pArgs)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(hAdapter);

    if (pArgs == NULL)
    {
        return 0;
    }

    C_ASSERT(PAGE_SIZE % sizeof(VIOGPU_PAGE_TABLE_ENTRY) == 0);
    const UINT PtesPerPage = PAGE_SIZE / sizeof(VIOGPU_PAGE_TABLE_ENTRY);
    UINT Pages = (pArgs->NumberOfPte + PtesPerPage - 1) / PtesPerPage;

    if (Pages == 0)
    {
        Pages = 1;
    }

    pArgs->NumberOfPte = Pages * PtesPerPage;

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("<---> %s: adapter %u, %u entries in %u bytes\n", __FUNCTION__,
              pArgs->PhysicalAdapterIndex, pArgs->NumberOfPte, Pages * PAGE_SIZE));

    return (SIZE_T)Pages * PAGE_SIZE;
}

/*
 * There is no register to point at a root page table on this device, so noting
 * the call is all this can do.  The entries it describes are written by VidMm
 * itself, because GPUMMUCAPS asks for CPU_VIRTUAL update mode.
 */
VOID
APIENTRY
VioGpu3DDdiSetRootPageTable(_In_ CONST HANDLE hAdapter,
                            _In_ CONST DXGKARG_SETROOTPAGETABLE *pSetPageTable)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(hAdapter);

    if (pSetPageTable == NULL)
    {
        return;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("<---> %s: context %p, %u entries at segment %u offset %I64x\n",
              __FUNCTION__, pSetPageTable->hContext, pSetPageTable->NumEntries,
              pSetPageTable->Address.SegmentId, pSetPageTable->Address.SegmentOffset));
}

/*
 * The one DDI dxgkrnl refuses to start a WDDM 2.0 adapter without.
 *
 * DXGADAPTER::Initialize checks, for a dxgmms2 adapter declaring DDI
 * version 0x5008 or later, that both DxgkDdiCalibrateGpuClock and
 * DxgkDdiSetStablePowerState are present; a null in either logs watchdog
 * error 1DD6 and fails the whole adapter with STATUS_INVALID_PARAMETER,
 * without ever calling the driver or naming what it wanted.  WDDM 1.3 is
 * DDI 0x4002, below the threshold, which is why the same driver starts
 * fine there.
 *
 * There is no GPU clock to read: work is timed on the CPU side and handed
 * to the host, so the performance counter is the only clock this device
 * has.  Reporting it as both timebases is the truthful answer here - the
 * two counters then advance together, which is what a shared timebase
 * means, rather than inventing a GPU frequency.
 *
 * Callable at DISPATCH_LEVEL, so this must not be pageable.
 */
#pragma code_seg(push)
#pragma code_seg()

NTSTATUS
APIENTRY
VioGpu3DDdiCalibrateGpuClock(_In_ CONST HANDLE hAdapter,
                             UINT32 NodeOrdinal,
                             UINT32 EngineOrdinal,
                             _Out_ DXGKARG_CALIBRATEGPUCLOCK *pClockCalibration)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(NodeOrdinal);
    UNREFERENCED_PARAMETER(EngineOrdinal);

    if (pClockCalibration == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    LARGE_INTEGER Frequency;
    LARGE_INTEGER Counter = KeQueryPerformanceCounter(&Frequency);

    RtlZeroMemory(pClockCalibration, sizeof(*pClockCalibration));
    pClockCalibration->GpuFrequency = (ULONGLONG)Frequency.QuadPart;
    pClockCalibration->GpuClockCounter = (ULONGLONG)Counter.QuadPart;
    pClockCalibration->CpuClockCounter = (ULONGLONG)Counter.QuadPart;

    return STATUS_SUCCESS;
}

#pragma code_seg(pop)

NTSTATUS
APIENTRY
VioGpu3DDdiCreateProcess(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_CREATEPROCESS *pArgs)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s\n", __FUNCTION__));

    UNREFERENCED_PARAMETER(hAdapter);

    if (pArgs == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* A handle dxgkrnl only ever hands back to us; nothing is behind it. */
    pArgs->hKmdProcess = (HANDLE)(ULONG_PTR)1;

    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
VioGpu3DDdiDestroyProcess(_In_ CONST HANDLE hAdapter, _In_ CONST HANDLE hKmdProcess)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s\n", __FUNCTION__));

    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(hKmdProcess);

    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
VioGpu3DDdiSubmitCommandVirtual(_In_ CONST HANDLE hAdapter,
                                _In_ CONST DXGKARG_SUBMITCOMMANDVIRTUAL *pSubmitCommand)
{
    DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s\n", __FUNCTION__));

    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pSubmitCommand);

    /* Submission still goes the way it always has; reaching here would mean
     * the scheduler took a path this driver does not implement. */
    return STATUS_NOT_IMPLEMENTED;
}
#endif

NTSTATUS
APIENTRY
VioGpu3DDdiGetNodeMetadata(_In_ CONST HANDLE hAdapter,
                           UINT NodeOrdinal,
                           _Out_ DXGKARG_GETNODEMETADATA *pGetNodeMetadata)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(NodeOrdinal);

    pGetNodeMetadata->EngineType = DXGK_ENGINE_TYPE_3D;
    pGetNodeMetadata->Flags.Value = 0;

    return STATUS_SUCCESS;
};

NTSTATUS
APIENTRY
VioGpu3DSetPointerPosition(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_SETPOINTERPOSITION *pSetPointerPosition)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    UNREFERENCED_PARAMETER(pSetPointerPosition);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);
    if (!pAdapter->IsDriverActive())
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("VioGpu (%p) is being called when not active!", pAdapter));
        VioGpuDbgBreak();
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
APIENTRY
VioGpu3DSetPointerShape(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    UNREFERENCED_PARAMETER(pSetPointerShape);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);
    if (!pAdapter->IsDriverActive())
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<---> %s VioGpu (%p) is being called when not active!\n", __FUNCTION__, pAdapter));
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
APIENTRY
VioGpu3DEscape(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_ESCAPE *pEscape)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);
    if (!pAdapter->IsDriverActive())
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<---> %s VioGpu (%p) is being called when not active!\n", __FUNCTION__, pAdapter));
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->Escape(pEscape);
}

NTSTATUS
APIENTRY
VioGpu3DCreateAllocation(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_CREATEALLOCATION *pCreateAllocation)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);
    if (!pAdapter->IsDriverActive())
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<---> %s VioGpu (%p) is being called when not active!\n", __FUNCTION__, pAdapter));
        return STATUS_UNSUCCESSFUL;
    }
    return VioGpuAllocation::DxgkCreateAllocation(pAdapter, pCreateAllocation);
}

NTSTATUS
APIENTRY
VioGpu3DDescribeAllocation(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_DESCRIBEALLOCATION *pDescribeAllocation)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAllocation *pAllocation = reinterpret_cast<VioGpuAllocation *>(pDescribeAllocation->hAllocation);
    VIOGPU_ASSERT_CHK(pAllocation != NULL);

    return pAllocation->DescribeAllocation(pDescribeAllocation);
}

NTSTATUS
APIENTRY
VioGpu3DOpenAllocation(_In_ CONST HANDLE hDevice, _In_ CONST DXGKARG_OPENALLOCATION *pOpenAllocation)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hDevice != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuDevice *pDxContext = reinterpret_cast<VioGpuDevice *>(hDevice);
    return pDxContext->OpenAllocation(pOpenAllocation);
}

NTSTATUS
APIENTRY
VioGpu3DCloseAllocation(_In_ CONST HANDLE hDevice, _In_ CONST DXGKARG_CLOSEALLOCATION *pCloseAllocation)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hDevice);
    VIOGPU_ASSERT_CHK(hDevice != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    for (ULONG i = 0; i < pCloseAllocation->NumAllocations; i++)
    {
        VioGpuDeviceAllocation *allocation = reinterpret_cast<VioGpuDeviceAllocation *>(pCloseAllocation->pOpenHandleList[i]);
        if (allocation != NULL)
        {
            delete allocation;
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
VioGpu3DDestroyAllocation(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_DESTROYALLOCATION *pDestroyAllocation)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    VIOGPU_ASSERT_CHK(pDestroyAllocation != NULL);

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("<---> %s num=%u adapter=%p\n",
              __FUNCTION__, pDestroyAllocation->NumAllocations, hAdapter));

    for (ULONG i = 0; i < pDestroyAllocation->NumAllocations; i++)
    {
        VioGpuAllocation *allocation = reinterpret_cast<VioGpuAllocation *>(pDestroyAllocation->pAllocationList[i]);
        if (allocation != NULL)
        {
            VIOGPU_RES_BUSY_REQ resBusy;
            resBusy.Wait = 1;
            allocation->EscapeResourceBusy(&resBusy);
            delete allocation;
        }
    }

    // No standalone VioGpuResource allocation exists. hResource aliases allocation state.
    // Resource cleanup is covered by per-allocation deletes above.

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s \n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
VioGpu3DGetStandardAllocationDriverData(_In_ CONST HANDLE hAdapter,
                                        _Inout_ DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *pStandardAllocation)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));
    return VioGpuAllocation::GetStandardAllocationDriverData(pStandardAllocation);
}

NTSTATUS
APIENTRY
VioGpu3DBuildPagingBuffer(_In_ CONST HANDLE hAdapter, _In_ DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    VIOGPU_ASSERT(pBuildPagingBuffer != NULL);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s operation=%d\n", __FUNCTION__, pBuildPagingBuffer->Operation));

    switch (pBuildPagingBuffer->Operation)
    {
        case DXGK_OPERATION_MAP_APERTURE_SEGMENT:
            {
                if (pBuildPagingBuffer->MapApertureSegment.hAllocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("<--- %s (map aperture segment) no allocation specified\n", __FUNCTION__));
                    return STATUS_SUCCESS;
                }

                VioGpuAllocation *allocation = reinterpret_cast<VioGpuAllocation *>(pBuildPagingBuffer->MapApertureSegment.hAllocation);
                NTSTATUS Status = allocation->MapApertureSegment(pBuildPagingBuffer);
                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s (map aperture segment)\n", __FUNCTION__));
                return Status;
            }
        case DXGK_OPERATION_UNMAP_APERTURE_SEGMENT:
            {
                if (pBuildPagingBuffer->UnmapApertureSegment.hAllocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("<--- %s (map aperture segment) no allocation specified\n", __FUNCTION__));
                    return STATUS_SUCCESS;
                }

                VioGpuAllocation *allocation = reinterpret_cast<VioGpuAllocation *>(pBuildPagingBuffer->UnmapApertureSegment.hAllocation);
                NTSTATUS Status = allocation->UnmapApertureSegment(pBuildPagingBuffer);
                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s (unmap aperture segment)\n", __FUNCTION__));
                return Status;
            }
#if VIOGPU_WDDM2
        case DXGK_OPERATION_UPDATE_PAGE_TABLE:
            {
                /*
                 * GPUMMUCAPS asks for CPU_VIRTUAL updates, so VidMm hands over
                 * a CPU mapping of the page table and the driver writes the
                 * entries itself.  The current driver format stores the DDI
                 * entries verbatim; VidMm treats that format as opaque.  Its
                 * mapped allocation is sized from PAGETABLELEVELDESC, so the
                 * advertised size and this write stride must agree.
                 */
                DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *pUpdate =
                    &pBuildPagingBuffer->UpdatePageTable;

                if (pUpdate->UpdateMode != DXGK_PAGETABLEUPDATE_CPU_VIRTUAL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("<--- %s (update page table) mode %d was not asked for\n",
                              __FUNCTION__, pUpdate->UpdateMode));
                    return STATUS_NOT_SUPPORTED;
                }

                if (pUpdate->Flags.Use64KBPages)
                {
                    /* Never advertised, so say so rather than guess a layout. */
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("<--- %s (update page table) 64KB pages are not supported\n",
                              __FUNCTION__));
                    return STATUS_NOT_SUPPORTED;
                }

                if (pUpdate->PageTableLevel >= VIOGPU_PAGE_TABLE_LEVEL_COUNT ||
                    pUpdate->StartIndex > VIOGPU_PAGE_TABLE_ENTRY_COUNT ||
                    pUpdate->NumPageTableEntries >
                        VIOGPU_PAGE_TABLE_ENTRY_COUNT - pUpdate->StartIndex)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("<--- %s (update page table) invalid range level=%u start=%u count=%u\n",
                              __FUNCTION__, pUpdate->PageTableLevel,
                              pUpdate->StartIndex, pUpdate->NumPageTableEntries));
                    return STATUS_INVALID_PARAMETER;
                }

                if (pUpdate->NumPageTableEntries == 0)
                {
                    return STATUS_SUCCESS;
                }

                VIOGPU_PAGE_TABLE_ENTRY *pDest =
                    (VIOGPU_PAGE_TABLE_ENTRY *)pUpdate->PageTableAddress.CpuVirtual;
                const DXGK_PTE *pSrc = pUpdate->pPageTableEntries;

                if (pDest == NULL || pSrc == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("<--- %s (update page table) missing %s\n", __FUNCTION__,
                              pDest == NULL ? "destination" : "entries"));
                    return STATUS_INVALID_PARAMETER;
                }

                pDest += pUpdate->StartIndex;

                if (pUpdate->Flags.Repeat)
                {
                    /* One entry stamped across the range, which is how VidMm
                     * fills or clears a span of addresses. */
                    for (UINT i = 0; i < pUpdate->NumPageTableEntries; i++)
                    {
                        pDest[i] = *pSrc;
                    }
                }
                else
                {
                    RtlCopyMemory(pDest, pSrc,
                                  pUpdate->NumPageTableEntries * sizeof(VIOGPU_PAGE_TABLE_ENTRY));
                }

                return STATUS_SUCCESS;
            }
        case DXGK_OPERATION_FLUSH_TLB:
            {
                /* There is no GPU MMU behind these tables, so nothing caches
                 * translations and there is nothing to flush. */
                return STATUS_SUCCESS;
            }
#endif
        default:
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("<--- %s (unknown operation %d)\n", __FUNCTION__, pBuildPagingBuffer->Operation));
                return STATUS_NOT_SUPPORTED;
            }
    };
}

NTSTATUS
APIENTRY
VioGpu3DPatch(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_PATCH *pPatch)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);
    if (!pAdapter->IsDriverActive())
    {
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->commander.Patch(pPatch);
};

#pragma code_seg(push)
#pragma code_seg()

NTSTATUS
APIENTRY
VioGpu3DSubmitCommand(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_SUBMITCOMMAND *pSubmitCommand)
{
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);
    if (!pAdapter->IsDriverActive())
    {
        // DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s VioGpu (%p) is being called when not active!\n", __FUNCTION__,
        // pAdapter));
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->commander.SubmitCommand(pSubmitCommand);
};

#pragma code_seg(pop)

NTSTATUS
APIENTRY
VioGpu3DCreateDevice(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_CREATEDEVICE *pCreateDevice)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);
    if (!pAdapter->IsDriverActive())
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<---> %s VioGpu (%p) is being called when not active!\n", __FUNCTION__, pAdapter));
        return STATUS_UNSUCCESSFUL;
    }

    pCreateDevice->hDevice = new (NonPagedPoolNx) VioGpuDevice(pAdapter);
    if (!pCreateDevice->hDevice)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s failed to allocate VioGpuDevice\n", __FUNCTION__));
        return STATUS_NO_MEMORY;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
VioGpu3DDestroyDevice(_In_ VOID *pDeviceContext)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_FATAL, ("---> %s pDeviceContext=%p\n", __FUNCTION__, pDeviceContext));

    VioGpuDevice *pDxContext = reinterpret_cast<VioGpuDevice *>(pDeviceContext);

    if (pDxContext)
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("%s destroying ctx_id=%u owner_pid=%p owner_process=%p\n",
                  __FUNCTION__,
                  pDxContext->GetId(),
                  pDxContext->GetOwnerProcessId(),
                  pDxContext->GetOwnerProcess()));
        delete pDxContext;
    }

    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
VioGpu3DDdiCreateContext(_In_ CONST HANDLE hDevice, _Inout_ DXGKARG_CREATECONTEXT *pCreateContext)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

#if VIOGPU_WDDM2
    /* The current UMD uses physical submissions. Reject incompatible virtual
     * contexts before they can reach SubmitCommandVirtual or RenderGdi.
     * Preserve VidMm's system paging context: CPU_VIRTUAL page-table updates
     * are performed immediately and the supported paging operations emit no
     * DMA commands. Virtual submission and fence completion still need a
     * real implementation before accepting application virtual contexts. */
    if (pCreateContext->Flags.VirtualAddressing && !pCreateContext->Flags.SystemContext)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s rejected virtual context owner=viogpu3d node=%u flags=0x%x "
                  "reason=virtual submission is not implemented\n",
                  __FUNCTION__, pCreateContext->NodeOrdinal, pCreateContext->Flags.Value));
        return STATUS_GRAPHICS_DRIVER_MISMATCH;
    }
#endif

    // We currently don't have sepraration between context and device
    pCreateContext->hContext = hDevice;

    pCreateContext->ContextInfo.DmaBufferSegmentSet = 0;
    pCreateContext->ContextInfo.DmaBufferSize = 256 * 1024;
    pCreateContext->ContextInfo.DmaBufferPrivateDataSize = 40;

    pCreateContext->ContextInfo.AllocationListSize = DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT;
    pCreateContext->ContextInfo.PatchLocationListSize = DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT;

    return STATUS_SUCCESS;
};

NTSTATUS
APIENTRY
VioGpu3DDdiDestroyContext(_In_ CONST HANDLE hContext)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(hContext);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    return STATUS_SUCCESS;
};

NTSTATUS
APIENTRY
VioGpu3DPresent(_In_ CONST HANDLE hDevice, _Inout_ DXGKARG_PRESENT *pPresent)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hDevice != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuDevice *pDxContext = reinterpret_cast<VioGpuDevice *>(hDevice);
    static volatile LONG presentLogCount;
    const LONG presentCount = InterlockedIncrement(&presentLogCount);
    {
        DXGK_ALLOCATIONLIST *dxgkSrc =
            pPresent && pPresent->pAllocationList ? &pPresent->pAllocationList[DXGK_PRESENT_SOURCE_INDEX] : NULL;
        DXGK_ALLOCATIONLIST *dxgkDst =
            pPresent && pPresent->pAllocationList ? &pPresent->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX] : NULL;

        DbgPrint(TRACE_LEVEL_VERBOSE,
                 ("VioGpu3DPresent entry count=%ld irql=%u hDevice=%p ctx=%u capset=%u owner=%p "
                  "flags blt=%u flip=%u flip_no_wait=%u color_fill=%u rotate=%u "
                  "dma=%p dma_size=%u alloc_list=%p src_hdev=%p dst_hdev=%p\n",
                  presentCount,
                  (UINT)KeGetCurrentIrql(),
                  hDevice,
                  pDxContext->GetId(),
                  pDxContext->GetCapsetId(),
                  pDxContext->GetOwnerProcessId(),
                  pPresent && pPresent->Flags.Blt ? 1 : 0,
                  pPresent && pPresent->Flags.Flip ? 1 : 0,
                  pPresent && pPresent->Flags.FlipWithNoWait ? 1 : 0,
                  pPresent && pPresent->Flags.ColorFill ? 1 : 0,
                  pPresent && pPresent->Flags.Rotate ? 1 : 0,
                  pPresent ? pPresent->pDmaBuffer : NULL,
                  pPresent ? pPresent->DmaSize : 0,
                  pPresent ? pPresent->pAllocationList : NULL,
                  dxgkSrc ? dxgkSrc->hDeviceSpecificAllocation : NULL,
                  dxgkDst ? dxgkDst->hDeviceSpecificAllocation : NULL));
    }

    return pDxContext->Present(pPresent);
}

NTSTATUS
APIENTRY
VioGpu3DRender(_In_ CONST HANDLE hDevice, _Inout_ DXGKARG_RENDER *pRender)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hDevice != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuDevice *pDxContext = reinterpret_cast<VioGpuDevice *>(hDevice);
    return pDxContext->Render(pRender);
}

NTSTATUS
APIENTRY
VioGpu3DStopDeviceAndReleasePostDisplayOwnership(_In_ VOID *pDeviceContext,
                                                 _In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                 _Out_ DXGK_DISPLAY_INFORMATION *DisplayInfo)
{
    PAGED_CODE();
    NTSTATUS status = STATUS_SUCCESS;
    VIOGPU_ASSERT_CHK(pDeviceContext != NULL);
    DbgPrint(TRACE_LEVEL_INFORMATION, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(pDeviceContext);
    if (pAdapter)
    {
        status = pAdapter->StopDeviceAndReleasePostDisplayOwnership(TargetId, DisplayInfo);
    }
    return status;
}

NTSTATUS
APIENTRY
VioGpu3DIsSupportedVidPn(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_ISSUPPORTEDVIDPN *pIsSupportedVidPn)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);
    if (!pAdapter->IsDriverActive())
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("VIOGPU (%p) is being called when not active!", pAdapter));
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.IsSupportedVidPn(pIsSupportedVidPn);
}

NTSTATUS
APIENTRY
VioGpu3DRecommendFunctionalVidPn(_In_ CONST HANDLE hAdapter,
                                 _In_ CONST DXGKARG_RECOMMENDFUNCTIONALVIDPN *CONST pRecommendFunctionalVidPn)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);
    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.RecommendFunctionalVidPn(pRecommendFunctionalVidPn);
}

NTSTATUS
APIENTRY
VioGpu3DRecommendVidPnTopology(_In_ CONST HANDLE hAdapter,
                               _In_ CONST DXGKARG_RECOMMENDVIDPNTOPOLOGY *CONST pRecommendVidPnTopology)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);
    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.RecommendVidPnTopology(pRecommendVidPnTopology);
}

NTSTATUS
APIENTRY
VioGpu3DRecommendMonitorModes(_In_ CONST HANDLE hAdapter,
                              _In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommendMonitorModes)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);
    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.RecommendMonitorModes(pRecommendMonitorModes);
}

NTSTATUS
APIENTRY
VioGpu3DEnumVidPnCofuncModality(_In_ CONST HANDLE hAdapter,
                                _In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY *CONST pEnumCofuncModality)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);
    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.EnumVidPnCofuncModality(pEnumCofuncModality);
}

NTSTATUS
APIENTRY
VioGpu3DSetVidPnSourceVisibility(_In_ CONST HANDLE hAdapter,
                                 _In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY *pSetVidPnSourceVisibility)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);
    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.SetVidPnSourceVisibility(pSetVidPnSourceVisibility);
}

NTSTATUS
APIENTRY
VioGpu3DCommitVidPn(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_COMMITVIDPN *CONST pCommitVidPn)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);
    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.CommitVidPn(pCommitVidPn);
}

NTSTATUS
APIENTRY
VioGpu3DUpdateActiveVidPnPresentPath(_In_ CONST HANDLE hAdapter,
                                     _In_ CONST DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *CONST pUpdateActiveVidPnPresentPath)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);
    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.UpdateActiveVidPnPresentPath(pUpdateActiveVidPnPresentPath);
}

NTSTATUS
APIENTRY
VioGpu3DQueryVidPnHWCapability(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY *pVidPnHWCaps)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);
    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.QueryVidPnHWCapability(pVidPnHWCaps);
}

NTSTATUS
APIENTRY
VioGpu3DDdiControlInterrupt(_In_ CONST HANDLE hAdapter,
                            _In_ CONST DXGK_INTERRUPT_TYPE InterruptType,
                            _In_ BOOLEAN EnableInterrupt)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hAdapter != NULL);
    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);

    if (InterruptType == DXGK_INTERRUPT_CRTC_VSYNC)
    {
        pAdapter->SetVsyncInterruptEnabled(EnableInterrupt);
    }
    return STATUS_SUCCESS;
};

NTSTATUS
APIENTRY
VioGpu3DDdiGetScanLine(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_GETSCANLINE *pGetScanLine)
{
    PAGED_CODE();
    VIOGPU_ASSERT_CHK(hAdapter != NULL);

    if (pGetScanLine == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);
    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }

    pGetScanLine->InVerticalBlank = FALSE;
    pGetScanLine->ScanLine = 0;
    return STATUS_SUCCESS;
}

// END: Paged Code
#pragma code_seg(pop)

#pragma code_seg(push)
#pragma code_seg()
// BEGIN: Non-Paged Code

VOID VioGpu3DDpcRoutine(_In_ VOID *pDeviceContext)
{
    VIOGPU_ASSERT_CHK(pDeviceContext != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(pDeviceContext);
    if (!pAdapter->IsHardwareInit())
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("VioGpu (%p) is being called when not active!", pAdapter));
        return;
    }
    pAdapter->DpcRoutine();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN
VioGpu3DInterruptRoutine(_In_ VOID *pDeviceContext, _In_ ULONG MessageNumber)
{
    VIOGPU_ASSERT_CHK(pDeviceContext != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(pDeviceContext);
    return pAdapter->InterruptRoutine(MessageNumber);
}

NTSTATUS VioGpu3DSetVidPnSourceAddress(_In_ CONST HANDLE hAdapter,
                                       _In_ CONST DXGKARG_SETVIDPNSOURCEADDRESS *pSetVidPnSourceAddress)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);
    pAdapter->vidpn.SetVidPnSourceAddress(pSetVidPnSourceAddress);

    return STATUS_SUCCESS;
}

VOID VioGpu3DResetDevice(_In_ VOID *pDeviceContext)
{
    VIOGPU_ASSERT_CHK(pDeviceContext != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(pDeviceContext);
    pAdapter->ResetDevice();
}

NTSTATUS
APIENTRY
VioGpu3DSystemDisplayEnable(_In_ VOID *pDeviceContext,
                            _In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                            _In_ PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                            _Out_ UINT *Width,
                            _Out_ UINT *Height,
                            _Out_ D3DDDIFORMAT *ColorFormat)
{
    VIOGPU_ASSERT_CHK(pDeviceContext != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(pDeviceContext);
    return pAdapter->vidpn.SystemDisplayEnable(TargetId, Flags, Width, Height, ColorFormat);
}

VOID APIENTRY VioGpu3DSystemDisplayWrite(_In_ VOID *pDeviceContext,
                                         _In_ VOID *Source,
                                         _In_ UINT SourceWidth,
                                         _In_ UINT SourceHeight,
                                         _In_ UINT SourceStride,
                                         _In_ UINT PositionX,
                                         _In_ UINT PositionY)
{
    VIOGPU_ASSERT_CHK(pDeviceContext != NULL);
    DbgPrint(TRACE_LEVEL_INFORMATION, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(pDeviceContext);
    pAdapter->vidpn.SystemDisplayWrite(Source, SourceWidth, SourceHeight, SourceStride, PositionX, PositionY);
}

NTSTATUS
APIENTRY
VioGpu3DDdiPreemptCommand(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_PREEMPTCOMMAND *pPreemptCommand)
{
    VIOGPU_ASSERT_CHK(hAdapter != NULL);

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);
    if (!pAdapter->IsDriverActive())
    {
        return STATUS_UNSUCCESSFUL;
    }

    return pAdapter->PreemptCommand(pPreemptCommand);
};

NTSTATUS
APIENTRY
VioGpu3DDdiRestartFromTimeout(_In_ CONST HANDLE hAdapter)
{
    UNREFERENCED_PARAMETER(hAdapter);
    DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s UNSUPPORTED PREEMPTION FUNCTION\n", __FUNCTION__));

    return STATUS_NOT_SUPPORTED;
};

NTSTATUS
APIENTRY
VioGpu3DDdiCancelCommand(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_CANCELCOMMAND *pCancelCommand)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pCancelCommand);

    DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s UNSUPPORTED PREEMPTION FUNCTION\n", __FUNCTION__));

    return STATUS_NOT_SUPPORTED;
};

NTSTATUS
APIENTRY
VioGpu3DDdiQueryCurrentFence(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_QUERYCURRENTFENCE *pCurrentFence)
{
    VIOGPU_ASSERT_CHK(hAdapter != NULL);

    VioGpuAdapter *pAdapter = reinterpret_cast<VioGpuAdapter *>(hAdapter);
    if (!pAdapter->IsDriverActive())
    {
        return STATUS_UNSUCCESSFUL;
    }

    return pAdapter->QueryCurrentFence(pCurrentFence);
};

NTSTATUS
APIENTRY
VioGpu3DDdiResetEngine(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_RESETENGINE *pResetEngine)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pResetEngine);

    DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s UNSUPPORTED PREEMPTION FUNCTION\n", __FUNCTION__));

    return STATUS_NOT_SUPPORTED;
};

NTSTATUS
APIENTRY
VioGpu3DDdiQueryEngineStatus(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_QUERYENGINESTATUS *pQueryEngineStatus)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pQueryEngineStatus);

    DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s UNSUPPORTED PREEMPTION FUNCTION\n", __FUNCTION__));

    return STATUS_NOT_SUPPORTED;
};

NTSTATUS
APIENTRY
VioGpu3DDdiCollectDbgInfo(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_COLLECTDBGINFO *pCollectDbgInfo)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pCollectDbgInfo);

    DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s UNSUPPORTED PREEMPTION FUNCTION\n", __FUNCTION__));

    return STATUS_NOT_SUPPORTED;
};

NTSTATUS
APIENTRY
VioGpu3DDdiResetFromTimeout(_In_ CONST HANDLE hAdapter)
{
    UNREFERENCED_PARAMETER(hAdapter);

    DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s UNSUPPORTED PREEMPTION FUNCTION\n", __FUNCTION__));

    return STATUS_NOT_SUPPORTED;
};

#if defined(DBG)

#if defined(COM_DEBUG)

#define RHEL_DEBUG_PORT  ((PUCHAR)0x3F8)
#define TEMP_BUFFER_SIZE 256

void DebugPrintFuncSerial(CONST char *format, ...)
{
    char buf[TEMP_BUFFER_SIZE];
    NTSTATUS status;
    size_t len;
    va_list list;
    va_start(list, format);
    status = RtlStringCbVPrintfA(buf, sizeof(buf), format, list);
    if (status == STATUS_SUCCESS)
    {
        len = strlen(buf);
    }
    else
    {
        len = 2;
        buf[0] = 'O';
        buf[1] = '\n';
    }
    if (len)
    {
        WRITE_PORT_BUFFER_UCHAR(RHEL_DEBUG_PORT, (PUCHAR)buf, (ULONG)len);
        WRITE_PORT_UCHAR(RHEL_DEBUG_PORT, '\r');
    }
    va_end(list);
}
#endif

#if defined(PRINT_DEBUG)
void DebugPrintFuncKdPrint(CONST char *format, ...)
{
    va_list list;
    va_start(list, format);
    vDbgPrintEx(DPFLTR_DEFAULT_ID, 9 | DPFLTR_MASK, format, list);
    va_end(list);
}
#endif

#endif
#pragma code_seg(pop) // End Non-Paged Code
