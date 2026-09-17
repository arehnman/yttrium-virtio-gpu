// Based on virglrenderer/src/virgl_hw.h
#pragma once

// Resource bind flags for kernel allocations
#define VIRGL_BIND_DEPTH_STENCIL            (1u << 0)
#define VIRGL_BIND_RENDER_TARGET            (1u << 1)
#define VIRGL_BIND_SAMPLER_VIEW             (1u << 3)
#define VIRGL_BIND_VERTEX_BUFFER            (1u << 4)
#define VIRGL_BIND_INDEX_BUFFER             (1u << 5)
#define VIRGL_BIND_CONSTANT_BUFFER          (1u << 6)
#define VIRGL_BIND_DISPLAY_TARGET           (1u << 7)
#define VIRGL_BIND_COMMAND_ARGS             (1u << 8)
#define VIRGL_BIND_STREAM_OUTPUT            (1u << 11)
#define VIRGL_BIND_SHADER_BUFFER            (1u << 14)
#define VIRGL_BIND_QUERY_BUFFER             (1u << 15)
#define VIRGL_BIND_CUSTOM                   (1u << 17)
#define VIRGL_BIND_SCANOUT                  (1u << 18)
#define VIRGL_BIND_STAGING                  (1u << 19)

#define VIRGL_TARGET_BUFFER                 0

// Virgl formats that do not have corresponding virtio-gpu scanout enum
// values.  The allocation exchange carries Virgl format numbers.
#define VIRGL_FORMAT_B8G8R8A8_SRGB          100
#define VIRGL_FORMAT_R8G8B8A8_SRGB          104

#define VIRGL_BIND_BUFFER_ONLY_MASK         (VIRGL_BIND_VERTEX_BUFFER | \
                                             VIRGL_BIND_INDEX_BUFFER | \
                                             VIRGL_BIND_CONSTANT_BUFFER | \
                                             VIRGL_BIND_COMMAND_ARGS | \
                                             VIRGL_BIND_STREAM_OUTPUT | \
                                             VIRGL_BIND_SHADER_BUFFER | \
                                             VIRGL_BIND_QUERY_BUFFER | \
                                             VIRGL_BIND_CUSTOM | \
                                             VIRGL_BIND_STAGING)

static inline ULONG VirglBindForTarget(ULONG target, ULONG bind)
{
    if (target != VIRGL_TARGET_BUFFER)
    {
        return bind & ~VIRGL_BIND_BUFFER_ONLY_MASK;
    }

    if (bind == 0 || (bind & ~VIRGL_BIND_BUFFER_ONLY_MASK) || !(bind & (bind - 1)))
    {
        return bind;
    }

    if (bind & VIRGL_BIND_STAGING)
    {
        return VIRGL_BIND_STAGING;
    }
    if (bind & VIRGL_BIND_CUSTOM)
    {
        return VIRGL_BIND_CUSTOM;
    }
    if (bind & VIRGL_BIND_STREAM_OUTPUT)
    {
        return VIRGL_BIND_STREAM_OUTPUT;
    }
    if (bind & VIRGL_BIND_CONSTANT_BUFFER)
    {
        return VIRGL_BIND_CONSTANT_BUFFER;
    }
    if (bind & VIRGL_BIND_INDEX_BUFFER)
    {
        return VIRGL_BIND_INDEX_BUFFER;
    }
    if (bind & VIRGL_BIND_VERTEX_BUFFER)
    {
        return VIRGL_BIND_VERTEX_BUFFER;
    }
    if (bind & VIRGL_BIND_SHADER_BUFFER)
    {
        return VIRGL_BIND_SHADER_BUFFER;
    }
    if (bind & VIRGL_BIND_QUERY_BUFFER)
    {
        return VIRGL_BIND_QUERY_BUFFER;
    }
    if (bind & VIRGL_BIND_COMMAND_ARGS)
    {
        return VIRGL_BIND_COMMAND_ARGS;
    }

    return bind;
}

#define VIRGL_BIND_FOR_TARGET(target, bind) VirglBindForTarget((target), (bind))

// Blt command specification for kernel BLTs
#define VIRGL_CMD0(cmd, obj, len)           ((cmd) | ((obj) << 8) | ((len) << 16))
#define VIRGL_CCMD_RESOURCE_COPY_REGION     17
#define VIRGL_CMD_RESOURCE_COPY_REGION_SIZE 13

// Flags for the driver about resource behaviour
#define VIRGL_RESOURCE_Y_0_TOP              (1 << 0)
#define VIRGL_RESOURCE_FLAG_MAP_PERSISTENT  (1 << 1)
#define VIRGL_RESOURCE_FLAG_MAP_COHERENT    (1 << 2)
