/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */
#pragma once

#include <ntddk.h>

struct VIOGPU_BLOB_USER_PROCESS;

/* A view owns a process-context reference until explicit unmap, including
 * after process exit revokes the mapping. Allocation callers serialize map
 * and explicit unmap; the owner's gate also serializes exit revocation. */
struct VIOGPU_BLOB_USER_VIEW
{
    LIST_ENTRY Entry;
    VIOGPU_BLOB_USER_PROCESS *Owner;
    PMDL Mdl;
    PVOID UserVa;
    BOOLEAN Registered;
};

NTSTATUS VioGpuInitializeBlobUserViews();
void VioGpuShutdownBlobUserViews();
NTSTATUS VioGpuMapBlobUserView(VIOGPU_BLOB_USER_VIEW *view, PEPROCESS owner,
                             PHYSICAL_ADDRESS pa, SIZE_T length,
                             MEMORY_CACHING_TYPE cache_type);
void VioGpuUnmapBlobUserView(VIOGPU_BLOB_USER_VIEW *view);
