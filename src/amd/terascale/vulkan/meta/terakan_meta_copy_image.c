/*
 * Copyright © 2024 Vitaliy Triang3l Kuzmin
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

#include "terakan_meta_impl.h"

#include "terakan_entrypoints.h"
#include "terakan_image.h"

#include "util/bitscan.h"
#include "util/macros.h"

#include <assert.h>

enum {
   /* In blocks, signed. */
   TERAKAN_META_COPY_IMAGE_CONST_SRC_MINUS_DST_OFFSET_X,
   TERAKAN_META_COPY_IMAGE_CONST_SRC_MINUS_DST_OFFSET_Y,

   TERAKAN_META_COPY_IMAGE_CONSTS_COUNT,
};

static uint32_t const terakan_meta_copy_image_ps_r8xx[] = {
   /* 0: Address offsetting. */
   S_SQ_CF_WORD0_ADDR(3) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(
      TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_COPY_IMAGE_CONST_SRC_MINUS_DST_OFFSET_X)) |
      S_SQ_CF_ALU_WORD1_COUNT(1) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch from the source texture. */
   S_SQ_CF_WORD0_ADDR(6),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Export the color and end the program. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) | S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 3: ALU clause. */

   /* +0-1: Apply the address offset to R0.XY.
    * Cycle 0: X = R0, Y = R0.
    */
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_COPY_IMAGE_CONST_SRC_MINUS_DST_OFFSET_X) |
      TERAKAN_SHADER_OP2(false, 0, 'X', ADD_INT, EG, 0, 'X', 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_COPY_IMAGE_CONST_SRC_MINUS_DST_OFFSET_Y) |
      TERAKAN_SHADER_OP2(true, 0, 'Y', ADD_INT, EG, 0, 'Y', 0, 0, VEC_012),

   /* 5 (alignment padding), 6-7: Fetch from the source texture to R0. */
   0,
   0,
   S_SQ_TEX_WORD0_TEX_INST(3) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(0) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W),
   S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_0),
   0,
};

static uint32_t const terakan_meta_copy_image_ps_r9xx[] = {
   /* 0: Address offsetting. */
   S_SQ_CF_WORD0_ADDR(4) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(
      TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_COPY_IMAGE_CONST_SRC_MINUS_DST_OFFSET_X)) |
      S_SQ_CF_ALU_WORD1_COUNT(1) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch from the source texture. */
   S_SQ_CF_WORD0_ADDR(6),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Export the color. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 3: End the program. */
   TERAKAN_SHADER_CF_END_R9XX,

   /* 4: ALU clause. */

   /* +0-1: Apply the address offset to R0.XY.
    * Cycle 0: X = R0, Y = R0.
    */
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_COPY_IMAGE_CONST_SRC_MINUS_DST_OFFSET_X) |
      TERAKAN_SHADER_OP2(false, 0, 'X', ADD_INT, EG, 0, 'X', 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_COPY_IMAGE_CONST_SRC_MINUS_DST_OFFSET_Y) |
      TERAKAN_SHADER_OP2(true, 0, 'Y', ADD_INT, EG, 0, 'Y', 0, 0, VEC_012),

   /* 6-7: Fetch from the source texture to R0. */
   S_SQ_TEX_WORD0_TEX_INST(3) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(0) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W),
   S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_0),
   0,
};

struct terakan_meta_shader const terakan_meta_copy_image_ps = {
   .r8xx =
      {
         .program = terakan_meta_copy_image_ps_r8xx,
         .program_size_bytes = sizeof(terakan_meta_copy_image_ps_r8xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     S_028844_NUM_GPRS(1) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                     TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                  },
               .stage =
                  {
                     .ps =
                        {
                           .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                           .spi_ps_in_control =
                              {
                                 S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),
                                 S_0286D0_FIXED_PT_POSITION_ENA(1) |
                                    S_0286D0_FIXED_PT_POSITION_ADDR(0),
                              },
                           .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
                           .cb_shader_mask = 0xF,
                        },
                  },
            },
      },
   .r9xx =
      {
         .program = terakan_meta_copy_image_ps_r9xx,
         .program_size_bytes = sizeof(terakan_meta_copy_image_ps_r9xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     S_028844_NUM_GPRS(1) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                     TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                  },
               .stage =
                  {
                     .ps =
                        {
                           .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                           .spi_ps_in_control =
                              {
                                 S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),
                                 S_0286D0_FIXED_PT_POSITION_ENA(1) |
                                    S_0286D0_FIXED_PT_POSITION_ADDR(0),
                              },
                           .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
                           .cb_shader_mask = 0xF,
                        },
                  },
            },
      },
   .kcache_used = BITFIELD_BIT(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS),
   .primary_meta_resource_used = true,
};

