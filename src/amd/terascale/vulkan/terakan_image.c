/*
 * Copyright © 2024 Vitaliy Triang3l Kuzmin
 *
 * Based in part on r600_texture.c which is:
 * Copyright 2010 Jerome Glisse <glisse@freedesktop.org>
 *
 * Surface calculations based in part on the address library for AMD drivers
 * (AddrLib) which is:
 * Copyright (c) 2007-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 * https://github.com/GPUOpen-Drivers/pal/tree/dc99f22e2999cbefb5d46bec9a8beb9a9b6fa5e8/src/core/imported/addrlib
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "terakan_image.h"

#include "terakan_device.h"
#include "terakan_device_memory.h"
#include "terakan_entrypoints.h"
#include "terakan_format.h"
#include "terakan_physical_device.h"

#include "gallium/drivers/r600/evergreend.h"
#include "util/bitscan.h"
#include "util/format/u_format.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "vk_alloc.h"
#include "vk_enum_to_str.h"
#include "vk_format.h"
#include "vk_log.h"
#include "vk_util.h"
#include "wsi_common.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct terakan_screen_rect
terakan_vk_rect_to_screen_rect(VkRect2D rect, struct terakan_screen_rect const clip_rect)
{
   /* Clamp to the right and bottom boundaries of the clip rectangle, and also prevent 32-bit
    * addition overflow for the right and the bottom coordinates of the rectangle.
    */
   rect.offset.x =
      CLAMP(rect.offset.x, -(int32_t)clip_rect.bounds[1][0], (int32_t)clip_rect.bounds[1][0]);
   rect.offset.y =
      CLAMP(rect.offset.y, -(int32_t)clip_rect.bounds[1][1], (int32_t)clip_rect.bounds[1][1]);
   rect.extent.width =
      MIN2(rect.extent.width, (uint32_t)((int32_t)clip_rect.bounds[1][0] - rect.offset.x));
   rect.extent.height =
      MIN2(rect.extent.height, (uint32_t)((int32_t)clip_rect.bounds[1][1] - rect.offset.y));

   int32_t const x1 = rect.offset.x + (int32_t)rect.extent.width;
   int32_t const y1 = rect.offset.y + (int32_t)rect.extent.height;

   /* Clamp to the left and top boundaries of the clip rectangle. */
   int32_t const x0 = MAX2(rect.offset.x, (int32_t)clip_rect.bounds[0][0]);
   int32_t const y0 = MAX2(rect.offset.y, (int32_t)clip_rect.bounds[0][1]);

   if (unlikely(x0 >= x1 || y0 >= y1)) {
      /* Empty or out of bounds (including `x0` or `y0` being the screen size,
       * `TERAKAN_IMAGE_MAX_WIDTH_HEIGHT`, if it's the right or bottom edge of the clip rectangle,
       * which is out of the range of the allowed TL register values), or the clip rectangle is
       * empty.
       */
      return (struct terakan_screen_rect){};
   }

   return (struct terakan_screen_rect){
      .bounds = {{(uint16_t)x0, (uint16_t)y0}, {(uint16_t)x1, (uint16_t)y1}},
   };
}

/* While old versions of AMD's PAL included the code of an implementation of the AddrLib for
 * R8xx/R9xx (the R800Lib class), its usage involves various inconveniences, such as:
 *
 * - 8_8_8, 16_16_16, 32_32_32 formats are handled in a strange way, with pitch returned in channels
 *   rather than in whole pixels, and its value doesn't even fit into PITCH_TILE_MAX for large image
 *   widths. Mesa GFX6+ drivers work around this by passing FMT_INVALID with an explicit number of
 *   bits per pixel instead.
 *
 * - The code of R800Lib contains a few references to a workaround for some hardware bug related to
 *   block-compressed images and padding of their storage to power of two dimensions. For "ECO'd or
 *   RTL fixed" revisions of R8xx, it contains an assertion that checks if the input width and
 *   height are powers of two, and for that hardware bug on the A11 revision of Cypress and Juniper
 *   it doesn't have a workaround at all. There are also some related assumptions in
 *   ElemLib::AdjustSurfaceInfo, and it's not clear how safe they would be if that power of two
 *   assertion is bypassed. However, it's not certain if any such workaround is needed at all, and
 *   what exactly is affected by that bug - and as of this writing, the Gallium R600 driver and
 *   libdrm_radeon don't have any related code.
 *
 * Instead of working around edge cases in AddrLib's interface, doing similar computations directly
 * in Terakan.
 *
 * libdrm_radeon also contains surface computation, but AddrLib is generally more detailed as of
 * March 2024.
 */

