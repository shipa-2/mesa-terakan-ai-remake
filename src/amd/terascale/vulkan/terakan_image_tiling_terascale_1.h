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
 * This covers the pitch/height/base alignment math, per-level layout (mip extent, aligned
 * pitch/height, pitch/slice byte size, and the degrade-to-1D-on-small-mip decision), the base-level
 * array-mode decision, and walking a full mip chain to cumulative byte offsets -- everything needed
 * to compute a complete TeraScale 1 surface layout in isolation. It is wired into
 * terakan_image.c's surface layout computation via terakan_image_surface_compute_terascale_1()
 * (terakan_image_surface_terascale_1.c), which is itself unverified beyond code review -- see its
 * own header comment. The DB_DEPTH_INFO/CB_COLOR_INFO register field computation this unblocks (per
 * the CB/DB register audit in TODO.md) still needs its own per-field compatibility check; see
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

/* Matches r600d.h's V_0280A0_ARRAY_LINEAR_ALIGNED/_1D_TILED_THIN1/_2D_TILED_THIN1 and evergreend.h's
 * V_028C70_ARRAY_LINEAR_ALIGNED/_1D_TILED_THIN1/_2D_TILED_THIN1 -- confirmed numerically identical
 * between the two headers, not assumed, so this file can return one of these without including
 * either register header (keeping it decoupled the way the rest of this file already is).
 * ARRAY_1D_TILED_THIN1 is not a possible return value of
 * terakan_image_tiling_terascale_1_select_array_mode() below (that function only ever picks linear
 * or 2D for the base level, matching R8xx/R9xx's own base-level decision), but a caller populating
 * a per-level array mode after terakan_image_tiling_terascale_1_mip_chain_layout() needs it for a
 * level that degraded from 2D to 1D-tiled partway through a mip chain.
 */
#define TERAKAN_IMAGE_TILING_TERASCALE_1_ARRAY_LINEAR_ALIGNED 1u
#define TERAKAN_IMAGE_TILING_TERASCALE_1_ARRAY_1D_TILED_THIN1 2u
#define TERAKAN_IMAGE_TILING_TERASCALE_1_ARRAY_2D_TILED_THIN1 4u

/* The base-level array mode decision from terakan_image_surface_tiling_compute() (terakan_image.c),
 * transcribed here rather than reused directly since that function is R8xx/R9xx-shaped throughout
 * (evergreend.h array-mode constants, terakan_physical_device_tiling_info-driven macro-tile
 * search). The policy itself -- prefer 2D-tiled unless linear tiling was requested, a debug
 * override is set, the format requires linear, or it's a non-multisampled non-DB 1D image whose
 * format doesn't require tiling -- is a driver-level Vulkan design choice, not hardware-specific,
 * so it is intentionally kept identical in shape to the R8xx/R9xx version rather than re-derived;
 * only the array-mode constants differ (see the two macros above), and the format tables a caller
 * uses to compute format_linear_only/format_tiled_only should be TERASCALE_FORMATS_LINEAR_ONLY
 * (generation-agnostic, confirmed by its own comment citing both the R800 AddrLib and the Gallium
 * R600 driver) and TERASCALE_FORMATS_TILED_ONLY_R6XX (already exists in terascale_format.h
 * specifically for this generation, alongside the R8xx one the existing code uses) rather than the
 * R8xx-suffixed table. Takes plain booleans rather than VkImageType/VkImageTiling/
 * terascale_format_index directly so this file stays decoupled from Vulkan and format-table headers
 * the way the rest of it already is; the caller computes them.
 *
 * debug_force_linear is expected to already have been computed with the same additional gating the
 * R8xx/R9xx side applies (`!used_by_db && samples <= 1 && !format_tiled_only`) before being passed
 * in here, matching TERAKAN_DEBUG_FORCE_LINEAR_IMAGES's existing semantics.
 */
uint8_t terakan_image_tiling_terascale_1_select_array_mode(bool tiling_linear_requested,
                                                            bool debug_force_linear,
                                                            bool format_linear_only,
                                                            bool used_by_db, bool multisampled,
                                                            bool format_tiled_only,
                                                            bool is_1d_image_type);

/* Maximum mip levels this driver ever creates a surface for: Vulkan images are limited to 8192 in
 * any dimension (TERAKAN_IMAGE_MAX_WIDTH_HEIGHT in terakan_image.h) elsewhere in this driver, and
 * log2(8192) + 1 = 14.
 */
#define TERAKAN_IMAGE_TILING_TERASCALE_1_MAX_MIP_LEVELS 14u

