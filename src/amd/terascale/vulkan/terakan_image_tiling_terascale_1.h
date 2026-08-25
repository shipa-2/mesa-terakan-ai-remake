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
 * This covers the pitch/height/base alignment math and per-level layout (mip extent, aligned
 * pitch/height, pitch/slice byte size, and the degrade-to-1D-on-small-mip decision) -- the
 * foundational pieces every further step (walking a full mip chain to cumulative offsets, and the
 * DB_DEPTH_INFO/CB_COLOR_INFO field computation this blocks per the CB/DB register audit in
 * TODO.md) builds on. It is not wired into terakan_image.c's surface layout computation yet; see
 * TODO.md for what remains.
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

/* mip_minify() in the reference: level 0 is plain u_minify(); every level above that is additionally
 * rounded up to the next power of two, since higher mip levels need power-of-two dimensions for the
 * tiled addressing scheme -- R8xx/R9xx's terakan_image_surface_aspect_compute() does the same
 * power-of-two rounding for its own mip chain (see the "pow2Pad" comment there), so this is not an
 * R600/R700-specific idea, just the same requirement confirmed against this generation's own
 * reference rather than assumed to carry over.
 */
uint32_t terakan_image_tiling_terascale_1_mip_extent(uint32_t base_extent, uint32_t level);

struct terakan_image_tiling_terascale_1_level_layout {
   /* Aligned up to the tiling mode's pitch/height alignment granularity (the .pitch_surfels/
    * .height_surfels fields the _alignments_*() functions above return); npix_x/npix_y before
    * alignment, in surfels, are the caller's job via
    * terakan_image_tiling_terascale_1_mip_extent(), matching the reference's surf_minify() taking
    * already-minified dimensions.
    */
   uint32_t aligned_pitch_surfels;
   uint32_t aligned_height_surfels;
   uint32_t pitch_bytes;
   /* One array slice/depth plane; the caller multiplies by array_layers/depth itself, matching
    * surf_minify() leaving that multiplication to its own caller (r6_surface_init_2d()'s
    * `surf->bo_size = offset + surflevel->slice_size * surflevel->nblk_z * surf->array_size`).
    */
   uint64_t slice_bytes;
   /* True when npix_x/npix_y (post-minify, pre-alignment) are smaller than the 2D tiling mode's own
    * pitch/height alignment -- surf_minify()'s degrade check
    * (`if (surflevel->nblk_x < xalign || surflevel->nblk_y < yalign) { mode = RADEON_SURF_MODE_1D;
    * return; }`), which the reference only applies for single-sample 2D-tiled, non-FMASK surfaces
    * (mirrored here by only being meaningful when the caller passes 2D-tiled alignments and
    * samples == 1). When true, none of the other fields in this struct are populated -- the caller
    * must recompute this level's layout using the 1D-tiled alignments instead, exactly as
    * r6_surface_init_2d() calls back into r6_surface_init_1d() for the level that degrades.
    */
   bool degrades_to_1d_tiled_thin1;
};

/* surf_minify(), given already-minified pixel dimensions (see
 * terakan_image_tiling_terascale_1_mip_extent()) and the tiling mode's alignment (see the
 * _alignments_*() functions above). is_2d_tiled_single_sample selects whether the degrade-to-1D
 * check applies, matching the reference's `nsamples == 1 && mode == RADEON_SURF_MODE_2D` condition
 * (FMASK is excluded there too, but this driver has no FMASK support to model -- see the file
 * comment above).
 */
struct terakan_image_tiling_terascale_1_level_layout
terakan_image_tiling_terascale_1_level_layout(uint32_t npix_x, uint32_t npix_y,
                                               uint32_t pitch_alignment_surfels,
                                               uint32_t height_alignment_surfels,
                                               uint32_t bytes_per_element, uint32_t samples,
                                               bool is_2d_tiled_single_sample);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_IMAGE_TILING_TERASCALE_1_H */
