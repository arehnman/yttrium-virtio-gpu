#pragma once

#include "virgl_hw.h"

// Include after viogpu.h, which defines the virtio-gpu wire formats.
// Zero is not a wire format and indicates an unsupported allocation format.
static inline ULONG VioGpuVirglFormatToScanout(ULONG format)
{
    switch (format)
    {
        case VIRGL_FORMAT_B8G8R8A8_UNORM:
        case VIRGL_FORMAT_B8G8R8A8_SRGB:
            return VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
        case VIRGL_FORMAT_B8G8R8X8_UNORM:
            return VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
        case VIRGL_FORMAT_A8R8G8B8_UNORM:
            return VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM;
        case VIRGL_FORMAT_X8R8G8B8_UNORM:
            return VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM;
        case VIRGL_FORMAT_R8G8B8A8_UNORM:
        case VIRGL_FORMAT_R8G8B8A8_SRGB:
            return VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM;
        case VIRGL_FORMAT_X8B8G8R8_UNORM:
            return VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM;
        case VIRGL_FORMAT_A8B8G8R8_UNORM:
            return VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM;
        case VIRGL_FORMAT_R8G8B8X8_UNORM:
            return VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM;
        default:
            return 0;
    }
}