static bool
terakan_image_compute_tc_non_display(struct terakan_format_info const * const format_info,
                                     unsigned const aspect_index, bool const is_r9xx,
                                     bool const is_linear)
{
   /* R9xx doesn't support displayable for 128bpp at all.
    *
    * Otherwise, when that exception doesn't apply, R8xx and R9xx TC:
    * - Requires displayable for linear images (CB, however, requires non-displayable for them).
    * - Supports non-displayable for 8bpp, 16bpp, 32bpp, 64bpp, 128bpp, and BC* images.
    *   Note that block-compressed formats are mentioned explicitly, but textures with other formats
    *   that have multiple pixels per "block" (1bpp, subsampled) must be linear.
    *
    * However, on R8xx (tested on Barts), it seems like there's some inconsistency between CB and TC
    * for non-displayable tiling order for 64bpp and 128bpp. This causes:
    * - With buffer to image copying for BC implemented by drawing to a 32_32 / 32_32_32_32 CB
    *   target, and image to buffer copying via reading from a 32_32 / 32_32_32_32 TC texture,
    *   failures of:
    *   dEQP-VK.api.copy_and_blit.core.image_to_buffer.2d_images.mip_copies_bc*_universal
    * - Broken SSAO in Sascha Willems's `ssao` example with the VK_FORMAT_R32G32B32A32_SFLOAT
    *   G-buffer containing position and depth.
    *
    * Because of that, unless there are other requirements, using non-displayable tiling order only
    * for depth / stencil formats (regardless of the usage). As of March 2023, this matches how the
    * Gallium R600 driver selects the tiling order. SQ_TEX_RESOURCE_WORD0_0 documentation in the
    * Evergreen and Cayman 3D Register Reference Guides also describes NON_DISP_TILING_ORDER as
    * "Memory tiling type (Color vs. Depth)".
    */
   if (is_r9xx &&
       terascale_format_bytes_per_block[format_info->aspect_formats[aspect_index].format] >= 16) {
      return true;
   }
   if (is_linear) {
      return false;
   }
   return (terakan_format_aspect_map_aspect_masks[format_info->aspect_map] &
           (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
}

static unsigned
terakan_image_macro_tile_bytes_log2(unsigned const bytes_per_block_log2,
                                    unsigned const samples_log2,
                                    unsigned const tile_split_bytes_log2)
{
   /* From AddrLib's EgBasedLib::ComputeSurfaceAlignmentsMacroTiled.
    * For thick tiling modes, thickness should be one of the multipliers also, but they aren't used.
    */
   return MIN2(6 + bytes_per_block_log2 + samples_log2, tile_split_bytes_log2);
}

/* Bank height must be aligned to this value. */
static unsigned
terakan_image_macro_tile_bank_height_alignment_log2(
   struct terakan_physical_device_tiling_info const * const tiling_info,
   unsigned const tile_bytes_log2, unsigned const bank_width_log2)
{
   /* From AddrLib's EgBasedLib::ComputeSurfaceAlignmentsMacroTiled. */
   return (unsigned)MAX2(
      (int)(tiling_info->pipe_interleave_bytes_log2 + tiling_info->bank_interleave_log2) -
         (int)(tile_bytes_log2 + bank_width_log2),
      0);
}

/* Macro-tile aspect ratio must be aligned to this value for single-sample images (mips have this
 * requirement, though AddrLib aligns it for single-sample images regardless of whether they have
 * mips).
 */
static unsigned
terakan_image_macro_tile_aspect_ratio_single_sample_alignment_log2(
   struct terakan_physical_device_tiling_info const * const tiling_info,
   unsigned const tile_bytes_log2, unsigned const bank_width_log2)
{
   /* From AddrLib's EgBasedLib::ComputeSurfaceAlignmentsMacroTiled. */
   return (unsigned)MAX2(
      (int)(tiling_info->pipe_interleave_bytes_log2 + tiling_info->bank_interleave_log2) -
         (int)(tile_bytes_log2 + tiling_info->pipes_log2 + bank_width_log2),
      0);
}

struct terakan_image_alignments {
   uint32_t pitch_surfels;
   uint32_t height_blocks;
   uint32_t base_bytes;
};

static struct terakan_image_alignments
terakan_image_alignments_linear(unsigned const pipe_interleave_bytes_log2,
                                unsigned const bytes_per_block)
{
   /* From AddrLib's R800Lib::HwlGetPitchAlignmentLinear.
    * Must not be smaller than what DRM Radeon evergreen_surface_check_linear_aligned computes for
    * command submission validation.
    */
   return (struct terakan_image_alignments){
      .pitch_surfels = terakan_format_pitch_alignment_linear_surfels(
         bytes_per_block / terakan_format_surfels_per_block(bytes_per_block),
         pipe_interleave_bytes_log2),
      .height_blocks = 1,
      .base_bytes = (uint32_t)1 << pipe_interleave_bytes_log2,
   };
}

#define TERAKAN_IMAGE_MICRO_TILE_WIDTH_HEIGHT_LOG2 3
#define TERAKAN_IMAGE_MICRO_TILE_WIDTH_HEIGHT      (1 << TERAKAN_IMAGE_MICRO_TILE_WIDTH_HEIGHT_LOG2)

/* Note that with 1D thin tiling, stencil requires a larger pitch alignment compared to depth, yet
 * there's only one DB_DEPTH_SIZE::PITCH_TILE_MAX for both depth and stencil.
 *
 * For the base level, this can be worked around by overaligning the depth surface in combined depth
 * and stencil images - this is handled here when is_combined_depth_stencil_base_level is true.
 *
 * This can't be done for mips, however, as TC computes their pitch implicitly from the width.
 * Neither can 2D tiling be forced for mips, because TC automatically degrades small mips to 1D
 * thin.
 * Because of that, mips require a different workaround - like rendering to an intermediate
 * overaligned depth buffer and copying to the one that will be read via TC.
 */
static struct terakan_image_alignments
terakan_image_alignments_1d_thin(unsigned const pipe_interleave_bytes_log2,
                                 unsigned const bytes_per_block_log2,
                                 bool const is_combined_depth_stencil_base_level_used_by_db)
{
   /* From AddrLib's EgBasedLib::ComputeSurfaceAlignmentsMicroTiled.
    * Must not be smaller than what DRM Radeon evergreen_surface_check_1d computes for command
    * submission validation.
    * If is_combined_depth_stencil_base_level_used_by_db, align depth like stencil (1 byte per
    * pixel).
    */
   unsigned const pixels_per_pipe_interleave_log2 =
      pipe_interleave_bytes_log2 -
      (is_combined_depth_stencil_base_level_used_by_db ? 0 : bytes_per_block_log2);
   unsigned const pixels_per_micro_tile_log2 = TERAKAN_IMAGE_MICRO_TILE_WIDTH_HEIGHT_LOG2 * 2;
   unsigned const micro_tiles_per_pipe_interleave_log2 =
      MAX2(pixels_per_pipe_interleave_log2, pixels_per_micro_tile_log2) -
      pixels_per_micro_tile_log2;
   return (struct terakan_image_alignments){
      .pitch_surfels = TERAKAN_IMAGE_MICRO_TILE_WIDTH_HEIGHT
                       << micro_tiles_per_pipe_interleave_log2,
      .height_blocks = TERAKAN_IMAGE_MICRO_TILE_WIDTH_HEIGHT,
      .base_bytes = (uint32_t)1 << pipe_interleave_bytes_log2,
   };
}

static struct terakan_image_alignments
terakan_image_alignments_2d_thin(
   struct terakan_physical_device_tiling_info const * const tiling_info,
   unsigned const tile_bytes_log2, unsigned const bank_width_log2, unsigned const bank_height_log2,
   unsigned const macro_tile_aspect_ratio_log2)
{
   /* From AddrLib's EgBasedLib::ComputeSurfaceAlignmentsMacroTiled.
    * Must not be smaller than what DRM Radeon evergreen_surface_check_2d computes for command
    * submission validation.
    */
   assert(macro_tile_aspect_ratio_log2 <= 3);
   static_assert(
      TERAKAN_IMAGE_MICRO_TILE_WIDTH_HEIGHT_LOG2 <= 3,
      "Expecting that the micro-tile height is divisible by the maximum macro-tile aspect ratio");
   return (struct terakan_image_alignments){
      .pitch_surfels =
         (uint32_t)TERAKAN_IMAGE_MICRO_TILE_WIDTH_HEIGHT
         << (macro_tile_aspect_ratio_log2 + bank_width_log2 + tiling_info->pipes_log2),
      .height_blocks =
         (uint32_t)(TERAKAN_IMAGE_MICRO_TILE_WIDTH_HEIGHT >> macro_tile_aspect_ratio_log2)
         << (bank_height_log2 + tiling_info->banks_log2),
      .base_bytes = (uint32_t)1 << (bank_width_log2 + tiling_info->pipes_log2 + bank_height_log2 +
                                    tiling_info->banks_log2 + tile_bytes_log2),
   };
}

/* According to AddrLib's AdjustPitchAlignment, display engine hardwires lower 5 bit of GRPH_PITCH
 * to zero, which means 32 pixel alignment.
 */
#define TERAKAN_IMAGE_SCANOUT_PITCH_ALIGNMENT_PIXELS_LOG2 5
#define TERAKAN_IMAGE_SCANOUT_PITCH_ALIGNMENT_PIXELS                                               \
   (1 << TERAKAN_IMAGE_SCANOUT_PITCH_ALIGNMENT_PIXELS_LOG2)

/* format_info must be the result of terakan_format_info_get for image_create_info->format.
 * Returns the preferred base level array mode, though it may need to be degraded from 2D to 1D
 * afterwards.
 */
static uint8_t
terakan_image_surface_tiling_compute(VkImageCreateInfo const * const image_create_info,
                                     struct terakan_format_info const * const format_info,
                                     unsigned const aspect_index,
                                     struct terakan_physical_device const * const physical_device,
                                     struct terakan_image_surface_tiling * const tiling_out)
{
   bool const used_by_db =
      (image_create_info->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;

   uint8_t array_mode;
   enum terascale_format_index const aspect_data_format =
      format_info->aspect_formats[aspect_index].format;
   if (image_create_info->tiling == VK_IMAGE_TILING_LINEAR ||
       (TERASCALE_FORMATS_LINEAR_ONLY & BITFIELD64_BIT(aspect_data_format))) {
      array_mode = V_028C70_ARRAY_LINEAR_ALIGNED;
   } else {
      array_mode = V_028C70_ARRAY_2D_TILED_THIN1;
      /* Multisampled images must be 2D-tiled.
       * Depth / stencil attachments must be tiled.
       */
      if (!used_by_db && image_create_info->samples <= VK_SAMPLE_COUNT_1_BIT &&
          !(TERASCALE_FORMATS_TILED_ONLY_R8XX & BITFIELD64_BIT(aspect_data_format))) {
         /* Handle common candidates for the linear mode.
          *
          * 1D storage images must be linear according to the Gallium R600 driver, and overall
          * linear is more compact for them.
          *
          * AddrLib, however, also normally makes images with a height of 1 linear because linear is
          * optimal for them. However, a comment in the R800 AddrLib says "Tex2D UAV on cypress will
          * fail/hang if tile mode is linear", and the R800 AddrLib disables the linear array mode
          * optimization for a height of 1 completely, so not doing this optimization for non-1D
          * images here either.
          */
         if (image_create_info->imageType == VK_IMAGE_TYPE_1D) {
            array_mode = V_028C70_ARRAY_LINEAR_ALIGNED;
         }
      }
   }

   memset(tiling_out, 0, sizeof(*tiling_out));

   if (array_mode == V_028C70_ARRAY_2D_TILED_THIN1) {
      /* Choose an optimal macro-tile configuration based on how R800 AddrLib's HwlSetupTileInfo
       * does that.
       */

      /* TODO(Triang3l): Debug flag for disabling 2D tiling. */

      bool const is_combined_depth_stencil_used_by_db =
         used_by_db && format_info->aspect_map == TERAKAN_FORMAT_ASPECT_MAP_0_DEPTH_1_STENCIL;

      /* Depth and stencil surfaces since they share bank size and macro-tile aspect ratio, make
       * sure they're calculated the same for both aspects (specifically, calculate them for depth,
       * and reuse for stencil).
       */
      unsigned const bytes_per_block_for_tiling = terascale_format_bytes_per_block
         [format_info->aspect_formats[is_combined_depth_stencil_used_by_db ? 0 : aspect_index]
             .format];
      /* Assume that images with non-power-of-two numbers of bytes per block are linear. */
      assert(IS_POT(bytes_per_block_for_tiling));
      unsigned const bytes_per_block_for_tiling_log2 = util_logbase2(bytes_per_block_for_tiling);

      /* Tile split. */

      unsigned tile_split_bytes_log2;
      unsigned const samples_log2 = util_logbase2((uint32_t)image_create_info->samples);
      if (used_by_db) {
         /* For compressed depth, this formula is recommended, for uncompressed, the chip's row size
          * is.
          */
         tile_split_bytes_log2 = 6 + (samples_log2 - (unsigned)(samples_log2 >= 2));
      } else {
         if (samples_log2 != 0) {
            tile_split_bytes_log2 =
               6 + bytes_per_block_for_tiling_log2 + util_logbase2_ceil(samples_log2);
            tile_split_bytes_log2 = MAX2(tile_split_bytes_log2, 8);
            /* For multisampled images, the calculated tile split may exceed the row size. */
            tile_split_bytes_log2 =
               MIN2(tile_split_bytes_log2, physical_device->tiling_info.row_bytes_log2);
         } else {
            tile_split_bytes_log2 = physical_device->tiling_info.row_bytes_log2;
         }
      }

      /* Bank width and height. */

      unsigned bank_extent_log2[] = {0, 0};

      unsigned const tile_bytes_log2 = terakan_image_macro_tile_bytes_log2(
         bytes_per_block_for_tiling_log2, samples_log2, tile_split_bytes_log2);
      /* For calculating alignments required by combined depth and stencil surfaces since they share
       * bank size and macro-tile aspect ratio, assuming that the tile split is computed the same
       * for depth and stencil.
       */
      unsigned const stencil_tile_bytes_log2 =
         terakan_image_macro_tile_bytes_log2(1, samples_log2, tile_split_bytes_log2);

      if (used_by_db) {
         /* According to the R800 AddrLib, test shows simple sample has best performance with 2x4.
          */
         if (samples_log2 != 0) {
            if (is_combined_depth_stencil_used_by_db) {
               /* To take into account that stencil might require a higher bank height alignment. */
               int const scale_factor_log2 =
                  (int)physical_device->tiling_info.pipe_interleave_bytes_log2 -
                  (int)stencil_tile_bytes_log2;
               if (scale_factor_log2 > 0) {
                  if (scale_factor_log2 > 2) {
                     bank_extent_log2[0] = 1;
                  }
                  bank_extent_log2[1] = scale_factor_log2 - bank_extent_log2[0];
               }
            }
            /* Else use the default 1x1. */
         } else {
            bank_extent_log2[0] = 1;
            bank_extent_log2[1] = 2;
         }
      } else {
         if (tile_bytes_log2 <= 5) {
            bank_extent_log2[1] = 3;
         } else if (tile_bytes_log2 <= 6) {
            bank_extent_log2[1] = 2;
         } else if (tile_bytes_log2 <= 7) {
            bank_extent_log2[1] = 1;
         }
      }

      unsigned bank_extent_test_value_log2;
      unsigned bank_extent_test_axis = 0;
      do {
         bank_extent_test_value_log2 = tile_bytes_log2 + bank_extent_log2[0] + bank_extent_log2[1];
         if (bank_extent_test_value_log2 >= 8) {
            break;
         }
         ++bank_extent_log2[bank_extent_test_axis];
         bank_extent_test_axis ^= 1;
      } while (true);
      bank_extent_test_axis = 0;
      /* Check before entering to avoid infinite loop. */
      if (tile_bytes_log2 <= physical_device->tiling_info.row_bytes_log2) {
         while (bank_extent_test_value_log2 > physical_device->tiling_info.row_bytes_log2) {
            if (bank_extent_test_axis == 0 && bank_extent_log2[0] != 0) {
               --bank_extent_log2[0];
            } else if (bank_extent_log2[1] != 0) {
               --bank_extent_log2[1];
            }
            bank_extent_test_axis ^= 1;
            bank_extent_test_value_log2 =
               tile_bytes_log2 + bank_extent_log2[0] + bank_extent_log2[1];
         }
      }

      /* Reduce the bank width and height if needed and possible to meet the constraint:
       * tile size * bank width * bank height <= row size
       * See the HwlReduceBankWidthHeight call in EgBasedLib::ComputeSurfaceAlignmentsMacroTiled.
       * This is moved above aligning the bank height compared to where it's done in AddrLib for
       * consistency of bank height and macro-tile aspect ratio calculations.
       */
      unsigned attrib_alignment_tile_bytes_log2 = tile_bytes_log2;
      if (is_combined_depth_stencil_used_by_db) {
         /* Stencil, which may have a larger tile size, may require higher bank height and
          * macro-tile aspect ratio alignments.
          */
         attrib_alignment_tile_bytes_log2 = MIN2(stencil_tile_bytes_log2, tile_bytes_log2);
      }
      unsigned bank_height_alignment_log2;
      do {
         bank_height_alignment_log2 = terakan_image_macro_tile_bank_height_alignment_log2(
            &physical_device->tiling_info, attrib_alignment_tile_bytes_log2, bank_extent_log2[0]);
         if (tile_bytes_log2 + bank_extent_log2[0] +
                   MAX2(bank_extent_log2[1], bank_height_alignment_log2) <=
                physical_device->tiling_info.row_bytes_log2 ||
             bank_extent_log2[0] == 0) {
            break;
         }
         --bank_extent_log2[0];
      } while (true);
      bank_extent_log2[1] = MAX2(bank_extent_log2[1], bank_height_alignment_log2);
      while (tile_bytes_log2 + bank_extent_log2[0] + bank_extent_log2[1] >
                physical_device->tiling_info.row_bytes_log2 &&
             bank_extent_log2[1] > bank_height_alignment_log2) {
         --bank_extent_log2[1];
      }
      assert(tile_bytes_log2 + bank_extent_log2[0] + bank_extent_log2[1] <=
             physical_device->tiling_info.row_bytes_log2);

      /* Macro-tile aspect ratio. */

      unsigned macro_tile_aspect_ratio_log2 = 0;

      /* According to the R800 AddrLib:
       *
       *     "width alignment = 8 * num_pipes * bank_width * macro_aspect_ratio
       *     height alignment = (8 * num_banks * bank_height) / macro_aspect_ratio
       *
       *     For some memory pressure case, we may want to increase macro_aspect_ratio to decrease
       *     height_align. e.g. set this for 2560x1600 8XAA HDR/Z buffer"
       */

      /* R800 AddrLib recommends not to perform optimization for space, including adjusting the
       * macro-tile aspect ratio, for swapchain images.
       */
      struct wsi_image_create_info const * const wsi_info =
         vk_find_struct_const(image_create_info->pNext, WSI_IMAGE_CREATE_INFO_MESA);
      bool const wsi_scanout = wsi_info != NULL && wsi_info->scanout;
      bool const optimize_for_space = !wsi_scanout;
      uint8_t const * const block_texels_log2 =
         terascale_format_block_texels_log2[aspect_data_format];
      /* TODO(Triang3l): Investigate how compatible everything involving this size is with
       * maintenance4's requirement that "The size memory requirement of a buffer or image is never
       * greater than that of another buffer or image created with a greater or equal size".
       */
      uint32_t const width_blocks =
         DIV_ROUND_UP(image_create_info->extent.width, 1u << block_texels_log2[0]);
      uint32_t const height_blocks =
         DIV_ROUND_UP(image_create_info->extent.height, 1u << block_texels_log2[1]);
      if (optimize_for_space) {
         uint32_t width_alignment =
            (uint32_t)8 << (physical_device->tiling_info.pipes_log2 + bank_extent_log2[0]);
         uint32_t height_alignment =
            (uint32_t)8 << (physical_device->tiling_info.banks_log2 + bank_extent_log2[1]);
         /* Search for an "optimal" macro-tile aspect ratio for saving space. */
         while (!(width_blocks & ((width_alignment << 1) - 1)) &&
                (height_blocks & (height_alignment - 1)) && macro_tile_aspect_ratio_log2 < 2) {
            ++macro_tile_aspect_ratio_log2;
            width_alignment <<= 1;
            height_alignment >>= 1;
         }
         if ((height_blocks & (height_alignment - 1)) && macro_tile_aspect_ratio_log2 < 2) {
            uint32_t const actual_size = ALIGN_POT(width_blocks, width_alignment) *
                                         ALIGN_POT(height_blocks, height_alignment);
            /* Try increasing the macro-tile aspect ratio to see if more space can be saved. */
            uint32_t const new_actual_size = ALIGN_POT(width_blocks, width_alignment << 1) *
                                             ALIGN_POT(height_blocks, height_alignment >> 1);
            if (new_actual_size < actual_size) {
               ++macro_tile_aspect_ratio_log2;
            }
         }
      }

      /* TODO(Triang3l): "For fmask used as texture, default ratio(1) is not enough when fmask is
       * treated as a 8bit texture. TC seems to expect ratio to be at least 2" from the R800
       * AddrLib.
       */

      if (!used_by_db) {
         /* In R800 AddrLib, one case is for `flags.texture` (conceptually likely meaning optimal
          * for random access), another is for `flags.color`, though for multisampled images, R800
          * AddrLib sets `texture` to 0 and `color` to 1 during tile split computation to "avoid
          * different tile_split" - "reported by DXX who is doing an MSAA color buffer to MSAA
          * texture memcpy".
          */
         if ((image_create_info->usage &
              (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) &&
             samples_log2 == 0) {
            if (bank_extent_log2[1] >= 2) {
               macro_tile_aspect_ratio_log2 = 1;
            }
         } else {
            if (bank_extent_log2[1] == 2) {
               macro_tile_aspect_ratio_log2 = 1;
            }
         }
      }

      macro_tile_aspect_ratio_log2 =
         MIN2(physical_device->tiling_info.banks_log2, macro_tile_aspect_ratio_log2);

      if (samples_log2 == 0) {
         unsigned const macro_tile_aspect_ratio_alignment_log2 =
            terakan_image_macro_tile_aspect_ratio_single_sample_alignment_log2(
               &physical_device->tiling_info, attrib_alignment_tile_bytes_log2,
               bank_extent_log2[0]);
         macro_tile_aspect_ratio_log2 =
            MAX2(macro_tile_aspect_ratio_log2, macro_tile_aspect_ratio_alignment_log2);
      }

      /* Try to degrade to 1D (AddrLib DegradeTo1D) to save space. Multisampled images must be
       * 2D-tiled, however. AddrLib also doesn't perform this optimization for swapchain images.
       */
      if (optimize_for_space && samples_log2 == 0) {
         struct terakan_image_alignments macro_tiled_alignments = terakan_image_alignments_2d_thin(
            &physical_device->tiling_info, tile_bytes_log2, bank_extent_log2[0],
            bank_extent_log2[1], macro_tile_aspect_ratio_log2);
         if (wsi_scanout) {
            macro_tiled_alignments.pitch_surfels = MAX2(
               macro_tiled_alignments.pitch_surfels, TERAKAN_IMAGE_SCANOUT_PITCH_ALIGNMENT_PIXELS);
         }
         /* Width or height below the alignment, or aligned size > 1.5 * unaligned size. */
         if (width_blocks < macro_tiled_alignments.pitch_surfels ||
             height_blocks < macro_tiled_alignments.height_blocks ||
             2 * ALIGN_POT(width_blocks, macro_tiled_alignments.pitch_surfels) *
                   (uint64_t)ALIGN_POT(height_blocks, macro_tiled_alignments.height_blocks) >
                3 * width_blocks * (uint64_t)height_blocks) {
            array_mode = V_028C70_ARRAY_1D_TILED_THIN1;
         }
         /* AddrLib's OptimizeTileMode has additional cases when degradation to 1D happens for
          * alignment minimization, but RadeonSI doesn't enable them.
          */
      }

      if (array_mode == V_028C70_ARRAY_2D_TILED_THIN1) {
         tiling_out->attrib_tile_split = tile_split_bytes_log2 - 6;
         tiling_out->attrib_bank_width = bank_extent_log2[0];
         tiling_out->attrib_bank_height = bank_extent_log2[1];
         tiling_out->attrib_macro_tile_aspect = macro_tile_aspect_ratio_log2;
      }
   }

   tiling_out->tc_non_display = terakan_image_compute_tc_non_display(
      format_info, aspect_index, physical_device->chip_info.is_r9xx,
      array_mode <= V_028C70_ARRAY_LINEAR_ALIGNED);

   return array_mode;
}

/* On failure, the output is left in an undefined state. */
static void
terakan_image_surface_aspect_compute(VkImageCreateInfo const * const image_create_info,
                                     enum terascale_format_index const aspect_format,
                                     struct terakan_physical_device const * const physical_device,
                                     struct terakan_image_surface_tiling const tiling,
                                     uint8_t const base_level_array_mode,
                                     bool const is_combined_depth_stencil_used_by_db,
                                     uint32_t const offset_in_memory_lower_bound_bytes_shr8,
                                     struct terakan_image_surface_aspect * const surface_aspect_out)
{
   unsigned const bytes_per_block = terascale_format_bytes_per_block[aspect_format];
   assert(bytes_per_block != 0);
   surface_aspect_out->bytes_per_block = bytes_per_block;
   unsigned const surfels_per_block = terakan_format_surfels_per_block(bytes_per_block);
   unsigned const bytes_per_surfel = bytes_per_block / surfels_per_block;

   surface_aspect_out->tiling = tiling;

   uint8_t const * const block_texels_log2 = terascale_format_block_texels_log2[aspect_format];
   unsigned const block_width = 1u << block_texels_log2[0];
   unsigned const block_height = 1u << block_texels_log2[1];

   struct terakan_image_alignments level_alignments;
   switch (base_level_array_mode) {
   case V_028C70_ARRAY_1D_TILED_THIN1:
      level_alignments = terakan_image_alignments_1d_thin(
         physical_device->tiling_info.pipe_interleave_bytes_log2, util_logbase2(bytes_per_block),
         is_combined_depth_stencil_used_by_db);
      break;
   case V_028C70_ARRAY_2D_TILED_THIN1:
      level_alignments = terakan_image_alignments_2d_thin(
         &physical_device->tiling_info,
         terakan_image_macro_tile_bytes_log2(util_logbase2(bytes_per_block),
                                             util_logbase2((uint32_t)image_create_info->samples),
                                             6 + tiling.attrib_tile_split),
         tiling.attrib_bank_width, tiling.attrib_bank_height, tiling.attrib_macro_tile_aspect);
      break;
   default:
      assert(base_level_array_mode == V_028C70_ARRAY_LINEAR_ALIGNED);
      level_alignments = terakan_image_alignments_linear(
         physical_device->tiling_info.pipe_interleave_bytes_log2, bytes_per_block);
      break;
   }
   assert(level_alignments.base_bytes >= 0x100);
   surface_aspect_out->alignment_bytes_shr8 = level_alignments.base_bytes >> 8;

   uint32_t level_offset_in_aspect_bytes_shr8 = 0;

   uint8_t level_array_mode = base_level_array_mode;
   bool alignments_are_for_1d_mips = false;

   unsigned const mip_surface_power_of_two_axes =
      image_create_info->imageType == VK_IMAGE_TYPE_3D ? 3 : 2;

   assert(image_create_info->mipLevels <= ARRAY_SIZE(surface_aspect_out->levels));
   for (uint32_t level_index = 0; level_index < image_create_info->mipLevels; ++level_index) {
      struct terakan_image_surface_level * const level = &surface_aspect_out->levels[level_index];

      /* Aspect offset in memory will be added later. */
      level->offset_in_memory_bytes_shr8 = level_offset_in_aspect_bytes_shr8;

      /* From AddrLib's Lib::ComputeSurfaceInfo:
       *
       *     "For 96 bit surface, the pixelPitch returned might be an odd number, but it is okay to
       *     program texture pitch as HW's mip calculator would multiply 3 first, then do the
       *     appropriate paddings (linear alignment requirement and possible the nearest
       *     power-of-two for mipmaps), which results in the original pitch."
       */

      uint32_t level_extent_compute_pixels_expand_3x[] = {
         surfels_per_block * u_minify(image_create_info->extent.width, level_index),
         u_minify(image_create_info->extent.height, level_index),
         image_create_info->imageType == VK_IMAGE_TYPE_3D
            ? u_minify(image_create_info->extent.depth, level_index)
            : image_create_info->arrayLayers,
      };
      /* TODO(Triang3l): Research power of two padding of the array slice count. */
      if (level_index != 0) {
         for (unsigned axis = 0; axis < mip_surface_power_of_two_axes; ++axis) {
            level_extent_compute_pixels_expand_3x[axis] =
               util_next_power_of_two(level_extent_compute_pixels_expand_3x[axis]);
         }
      }

      /* TODO(Triang3l): Accept the level 0 pitch externally, from BO metadata. */
      uint32_t const level_width_compute_surfels =
         DIV_ROUND_UP(level_extent_compute_pixels_expand_3x[0], block_width);
      uint32_t const level_height_compute_blocks =
         DIV_ROUND_UP(level_extent_compute_pixels_expand_3x[1], block_height);

      if (level_index != 0) {
         if (level_array_mode == V_028C70_ARRAY_2D_TILED_THIN1) {
            /* Check if TC degrades tiling to 1D starting from this level.
             *
             * The condition matches AddrLib's EgBasedLib::ComputeSurfaceMipLevelTileMode and also
             * the kernel validation (evergreen_cs_track_validate_texture).
             *
             * Note that EgBasedLib::ComputeSurfaceMipLevelTileMode also contains two conditions
             * that are OR'd, which are missing from DRM Radeon 2.50.0:
             * (1) pipe interleave * bank interleave >
             *     bytes per tile * pipes * bank width * macro-tile aspect ratio
             * (2) pipe interleave * bank interleave >
             *     bytes per tile * bank width * bank height
             * However, there should likely never be mismatch between AddrLib's and DRM
             * Radeon 2.50.0's computations, as (1) seems to never be true if the macro-tile aspect
             * ratio is properly aligned, and (2) is never true if the bank height is - the
             * inequalities become (1 > macro-tile aspect ratio / macro-tile aspect ratio alignment)
             * and (1 > bank height / bank height alignment) respectively (although disregarding the
             * clamping to the lower bound of 1 done in alignment computations, not sure if it may
             * have an effect here).
             */
            if (level_width_compute_surfels < level_alignments.pitch_surfels ||
                level_height_compute_blocks < level_alignments.height_blocks) {
               level_array_mode = V_028C70_ARRAY_1D_TILED_THIN1;
            }
         }
         if (level_array_mode == V_028C70_ARRAY_1D_TILED_THIN1 && !alignments_are_for_1d_mips) {
            /* Calculate the alignments for the degraded mips, and also remove the depth base level
             * pitch overalignment if present as TC calculates the pitch implicitly for mips.
             */
            level_alignments = terakan_image_alignments_1d_thin(
               physical_device->tiling_info.pipe_interleave_bytes_log2,
               util_logbase2(bytes_per_block), false);
            alignments_are_for_1d_mips = true;
            assert(level_alignments.base_bytes >= 0x100);
            surface_aspect_out->alignment_bytes_shr8 =
               MAX2(level_alignments.base_bytes >> 8, surface_aspect_out->alignment_bytes_shr8);
         }
      }
      level->array_mode = level_array_mode;

      level->aligned_extent_surfels[0] =
         (uint16_t)ALIGN_POT(level_width_compute_surfels, level_alignments.pitch_surfels);
      level->aligned_extent_surfels[1] =
         (uint16_t)ALIGN_POT(level_height_compute_blocks, level_alignments.height_blocks);
      level->aligned_extent_surfels[2] = (uint16_t)level_extent_compute_pixels_expand_3x[2];
      if (level_index == 0) {
         if (surfels_per_block == 3) {
            /* Hardware image descriptors have the base level pitch specified with a granularity of
             * 8 texels, so for 3x-expanded formats, the base level pitch in surfels must be a
             * multiple of both 24 and `level_alignments.pitch_surfels`.
             *
             * In the AddrLib, this is covered by `Lib::HwlPreHandleBaseLvl3xPitch`
             * dividing (rounding down) the pitch in surfels by 3, and then rounding the number of
             * texels up to a power of 2. Here the alignment is ensured more precisely to avoid
             * excessive padding.
             *
             * The least common multiple of 24 and powers of 2 starting from 2^5 is the given power
             * of 2 times 3. On R8xx/R9xx, the pitch alignment for surfaces with 3x-expanded formats
             * (which always are linear) is at least 2^6 surfels.
             */
            uint16_t const expand_3x_pitch_alignment_surfels = 3u * level_alignments.pitch_surfels;
            uint16_t const expand_3x_pitch_misalignment_surfels =
               level->aligned_extent_surfels[0] % expand_3x_pitch_alignment_surfels;
            if (expand_3x_pitch_misalignment_surfels != 0) {
               level->aligned_extent_surfels[0] +=
                  expand_3x_pitch_alignment_surfels - expand_3x_pitch_misalignment_surfels;
            }
         } else {
            struct wsi_image_create_info const * const wsi_info =
               vk_find_struct_const(image_create_info->pNext, WSI_IMAGE_CREATE_INFO_MESA);
            if (wsi_info != NULL && wsi_info->scanout) {
               assert(surfels_per_block == 1);
               assert(block_height == 1);
               assert(block_width == 1);
               level->aligned_extent_surfels[0] = ALIGN_POT(
                  level->aligned_extent_surfels[0], TERAKAN_IMAGE_SCANOUT_PITCH_ALIGNMENT_PIXELS);
            }
         }
      }

      uint64_t const level_slice_size_bytes = bytes_per_surfel * image_create_info->samples *
                                              (uint64_t)level->aligned_extent_surfels[0] *
                                              level->aligned_extent_surfels[1];
      assert(!(level_slice_size_bytes & 0xFF));
      level->slice_size_bytes_shr8 = (uint32_t)(level_slice_size_bytes >> 8);

      level_offset_in_aspect_bytes_shr8 +=
         level->slice_size_bytes_shr8 * level->aligned_extent_surfels[2];
   }

   uint32_t const aspect_offset_in_memory_bytes_shr8 =
      ALIGN_POT(offset_in_memory_lower_bound_bytes_shr8, surface_aspect_out->alignment_bytes_shr8);
   surface_aspect_out->offset_in_memory_bytes_shr8 = aspect_offset_in_memory_bytes_shr8;

   for (uint32_t level_index = 0; level_index < image_create_info->mipLevels; ++level_index) {
      surface_aspect_out->levels[level_index].offset_in_memory_bytes_shr8 +=
         aspect_offset_in_memory_bytes_shr8;
   }

   surface_aspect_out->size_bytes_shr8 = level_offset_in_aspect_bytes_shr8;
}

/* format_info must be the result of terakan_format_info_get for image_create_info->format.
 * On failure, the output is left in an undefined state.
 */
static void
terakan_image_surface_compute(VkImageCreateInfo const * const image_create_info,
                              struct terakan_format_info const * const format_info,
                              struct terakan_physical_device const * const physical_device,
                              struct terakan_image_surface * const surface_out)
{
   /* Simplify handling of missing aspects. */
   memset(surface_out, 0, sizeof(*surface_out));

   surface_out->alignment_bytes_shr8 = 1;
   surface_out->size_bytes_shr8 = 0;

   VkImageAspectFlagBits const * const aspect_map_aspects =
      terakan_format_aspect_map_aspects[format_info->aspect_map];

   bool const is_combined_depth_stencil_used_by_db =
      format_info->aspect_map == TERAKAN_FORMAT_ASPECT_MAP_0_DEPTH_1_STENCIL &&
      (image_create_info->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);

   for (unsigned aspect_index = 0; aspect_index < TERAKAN_FORMAT_MAX_ASPECTS; ++aspect_index) {
      VkImageAspectFlagBits const aspect = aspect_map_aspects[aspect_index];
      if (aspect == VK_IMAGE_ASPECT_NONE) {
         break;
      }
      struct terakan_image_surface_aspect * const surface_aspect =
         &surface_out->aspects[aspect_index];
      struct terakan_image_surface_tiling aspect_tiling;
      uint8_t aspect_array_mode;
      if (is_combined_depth_stencil_used_by_db && aspect_index == 1) {
         /* Depth and stencil have common registers for most of the surface parameters except for
          * the tile split, though tile split is computed the same by
          * terakan_image_surface_tiling_compute.
          */
         aspect_tiling = surface_out->aspects[0].tiling;
         aspect_array_mode = surface_out->aspects[0].levels[0].array_mode;
      } else {
         aspect_array_mode = terakan_image_surface_tiling_compute(
            image_create_info, format_info, aspect_index, physical_device, &aspect_tiling);
      }
      terakan_image_surface_aspect_compute(
         image_create_info, format_info->aspect_formats[aspect_index].format, physical_device,
         aspect_tiling, aspect_array_mode, is_combined_depth_stencil_used_by_db,
         surface_out->size_bytes_shr8, surface_aspect);
      surface_out->alignment_bytes_shr8 =
         MAX2(surface_aspect->alignment_bytes_shr8, surface_out->alignment_bytes_shr8);
      surface_out->size_bytes_shr8 =
         surface_aspect->offset_in_memory_bytes_shr8 + surface_aspect->size_bytes_shr8;
   }

   /* TODO(Triang3l): Intermediate depth surface for the largest 1D tiled mip whose pitch alignment
    * is below that of stencil. Need to be careful with maintenance4's requirement that "The size
    * memory requirement of a buffer or image is never greater than that of another buffer or image
    * created with a greater or equal size."
    */
   /* TODO(Triang3l): Attachment compression metadata. */
}

VKAPI_ATTR void VKAPI_CALL
terakan_GetDeviceImageMemoryRequirements(VkDevice const deviceHandle,
                                         VkDeviceImageMemoryRequirements const * const pInfo,
                                         VkMemoryRequirements2 * const pMemoryRequirements)
{
   struct terakan_device const * const device = terakan_device_from_handle(deviceHandle);
   struct terakan_physical_device const * const physical_device =
      terakan_device_physical_device(device);

   struct terakan_format_info format_info;
   if (likely(terakan_format_info_get(pInfo->pCreateInfo->format, &format_info))) {
      struct terakan_image_surface surface;
      terakan_image_surface_compute(pInfo->pCreateInfo, &format_info, physical_device, &surface);
      pMemoryRequirements->memoryRequirements.size = (VkDeviceSize)surface.size_bytes_shr8 << 8;
      pMemoryRequirements->memoryRequirements.alignment = (VkDeviceSize)surface.alignment_bytes_shr8
                                                          << 8;
   } else {
      vk_loge(VK_LOG_OBJS(terakan_device_log_obj(device)),
              "Terakan vkGetDeviceImageMemoryRequirements: Image format %s is not supported",
              vk_Format_to_str(pInfo->pCreateInfo->format));
      pMemoryRequirements->memoryRequirements.size = 1;
      pMemoryRequirements->memoryRequirements.alignment = 1;
   }

   pMemoryRequirements->memoryRequirements.memoryTypeBits =
      ((uint32_t)1 << physical_device->memory_properties.memoryTypeCount) - 1;

   vk_foreach_struct (ext, pMemoryRequirements->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS: {
         VkMemoryDedicatedRequirements * const dedicated_requirements =
            (VkMemoryDedicatedRequirements *)ext;
         VkExternalMemoryImageCreateInfo const * const external_memory_info =
            vk_find_struct_const(pInfo->pNext, EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
         dedicated_requirements->requiresDedicatedAllocation =
            external_memory_info != NULL && external_memory_info->handleTypes != 0;
         dedicated_requirements->prefersDedicatedAllocation =
            dedicated_requirements->requiresDedicatedAllocation;
      } break;

      default:
         break;
      }
   }
}

/* Skipping the translation into the surface structure as it has already been done. */
VKAPI_ATTR void VKAPI_CALL
terakan_GetImageMemoryRequirements2(VkDevice const deviceHandle,
                                    VkImageMemoryRequirementsInfo2 const * const pInfo,
                                    VkMemoryRequirements2 * const pMemoryRequirements)
{
   struct terakan_image const * const image = terakan_image_from_handle(pInfo->image);

   /* TODO(Triang3l): For images with tiling overridden by binding a BO with metadata, provide the
    * initial requirements rather than the one for the imported surface.
    *
    * Section 12.8. "Resource Memory Association" of the Vulkan 1.3.281 specification says:
    *
    *     "If the maintenance4 feature is enabled, then the alignment member is identical for all
    *     VkImage objects created with the same combination of values for the flags, imageType,
    *     format, extent, mipLevels, arrayLayers, samples, tiling and usage members in the
    *     VkImageCreateInfo structure passed to vkCreateImage."
    *
    *     "The size member is identical for all VkImage objects created with the same combination of
    *     creation parameters specified in VkImageCreateInfo and its pNext chain."
    */
   pMemoryRequirements->memoryRequirements.size = (VkDeviceSize)image->surface.size_bytes_shr8 << 8;
   pMemoryRequirements->memoryRequirements.alignment =
      (VkDeviceSize)image->surface.alignment_bytes_shr8 << 8;

   struct terakan_physical_device const * const physical_device =
      terakan_device_physical_device(terakan_device_from_handle(deviceHandle));
   pMemoryRequirements->memoryRequirements.memoryTypeBits =
      ((uint32_t)1 << physical_device->memory_properties.memoryTypeCount) - 1;

   vk_foreach_struct (ext, pMemoryRequirements->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS: {
         VkMemoryDedicatedRequirements * const dedicated_requirements =
            (VkMemoryDedicatedRequirements *)ext;
         dedicated_requirements->requiresDedicatedAllocation = image->vk.external_handle_types != 0;
         dedicated_requirements->prefersDedicatedAllocation =
            dedicated_requirements->requiresDedicatedAllocation;
      } break;

      default:
         break;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
terakan_GetImageSubresourceLayout(UNUSED VkDevice const device, VkImage const imageHandle,
                                  VkImageSubresource const * pSubresource,
                                  VkSubresourceLayout * const pLayout)
{
   struct terakan_image const * const image = terakan_image_from_handle(imageHandle);

   unsigned const aspect_index =
      terakan_format_aspect_index(image->format_info.aspect_map, pSubresource->aspectMask, 0);
   struct terakan_image_surface_aspect const * const surface_aspect =
      &image->surface.aspects[aspect_index];
   struct terakan_image_surface_level const * const surface_level =
      &surface_aspect->levels[pSubresource->mipLevel];

   VkDeviceSize const slice_size = (VkDeviceSize)surface_level->slice_size_bytes_shr8 << 8;

   pLayout->offset = ((VkDeviceSize)surface_level->offset_in_memory_bytes_shr8 << 8) +
                     slice_size * pSubresource->arrayLayer;

   pLayout->size = slice_size;
   if (image->vk.image_type == VK_IMAGE_TYPE_3D) {
      pLayout->size *= u_minify(image->vk.extent.depth, pSubresource->mipLevel);
   }

   pLayout->rowPitch = (surface_aspect->bytes_per_block /
                        terakan_format_surfels_per_block(surface_aspect->bytes_per_block)) *
                       (uint32_t)surface_level->aligned_extent_surfels[0];

   pLayout->arrayPitch = slice_size;
   pLayout->depthPitch = slice_size;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_BindImageMemory2(UNUSED VkDevice const device, uint32_t const bindInfoCount,
                         VkBindImageMemoryInfo const * const pBindInfos)
{
   for (uint32_t bind_info_index = 0; bind_info_index < bindInfoCount; ++bind_info_index) {
      VkBindImageMemoryInfo const * const bind_info = &pBindInfos[bind_info_index];
      struct terakan_image * const image = terakan_image_from_handle(bind_info->image);
      image->bo = terakan_device_memory_from_handle(bind_info->memory)->bo;
      image->va = image->bo->va + bind_info->memoryOffset;
   }

   return VK_SUCCESS;
}

bool
terakan_image_descriptor_subresource_range_sanitize(
   struct terakan_image const * const image,
   struct terakan_image_descriptor_subresource_range * const subresource_range,
   bool const is_cube_view)
{
   /* Validate, without modifying the create info structure. */
   if (subresource_range->max_level_count == 0 ||
       subresource_range->base_mip_level >= image->vk.mip_levels) {
      return false;
   }
   uint32_t const level_layer_count =
      terakan_image_depth_or_array_layers(image, subresource_range->base_mip_level);
   if (subresource_range->base_z_or_array_layer >= level_layer_count) {
      return false;
   }
   uint32_t descriptor_layer_count =
      MIN2(subresource_range->max_depth_or_layer_count,
           level_layer_count - subresource_range->base_z_or_array_layer);
   if (is_cube_view) {
      descriptor_layer_count = descriptor_layer_count / 6u * 6u;
   }
   if (descriptor_layer_count == 0) {
      return false;
   }

   /* Normalize. */
   subresource_range->max_level_count = MIN2(
      subresource_range->max_level_count, image->vk.mip_levels - subresource_range->base_mip_level);
   subresource_range->max_depth_or_layer_count = descriptor_layer_count;
   return true;
}

bool
terakan_image_create_resource_descriptor(
   struct terakan_image_descriptor_create_info const * const descriptor_create_info,
   uint32_t const desired_dimensionality, VkComponentMapping const * const component_mapping_opt,
   struct terakan_resource_descriptor * const descriptor_out)
{
   struct terascale_format_info const view_format = descriptor_create_info->view_format;

   if (!view_format.supports_sq_texture_fetch) {
      return false;
   }

   struct terakan_image const * const image = descriptor_create_info->image;

   struct terakan_image_surface_aspect const * const surface_aspect =
      &image->surface.aspects[descriptor_create_info->image_aspect_index];
   /* #MemoryIntegrity. */
   if (unlikely(surface_aspect->bytes_per_block !=
                terascale_format_bytes_per_block[view_format.format])) {
      return false;
   }

   /* Whether it's possible to create the descriptor only for a single mip level (`base_mip_level`
    * in the subresource range in the create info) because the view needs memory addressing for
    * `base_mip_level` that's different from what would be implicitly calculated by the texture
    * fetch hardware for this descriptor if the base address pointed to the actual level 0 and the
    * mip address pointed to the level 1.
    *
    * In this case, only one mip level will be readable through this descriptor, and the data
    * addresses and the base mip level index in the descriptor may be adjusted so that texture
    * fetches from the descriptor will be done with the memory addressing needed for the view.
    *
    * This functionality must be used only in cases where the client API doesn't require more than 1
    * mip level to be accessible via the view.
    */
   bool extract_single_level = false;

   /* Check if creating a size-compatible uncompressed view of a compressed image, for purposes of
    * the client API (see `VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT`) or transfer operation
    * implementation.
    */
   uint8_t const * const image_block_texels_log2 = terascale_format_block_texels_log2
      [image->format_info.aspect_formats[descriptor_create_info->image_aspect_index].format];
   uint8_t const * const view_block_texels_log2 =
      terascale_format_block_texels_log2[view_format.format];
   bool const is_size_compatible_uncompressed_view =
      view_block_texels_log2[0] != image_block_texels_log2[0] ||
      view_block_texels_log2[1] != image_block_texels_log2[1];
   if (is_size_compatible_uncompressed_view) {
      if (unlikely(view_block_texels_log2[0] != 0 || view_block_texels_log2[1] != 0)) {
         return false;
      }
      /* Minification for compressed textures and their size-compatible uncompressed views differs.
       * For example, a 10x4-texel (3x1-block) BC texture has:
       * - Mip 1: 2x1 blocks (5x2 texels);
       * - Mip 2: 1x1 blocks (2x1 texels);
       * - Mip 3: 1x1 blocks (1x1 texels).
       * However, a 3x1-texel uncompressed texture only has:
       * - Mip 1: 1x1.
       *
       * In Vulkan, restricting the view to a single level in this case is possible due to
       * VUID-VkImageViewCreateInfo-image-07072:
       *     "If `image` was created with the `VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT` flag
       *     and `format` is a non-compressed format, the `levelCount` member of `subresourceRange`
       *     must be 1"
       *
       * This case also occurs in transfer implementations in the driver, which access only one
       * level.
       */
      extract_single_level = true;
   }

   /* Handle the dimensionality. */

   /* Minification differs for arrays (layer count isn't minified) and 3D (depth is minified). */
   if (desired_dimensionality == V_030000_SQ_TEX_DIM_3D) {
      if (image->vk.array_layers > 1) {
         /* 3D views aren't compatible with 1D or 2D textures in Vulkan at all, so the behavior for
          * 3D views of array textures doesn't matter for Vulkan image views.
          */
         extract_single_level = true;
      }
   } else {
      if (image->vk.extent.depth > 1) {
         /* In Vulkan, restricting the view to a single level for non-3D views of 3D images
          * (particularly, only 2D and 3D views are compatible with them) is possible due to
          * VUID-VkImageViewCreateInfo-image-04970:
          *     "If `image` was created with `VK_IMAGE_TYPE_3D` and `viewType` is
          *     `VK_IMAGE_VIEW_TYPE_2D` or `VK_IMAGE_VIEW_TYPE_2D_ARRAY` then
          *     `subresourceRange.levelCount` must be 1"
          *
          * This case also occurs in transfer implementations in the driver, which access only one
          * level.
          */
         extract_single_level = true;
      }
   }

   uint32_t const origin_level =
      extract_single_level ? descriptor_create_info->subresource_range.base_mip_level : 0;

   uint32_t origin_level_view_width = u_minify(image->vk.extent.width, origin_level);
   uint32_t origin_level_view_height = u_minify(image->vk.extent.height, origin_level);
   if (is_size_compatible_uncompressed_view) {
      origin_level_view_width =
         (origin_level_view_width + ((1u << image_block_texels_log2[0]) - 1u)) >>
         image_block_texels_log2[0];
      origin_level_view_height =
         (origin_level_view_height + ((1u << image_block_texels_log2[1]) - 1u)) >>
         image_block_texels_log2[1];
   }
   uint32_t const origin_level_depth_or_array_layers =
      terakan_image_depth_or_array_layers(image, origin_level);

   struct terakan_image_surface_level const * const surface_origin_level =
      &surface_aspect->levels[origin_level];

   uint32_t hw_dimensionality;
   bool const is_multisampled = desired_dimensionality == V_030000_SQ_TEX_DIM_2D_MSAA ||
                                desired_dimensionality == V_030000_SQ_TEX_DIM_2D_ARRAY_MSAA;
   if (is_multisampled) {
      if (image->vk.samples <= VK_SAMPLE_COUNT_1_BIT) {
         return false;
      }
      hw_dimensionality = origin_level_depth_or_array_layers > 1 ? V_030000_SQ_TEX_DIM_2D_ARRAY_MSAA
                                                                 : V_030000_SQ_TEX_DIM_2D_MSAA;
   } else {
      if (image->vk.samples > VK_SAMPLE_COUNT_1_BIT) {
         return false;
      }
      switch (desired_dimensionality) {
      case V_030000_SQ_TEX_DIM_1D:
      case V_030000_SQ_TEX_DIM_1D_ARRAY:
         if (surface_origin_level->array_mode <= V_028C70_ARRAY_LINEAR_ALIGNED &&
             likely(origin_level_view_height <= 1)) {
            hw_dimensionality = origin_level_depth_or_array_layers > 1
                                   ? V_030000_SQ_TEX_DIM_1D_ARRAY
                                   : V_030000_SQ_TEX_DIM_1D;
            break;
         }
         /* Promote images not compatible with 1D descriptors to 2D. */
         FALLTHROUGH;
      case V_030000_SQ_TEX_DIM_2D:
      case V_030000_SQ_TEX_DIM_2D_ARRAY:
         hw_dimensionality = origin_level_depth_or_array_layers > 1 ? V_030000_SQ_TEX_DIM_2D_ARRAY
                                                                    : V_030000_SQ_TEX_DIM_2D;
         break;
      case V_030000_SQ_TEX_DIM_3D:
         hw_dimensionality = V_030000_SQ_TEX_DIM_3D;
         break;
      case V_030000_SQ_TEX_DIM_CUBEMAP:
         if (unlikely(origin_level_view_width != origin_level_view_height)) {
            return false;
         }
         hw_dimensionality = V_030000_SQ_TEX_DIM_CUBEMAP;
         break;
      default:
         assert(!"Unsupported image resource descriptor dimensionality");
         return false;
      }
   }

   uint32_t descriptor_width = origin_level_view_width;
   uint32_t descriptor_height = origin_level_view_height;
   uint32_t descriptor_depth_or_array_layers = origin_level_depth_or_array_layers;
   if (origin_level != 0) {
      /* The base level and mips are stored differently - the hardware rounds the extents up to
       * powers of 2 in memory addressing for mips, but not for the base level.
       *
       * In color target descriptors, the necessary padding is added to `PITCH_TILE_MAX` and
       * `SLICE_TILE_MAX`. However, sampled image descriptors have only the row pitch of the base
       * level set directly - the slice pitch is implicitly computed by texture fetch hardware from
       * the height.
       *
       * Because of this, to make sure multiple slices are addressed correctly using a descriptor
       * for a single non-base mip level with addressing that differs from the regular texel
       * dimension minification behavior, the level must be bound as a mip, not as the base level.
       *
       * This is done by binding the mip to be accessed as level 1. For that, a fake, inaccessible
       * base level is set up in the descriptor - by setting the image dimensions in the descriptor
       * to those that would be minified to the real dimensions of the bound level when it's
       * accessed as level 1 via the descriptor.
       *
       * The fake base level must be as small as possible, to make sure the total memory footprint
       * of the fake base level never exceeds the total size of the image, otherwise the kernel
       * driver may refuse the submission due to the BO size being insufficient for the image.
       *
       * Relevant CTS tests:
       * dEQP-VK.api.copy_and_blit.core.image_to_buffer.2d_images.mip_copies_bc*_universal
       * (the copy source being a block-compressed 64x192 array image with mips - assuming that the
       * implementation of `vkCmdCopyImageToBuffer` reads from a size-compatible uncompressed format
       * SQ texture resource).
       */
      if (origin_level_view_width > 1) {
         descriptor_width = origin_level_view_width << 1;
      }
      if (origin_level_view_height > 1) {
         /* Note that for size-compatible uncompressed views of compressed images, doubling the
          * height in blocks may increase the slice pitch more than doubling the height in texels.
          *
          * For example, with 4-texel blocks, 5 texels require 2 blocks, but 10 texels need only 3
          * rather than 4.
          *
          * Thankfully, the hardware requires tiling for all formats with a block height of more
          * than 1 (specifically, BC-compressed textures with a block height of 4), so the height is
          * always a multiple of 8 blocks in slice pitch calculation.
          *
          * In this case, doubling the height in blocks never increases the slice pitch more than
          * doubling the height in texels, which can be verified by the following code:
          *
          *     unsigned const block_size = 4;
          *     unsigned const extent_alignment = TERAKAN_IMAGE_MICRO_TILE_WIDTH_HEIGHT;
          *     for (unsigned size = 1; size <= TERAKAN_IMAGE_MAX_WIDTH_HEIGHT; ++size) {
          *        unsigned const real_blocks = DIV_ROUND_UP(size, block_size);
          *        unsigned const fake_blocks = DIV_ROUND_UP(2 * u_minify(size, 1), block_size);
          *        unsigned const real_extent = DIV_ROUND_UP(real_blocks, extent_alignment);
          *        unsigned const fake_extent = DIV_ROUND_UP(fake_blocks, extent_alignment);
          *        assert(real_extent >= fake_extent);
          *     }
          *
          * Moreover, this is also true for any block size, and for any even storage extent
          * alignment.
          */
         descriptor_height = origin_level_view_height << 1;
      }
      if (hw_dimensionality == V_030000_SQ_TEX_DIM_3D && origin_level_depth_or_array_layers > 1) {
         descriptor_depth_or_array_layers = origin_level_depth_or_array_layers << 1;
      }
      if (descriptor_width <= 1 && descriptor_height <= 1 &&
          (hw_dimensionality != V_030000_SQ_TEX_DIM_3D || descriptor_depth_or_array_layers <= 1)) {
         /* A 1x1x1 base wouldn't be minified, make sure at least one coordinate of the descriptor
          * base level is not 1. Increasing the width from 1 to 2 is always safe, because that can
          * be done regardless of the dimensionality, and without affecting row or slice pitch
          * calculations (row pitches are always a multiple of 8 surfels).
          */
         descriptor_width = 2;
         if (hw_dimensionality == V_030000_SQ_TEX_DIM_CUBEMAP) {
            /* Setting the height to 2 is safe here. A cubemap must be square, so if it's minified
             * at all (the extracted level being nonzero), then the previous level is both 2x or
             * 2x+1 wider and taller, it can't be just wider, but not taller.
             *
             * The only case where this rule doesn't apply is a cubemap view of a 3D image (not
             * valid in Vulkan, VUID-VkImageCreateInfo-flags-00949: "If `flags` contains
             * `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT`, `imageType` must be `VK_IMAGE_TYPE_2D`"), if
             * the level is minified from 1x1x2 or 1x1x3, but row pitch can't exceed slice pitch,
             * so a 2x2 slice is always within 2 1x1 slices padded to the slice pitch.
             */
            descriptor_height = 2;
         }
      }
   }

   /* For the fake base level when extracting a single mip, it's okay to keep using the pitch of the
    * real base level, as the minimum pitch actually needed for the width of the fake base never
    * exceeds it.
    *
    * For 3x-expanded formats, it's also pre-aligned to both the hardware linear image
    * pitch alignment requirement as surfels and 8 texels (the granularity of `PITCH` in resource
    * descriptors), while pitches of non-base mips of 3x-expanded format images may not always be
    * representable by the `PITCH` field of the descriptor.
    *
    * Note that macro-tiled images may have a smaller pitch requirement than non-macro-tiled images.
    * For example, for 1 byte per surfel, the pitch alignment for `1D_THIN` with 2^8-byte pipe
    * interleave is 2^5 bytes, but for `2D_THIN` with 2 pipes, 1:1 macro-tile aspect ratio, and bank
    * width of 1, the pitch alignment is 2^4 bytes. Therefore, if the `PITCH` was obtained from the
    * base level, `ARRAY_MODE` must be set to the mode for the base level as well. If the extracted
    * single mip needs to be degraded to non-micro-tiled, it will still be degraded by the texture
    * fetch hardware even if the `ARRAY_MODE` is set to macro-tiled in this case, as whether it's
    * degraded depends on the dimensions of the mip itself in surfels.
    */
   uint32_t descriptor_pitch_texels =
      surface_aspect->levels[0].aligned_extent_surfels[0] /
      terakan_format_surfels_per_block(surface_aspect->bytes_per_block);
   if (!is_size_compatible_uncompressed_view) {
      descriptor_pitch_texels <<= image_block_texels_log2[0];
   }
   assert((descriptor_pitch_texels & 7) == 0);

   uint32_t const image_va_shr8 = (uint32_t)(image->va >> 8);

   struct terakan_physical_device const * const physical_device =
      container_of(image->vk.base.device->physical, struct terakan_physical_device const, vk);

   /* Fill the descriptor. Must not return false from now on. */
   descriptor_out->resource[0] =
      S_030000_DIM(hw_dimensionality) |
      (physical_device->chip_info.is_r9xx
          ? CM_S_030000_NON_DISP_TILING_ORDER(surface_aspect->tiling.tc_non_display)
          : S_030000_NON_DISP_TILING_ORDER(surface_aspect->tiling.tc_non_display)) |
      S_030000_PITCH(descriptor_pitch_texels / 8 - 1) | S_030000_TEX_WIDTH(descriptor_width - 1);
   descriptor_out->resource[1] = S_030004_TEX_HEIGHT(descriptor_height - 1) |
                                 S_030004_TEX_DEPTH(descriptor_depth_or_array_layers - 1) |
                                 S_030004_ARRAY_MODE(surface_aspect->levels[0].array_mode);
   descriptor_out->resource[2] = image_va_shr8 + surface_aspect->offset_in_memory_bytes_shr8;
   descriptor_out->resource[4] =
      S_030010_FORMAT_COMP_X(view_format.channels_signed & (1u << 0)
                                ? V_030010_SQ_FORMAT_COMP_SIGNED
                                : V_030010_SQ_FORMAT_COMP_UNSIGNED) |
      S_030010_FORMAT_COMP_Y(view_format.channels_signed & (1u << 1)
                                ? V_030010_SQ_FORMAT_COMP_SIGNED
                                : V_030010_SQ_FORMAT_COMP_UNSIGNED) |
      S_030010_FORMAT_COMP_Z(view_format.channels_signed & (1u << 2)
                                ? V_030010_SQ_FORMAT_COMP_SIGNED
                                : V_030010_SQ_FORMAT_COMP_UNSIGNED) |
      S_030010_FORMAT_COMP_W(view_format.channels_signed & (1u << 3)
                                ? V_030010_SQ_FORMAT_COMP_SIGNED
                                : V_030010_SQ_FORMAT_COMP_UNSIGNED) |
      S_030010_NUM_FORMAT_ALL(terascale_format_get_sq_num_format(
         (enum terascale_format_number_type)view_format.number_type)) |
      S_030010_SRF_MODE_ALL(V_030010_SRF_MODE_ZERO_CLAMP_MINUS_ONE) |
      S_030010_FORCE_DEGAMMA(view_format.number_type == TERASCALE_FORMAT_NUMBER_TYPE_SRGB) |
      S_030010_ENDIAN_SWAP(terakan_image_is_big_endian(image)
                              ? terascale_format_big_endian_swap[view_format.format]
                              : TERASCALE_ENDIAN_SWAP_NONE);
   if (component_mapping_opt != NULL) {
      descriptor_out->resource[4] |=
         S_030010_DST_SEL_X(terakan_format_apply_component_swizzle(
            view_format, component_mapping_opt->r, VK_COMPONENT_SWIZZLE_R)) |
         S_030010_DST_SEL_Y(terakan_format_apply_component_swizzle(
            view_format, component_mapping_opt->g, VK_COMPONENT_SWIZZLE_G)) |
         S_030010_DST_SEL_Z(terakan_format_apply_component_swizzle(
            view_format, component_mapping_opt->b, VK_COMPONENT_SWIZZLE_B)) |
         S_030010_DST_SEL_W(terakan_format_apply_component_swizzle(
            view_format, component_mapping_opt->a, VK_COMPONENT_SWIZZLE_A));
   } else {
      descriptor_out->resource[4] |=
         S_030010_DST_SEL_X(view_format.swizzle_r) | S_030010_DST_SEL_Y(view_format.swizzle_g) |
         S_030010_DST_SEL_Z(view_format.swizzle_b) | S_030010_DST_SEL_W(view_format.swizzle_a);
   }
   descriptor_out->resource[5] =
      hw_dimensionality == V_030000_SQ_TEX_DIM_3D
         ? S_030014_LAST_ARRAY(descriptor_depth_or_array_layers - 1)
         : S_030014_BASE_ARRAY(descriptor_create_info->subresource_range.base_z_or_array_layer) |
              S_030014_LAST_ARRAY(
                 descriptor_create_info->subresource_range.base_z_or_array_layer +
                 (descriptor_create_info->subresource_range.max_depth_or_layer_count - 1));
   /* TODO(Triang3l): `MIN_LOD` for `VK_EXT_image_view_min_lod` (research the base level and out of
    * bounds behavior).
    */
   descriptor_out->resource[6] = S_030018_TILE_SPLIT(surface_aspect->tiling.attrib_tile_split);
   descriptor_out->resource[7] =
      S_03001C_DATA_FORMAT(view_format.format) |
      S_03001C_MACRO_TILE_ASPECT(surface_aspect->tiling.attrib_macro_tile_aspect) |
      S_03001C_BANK_WIDTH(surface_aspect->tiling.attrib_bank_width) |
      S_03001C_BANK_HEIGHT(surface_aspect->tiling.attrib_bank_height) |
      S_03001C_DEPTH_SAMPLE_ORDER((image->vk.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) !=
                                  0) |
      S_03001C_NUM_BANKS(physical_device->tiling_info.banks_log2 - 1) |
      S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_TEXTURE);
   if (is_multisampled) {
      /* TODO(Triang3l): FMask (address in `MIP_ADDRESS`, `FMASK_BANK_HEIGHT`). */
      descriptor_out->resource[3] = descriptor_out->resource[2];
      unsigned const samples_log2 = util_logbase2((uint32_t)image->vk.samples);
      if (physical_device->chip_info.is_r9xx) {
         descriptor_out->resource[4] |= S_030010_LOG2_NUM_FRAGMENTS(samples_log2);
      }
      /* `LAST_LEVEL` is used for the sample count instead. */
      descriptor_out->resource[5] |= S_030014_LAST_LEVEL(samples_log2);
   } else {
      if (extract_single_level) {
         descriptor_out->resource[3] =
            image_va_shr8 + surface_origin_level->offset_in_memory_bytes_shr8;
         if (origin_level != 0) {
            descriptor_out->resource[4] |= S_030010_BASE_LEVEL(1);
            descriptor_out->resource[5] |= S_030014_LAST_LEVEL(1);
         }
      } else {
         descriptor_out->resource[3] =
            image_va_shr8 +
            surface_aspect->levels[MIN2(image->vk.mip_levels - 1u, 1u)].offset_in_memory_bytes_shr8;
         descriptor_out->resource[4] |=
            S_030010_BASE_LEVEL(descriptor_create_info->subresource_range.base_mip_level);
         descriptor_out->resource[5] |=
            S_030014_LAST_LEVEL(descriptor_create_info->subresource_range.base_mip_level +
                                (descriptor_create_info->subresource_range.max_level_count - 1));
      }
      descriptor_out->resource[6] |= S_030018_MAX_ANISO_RATIO(4);
   }
   return true;
}

uint32_t
terakan_image_create_color_descriptor(
   struct terakan_image_descriptor_create_info const * const descriptor_create_info,
   uint32_t const desired_info_resource_type,
   struct terakan_color_descriptor * const descriptor_out,
   struct terakan_color_meta_descriptor * const meta_descriptor_out_opt)
{
   assert(descriptor_create_info->image_aspect_index < TERAKAN_FORMAT_MAX_ASPECTS);

   struct terascale_format_info const view_format = descriptor_create_info->view_format;

   if (!view_format.supports_cb_color) {
      /* Avoid undefined hardware behavior. */
      return 0;
   }

   struct terakan_image const * const image = descriptor_create_info->image;

   /* Color descriptors support fewer slices than texture resource descriptors, but meta draws may
    * still need to access all the slices. Between the slices, there's bank rotation in the tiling,
    * so it's not possible to just adjust the base pointer directly to `base_z_or_array_layer` all
    * the time, only by numbers of slices aligned to the rotation granularity on the chip.
    * Restrict the descriptor to the range of `TERAKAN_IMAGE_MAX_TARGET_SLICES` slices that includes
    * `base_z_or_array_layer`.
    */
   uint32_t const base_slice_start =
      descriptor_create_info->subresource_range.base_z_or_array_layer &
      ~(uint32_t)(TERAKAN_IMAGE_MAX_TARGET_SLICES - 1);
   uint32_t const view_slice_start =
      descriptor_create_info->subresource_range.base_z_or_array_layer - base_slice_start;
   uint32_t const view_slice_end =
      MIN2(descriptor_create_info->subresource_range.base_z_or_array_layer +
              descriptor_create_info->subresource_range.max_depth_or_layer_count - base_slice_start,
           TERAKAN_IMAGE_MAX_TARGET_SLICES);

   struct terakan_image_surface_aspect const * const surface_aspect =
      &image->surface.aspects[descriptor_create_info->image_aspect_index];
   /* #MemoryIntegrity. */
   if (unlikely(surface_aspect->bytes_per_block !=
                terascale_format_bytes_per_block[descriptor_create_info->view_format.format])) {
      return 0;
   }
   struct terakan_image_surface_level const * const surface_level =
      &surface_aspect->levels[descriptor_create_info->subresource_range.base_mip_level];
   /* Only `LINEAR_ALIGNED` is currently supported for linear, not `LINEAR_GENERAL`. */
   assert(surface_level->array_mode != V_028C70_ARRAY_LINEAR_GENERAL);

   /* Fill the descriptor. Must not return 0 from now on. */

   descriptor_out->base = (uint32_t)(image->va >> 8) + surface_level->offset_in_memory_bytes_shr8 +
                          surface_level->slice_size_bytes_shr8 * base_slice_start;

   descriptor_out->pitch =
      S_028C64_PITCH_TILE_MAX(surface_level->aligned_extent_surfels[0] / 8 - 1);
   descriptor_out->slice =
      S_028C68_SLICE_TILE_MAX((uint32_t)surface_level->aligned_extent_surfels[0] *
                                 surface_level->aligned_extent_surfels[1] / 64 -
                              1);

   descriptor_out->view =
      S_028C6C_SLICE_START(view_slice_start) | S_028C6C_SLICE_MAX(view_slice_end - 1);

   bool const array_mode_is_linear = surface_level->array_mode <= V_028C70_ARRAY_LINEAR_ALIGNED;

   /* Allowing uncompressed views of compressed and subsampled images. */
   uint8_t const * const image_block_texels_log2 = terascale_format_block_texels_log2
      [image->format_info.aspect_formats[descriptor_create_info->image_aspect_index].format];
   uint32_t const descriptor_height_minus_1 =
      DIV_ROUND_UP(u_minify(image->vk.extent.height,
                            descriptor_create_info->subresource_range.base_mip_level),
                   1u << image_block_texels_log2[1]) -
      1;
   descriptor_out->dim =
      S_028C78_WIDTH_MAX(
         DIV_ROUND_UP(u_minify(image->vk.extent.width,
                               descriptor_create_info->subresource_range.base_mip_level),
                      1u << image_block_texels_log2[0]) -
         1) |
      S_028C78_HEIGHT_MAX(descriptor_height_minus_1);

   struct terakan_physical_device const * const physical_device =
      container_of(image->vk.base.device->physical, struct terakan_physical_device const, vk);

   bool blend_clamp = false;
   uint32_t source_format = V_028C70_EXPORT_4C_32BPC;
   switch (view_format.number_type) {
   case TERASCALE_FORMAT_NUMBER_TYPE_UNORM:
   case TERASCALE_FORMAT_NUMBER_TYPE_SNORM:
   case TERASCALE_FORMAT_NUMBER_TYPE_SRGB:
      blend_clamp = true;
      if (TERASCALE_FORMATS_CB_COLOR_EXPORT_16BPC_NORM & BITFIELD64_BIT(view_format.format)) {
         source_format = V_028C70_EXPORT_4C_16BPC;
      }
      break;
   case TERASCALE_FORMAT_NUMBER_TYPE_FLOAT:
      if (TERASCALE_FORMATS_CB_COLOR_EXPORT_16BPC_FLOAT & BITFIELD64_BIT(view_format.format)) {
         source_format = V_028C70_EXPORT_4C_16BPC;
      }
      break;
   default:
      break;
   }
   if (source_format == V_028C70_EXPORT_4C_32BPC && physical_device->chip_info.is_r9xx) {
      uint8_t const export_component_mask = terascale_format_cb_color_export_component_masks
         [terascale_format_channel_count[view_format.format]][view_format.cb_color_swap];
      if (!(export_component_mask & 0b0110)) {
         source_format = V_028C70_EXPORT_2C_32BPC_AR;
      } else if (!(export_component_mask & 0b1100) &&
                 view_format.number_type != TERASCALE_FORMAT_NUMBER_TYPE_UINT &&
                 view_format.number_type != TERASCALE_FORMAT_NUMBER_TYPE_SINT) {
         /* Alpha may be needed for blending or alpha to coverage, so using GR only for integer
          * formats.
          */
         /* TODO(Triang3l): Toggle dynamically based on whether alpha is actually needed. */
         source_format = V_028C70_EXPORT_2C_32BPC_GR;
      }
   }

   uint32_t hw_info_resource_type = desired_info_resource_type;
   /* 1D storage images must be linear according to the Gallium R600 driver.
    * DB, however, requires tiling, so 1D images with depth / stencil usage will be tiled, thus they
    * have to be accessed as 2D, with shaders forcing Y to 0.
    * The 2D fallback can't be used for linear 1D images, however, because according to a comment in
    * the R800 AddrLib, "Tex2D UAV on cypress will fail/hang if tile mode is linear".
    */
   if (!array_mode_is_linear || descriptor_height_minus_1 != 0) {
      if (hw_info_resource_type == V_028C70_TEXTURE1D) {
         hw_info_resource_type = V_028C70_TEXTURE2D;
      } else if (hw_info_resource_type == V_028C70_TEXTURE1DARRAY) {
         hw_info_resource_type = V_028C70_TEXTURE2DARRAY;
      }
   }

   descriptor_out->info =
      S_028C70_ENDIAN(terakan_image_is_big_endian(image)
                         ? terascale_format_big_endian_swap[view_format.format]
                         : TERASCALE_ENDIAN_SWAP_NONE) |
      S_028C70_FORMAT(view_format.format) | S_028C70_ARRAY_MODE(surface_level->array_mode) |
      S_028C70_NUMBER_TYPE(view_format.number_type) |
      S_028C70_COMP_SWAP(view_format.cb_color_swap) | S_028C70_SIMPLE_FLOAT(true) |
      S_028C70_SOURCE_FORMAT(source_format) | S_028C70_RESOURCE_TYPE(hw_info_resource_type);
   if (terascale_format_blend_bypass((enum terascale_format_number_type)view_format.number_type,
                                     (enum terascale_format_index)view_format.format)) {
      descriptor_out->info |= S_028C70_BLEND_BYPASS(true);
   } else {
      descriptor_out->info |= S_028C70_BLEND_CLAMP(blend_clamp);
   }

   unsigned const samples_log2 = util_logbase2((uint32_t)image->vk.samples);
   descriptor_out->attrib =
      S_028C74_NON_DISP_TILING_ORDER(array_mode_is_linear ||
                                     surface_aspect->tiling.tc_non_display) |
      S_028C74_TILE_SPLIT(surface_aspect->tiling.attrib_tile_split) |
      S_028C74_NUM_BANKS(physical_device->tiling_info.banks_log2 - 1) |
      S_028C74_BANK_WIDTH(surface_aspect->tiling.attrib_bank_width) |
      S_028C74_BANK_HEIGHT(surface_aspect->tiling.attrib_bank_height) |
      S_028C74_MACRO_TILE_ASPECT(surface_aspect->tiling.attrib_macro_tile_aspect) |
      S_028C74_FMASK_BANK_HEIGHT(surface_aspect->tiling.attrib_bank_height) |
      S_028C74_NUM_SAMPLES(samples_log2) | S_028C74_NUM_FRAGMENTS(samples_log2);
   /* `FORCE_DST_ALPHA_1` or the equivalent `CB_BLEND_CONTROL` adjustment is not needed as Vulkan
    * doesn't have formats with a void alpha channel or with less than 4 channels that would use a
    * 4-channel one in the hardware.
    */

   if (meta_descriptor_out_opt != NULL) {
      *meta_descriptor_out_opt = terakan_color_meta_descriptor_create_disabled(descriptor_out);
   }

   /* TODO(Triang3l): CMask, FMask. */

   return view_slice_end - view_slice_start;
}

bool
terakan_image_create_depth_stencil_descriptor(
   struct terakan_image const * const image, enum terascale_r8xx_depth_format view_depth_format,
   bool view_may_have_stencil,
   struct terakan_image_descriptor_subresource_range const * const subresource_range,
   struct terakan_depth_stencil_descriptor * const descriptor_out)
{
   /* DB has certain requirements for images beyond those of TC and CB, don't create depth / stencil
    * descriptors for images merely created with a depth / stencil format, but only for non-DB uses.
    */
   if (!(image->vk.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
      return false;
   }

   /* Get the aspects. */
   VkImageAspectFlagBits const * const image_format_aspects =
      terakan_format_aspect_map_aspects[image->format_info.aspect_map];
   unsigned image_depth_aspect_index = TERAKAN_FORMAT_MAX_ASPECTS;
   unsigned image_stencil_aspect_index = TERAKAN_FORMAT_MAX_ASPECTS;
   for (unsigned aspect_index = 0; aspect_index < TERAKAN_FORMAT_MAX_ASPECTS &&
                                   image_format_aspects[aspect_index] != VK_IMAGE_ASPECT_NONE;
        ++aspect_index) {
      switch (image_format_aspects[aspect_index]) {
      case VK_IMAGE_ASPECT_DEPTH_BIT:
         image_depth_aspect_index = aspect_index;
         break;
      case VK_IMAGE_ASPECT_STENCIL_BIT:
         image_stencil_aspect_index = aspect_index;
         break;
      default:
         break;
      }
   }
   struct terakan_image_surface_aspect const * const surface_depth =
      image_depth_aspect_index < TERAKAN_FORMAT_MAX_ASPECTS
         ? &image->surface.aspects[image_depth_aspect_index]
         : NULL;
   struct terakan_image_surface_aspect const * const surface_stencil =
      image_stencil_aspect_index < TERAKAN_FORMAT_MAX_ASPECTS
         ? &image->surface.aspects[image_stencil_aspect_index]
         : NULL;
   if (surface_depth == NULL ||
       unlikely(surface_depth->bytes_per_block !=
                (view_depth_format == TERASCALE_R8XX_DEPTH_FORMAT_16 ? 2 : 4))) {
      view_depth_format = TERASCALE_R8XX_DEPTH_FORMAT_INVALID;
   }
   if (surface_stencil == NULL) {
      view_may_have_stencil = false;
   }
   if (view_depth_format == TERASCALE_R8XX_DEPTH_FORMAT_INVALID && !view_may_have_stencil) {
      return false;
   }
   struct terakan_image_surface_aspect const * const surface_main_aspect =
      view_depth_format != TERASCALE_R8XX_DEPTH_FORMAT_INVALID ? surface_depth : surface_stencil;
   struct terakan_image_surface_level const * const surface_main_aspect_level =
      &surface_main_aspect->levels[subresource_range->base_mip_level];

   uint32_t const image_va_shr8 = (uint32_t)(image->va >> 8);

   /* Fill the descriptor. Must not return false from now on. */

   uint32_t const slice_max =
      subresource_range->base_z_or_array_layer + (subresource_range->max_depth_or_layer_count - 1);
   /* Not expecting DB-compatible images with more than `TERAKAN_IMAGE_MAX_TARGET_SLICES` slices to
    * be created.
    */
   assert(slice_max < TERAKAN_IMAGE_MAX_TARGET_SLICES);
   descriptor_out->view = S_028008_SLICE_START(subresource_range->base_z_or_array_layer) |
                          S_028008_SLICE_MAX(slice_max);

   /* `NUM_SAMPLES` is needed by the driver logic regardless of the architecture generation. */
   descriptor_out->z_info =
      S_028040_FORMAT(view_depth_format) |
      S_028040_NUM_SAMPLES(util_logbase2((uint32_t)image->vk.samples)) |
      S_028040_ARRAY_MODE(surface_main_aspect_level->array_mode) |
      S_028040_NUM_BANKS(
         container_of(image->vk.base.device->physical, struct terakan_physical_device const, vk)
            ->tiling_info.banks_log2 -
         1) |
      S_028040_BANK_WIDTH(surface_main_aspect->tiling.attrib_bank_width) |
      S_028040_BANK_HEIGHT(surface_main_aspect->tiling.attrib_bank_height) |
      S_028040_MACRO_TILE_ASPECT(surface_main_aspect->tiling.attrib_macro_tile_aspect);
   if (view_depth_format != TERASCALE_R8XX_DEPTH_FORMAT_INVALID) {
      /* `ZRANGE_PRECISION` can't be set from the GPU based on the value the last clear was actually
       * done to because it's in `DB_Z_INFO`, which is used by the kernel driver for surface
       * validation, so set it according to how the numeric format is commonly used in applications
       * (normalized with Z towards 1 as the distance grows historically, floating-point with Z
       * towards 0 as the distance grows to greatly increase the precision by allowing the exponent
       * to vary throughout the Z range rather than only at extremely short distances).
       */
      descriptor_out->z_info |=
         S_028040_TILE_SPLIT(surface_depth->tiling.attrib_tile_split) |
         S_028040_ZRANGE_PRECISION(view_depth_format != TERASCALE_R8XX_DEPTH_FORMAT_32_FLOAT);
      descriptor_out->z_base = image_va_shr8 + surface_depth->offset_in_memory_bytes_shr8;
   } else {
      descriptor_out->z_base = image_va_shr8;
   }

   descriptor_out->stencil_info =
      S_028044_FORMAT(view_may_have_stencil ? V_028044_STENCIL_8 : V_028044_STENCIL_INVALID);
   if (view_may_have_stencil) {
      descriptor_out->stencil_info = S_028044_FORMAT(V_028044_STENCIL_8) |
                                     S_028044_TILE_SPLIT(surface_stencil->tiling.attrib_tile_split);
      descriptor_out->stencil_base = image_va_shr8 + surface_stencil->offset_in_memory_bytes_shr8;
   } else {
      descriptor_out->stencil_info = S_028044_FORMAT(V_028044_STENCIL_INVALID);
      descriptor_out->stencil_base = image_va_shr8;
   }

   descriptor_out->size =
      S_028058_PITCH_TILE_MAX(surface_main_aspect_level->aligned_extent_surfels[0] / 8 - 1) |
      S_028058_HEIGHT_TILE_MAX(surface_main_aspect_level->aligned_extent_surfels[1] / 8 - 1);
   descriptor_out->slice =
      S_028C68_SLICE_TILE_MAX((uint32_t)surface_main_aspect_level->aligned_extent_surfels[0] *
                                 surface_main_aspect_level->aligned_extent_surfels[1] / 64 -
                              1);

   /* TODO(Triang3l): HTile. */

   return true;
}

VKAPI_ATTR void VKAPI_CALL
terakan_DestroyImage(VkDevice const deviceHandle, VkImage const imageHandle,
                     VkAllocationCallbacks const * const pAllocator)
{
   struct terakan_image * const image = terakan_image_from_handle(imageHandle);

   if (image == NULL) {
      return;
   }

   struct terakan_device const * const device = terakan_device_from_handle(deviceHandle);

   vk_image_finish(&image->vk);

   vk_free2(&device->vk.alloc, pAllocator, image);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreateImage(VkDevice const deviceHandle, VkImageCreateInfo const * const pCreateInfo,
                    VkAllocationCallbacks const * const pAllocator, VkImage * const pImage)
{
   VkResult result;

   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   struct terakan_image * const image =
      vk_alloc2(&device->vk.alloc, pAllocator, sizeof(struct terakan_image),
                alignof(struct terakan_image), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (image == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   vk_image_init(&device->vk, &image->vk, pCreateInfo);

   if (unlikely(!terakan_format_info_get(pCreateInfo->format, &image->format_info))) {
      result = vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT, "Image format %s is not supported",
                         vk_Format_to_str(pCreateInfo->format));
      goto fail_image;
   }

   terakan_image_surface_compute(pCreateInfo, &image->format_info,
                                 terakan_device_physical_device(device), &image->surface);

   image->bo = NULL;
   image->va = 0;

   *pImage = terakan_image_to_handle(image);
   return VK_SUCCESS;

fail_image:
   vk_image_finish(&image->vk);
   return result;
}

VKAPI_ATTR void VKAPI_CALL
terakan_DestroyImageView(VkDevice const deviceHandle, VkImageView const imageView,
                         VkAllocationCallbacks const * const pAllocator)
{
   struct terakan_image_view * const image_view = terakan_image_view_from_handle(imageView);

   if (image_view == NULL) {
      return;
   }

   struct terakan_device const * const device = terakan_device_from_handle(deviceHandle);

   vk_image_view_finish(&image_view->vk);

   vk_free2(&device->vk.alloc, pAllocator, image_view);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreateImageView(VkDevice const deviceHandle,
                        VkImageViewCreateInfo const * const pCreateInfo,
                        VkAllocationCallbacks const * const pAllocator, VkImageView * const pView)
{
   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   struct terakan_format_info view_format_info;
   if (!terakan_format_info_get(pCreateInfo->format, &view_format_info)) {
      return vk_errorf(device, VK_ERROR_VALIDATION_FAILED_EXT,
                       "Image view format %s is not supported",
                       vk_Format_to_str(pCreateInfo->format));
   }

   struct terakan_image_view * const image_view =
      vk_alloc2(&device->vk.alloc, pAllocator, sizeof(struct terakan_image_view),
                alignof(struct terakan_image_view), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (image_view == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   vk_image_view_init(&device->vk, &image_view->vk, false, pCreateInfo);

   struct terakan_image const * const image = terakan_image_from_handle(pCreateInfo->image);

   image_view->bo = image->bo;

   struct terakan_image_descriptor_create_info descriptor_create_info = {
      .image = image,
      .subresource_range =
         {
            .base_mip_level = pCreateInfo->subresourceRange.baseMipLevel,
            .max_level_count = pCreateInfo->subresourceRange.levelCount,
            .base_z_or_array_layer = pCreateInfo->subresourceRange.baseArrayLayer,
            .max_depth_or_layer_count = 1,
         },
   };

   /* Using a view with both depth and stencil aspects for sampled or storage image descriptors is
    * not valid according to the `VkImageSubresourceRange` documentation, but very easy to produce,
    * so create depth-only resource and color descriptors unless only stencil is needed.
    */
   unsigned view_format_main_aspect_index = 0, image_format_main_aspect_index = 0;
   if ((pCreateInfo->subresourceRange.aspectMask &
        (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) == VK_IMAGE_ASPECT_STENCIL_BIT) {
      view_format_main_aspect_index =
         terakan_format_aspect_index(view_format_info.aspect_map, VK_IMAGE_ASPECT_STENCIL_BIT, 0);
      image_format_main_aspect_index =
         terakan_format_aspect_index(image->format_info.aspect_map, VK_IMAGE_ASPECT_STENCIL_BIT, 0);
   }
   /* TODO(Triang3l): For multi-planar images, select the correct plane for the main aspect, and
    * also create resource descriptors for all planes if requested (the aspect mask is COLOR).
    */

   descriptor_create_info.view_format =
      view_format_info.aspect_formats[view_format_main_aspect_index];
   descriptor_create_info.image_aspect_index = image_format_main_aspect_index;

   uint32_t resource_dimensionality = V_030000_SQ_TEX_DIM_1D;
   uint32_t color_resource_type = V_028C70_TEXTURE1D;
   switch (pCreateInfo->viewType) {
   case VK_IMAGE_VIEW_TYPE_1D:
      resource_dimensionality = V_030000_SQ_TEX_DIM_1D;
      color_resource_type = V_028C70_TEXTURE1D;
      break;
   case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
      descriptor_create_info.subresource_range.max_depth_or_layer_count =
         pCreateInfo->subresourceRange.layerCount;
      resource_dimensionality = V_030000_SQ_TEX_DIM_1D_ARRAY;
      color_resource_type = V_028C70_TEXTURE1DARRAY;
      break;
   case VK_IMAGE_VIEW_TYPE_2D:
      resource_dimensionality = image->vk.samples > VK_SAMPLE_COUNT_1_BIT
                                   ? V_030000_SQ_TEX_DIM_2D_MSAA
                                   : V_030000_SQ_TEX_DIM_2D;
      color_resource_type = V_028C70_TEXTURE2D;
      break;
   case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
      descriptor_create_info.subresource_range.max_depth_or_layer_count =
         pCreateInfo->subresourceRange.layerCount;
      resource_dimensionality = image->vk.samples > VK_SAMPLE_COUNT_1_BIT
                                   ? V_030000_SQ_TEX_DIM_2D_ARRAY_MSAA
                                   : V_030000_SQ_TEX_DIM_2D_ARRAY;
      color_resource_type = V_028C70_TEXTURE2DARRAY;
      break;
   case VK_IMAGE_VIEW_TYPE_3D:
      descriptor_create_info.subresource_range.base_z_or_array_layer = 0;
      descriptor_create_info.subresource_range.max_depth_or_layer_count =
         u_minify(image->vk.extent.depth, pCreateInfo->subresourceRange.baseMipLevel);
      resource_dimensionality = V_030000_SQ_TEX_DIM_3D;
      color_resource_type = V_028C70_TEXTURE3D;
      break;
   case VK_IMAGE_VIEW_TYPE_CUBE:
      descriptor_create_info.subresource_range.max_depth_or_layer_count = 6;
      resource_dimensionality = V_030000_SQ_TEX_DIM_CUBEMAP;
      color_resource_type = V_028C70_TEXTURE2DARRAY;
      break;
   case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
      descriptor_create_info.subresource_range.max_depth_or_layer_count =
         pCreateInfo->subresourceRange.layerCount;
      resource_dimensionality = V_030000_SQ_TEX_DIM_CUBEMAP;
      color_resource_type = V_028C70_TEXTURE2DARRAY;
      break;
   default:
      assert(!"Unsupported image view type");
   }

   /* TODO(Triang3l): Verify sanitization for cubemaps. */
   if (unlikely(!terakan_image_descriptor_subresource_range_sanitize(
          image, &descriptor_create_info.subresource_range,
          resource_dimensionality == V_030000_SQ_TEX_DIM_CUBEMAP))) {
      image_view->resource = (struct terakan_resource_descriptor){};
      image_view->color = (struct terakan_color_descriptor){};
      image_view->color_meta = (struct terakan_color_meta_descriptor){};
      image_view->depth_stencil = (struct terakan_depth_stencil_descriptor){};
   } else {
      if (!terakan_image_create_resource_descriptor(
             &descriptor_create_info, resource_dimensionality, &pCreateInfo->components,
             &image_view->resource)) {
         image_view->resource = (struct terakan_resource_descriptor){};
      }
      if (!descriptor_create_info.view_format.supports_cb_color ||
          terakan_image_create_color_descriptor(&descriptor_create_info, color_resource_type,
                                                &image_view->color, &image_view->color_meta) == 0) {
         image_view->color = (struct terakan_color_descriptor){};
         image_view->color_meta = (struct terakan_color_meta_descriptor){};
      }
      /* According to the `VkFramebufferCreateInfo` and `VkRenderingInfo` reference, the image view
       * `aspectMask` is ignored for depth / stencil attachments, so toggle depth and stencil based
       * purely on the format.
       */
      enum terascale_r8xx_depth_format view_depth_format = TERASCALE_R8XX_DEPTH_FORMAT_INVALID;
      bool view_has_stencil = false;
      if (!terascale_get_r8xx_depth_stencil_format(vk_format_to_pipe_format(pCreateInfo->format),
                                                   &view_depth_format, &view_has_stencil) ||
          !terakan_image_create_depth_stencil_descriptor(image, view_depth_format, view_has_stencil,
                                                         &descriptor_create_info.subresource_range,
                                                         &image_view->depth_stencil)) {
         image_view->depth_stencil = (struct terakan_depth_stencil_descriptor){};
      }
   }

   *pView = terakan_image_view_to_handle(image_view);
   return VK_SUCCESS;
}