VKAPI_ATTR void VKAPI_CALL
terakan_CmdCopyImage2(VkCommandBuffer const commandBuffer,
                      VkCopyImageInfo2 const * const pCopyImageInfo)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   struct terakan_image const * const dst_image =
      terakan_image_from_handle(pCopyImageInfo->dstImage);

   if (terakan_format_is_expand_3x(dst_image->surface.aspects[0].bytes_per_block)) {
      terakan_meta_copy_expand_3x_image(command_writer, pCopyImageInfo);
      return;
   }

   struct terakan_image const * const src_image =
      terakan_image_from_handle(pCopyImageInfo->srcImage);

   /* TODO(Triang3l): Multisampled image copying, possibly by copying fragments (directly, not via
    * coverage samples) with sample shading, and the FMask with pixel shading - and with some path
    * for depth/stencil.
    */

   struct terakan_image_descriptor_create_info dst_descriptor_create_info = {.image = dst_image};
   struct terakan_image_descriptor_create_info src_descriptor_create_info = {.image = src_image};

   command_writer->post_color_image_copy_write_barrier_actions |=
      TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
      TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;

   struct terakan_meta_config_draw_begin_options const meta_begin_options = {
      .vgt_primitive_type = V_008958_DI_PT_RECTLIST,
      .cb_and_db_shader_control_mode =
         TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_NORMAL_WITH_RTV_AND_DYNAMIC_DB_SHADER_CONTROL,
      .rasterization = {.enable = true},
   };
   terakan_meta_config_draw_begin(command_writer, &meta_begin_options);
   terakan_meta_config_draw_set_sq_pgm_vs(command_writer,
                                          TERAKAN_META_SHADER_POSITION_AND_LAYER_FROM_INDEX_VS);
   terakan_meta_config_draw_set_sq_pgm_ps(command_writer, TERAKAN_META_SHADER_COPY_IMAGE_PS);

   int32_t constants[TERAKAN_META_COPY_IMAGE_CONSTS_COUNT] = {};
   bool constants_set = false;

   for (uint32_t region_index = 0; region_index < pCopyImageInfo->regionCount; ++region_index) {
      VkImageCopy2 const * const region = &pCopyImageInfo->pRegions[region_index];

      /* Section 49.1.7. "Format Compatibility Classes" of the Vulkan 1.3.283 specification says:
       *
       *     "Copy operations are able to copy between size-compatible formats in different
       *     resources to enable manipulation of data in different formats. The extent used in these
       *     copy operations always matches the source image, and is resized to the expectations of
       *     the block extents noted above for the destination image."
       *
       * The offset must be block-aligned, but offset + extent is limited to the extent of the
       * subresource, which is not block-aligned.
       */
      VkOffset3D const dst_offset_blocks =
         vk_image_offset_to_elements(&dst_image->vk, region->dstOffset);
      VkOffset3D const src_offset_blocks =
         vk_image_offset_to_elements(&src_image->vk, region->srcOffset);
      VkExtent3D const extent_blocks = vk_image_extent_to_elements(&src_image->vk, region->extent);
      VkRect2D const dst_region_rect = {
         .offset = {.x = dst_offset_blocks.x, .y = dst_offset_blocks.y},
         .extent = {.width = extent_blocks.width, .height = extent_blocks.height}};

      int32_t const src_minus_dst_offset_x = src_offset_blocks.x - dst_region_rect.offset.x;
      int32_t const src_minus_dst_offset_y = src_offset_blocks.y - dst_region_rect.offset.y;
      if (constants[TERAKAN_META_COPY_IMAGE_CONST_SRC_MINUS_DST_OFFSET_X] !=
             src_minus_dst_offset_x ||
          constants[TERAKAN_META_COPY_IMAGE_CONST_SRC_MINUS_DST_OFFSET_Y] !=
             src_minus_dst_offset_y) {
         constants_set = NULL;
         constants[TERAKAN_META_COPY_IMAGE_CONST_SRC_MINUS_DST_OFFSET_X] = src_minus_dst_offset_x;
         constants[TERAKAN_META_COPY_IMAGE_CONST_SRC_MINUS_DST_OFFSET_Y] = src_minus_dst_offset_y;
      }
      if (!constants_set) {
         terakan_meta_config_draw_set_kcache_push_constants(command_writer, sizeof(constants),
                                                            constants, false, true);
         constants_set = true;
      }

      unsigned src_vk_aspect_mask_remaining = (unsigned)region->srcSubresource.aspectMask;
      u_foreach_bit (dst_vk_aspect_bit_index, region->dstSubresource.aspectMask) {
         dst_descriptor_create_info.image_aspect_index = terakan_format_aspect_index(
            dst_image->format_info.aspect_map, (VkImageAspectFlags)1 << dst_vk_aspect_bit_index, 0);
         dst_descriptor_create_info.view_format = terakan_meta_transfer_image_block_format_info(
            terascale_format_bytes_per_block
               [dst_image->format_info.aspect_formats[dst_descriptor_create_info.image_aspect_index]
                   .format]);
         src_descriptor_create_info.image_aspect_index = terakan_format_aspect_index(
            src_image->format_info.aspect_map,
            (VkImageAspectFlags)1 << u_bit_scan(&src_vk_aspect_mask_remaining), 0);
         src_descriptor_create_info.view_format = terakan_meta_transfer_image_block_format_info(
            terascale_format_bytes_per_block
               [src_image->format_info.aspect_formats[src_descriptor_create_info.image_aspect_index]
                   .format]);

         dst_descriptor_create_info.subresource_range.base_mip_level =
            region->dstSubresource.mipLevel;
         dst_descriptor_create_info.subresource_range.max_level_count = 1;
         src_descriptor_create_info.subresource_range.base_mip_level =
            region->srcSubresource.mipLevel;
         src_descriptor_create_info.subresource_range.max_level_count = 1;

         if (src_image->vk.image_type == VK_IMAGE_TYPE_3D) {
            src_descriptor_create_info.subresource_range.base_z_or_array_layer =
               (uint32_t)region->srcOffset.z;
            src_descriptor_create_info.subresource_range.max_depth_or_layer_count =
               region->extent.depth;
         } else {
            src_descriptor_create_info.subresource_range.base_z_or_array_layer =
               region->srcSubresource.baseArrayLayer;
            src_descriptor_create_info.subresource_range.max_depth_or_layer_count =
               region->srcSubresource.layerCount;
         }
         dst_descriptor_create_info.subresource_range.base_z_or_array_layer =
            dst_image->vk.image_type == VK_IMAGE_TYPE_3D ? (uint32_t)region->dstOffset.z
                                                         : region->dstSubresource.baseArrayLayer;
         dst_descriptor_create_info.subresource_range.max_depth_or_layer_count =
            src_descriptor_create_info.subresource_range.max_depth_or_layer_count;

         if (unlikely(!terakan_image_descriptor_subresource_range_sanitize(
                         dst_image, &dst_descriptor_create_info.subresource_range, false) ||
                      !terakan_image_descriptor_subresource_range_sanitize(
                         src_image, &src_descriptor_create_info.subresource_range, false))) {
            continue;
         }
         /* To avoid sanitizing the subresource range at every slice subrange iteration for
          * #MemoryIntegrity.
          */
         src_descriptor_create_info.subresource_range.max_depth_or_layer_count =
            MIN2(src_descriptor_create_info.subresource_range.max_depth_or_layer_count,
                 dst_descriptor_create_info.subresource_range.max_depth_or_layer_count);
         dst_descriptor_create_info.subresource_range.max_depth_or_layer_count =
            src_descriptor_create_info.subresource_range.max_depth_or_layer_count;

         struct terakan_resource_descriptor src_descriptor;
         if (unlikely(!terakan_image_create_resource_descriptor(&src_descriptor_create_info,
                                                                V_030000_SQ_TEX_DIM_2D_ARRAY, NULL,
                                                                &src_descriptor))) {
            continue;
         }

         do {
            struct terakan_color_descriptor dst_descriptor;
            uint32_t const dst_descriptor_slices = terakan_image_create_color_descriptor(
               &dst_descriptor_create_info, V_028C70_TEXTURE2DARRAY, &dst_descriptor, NULL);
            if (unlikely(dst_descriptor_slices == 0)) {
               break;
            }
            terakan_meta_config_draw_set_cb_rtvs_and_db_shader_control(
               command_writer, 0xF, &dst_image->bo, &dst_descriptor, NULL,
               TERAKAN_SHADER_DB_SHADER_CONTROL_IDENTITY);

            terakan_hw_config_sqk_set_resource_fs(
               &command_writer->hw_config_sqk,
               TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META, src_image->bo,
               &src_descriptor);

            terakan_meta_draw_rect(
               command_writer,
               terakan_vk_rect_to_screen_rect(
                  dst_region_rect,
                  (struct terakan_screen_rect){
                     .bounds = {[1] = {G_028C78_WIDTH_MAX(dst_descriptor.dim) + 1,
                                       G_028C78_HEIGHT_MAX(dst_descriptor.dim) + 1}}}),
               dst_descriptor_slices);

            dst_descriptor_create_info.subresource_range.base_z_or_array_layer +=
               dst_descriptor_slices;
            dst_descriptor_create_info.subresource_range.max_depth_or_layer_count -=
               dst_descriptor_slices;
            src_descriptor.resource[5] = (src_descriptor.resource[5] & C_030014_BASE_ARRAY) |
                                         S_030014_BASE_ARRAY(G_030014_BASE_ARRAY(
                                            src_descriptor.resource[5] + dst_descriptor_slices));
         } while (dst_descriptor_create_info.subresource_range.max_depth_or_layer_count != 0);
      }
   }
}
