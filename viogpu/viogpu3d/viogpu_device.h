/*
 * Copyright 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 * 
 */

#pragma once
#include "helper.h"
#include "viogpum.h"

class VioGpuAdapter;
class VioGpuAllocation;
class CtrlQueue;

#define VIOGPU_CONTEXT_FLAG_CAPSET_ID_MASK 0xff
#define VIOGPU_DRM_CAPSET_VENUS 4

// Class that represents DXGKRNL Context, often passed as hContext
class VioGpuDevice
{
  public:
    VioGpuDevice(VioGpuAdapter *pAdapter);
    ~VioGpuDevice();

    NTSTATUS Init(VIOGPU_CTX_INIT_REQ *pOptions);
    NTSTATUS OpenAllocation(_In_ CONST DXGKARG_OPENALLOCATION *pOpenAllocation);

    NTSTATUS GenerateBltPresent(DXGKARG_PRESENT *pPresent, VioGpuAllocation *src, VioGpuAllocation *dst);
    NTSTATUS Present(_Inout_ DXGKARG_PRESENT *pPresent);
    NTSTATUS Render(DXGKARG_RENDER *pRender);

    ULONG GetId()
    {
        return m_id;
    }
    ULONG GetCapsetId() const
    {
        return m_capset_id;
    }
    bool IsVenusContext() const
    {
        return (m_capset_id & VIOGPU_CONTEXT_FLAG_CAPSET_ID_MASK) ==
               VIOGPU_DRM_CAPSET_VENUS;
    }
    bool IsContextCreated() const
    {
        return m_context_created;
    }

    CtrlQueue *GetCtrlQueue();
    PEPROCESS GetOwnerProcess() const
    {
        return m_owner_process;
    }
    HANDLE GetOwnerProcessId() const
    {
        return m_owner_pid;
    }
    VIOGPU_CLOCK_PUBLISHER *GetNativeClockPublisher()
    {
        return &m_clockPublisher;
    }

  private:
    VioGpuAdapter *m_pAdapter;
    ULONG m_id;
    ULONG m_capset_id;
    bool m_context_created;
    PEPROCESS m_owner_process;
    HANDLE m_owner_pid;
    VIOGPU_CLOCK_PUBLISHER m_clockPublisher;
};

class VioGpuDeviceAllocation
{
  public:
    VioGpuDeviceAllocation(VioGpuDevice *device, VioGpuAllocation *allocation);
    ~VioGpuDeviceAllocation();

    VioGpuAllocation *GetAllocation();

  private:
    VioGpuAllocation *m_pAllocation;
    VioGpuDevice *m_pDevice;
    bool m_attached;
};
