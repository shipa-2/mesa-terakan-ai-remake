/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TERAKAN_IMAGE_TILING_TERASCALE_1_H
#define TERAKAN_IMAGE_TILING_TERASCALE_1_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* TeraScale 1 (R600/R700) surface pitch/height/base-alignment computation.
 *
 * Ported directly from libdrm's radeon/radeon_surface.c (r6_surface_init_linear_aligned() /
 * r6_surface_init_1d() / r6_surface_init_2d()) -- the same reference already used for the
 * RADEON_INFO_TILING_CONFIG decode (terakan_physical_device_tiling_config.c) and the
 * begin-command-buffer atom. This is deliberately NOT the R8xx/R9xx AddrLib-derived algorithm in
 * terakan_image.c (terakan_image_alignments_linear()/_1d_thin()/_2d_thin()): TeraScale 1 has no
 * TILE_SPLIT, no per-surface bank_width/bank_height/macro_tile_aspect selection, and no
 * macro-tile-aspect-ratio search at all -- confirmed by the tiling_config decode work (no
 * TILE_SPLIT field exists in r600d.h for any CB/DB register) and by r6_surface_best() in the
 * reference being a literal no-op ("no value to optimize for r6xx/r7xx"). The whole algorithm is a
 * handful of fixed formulas over group_bytes (pipe interleave)/num_pipes/num_banks/bytes-per-element/
 * sample count, not a per-surface optimization search, which is why this is a much smaller port
 * than R8xx/R9xx's tiling code.
 *
 * This covers only the alignment math -- the foundational piece every further step (per-level
 * offset/size computation, degrade-to-1D-on-small-mip, and the DB_DEPTH_INFO/CB_COLOR_INFO field
 * computation this blocks per the CB/DB register audit in TODO.md) builds on. It is not wired into
 * terakan_image.c's surface layout computation yet; see TODO.md for what remains.
 *
 * 2D tiling additionally requires the kernel driver to actually support it
 * (surf_man->hw_info.allow_2d in the reference, gated on DRM minor version >= 14). Terakan already
 * requires DRM >= 2.50 for any device it recognizes at all (see
 * terakan_physical_device_drm_radeon.c), comfortably above that floor, so this is not modeled here
 * as a runtime condition -- unlike the reference, which supports much older kernels.
 *
 * FMASK is not modeled (its extra pitch floor in the reference, `xalign = MAX2(128, xalign)` for 2D
 * tiling): FMASK/CMASK are unimplemented for every generation this driver supports (see the P0 list
 * in TODO.md), so adding it here would be speculative code with no consumer.
 */

struct terakan_image_tiling_terascale_1_alignments {
   /* In surfels (pixels/texels), not bytes. */
   uint32_t pitch_surfels;
   uint32_t height_surfels;
   /* Only meaningful for the base level -- matches r6_surface_init_*()'s own
    * `if (!start_level) surf->bo_alignment = ...` gating in the reference.
    */
   uint32_t base_level_bo_alignment_bytes;
};

/* r6_surface_init_linear_aligned(). Used for VK_IMAGE_TILING_LINEAR images and other cases needing
 * a texture-cache-fetchable linear layout, matching how R8xx/R9xx's terakan_image_alignments_linear()
 * is used for the same purpose (as opposed to the reference's separate, non-TC-compatible
 * r6_surface_init_linear(), which this does not port: nothing in this driver's architecture needs an
 * unaligned-linear surface, matching the R8xx/R9xx side having no such mode either).
 */
struct terakan_image_tiling_terascale_1_alignments
terakan_image_tiling_terascale_1_alignments_linear_aligned(uint32_t group_bytes,
                                                            uint32_t bytes_per_element);

/* r6_surface_init_1d(). is_scanout applies the reference's RADEON_SURF_SCANOUT pitch floor
 * (64 surfels for 1-byte elements, 32 otherwise).
 */
struct terakan_image_tiling_terascale_1_alignments
terakan_image_tiling_terascale_1_alignments_1d_tiled_thin1(uint32_t group_bytes,
                                                            uint32_t bytes_per_element,
                                                            uint32_t samples, bool is_scanout);

/* r6_surface_init_2d(). */
struct terakan_image_tiling_terascale_1_alignments
terakan_image_tiling_terascale_1_alignments_2d_tiled_thin1(uint32_t group_bytes,
                                                            uint32_t num_pipes, uint32_t num_banks,
                                                            uint32_t bytes_per_element,
                                                            uint32_t samples, bool is_scanout);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_IMAGE_TILING_TERASCALE_1_H */
