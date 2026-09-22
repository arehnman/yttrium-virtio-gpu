/*
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

#pragma once

#include <d3dkmthk.h>

#pragma pack(1)
typedef struct _VIOGPU_BOX
{
    ULONG x;
    ULONG y;
    ULONG z;
    ULONG width;
    ULONG height;
    ULONG depth;
} VIOGPU_BOX;
#pragma pack()

// ================= QueryAdapterInfo UMDRIVERPRIVATE
#define VIOGPU_IAM 0x56696f475055 // Identified for queryadapterinfo (VioGPU as hex)

typedef struct _VIOGPU_ADAPTERINFO
{
    ULONGLONG IamVioGPU; // Should be set by driver to VIOGPU_IAM
    struct
    {
        UINT Supports3d : 1;
        UINT has_capset_query_fix : 1;
        UINT has_resource_blob : 1;
        UINT has_host_visible : 1;
        UINT has_resource_assign_uuid : 1;
        UINT has_context_init : 1;
        UINT Reserved : 26;
    } Flags;
    ULONGLONG SupportedCapsetIDs;
} VIOGPU_ADAPTERINFO;

// ================= ESCAPES
#define VIOGPU_GET_DEVICE_ID         0x000
#define VIOGPU_GET_CUSTOM_RESOLUTION 0x001
#define VIOGPU_GET_CAPS              0x002

#define VIOGPU_RES_INFO              0x100
#define VIOGPU_RES_BUSY              0x101
#define VIOGPU_RES_MAP_BLOB          0x102
#define VIOGPU_RES_UNMAP_BLOB        0x103
#define VIOGPU_RES_SET_SCANOUT_BLOB  0x105
#define VIOGPU_RES_ATTACH_WAIT       0x106

#define VIOGPU_CTX_INIT              0x200
#define VIOGPU_QUERY_TIMELINE_SUBMIT 0x301
#define VIOGPU_TIMELINE_SUBMIT_VERSION 1u

#define VIOGPU_SUBMIT_CMD            0x300

#pragma pack(1)
typedef struct _VIOGPU_DISP_MODE
{
    USHORT XResolution;
    USHORT YResolution;
} VIOGPU_DISP_MODE, *PVIOGPU_DISP_MODE;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_PARAM_REQ
{
    ULONG ParamId;
    UINT64 Value;
} VIOGPU_PARAM_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_CAPSET_REQ
{
    ULONG CapsetId;
    ULONG Version;
    ULONG Size;
    ULONGLONG Capset;
} VIOGPU_CAPSET_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_RES_INFO_REQ
{
    D3DKMT_HANDLE ResHandle;
    ULONG Id;
} VIOGPU_RES_INFO_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_RES_BUSY_REQ
{
    D3DKMT_HANDLE ResHandle;
    BOOL Wait;
    BOOL IsBusy;
} VIOGPU_RES_BUSY_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_RES_MAP_BLOB_REQ
{
    D3DKMT_HANDLE ResHandle;
    ULONGLONG Offset;
    ULONGLONG Size;
    ULONGLONG UserVa;
    ULONG MapInfo;
} VIOGPU_RES_MAP_BLOB_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_RES_UNMAP_BLOB_REQ
{
    D3DKMT_HANDLE ResHandle;
} VIOGPU_RES_UNMAP_BLOB_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_RES_SET_SCANOUT_BLOB_REQ
{
    D3DKMT_HANDLE ResHandle;
    ULONG ScanoutId;
    ULONG Width;
    ULONG Height;
    ULONG X;
    ULONG Y;
    ULONG Format;
    ULONG Stride;
    ULONG Offset;
} VIOGPU_RES_SET_SCANOUT_BLOB_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_RES_ATTACH_WAIT_REQ
{
    D3DKMT_HANDLE ResHandle;
    ULONG Id;
} VIOGPU_RES_ATTACH_WAIT_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_CTX_INIT_REQ
{
    UINT CapsetID;
} VIOGPU_CTX_INIT_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_SUBMIT_CMD_REQ
{
    D3DKMT_HANDLE hContext;
    UINT CmdType;
    UINT CmdSize;
} VIOGPU_SUBMIT_CMD_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_ESCAPE
{
    USHORT Type;
    USHORT DataLength;
    union {
        ULONG Id;
        VIOGPU_DISP_MODE Resolution;
        VIOGPU_PARAM_REQ Parameter;
        VIOGPU_CAPSET_REQ Capset;

        VIOGPU_RES_INFO_REQ ResourceInfo;
        VIOGPU_RES_BUSY_REQ ResourceBusy;
        VIOGPU_RES_MAP_BLOB_REQ ResourceMapBlob;
        VIOGPU_RES_UNMAP_BLOB_REQ ResourceUnmapBlob;
        VIOGPU_RES_SET_SCANOUT_BLOB_REQ ResourceSetScanoutBlob;
        VIOGPU_RES_ATTACH_WAIT_REQ ResourceAttachWait;

        VIOGPU_CTX_INIT_REQ CtxInit;
    } DUMMYUNIONNAME;
} VIOGPU_ESCAPE, *PVIOGPU_ESCAPE;
#pragma pack()

// ================= CreateResource
#pragma pack(1)
typedef struct _VIOGPU_RESOURCE_OPTIONS
{
    ULONG target;
    ULONG format;
    ULONG bind;
    ULONG width;
    ULONG height;
    ULONG depth;
    ULONG array_size;
    ULONG last_level;
    ULONG nr_samples;
    ULONG flags;
} VIOGPU_RESOURCE_OPTIONS;

#pragma pack(1)
typedef struct _VIOGPU_CREATE_RESOURCE_EXCHANGE
{
    ULONG magic;
} VIOGPU_CREATE_RESOURCE_EXCHANGE;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_CREATE_ALLOCATION_EXCHANGE
{
    VIOGPU_RESOURCE_OPTIONS ResourceOptions;
    ULONGLONG Size;
    ULONGLONG BlobId;
    ULONG BlobMem;
    ULONG BlobFlags;
    ULONG Stride;
    ULONG ScanoutOffset;
} VIOGPU_CREATE_ALLOCATION_EXCHANGE;
#pragma pack()

// ================= COMMAND BUFFER
#define VIOGPU_CMD_NOP                0x0
#define VIOGPU_CMD_SUBMIT             0x1 // Submit Command to virgl
#define VIOGPU_CMD_TRANSFER_TO_HOST   0x2 // Transfer resource to host
#define VIOGPU_CMD_TRANSFER_FROM_HOST 0x3 // Transfer resource to host
#define VIOGPU_CMD_PRESENT_FLIP       0x4 // Flip scanout to a resource

#define VIOGPU_CMD_SUBMIT_TIMELINE 0x5 // Venus GPU queue timeline, WDDM 2 only

typedef struct _VIOGPU_TIMELINE_SUBMIT
{
    UINT RingIndex; // 1..63; timeline 0 is CPU transport completion.
    UINT Reserved;  // Must be zero; Venus protocol bytes follow.
} VIOGPU_TIMELINE_SUBMIT;

#pragma pack(1)
typedef struct _VIOGPU_COMMAND_HDR
{
    UINT type;
    UINT size;
} VIOGPU_COMMAND_HDR;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_TRANSFER_CMD
{
    ULONG res_id;

    VIOGPU_BOX box;

    ULONGLONG offset;
    ULONG level;
    ULONG stride;
    ULONG layer_stride;
} VIOGPU_TRANSFER_CMD;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_PRESENT_FLIP_CMD
{
    ULONG scan_id;
    ULONG res_id;
    ULONG width;
    ULONG height;
    ULONG x;
    ULONG y;
    ULONG is_blob;
    ULONG format;
    ULONG stride;
    ULONG offset;
} VIOGPU_PRESENT_FLIP_CMD;
#pragma pack()

#define BASE_NAMED_OBJECTS    L"\\BaseNamedObjects\\"
#define GLOBAL_OBJECTS        L"Global\\"
#define RESOLUTION_EVENT_NAME L"VioGpuResolutionEvent"