struct terakan_image_tiling_terascale_1_mip_chain_level {
   uint64_t offset_bytes;
   uint32_t aligned_pitch_surfels;
   uint32_t aligned_height_surfels;
   uint32_t pitch_bytes;
   /* One array layer/3D-depth-plane's worth of bytes (before the array_layers/depth_planes
    * multiplication `offset_bytes` already includes) -- a caller populating a per-level memory
    * footprint field (as opposed to the register-facing tile-count fields, computed separately from
    * aligned_pitch_surfels/aligned_height_surfels) uses this directly.
    */
   uint64_t slice_bytes;
   /* This level's depth-plane count (3D images) or array layer count (everything else) -- whichever
    * depth_minifies_per_level selected. Exposed since a caller populating a per-level struct
    * typically needs this alongside aligned_pitch_surfels/aligned_height_surfels/slice_bytes, not
    * because this function does anything with it beyond already using it to compute slice_bytes's
    * contribution to offset_bytes.
    */
   uint32_t depth_planes_or_array_layers;
   /* True if this level ended up 1D-tiled, either because the whole chain was requested as 1D-tiled
    * (or linear-aligned, in which case this is still set for simplicity, since linear-aligned and
    * 1D-tiled share the same "no further degrade" behavior in this function) or because a 2D-tiled
    * chain degraded at or before this level. A caller emitting per-level ARRAY_MODE needs this to
    * know which value to write for each level -- the reference's own mip chain can have some levels
    * 2D-tiled and later, smaller ones 1D-tiled within the same image.
    */
   bool is_1d_tiled_thin1_or_fixed;
};

/* Walks a full mip chain, calling terakan_image_tiling_terascale_1_mip_extent() and
 * terakan_image_tiling_terascale_1_level_layout() for each level and accumulating offsets, mirroring
 * what the reference's surf_minify() does across a whole call to r6_surface_init_1d()/_2d() (its own
 * `offset`/`surf->bo_size` running state across the `for (i = start_level; i <= surf->last_level;
 * i++)` loop, including r6_surface_init_2d() calling back into r6_surface_init_1d() for the level
 * that degrades and every level after it -- once a chain degrades to 1D-tiled it never reverts to
 * 2D-tiled for a smaller, later mip, matching the reference exactly).
 *
 * alignments_2d is the 2D-tiled alignment (terakan_image_tiling_terascale_1_alignments_2d_tiled_thin1())
 * to try first, with degrade checked against it; pass NULL for a chain that should never attempt 2D
 * tiling at all (a linear-aligned or 1D-tiled base array mode, from
 * terakan_image_tiling_terascale_1_select_array_mode() returning something other than
 * ARRAY_2D_TILED_THIN1) -- in that case every level uses fixed_or_1d_alignments unconditionally, and
 * `is_1d_tiled_thin1_or_fixed` is set on every output level even though it may really be
 * linear-aligned; the caller already knows which from its own array-mode decision and doesn't need
 * this function to repeat it.
 *
 * depth_minifies_per_level selects between a 3D image's npix_z (mip-minified like npix_x/npix_y --
 * VkImageType 3D, base_depth_or_array_layers is imageCreateInfo->extent.depth) and a 2D image's
 * array layer count (constant across all mip levels -- imageCreateInfo->arrayLayers), matching the
 * same `imageType == VK_IMAGE_TYPE_3D ? u_minify(extent.depth, level) : arrayLayers` distinction
 * terakan_image_surface_aspect_compute() already makes for R8xx/R9xx (this is Vulkan-level surface
 * shape, not hardware-specific, so it is not re-derived from the reference the way the tiling math
 * itself is -- r6_surface_init_2d()'s own `nblk_z` and `array_size` play the equivalent roles).
 * Width and height are likewise minified in texels before being divided by block_width/block_height,
 * matching surf_minify()'s npix-to-nblk order for BC and subsampled formats. surfels_per_block is
 * normally 1, but is 3 for the linear-only 8_8_8, 16_16_16 and 32_32_32 formats. For those, width
 * is expanded after texel minification and before the higher-mip power-of-two padding, matching
 * AddrLib's 3x handling and the existing R8xx/R9xx surface path. The base pitch is additionally
 * aligned to three times the normal pitch alignment because the descriptor expresses it in texels
 * with an 8-texel granularity while memory contains three surfels per texel.
 *
 * mip_levels must not exceed TERAKAN_IMAGE_TILING_TERASCALE_1_MAX_MIP_LEVELS, and levels_out must
 * have room for at least that many entries. Returns the total surface size in bytes (surf->bo_size
 * in the reference, after the whole loop).
 */
uint64_t terakan_image_tiling_terascale_1_mip_chain_layout(
   uint32_t base_width, uint32_t base_height, uint32_t base_depth_or_array_layers,
   bool depth_minifies_per_level, uint32_t mip_levels, uint32_t block_width,
   uint32_t block_height, uint32_t surfels_per_block, uint32_t bytes_per_element, uint32_t samples,
   struct terakan_image_tiling_terascale_1_alignments const * alignments_2d,
   struct terakan_image_tiling_terascale_1_alignments const * fixed_or_1d_alignments,
   struct terakan_image_tiling_terascale_1_mip_chain_level * levels_out);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_IMAGE_TILING_TERASCALE_1_H */
