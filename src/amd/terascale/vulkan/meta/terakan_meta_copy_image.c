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

#include "terakan_meta.h"

#include "terakan_command_buffer.h"
#include "terakan_descriptor.h"
#include "terakan_entrypoints.h"
#include "terakan_format.h"
#include "terakan_image.h"

#include "gallium/drivers/r600/eg_sq.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600_opcodes.h"
#include "util/bitscan.h"
#include "util/macros.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

struct terakan_meta_copy_image_push_constants {
   /* In blocks. */
   int32_t texture_minus_screen_position[2];
};

static uint32_t const terakan_meta_copy_image_ps_r8xx[] = {
   /* Control flow. */

   /* 0: Address offsetting. */

   S_SQ_CF_WORD0_ADDR(3) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(TERAKAN_KCACHE_FIELD_LINE(
      struct terakan_meta_copy_image_push_constants, texture_minus_screen_position)) |
      S_SQ_CF_ALU_WORD1_COUNT(4 - 3) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch from the source texture. */

   S_SQ_CF_WORD0_ADDR(6),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(1) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Export the color and end the program. */

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(V_03000C_SQ_SEL_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(V_03000C_SQ_SEL_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(V_03000C_SQ_SEL_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(V_03000C_SQ_SEL_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(1) | S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(1) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* ALU clause. */

   /* Apply the address offset.
    *
    * 3:     R0.X = ADD_INT R0.X, CB[push].texture_minus_screen_position.X
    * 4: (v) R0.Y = ADD_INT R0.Y, CB[push].texture_minus_screen_position.Y
    * Cycle 0: X = R0, Y = R0
    */

   S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      TERAKAN_KCACHE_FIELD_WORD0_SRC1(struct terakan_meta_copy_image_push_constants,
                                      texture_minus_screen_position[0]),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD_INT),

   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(1) |
      TERAKAN_KCACHE_FIELD_WORD0_SRC1(struct terakan_meta_copy_image_push_constants,
                                      texture_minus_screen_position[1]),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD_INT),

   /* 5 (alignment padding), 6, 7: Fetch from the source texture to R0. */

   0,
   0,

   S_SQ_TEX_WORD0_TEX_INST(3) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(0) | S_SQ_TEX_WORD1_DST_SEL_X(V_03000C_SQ_SEL_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(V_03000C_SQ_SEL_Y) | S_SQ_TEX_WORD1_DST_SEL_Z(V_03000C_SQ_SEL_Z) |
      S_SQ_TEX_WORD1_DST_SEL_W(V_03000C_SQ_SEL_W),
   S_SQ_TEX_WORD2_SRC_SEL_X(V_03000C_SQ_SEL_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(V_03000C_SQ_SEL_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(V_03000C_SQ_SEL_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(V_03000C_SQ_SEL_0),
   0,
};

static uint32_t const terakan_meta_copy_image_ps_r9xx[] = {
   /* Control flow. */

   /* 0: Address offsetting. */

   S_SQ_CF_WORD0_ADDR(4) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(TERAKAN_KCACHE_FIELD_LINE(
      struct terakan_meta_copy_image_push_constants, texture_minus_screen_position)) |
      S_SQ_CF_ALU_WORD1_COUNT(5 - 4) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch from the source texture. */

   S_SQ_CF_WORD0_ADDR(6),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(1) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Export the color. */

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(V_03000C_SQ_SEL_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(V_03000C_SQ_SEL_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(V_03000C_SQ_SEL_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(V_03000C_SQ_SEL_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(1) | EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 3: End the program. */

   0,
   S_SQ_CF_WORD1_BARRIER(1) | CM_V_SQ_CF_WORD1_SQ_CF_INST_END,

   /* ALU clause. */

   /* Apply the address offset.
    *
    * 4: R0.X = ADD_INT R0.X, CB[push].texture_minus_screen_position.X
    * 5: R0.Y = ADD_INT R0.Y, CB[push].texture_minus_screen_position.Y
    * Cycle 0: X = R0, Y = R0
    */

   S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      TERAKAN_KCACHE_FIELD_WORD0_SRC1(struct terakan_meta_copy_image_push_constants,
                                      texture_minus_screen_position[0]),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD_INT),

   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(1) |
      TERAKAN_KCACHE_FIELD_WORD0_SRC1(struct terakan_meta_copy_image_push_constants,
                                      texture_minus_screen_position[1]),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD_INT),

   /* 6, 7: Fetch from the source texture to R0. */

   S_SQ_TEX_WORD0_TEX_INST(3) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(0) | S_SQ_TEX_WORD1_DST_SEL_X(V_03000C_SQ_SEL_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(V_03000C_SQ_SEL_Y) | S_SQ_TEX_WORD1_DST_SEL_Z(V_03000C_SQ_SEL_Z) |
      S_SQ_TEX_WORD1_DST_SEL_W(V_03000C_SQ_SEL_W),
   S_SQ_TEX_WORD2_SRC_SEL_X(V_03000C_SQ_SEL_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(V_03000C_SQ_SEL_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(V_03000C_SQ_SEL_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(V_03000C_SQ_SEL_0),
   0,
};

struct terakan_meta_shader const terakan_meta_copy_image_ps =
   {
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
      .kcache_needed = (uint16_t)1 << TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
      .resources_needed =
         {
            [BITSET_BITWORD(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META)] =
               BITSET_BIT(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META),
         },
      .stage =
         {
            .ps =
               {
                  .db_shader_control = TERAKAN_META_DB_SHADER_CONTROL_DEFAULT,
               },
         },
};

VKAPI_ATTR void VKAPI_CALL
terakan_CmdCopyImage2(VkCommandBuffer const commandBuffer,
                      VkCopyImageInfo2 const * const pCopyImageInfo)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   struct terakan_image const * const src_image =
      terakan_image_from_handle(pCopyImageInfo->srcImage);

   if (terakan_format_is_expand_3x(src_image->surface.planes[0].bytes_per_block)) {
      terakan_meta_copy_expand_3x_image(command_writer, pCopyImageInfo);
      return;
   }

   struct terakan_image const * const dst_image =
      terakan_image_from_handle(pCopyImageInfo->dstImage);

   /* TODO(Triang3l): Multisampled image copying, possibly by copying fragments (directly, not via
    * coverage samples) with sample shading, and the FMask with pixel shading - and with some path
    * for depth/stencil.
    */

   VkImageViewCreateInfo src_image_view_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = pCopyImageInfo->srcImage,
      .viewType = terakan_meta_transfer_image_view_type(src_image->vk.image_type),
      .subresourceRange =
         {
            .levelCount = 1,
         },
   };
   VkImageViewCreateInfo dst_image_view_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = pCopyImageInfo->dstImage,
      .viewType = terakan_meta_transfer_image_view_type(dst_image->vk.image_type),
      .subresourceRange =
         {
            .levelCount = 1,
         },
   };

   unsigned const src_block_width = vk_format_get_blockwidth(src_image->vk.format);
   unsigned const src_block_height = vk_format_get_blockheight(src_image->vk.format);
   unsigned const dst_block_width = vk_format_get_blockwidth(dst_image->vk.format);
   unsigned const dst_block_height = vk_format_get_blockheight(dst_image->vk.format);

   command_writer->post_color_image_copy_write_barrier_actions |=
      TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
      TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;

   terakan_meta_begin_2d_immediate_rects(command_writer, TERAKAN_META_PA_CL_VTE_CNTL_2D,
                                         TERAKAN_META_DB_RENDER_OVERRIDE_DEFAULT, true);

   terakan_meta_set_vs(command_writer, TERAKAN_META_SHADER_POSITION_AND_LAYER_FROM_INDEX_VS);
   terakan_meta_set_ps(command_writer, TERAKAN_META_SHADER_COPY_IMAGE_PS, false);

   terakan_meta_begin_cb(command_writer, V_028808_CB_NORMAL, 0xF, 0b1);

   command_writer->push_constants_state.up_to_date_push_constants_bound_to_stages &=
      ~VK_SHADER_STAGE_FRAGMENT_BIT;

   struct terakan_meta_copy_image_push_constants push_constants = {};
   struct terakan_bo const * push_constants_bo = NULL;
   uint32_t push_constants_va_lines;

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
      VkRect2D const rect = {
         .offset =
            {
               .x = region->dstOffset.x / dst_block_width,
               .y = region->dstOffset.y / dst_block_height,
            },
         .extent =
            {
               .width = DIV_ROUND_UP(region->extent.width, src_block_width),
               .height = DIV_ROUND_UP(region->extent.height, src_block_height),
            },
      };
      int32_t const texture_minus_screen_position[2] = {
         region->srcOffset.x / src_block_width - rect.offset.x,
         region->srcOffset.y / src_block_height - rect.offset.y,
      };
      if (push_constants.texture_minus_screen_position[0] != texture_minus_screen_position[0] ||
          push_constants.texture_minus_screen_position[1] != texture_minus_screen_position[1]) {
         push_constants.texture_minus_screen_position[0] = texture_minus_screen_position[0];
         push_constants.texture_minus_screen_position[1] = texture_minus_screen_position[1];
         push_constants_bo = NULL;
      }

      if (push_constants_bo == NULL) {
         void * const push_constants_mapping = terakan_push_buffer_allocate_kcache(
            command_writer->base.command_buffer, sizeof(push_constants), &push_constants_bo,
            &push_constants_va_lines);
         if (unlikely(push_constants_mapping == NULL)) {
            return;
         }
         memcpy(push_constants_mapping, &push_constants, sizeof(push_constants));
         terakan_hw_state_draw_set_sq_kcache_fs(
            &command_writer->hw_state_draw, TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
            (sizeof(push_constants) + (TERAKAN_KCACHE_HW_LINE_BYTES - 1)) /
               TERAKAN_KCACHE_HW_LINE_BYTES,
            push_constants_bo, push_constants_va_lines);
      }

      src_image_view_create_info.subresourceRange.baseMipLevel = region->srcSubresource.mipLevel;
      if (src_image->vk.image_type == VK_IMAGE_TYPE_3D) {
         src_image_view_create_info.subresourceRange.baseArrayLayer = (uint32_t)region->srcOffset.z;
         src_image_view_create_info.subresourceRange.layerCount = region->extent.depth;
      } else {
         src_image_view_create_info.subresourceRange.baseArrayLayer =
            region->srcSubresource.baseArrayLayer;
         src_image_view_create_info.subresourceRange.layerCount =
            vk_image_subresource_layer_count(&src_image->vk, &region->srcSubresource);
      }

      dst_image_view_create_info.subresourceRange.baseMipLevel = region->dstSubresource.mipLevel;
      uint32_t const dst_base_layer = dst_image->vk.image_type == VK_IMAGE_TYPE_3D
                                         ? (uint32_t)region->dstOffset.z
                                         : region->dstSubresource.baseArrayLayer;

      unsigned dst_aspect_mask_remaining = (unsigned)region->dstSubresource.aspectMask;
      u_foreach_bit (src_aspect_index, region->srcSubresource.aspectMask) {
         src_image_view_create_info.subresourceRange.aspectMask = (VkImageAspectFlags)1
                                                                  << src_aspect_index;
         dst_image_view_create_info.subresourceRange.aspectMask =
            (VkImageAspectFlags)1 << u_bit_scan(&dst_aspect_mask_remaining);

         src_image_view_create_info.format = terakan_meta_transfer_image_block_format(
            src_image->surface
               .planes[terakan_image_surface_aspect_plane(
                  src_image->vk.format, src_image_view_create_info.subresourceRange.aspectMask)]
               .bytes_per_block);
         dst_image_view_create_info.format = terakan_meta_transfer_image_block_format(
            dst_image->surface
               .planes[terakan_image_surface_aspect_plane(
                  dst_image->vk.format, dst_image_view_create_info.subresourceRange.aspectMask)]
               .bytes_per_block);

         dst_image_view_create_info.subresourceRange.baseArrayLayer = dst_base_layer;
         dst_image_view_create_info.subresourceRange.layerCount =
            src_image_view_create_info.subresourceRange.layerCount;

         uint32_t src_resource[8];
         if (unlikely(!terakan_image_create_resource_descriptor(&src_image_view_create_info,
                                                                src_resource))) {
            assert(!"Invalid source image view create info");
            return;
         }

         while (dst_image_view_create_info.subresourceRange.layerCount > 0) {
            terakan_hw_state_draw_set_sq_resource_fs(
               &command_writer->hw_state_draw,
               TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META, src_image->bo, src_resource);

            struct terakan_color_descriptor dst_color;
            struct terakan_color_meta_descriptor dst_color_meta;
            uint32_t const dst_color_layer_count = terakan_image_create_color_descriptor(
               &dst_image_view_create_info, &dst_color, &dst_color_meta);
            if (unlikely(dst_color_layer_count == 0)) {
               assert(!"Invalid destination image view create info");
               return;
            }
            terakan_color_descriptor_image_view_to_color_attachment(&dst_color);
            terakan_hw_state_draw_set_cb_color(&command_writer->hw_state_draw, 0, dst_image->bo,
                                               &dst_color, &dst_color_meta, false);
            terakan_meta_set_db_shader_control_with_rtv(
               command_writer, terakan_meta_copy_image_ps.stage.ps.db_shader_control,
               dst_color.info);

            terakan_meta_emit_rect_3_vertices_draw(command_writer, &rect, dst_color_layer_count);

            src_resource[5] =
               (src_resource[5] & C_030014_BASE_ARRAY) |
               S_030014_BASE_ARRAY(G_030014_BASE_ARRAY(src_resource[5]) + dst_color_layer_count);
            dst_image_view_create_info.subresourceRange.baseArrayLayer += dst_color_layer_count;
            dst_image_view_create_info.subresourceRange.layerCount -= dst_color_layer_count;
         }
      }
   }
}
