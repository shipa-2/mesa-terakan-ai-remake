/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_image_tiling_terascale_1.h"

#include "util/macros.h"
#include "util/u_math.h"

#define TERASCALE_1_MICRO_TILE_WIDTH_HEIGHT_SURFELS 8u

static uint32_t
max2_u32(uint32_t const a, uint32_t const b)
{
   return a > b ? a : b;
}

struct terakan_image_tiling_terascale_1_alignments
terakan_image_tiling_terascale_1_alignments_linear_aligned(uint32_t const group_bytes,
                                                            uint32_t const bytes_per_element)
{
   return (struct terakan_image_tiling_terascale_1_alignments){
      .pitch_surfels = max2_u32(64u, group_bytes / bytes_per_element),
      .height_surfels = 1u,
      .base_level_bo_alignment_bytes = max2_u32(256u, group_bytes),
   };
}

struct terakan_image_tiling_terascale_1_alignments
terakan_image_tiling_terascale_1_alignments_1d_tiled_thin1(uint32_t const group_bytes,
                                                            uint32_t const bytes_per_element,
                                                            uint32_t const samples,
                                                            bool const is_scanout)
{
   uint32_t const tile_width_height = TERASCALE_1_MICRO_TILE_WIDTH_HEIGHT_SURFELS;
   uint32_t pitch_surfels = max2_u32(
      tile_width_height, group_bytes / (tile_width_height * bytes_per_element * samples));
   if (is_scanout) {
      pitch_surfels = max2_u32(bytes_per_element == 1 ? 64u : 32u, pitch_surfels);
   }
   return (struct terakan_image_tiling_terascale_1_alignments){
      .pitch_surfels = pitch_surfels,
      .height_surfels = tile_width_height,
      .base_level_bo_alignment_bytes = max2_u32(256u, group_bytes),
   };
}

struct terakan_image_tiling_terascale_1_alignments
terakan_image_tiling_terascale_1_alignments_2d_tiled_thin1(uint32_t const group_bytes,
                                                            uint32_t const num_pipes,
                                                            uint32_t const num_banks,
                                                            uint32_t const bytes_per_element,
                                                            uint32_t const samples,
                                                            bool const is_scanout)
{
   uint32_t const tile_width_height = TERASCALE_1_MICRO_TILE_WIDTH_HEIGHT_SURFELS;
   uint32_t pitch_surfels =
      max2_u32(tile_width_height * num_banks,
               (group_bytes * num_banks) / (tile_width_height * bytes_per_element * samples));
   if (is_scanout) {
      pitch_surfels = max2_u32(bytes_per_element == 1 ? 64u : 32u, pitch_surfels);
   }
   uint32_t const height_surfels = tile_width_height * num_pipes;
   return (struct terakan_image_tiling_terascale_1_alignments){
      .pitch_surfels = pitch_surfels,
      .height_surfels = height_surfels,
      .base_level_bo_alignment_bytes =
         max2_u32(num_pipes * num_banks * samples * bytes_per_element * 64u,
                  pitch_surfels * height_surfels * samples * bytes_per_element),
   };
}

uint32_t
terakan_image_tiling_terascale_1_mip_extent(uint32_t const base_extent, uint32_t const level)
{
   uint32_t const minified = u_minify(base_extent, level);
   return level > 0 ? util_next_power_of_two(minified) : minified;
}

struct terakan_image_tiling_terascale_1_level_layout
terakan_image_tiling_terascale_1_level_layout(uint32_t const npix_x, uint32_t const npix_y,
                                              uint32_t const pitch_alignment_surfels,
                                              uint32_t const height_alignment_surfels,
                                              uint32_t const bytes_per_element,
                                              uint32_t const samples,
                                              bool const is_2d_tiled_single_sample)
{
   if (is_2d_tiled_single_sample &&
       (npix_x < pitch_alignment_surfels || npix_y < height_alignment_surfels)) {
      return (struct terakan_image_tiling_terascale_1_level_layout){
         .degrades_to_1d_tiled_thin1 = true,
      };
   }

   uint32_t const aligned_pitch_surfels = ALIGN_POT(npix_x, pitch_alignment_surfels);
   uint32_t const aligned_height_surfels = ALIGN_POT(npix_y, height_alignment_surfels);
   uint32_t const pitch_bytes = aligned_pitch_surfels * bytes_per_element * samples;
   return (struct terakan_image_tiling_terascale_1_level_layout){
      .aligned_pitch_surfels = aligned_pitch_surfels,
      .aligned_height_surfels = aligned_height_surfels,
      .pitch_bytes = pitch_bytes,
      .slice_bytes = (uint64_t)pitch_bytes * aligned_height_surfels,
      .degrades_to_1d_tiled_thin1 = false,
   };
}

uint8_t
terakan_image_tiling_terascale_1_select_array_mode(bool const tiling_linear_requested,
                                                    bool const debug_force_linear,
                                                    bool const format_linear_only,
                                                    bool const used_by_db, bool const multisampled,
                                                    bool const format_tiled_only,
                                                    bool const is_1d_image_type)
{
   if (tiling_linear_requested || debug_force_linear || format_linear_only) {
      return TERAKAN_IMAGE_TILING_TERASCALE_1_ARRAY_LINEAR_ALIGNED;
   }
   if (!used_by_db && !multisampled && !format_tiled_only && is_1d_image_type) {
      return TERAKAN_IMAGE_TILING_TERASCALE_1_ARRAY_LINEAR_ALIGNED;
   }
   return TERAKAN_IMAGE_TILING_TERASCALE_1_ARRAY_2D_TILED_THIN1;
}
