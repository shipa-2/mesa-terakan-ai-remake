/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_image_surface_terascale_1.h"

#include "terakan_image_tiling_terascale_1.h"
#include "terakan_physical_device.h"

#include "amd/terascale/common/terascale_format.h"
#include "util/bitscan.h"
#include "util/macros.h"
#include "vk_util.h"
#include "wsi_common.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static bool
terakan_image_surface_compute_aspect_terascale_1(
   VkImageCreateInfo const * const image_create_info, enum terascale_format_index const aspect_format,
   struct terakan_physical_device const * const physical_device, uint8_t const array_mode,
   uint32_t const offset_in_memory_lower_bound_bytes_shr8,
   struct terakan_image_surface_aspect * const surface_aspect_out)
{
   unsigned const bytes_per_block = terascale_format_bytes_per_block[aspect_format];
   assert(bytes_per_block != 0);
   surface_aspect_out->bytes_per_block = bytes_per_block;
   unsigned const surfels_per_block = terakan_format_surfels_per_block(bytes_per_block);
   unsigned const bytes_per_element = bytes_per_block / surfels_per_block;

   uint8_t const * const block_texels_log2 = terascale_format_block_texels_log2[aspect_format];
   uint32_t const block_width = 1u << block_texels_log2[0];
   uint32_t const block_height = 1u << block_texels_log2[1];

   memset(&surface_aspect_out->tiling, 0, sizeof(surface_aspect_out->tiling));

   uint32_t const num_pipes = 1u << physical_device->tiling_info.pipes_log2;
   uint32_t const num_banks = 1u << physical_device->tiling_info.banks_log2;
   uint32_t const group_bytes = 1u << physical_device->tiling_info.pipe_interleave_bytes_log2;

   struct wsi_image_create_info const * const wsi_info =
      vk_find_struct_const(image_create_info->pNext, WSI_IMAGE_CREATE_INFO_MESA);
   bool const is_scanout = wsi_info != NULL && wsi_info->scanout;

   uint32_t const samples = (uint32_t)image_create_info->samples;

   struct terakan_image_tiling_terascale_1_alignments const alignments_2d =
      terakan_image_tiling_terascale_1_alignments_2d_tiled_thin1(
         group_bytes, num_pipes, num_banks, bytes_per_element, samples, is_scanout);
   struct terakan_image_tiling_terascale_1_alignments alignments_1d_or_fixed;
   if (array_mode == TERAKAN_IMAGE_TILING_TERASCALE_1_ARRAY_LINEAR_ALIGNED) {
      alignments_1d_or_fixed =
         terakan_image_tiling_terascale_1_alignments_linear_aligned(group_bytes, bytes_per_element);
   } else {
      alignments_1d_or_fixed = terakan_image_tiling_terascale_1_alignments_1d_tiled_thin1(
         group_bytes, bytes_per_element, samples, is_scanout);
   }

   bool const is_3d = image_create_info->imageType == VK_IMAGE_TYPE_3D;
   struct terakan_image_tiling_terascale_1_mip_chain_level
      levels[TERAKAN_IMAGE_TILING_TERASCALE_1_MAX_MIP_LEVELS];
   assert(image_create_info->mipLevels <= TERAKAN_IMAGE_TILING_TERASCALE_1_MAX_MIP_LEVELS);
   uint64_t const size_bytes = terakan_image_tiling_terascale_1_mip_chain_layout(
      image_create_info->extent.width, image_create_info->extent.height,
      is_3d ? image_create_info->extent.depth : image_create_info->arrayLayers, is_3d,
      image_create_info->mipLevels, block_width, block_height, surfels_per_block, bytes_per_element,
      samples,
      array_mode == TERAKAN_IMAGE_TILING_TERASCALE_1_ARRAY_2D_TILED_THIN1 ? &alignments_2d : NULL,
      &alignments_1d_or_fixed, levels);

   struct terakan_image_tiling_terascale_1_alignments const * const base_level_alignments =
      levels[0].is_1d_tiled_thin1_or_fixed ? &alignments_1d_or_fixed : &alignments_2d;
   assert(base_level_alignments->base_level_bo_alignment_bytes >= 0x100);
   surface_aspect_out->alignment_bytes_shr8 = base_level_alignments->base_level_bo_alignment_bytes >> 8;

   uint32_t const aspect_offset_in_memory_bytes_shr8 = ALIGN_POT(
      offset_in_memory_lower_bound_bytes_shr8, surface_aspect_out->alignment_bytes_shr8);
   surface_aspect_out->offset_in_memory_bytes_shr8 = aspect_offset_in_memory_bytes_shr8;

