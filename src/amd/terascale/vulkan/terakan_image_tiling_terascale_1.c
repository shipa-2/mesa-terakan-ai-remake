/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_image_tiling_terascale_1.h"

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
