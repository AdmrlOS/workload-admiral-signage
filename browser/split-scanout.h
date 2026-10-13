/* SPDX-License-Identifier: MIT */
#ifndef SIGNAGE_SPLIT_SCANOUT_H
#define SIGNAGE_SPLIT_SCANOUT_H
#include <stdbool.h>
#include <stdint.h>

typedef struct { uint32_t left, right, height; } SignageSplitGeometry;

/* DRM type is descriptive, not a size/crop capability: sun4i uses Cursor
 * for its second full-size RGB UI channel. Actual formats and TEST_ONLY
 * still decide whether it can supply a view; never take another primary. */
static inline bool signage_split_plane_type(uint64_t type)
{
    return type == 0 /* Overlay */ || type == 2 /* Cursor */;
}

/* Untiled AFBC 32x8 uses Mesa 32x16 render alignment. Keep the full BO;
 * neither decoded UI window exceeds the DE350's 2560-pixel lane limit.
 * This is a geometry check, not a substitute for KMS TEST_ONLY acceptance. */
static inline bool signage_split_geometry(uint32_t width, uint32_t height,
                                         uint32_t mode_width, uint32_t mode_height,
                                         SignageSplitGeometry *geometry)
{
    if (!geometry || width <= 2560 || width > 4096 || !height || height > 4096 ||
        width != mode_width || height != mode_height || width % 64 || height % 16)
        return false;
    *geometry = (SignageSplitGeometry){ width / 2, width / 2, height };
    return true;
}
#endif