   assert(image_create_info->mipLevels <= ARRAY_SIZE(surface_aspect_out->levels));
   for (uint32_t level_index = 0; level_index < image_create_info->mipLevels; ++level_index) {
      struct terakan_image_surface_level * const level = &surface_aspect_out->levels[level_index];
      struct terakan_image_tiling_terascale_1_mip_chain_level const * const computed_level =
         &levels[level_index];
      assert((computed_level->offset_bytes & 0xFF) == 0);
      assert((computed_level->slice_bytes & 0xFF) == 0);
      level->offset_in_memory_bytes_shr8 =
         (uint32_t)(aspect_offset_in_memory_bytes_shr8 + (computed_level->offset_bytes >> 8));
      level->slice_size_bytes_shr8 = (uint32_t)(computed_level->slice_bytes >> 8);
      level->aligned_extent_surfels[0] = (uint16_t)computed_level->aligned_pitch_surfels;
      level->aligned_extent_surfels[1] = (uint16_t)computed_level->aligned_height_surfels;
      level->aligned_extent_surfels[2] = (uint16_t)computed_level->depth_planes_or_array_layers;
      if (array_mode == TERAKAN_IMAGE_TILING_TERASCALE_1_ARRAY_LINEAR_ALIGNED) {
         /* Linear-aligned chains never degrade -- every level stays linear-aligned. */
         level->array_mode = TERAKAN_IMAGE_TILING_TERASCALE_1_ARRAY_LINEAR_ALIGNED;
      } else {
         level->array_mode = computed_level->is_1d_tiled_thin1_or_fixed
                                ? TERAKAN_IMAGE_TILING_TERASCALE_1_ARRAY_1D_TILED_THIN1
                                : TERAKAN_IMAGE_TILING_TERASCALE_1_ARRAY_2D_TILED_THIN1;
      }
   }

   assert((size_bytes & 0xFF) == 0);
   surface_aspect_out->size_bytes_shr8 = (uint32_t)(size_bytes >> 8);
   return true;
}

bool
terakan_image_surface_compute_terascale_1(
   VkImageCreateInfo const * const image_create_info,
   struct terakan_format_info const * const format_info,
   struct terakan_physical_device const * const physical_device,
   struct terakan_image_surface * const surface_out)
{
   memset(surface_out, 0, sizeof(*surface_out));
   surface_out->alignment_bytes_shr8 = 1;
   surface_out->size_bytes_shr8 = 0;

   VkImageAspectFlagBits const * const aspect_map_aspects =
      terakan_format_aspect_map_aspects[format_info->aspect_map];

   bool const used_by_db =
      (image_create_info->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
   bool const is_combined_depth_stencil_used_by_db =
      format_info->aspect_map == TERAKAN_FORMAT_ASPECT_MAP_0_DEPTH_1_STENCIL && used_by_db;
   bool const multisampled = image_create_info->samples > VK_SAMPLE_COUNT_1_BIT;
   bool const is_1d_image_type = image_create_info->imageType == VK_IMAGE_TYPE_1D;
   bool const tiling_linear_requested = image_create_info->tiling == VK_IMAGE_TILING_LINEAR;

   for (unsigned aspect_index = 0; aspect_index < TERAKAN_FORMAT_MAX_ASPECTS; ++aspect_index) {
      VkImageAspectFlagBits const aspect = aspect_map_aspects[aspect_index];
      if (aspect == VK_IMAGE_ASPECT_NONE) {
         break;
      }

      enum terascale_format_index const aspect_format =
         format_info->aspect_formats[aspect_index].format;

      uint8_t array_mode;
      if (is_combined_depth_stencil_used_by_db && aspect_index == 1) {
         /* Depth and stencil share the same tiling decision -- matches
          * terakan_image_surface_compute()'s own reasoning for R8xx/R9xx: the shared DB_Z_INFO
          * fields (confirmed also shared with DB_DEPTH_INFO on TeraScale 1 -- see the CB/DB/PA/SPI/SQ
          * register audit in TODO.md) must agree between the two aspects.
          */
         array_mode = surface_out->aspects[0].levels[0].array_mode;
      } else {
         bool const format_linear_only =
            (TERASCALE_FORMATS_LINEAR_ONLY & BITFIELD64_BIT(aspect_format)) != 0;
         /* Only the actual 4x4-compressed formats force tiling -- TERASCALE_FORMATS_TILED_ONLY_R6XX,
          * not the broader TERASCALE_FORMATS_BLOCK_R6XX used for the bail-out check above (which
          * also includes the 8x1/2x1 LINEAR_ONLY formats), matching R8xx/R9xx's own
          * TERASCALE_FORMATS_TILED_ONLY_R8XX usage exactly.
          */
         bool const format_tiled_only =
            (TERASCALE_FORMATS_TILED_ONLY_R6XX & BITFIELD64_BIT(aspect_format)) != 0;
         bool const debug_force_linear = getenv("TERAKAN_DEBUG_FORCE_LINEAR_IMAGES") != NULL &&
                                         !used_by_db && !multisampled && !format_tiled_only;
         array_mode = terakan_image_tiling_terascale_1_select_array_mode(
            tiling_linear_requested, debug_force_linear, format_linear_only, used_by_db,
            multisampled, format_tiled_only, is_1d_image_type);
      }

      if (!terakan_image_surface_compute_aspect_terascale_1(
             image_create_info, aspect_format, physical_device, array_mode,
             surface_out->size_bytes_shr8, &surface_out->aspects[aspect_index])) {
         return false;
      }

      surface_out->alignment_bytes_shr8 =
         MAX2(surface_out->aspects[aspect_index].alignment_bytes_shr8,
              surface_out->alignment_bytes_shr8);
      surface_out->size_bytes_shr8 = surface_out->aspects[aspect_index].offset_in_memory_bytes_shr8 +
                                     surface_out->aspects[aspect_index].size_bytes_shr8;
   }

   return true;
}
