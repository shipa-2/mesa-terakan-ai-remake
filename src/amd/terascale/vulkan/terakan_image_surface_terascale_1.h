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
 * terakan_image_tiling_terascale_1.c. Real RV710 create/layout/allocate/bind coverage validates
 * linear RGBA8, 2D-tiled RGBA8 mip chains and 2D-tiled BC1 mip chains against the DRM-reported
 * topology. Queue submission remains disabled, so tiled GPU addressing still needs a copy/readback
 * test before this layout can be treated as rendering-validated.
 *
 * Block-compressed and subsampled formats minify in texels first, then convert to blocks using
 * terascale_format_block_texels_log2, matching surf_minify() in the classic radeon winsys. Returns
 * false for 3x-expand formats (8_8_8, 16_16_16, 32_32_32):
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
