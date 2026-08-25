/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TERAKAN_IMAGE_SURFACE_TERASCALE_1_H
#define TERAKAN_IMAGE_SURFACE_TERASCALE_1_H

#include "terakan_format.h"
#include "terakan_image.h"

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

struct terakan_physical_device;

/* TeraScale 1 (R600/R700) counterpart to terakan_image_surface_compute() (terakan_image.c),
 * assembled from the pure, already-unit-tested tiling functions in
 * terakan_image_tiling_terascale_1.c. Not called from anywhere yet -- terakan_CreateDevice still
 * refuses TeraScale 1 physical devices, so there is no way to exercise image creation on this
 * generation at all, which means this function is UNTESTED beyond code review: unlike every other
 * piece of the TeraScale 1 port so far, there is no unit test or real-hardware run backing it, only
 * a careful line-by-line structural match against terakan_image_surface_compute()'s own,
 * already-working R8xx/R9xx logic. Treat it as a draft to re-verify once TeraScale 1 device creation
 * exists and image creation can actually run, not as verified working code.
 *
 * Does not handle formats needing block_texels_log2-based block-width/height division: the
 * 4x4-compressed formats (BC1-7 and friends) and the 8x1/2x1 subsampled ones both need the
 * width_blocks/height_blocks handling terakan_image_surface_tiling_compute() has for R8xx/R9xx,
 * which this does not port yet -- returns false for any of them rather than computing a wrong
 * layout. Also returns false for 3x-expand formats (8_8_8, 16_16_16, 32_32_32):
 * terakan_image_tiling_terascale_1_mip_chain_layout() minifies its base width directly, with no
 * parameter to apply the per-channel-surfel expansion at the point in its per-level loop the AddrLib
 * convention requires (minify in texel units first, expand to surfels only afterward -- see
 * terakan_image_surface_aspect_compute()'s own comment quoting it), so handling these correctly
 * needs either a new parameter on that function or a different composition, not yet done.
 *
 * Does not compute FMASK/CMASK metadata: neither is implemented for any generation this driver
 * supports (see the P0 list in TODO.md), so surface_out's .fmask/.cmask stay zeroed, matching what
 * terakan_image_surface_compute() itself does before either tiling path runs.
 */
bool terakan_image_surface_compute_terascale_1(
   VkImageCreateInfo const * image_create_info, struct terakan_format_info const * format_info,
   struct terakan_physical_device const * physical_device,
   struct terakan_image_surface * surface_out);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_IMAGE_SURFACE_TERASCALE_1_H */
