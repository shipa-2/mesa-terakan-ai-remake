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
#include "terakan_entrypoints.h"
#include "terakan_image.h"

#include "util/macros.h"
#include "util/u_math.h"

#include <assert.h>
#include <math.h>
#include <string.h>

enum {
   TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE,
   TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE_R = TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE,
   TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE_G,
   TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE_B,
   TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE_A,

   TERAKAN_META_CLEAR_COLOR_CONSTS_COUNT,
};

static uint32_t const terakan_meta_clear_color_ps_r8xx[] = {
   /* 0: Clear value loading. */
   S_SQ_CF_WORD0_ADDR(2) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(
      TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE)) |
      S_SQ_CF_ALU_WORD1_COUNT(3) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Export the color and end the program. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) | S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 2: ALU clause. */

   /* +0-3: Move the clear value from the kcache buffer to R0. */
   TERAKAN_KCACHE_DWORD_WORD0_SRC01(0, TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE_R) |
      TERAKAN_SHADER_OP1(false, 0, 'X', MOV, EG, 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC01(0, TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE_G) |
      TERAKAN_SHADER_OP1(false, 0, 'Y', MOV, EG, 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC01(0, TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE_B) |
      TERAKAN_SHADER_OP1(false, 0, 'Z', MOV, EG, 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC01(0, TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE_A) |
      TERAKAN_SHADER_OP1(true, 0, 'W', MOV, EG, 0, 0, VEC_012),
};

static uint32_t const terakan_meta_clear_color_ps_r9xx[] = {
   /* 0: Clear value loading. */
   S_SQ_CF_WORD0_ADDR(3) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(
      TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE)) |
      S_SQ_CF_ALU_WORD1_COUNT(3) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Export the color. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 2: End the program. */
   TERAKAN_SHADER_CF_END_R9XX,

   /* 3: ALU clause. */

   /* +0-3: Move the clear value from the kcache buffer to R0. */
   TERAKAN_KCACHE_DWORD_WORD0_SRC01(0, TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE_R) |
      TERAKAN_SHADER_OP1(false, 0, 'X', MOV, EG, 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC01(0, TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE_G) |
      TERAKAN_SHADER_OP1(false, 0, 'Y', MOV, EG, 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC01(0, TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE_B) |
      TERAKAN_SHADER_OP1(false, 0, 'Z', MOV, EG, 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC01(0, TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE_A) |
      TERAKAN_SHADER_OP1(true, 0, 'W', MOV, EG, 0, 0, VEC_012),
};

struct terakan_meta_shader const terakan_meta_clear_color_ps = {
   .r8xx =
      {
         .program = terakan_meta_clear_color_ps_r8xx,
         .program_size_bytes = sizeof(terakan_meta_clear_color_ps_r8xx),
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
                                 0,
                              },
                           .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
                           .cb_shader_mask = 0xF,
                        },
                  },
            },
      },
   .r9xx =
      {
         .program = terakan_meta_clear_color_ps_r9xx,
         .program_size_bytes = sizeof(terakan_meta_clear_color_ps_r9xx),
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
                                 0,
                              },
                           .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
                           .cb_shader_mask = 0xF,
                        },
                  },
            },
      },
   .kcache_needed = (uint16_t)1 << TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
   .stage =
      {
         .ps =
            {
               .db_shader_control = TERAKAN_META_DB_SHADER_CONTROL_DEFAULT,
            },
      },
};

VKAPI_ATTR void VKAPI_CALL
terakan_CmdClearColorImage(VkCommandBuffer const commandBuffer, VkImage const imageHandle,
                           UNUSED VkImageLayout const imageLayout,
                           VkClearColorValue const * const pColor, uint32_t const rangeCount,
                           VkImageSubresourceRange const * const pRanges)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   struct terakan_image const * const image = terakan_image_from_handle(imageHandle);

   /* VUID-vkCmdClearColorImage-aspectMask-02498: "The VkImageSubresourceRange::aspectMask members
    * of the elements of the pRanges array must each only include VK_IMAGE_ASPECT_COLOR_BIT"
    */
   unsigned const aspect_index =
      terakan_format_aspect_index(image->format_info.aspect_map, VK_IMAGE_ASPECT_COLOR_BIT, 0);

   struct terascale_format_info const image_format_info =
      image->format_info.aspect_formats[aspect_index];

   /* TODO(Triang3l): 3x-expanded format clearing. */
   if (unlikely(!IS_POT(terascale_format_bytes_per_block[image_format_info.format]))) {
      return;
   }

   struct terakan_image_descriptor_create_info image_descriptor_create_info = {
      .image = image,
      .view_type = terakan_meta_transfer_image_view_type(image->vk.image_type),
      .view_format = terakan_meta_transfer_image_block_format_info(
         terascale_format_bytes_per_block[image_format_info.format]),
      /* The clear value is a byte array with the needed endianness. */
      .force_little_endian = true,
      .image_aspect_index = aspect_index,
      .level_count = 1,
   };

   struct terakan_bo const * constants_bo;
   uint32_t constants_va_lines;
   uint32_t * const constants_mapping = terakan_push_buffer_allocate_kcache(
      command_writer->base.command_buffer, sizeof(uint32_t) * TERAKAN_META_CLEAR_COLOR_CONSTS_COUNT,
      &constants_bo, &constants_va_lines);
   if (unlikely(constants_mapping == NULL)) {
      return;
   }

   uint8_t clear_value[sizeof(uint32_t) * 4] = {};
   terascale_format_pack_color(&image_format_info, pColor->uint32, clear_value);
   if (image_descriptor_create_info.view_format.number_type == TERASCALE_FORMAT_NUMBER_TYPE_UNORM) {
      /* Writing via a 8 bits per channel unorm format for 16bpc export to be usable. */
      assert(image_descriptor_create_info.view_format.format == TERASCALE_FORMAT_INDEX_8 ||
             image_descriptor_create_info.view_format.format == TERASCALE_FORMAT_INDEX_8_8 ||
             image_descriptor_create_info.view_format.format == TERASCALE_FORMAT_INDEX_8_8_8_8);
      for (unsigned byte_index = 0; byte_index < 4; ++byte_index) {
         constants_mapping[TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE + byte_index] =
            fui((float)clear_value[byte_index] / 255.0f);
      }
   } else {
      memcpy(&constants_mapping[TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE], clear_value,
             sizeof(uint32_t) * 4);
   }

   command_writer->push_constants_state.up_to_date_push_constants_bound_to_stages &=
      ~VK_SHADER_STAGE_FRAGMENT_BIT;
   terakan_hw_state_sqc_set_kcache_fs(
      &command_writer->hw_state_sqc, TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
      DIV_ROUND_UP(sizeof(uint32_t) * TERAKAN_META_CLEAR_COLOR_CONSTS_COUNT,
                   TERAKAN_KCACHE_HW_LINE_BYTES),
      constants_bo, constants_va_lines);

   terakan_meta_begin_2d_immediate_rects(command_writer, TERAKAN_META_PA_CL_VTE_CNTL_2D,
                                         TERAKAN_META_DB_RENDER_OVERRIDE_DEFAULT, true);

   terakan_meta_set_vs(command_writer, TERAKAN_META_SHADER_POSITION_AND_LAYER_FROM_INDEX_VS);
   terakan_meta_set_ps(command_writer, TERAKAN_META_SHADER_CLEAR_COLOR_PS, false);

   terakan_meta_begin_cb(command_writer, 0xF, 0b1);

   for (uint32_t range_index = 0; range_index < rangeCount; ++range_index) {
      VkImageSubresourceRange const * const range = &pRanges[range_index];

      uint32_t const range_level_count = vk_image_subresource_level_count(&image->vk, range);
      for (uint32_t level_base_relative_index = 0; level_base_relative_index < range_level_count;
           ++level_base_relative_index) {
         image_descriptor_create_info.base_mip_level =
            range->baseMipLevel + level_base_relative_index;

         if (image->vk.image_type == VK_IMAGE_TYPE_3D) {
            image_descriptor_create_info.base_array_layer = 0;
            image_descriptor_create_info.layer_count =
               u_minify(image->vk.extent.depth, image_descriptor_create_info.base_mip_level);
         } else {
            image_descriptor_create_info.base_array_layer = range->baseArrayLayer;
            image_descriptor_create_info.layer_count =
               vk_image_subresource_layer_count(&image->vk, range);
         }

         VkRect2D const rect = {
            .extent =
               {
                  .width =
                     u_minify(image->vk.extent.width, image_descriptor_create_info.base_mip_level),
                  .height =
                     u_minify(image->vk.extent.height, image_descriptor_create_info.base_mip_level),
               },
         };

         while (image_descriptor_create_info.layer_count > 0) {
            struct terakan_color_descriptor color_descriptor;
            struct terakan_color_meta_descriptor color_meta_descriptor;
            uint32_t const color_descriptor_layer_count = terakan_image_create_color_descriptor(
               &image_descriptor_create_info, &color_descriptor, &color_meta_descriptor);
            terakan_color_descriptor_image_view_to_color_attachment(&color_descriptor);
            terakan_hw_state_draw_set_cb_color(&command_writer->hw_state_draw, 0, image->bo,
                                               &color_descriptor, &color_meta_descriptor, false);
            terakan_meta_set_db_shader_control_with_rtv(
               command_writer, terakan_meta_clear_color_ps.stage.ps.db_shader_control,
               color_descriptor.info);

            terakan_meta_emit_rect_3_vertices_draw(command_writer, &rect,
                                                   color_descriptor_layer_count);

            image_descriptor_create_info.base_array_layer += color_descriptor_layer_count;
            image_descriptor_create_info.layer_count -= color_descriptor_layer_count;
         }
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdClearAttachments(VkCommandBuffer const commandBuffer, uint32_t const attachmentCount,
                            VkClearAttachment const * const pAttachments, uint32_t const rectCount,
                            VkClearRect const * const pRects)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   /* Setup depth / stencil clearing.
    * If possible, it will be cleared alongside a color attachment.
    * For clearing depth, in addition to depth clamping (which is overall recommended for optimal
    * performance according to PAL), using vertex Z (applied via the viewport offset to zero
    * returned from the shader) matching the clamping bounds so the clear rectangle can be expressed
    * as a plane in HTile.
    */

   VkClearDepthStencilValue depth_stencil_clear_value = {};
   VkImageAspectFlags depth_stencil_clear_aspects = 0;
   for (uint32_t attachment_index = 0; attachment_index < attachmentCount; ++attachment_index) {
      VkClearAttachment const * const attachment = &pAttachments[attachment_index];
      if (attachment->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
         depth_stencil_clear_value.depth = attachment->clearValue.depthStencil.depth;
         depth_stencil_clear_aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
      }
      if (attachment->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
         depth_stencil_clear_value.stencil = attachment->clearValue.depthStencil.stencil;
         depth_stencil_clear_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
      }
   }
   if (command_writer->state_draw.db_depth_stencil_buffer.bo != NULL) {
      if (G_028040_FORMAT(command_writer->state_draw.db_depth_stencil_buffer.descriptor.z_info) ==
          V_028040_Z_INVALID) {
         depth_stencil_clear_aspects &= ~VK_IMAGE_ASPECT_DEPTH_BIT;
      }
      if (G_028044_FORMAT(
             command_writer->state_draw.db_depth_stencil_buffer.descriptor.stencil_info) ==
          V_028044_STENCIL_INVALID) {
         depth_stencil_clear_aspects &= ~VK_IMAGE_ASPECT_STENCIL_BIT;
      }
   } else {
      depth_stencil_clear_aspects = 0;
   }

   /* depth_stencil_base_layer is undefined if not clearing depth / stencil. */
   uint32_t const depth_stencil_base_layer =
      G_028008_SLICE_START(command_writer->state_draw.db_depth_stencil_buffer.descriptor.view);

   if (depth_stencil_clear_aspects) {
      assert(command_writer->state_draw.db_depth_stencil_buffer.bo != NULL);
      struct terakan_depth_stencil_descriptor depth_stencil_descriptor =
         command_writer->state_draw.db_depth_stencil_buffer.descriptor;
      depth_stencil_descriptor.view =
         (depth_stencil_descriptor.view & C_028008_SLICE_START) |
         S_028008_SLICE_START(depth_stencil_base_layer + pRects[0].baseArrayLayer);
      bool const depth_stencil_binding_modified =
         command_writer->hw_state_draw.db_depth_stencil_buffer.bo !=
            command_writer->state_draw.db_depth_stencil_buffer.bo ||
         memcmp(&command_writer->hw_state_draw.db_depth_stencil_buffer.descriptor,
                &depth_stencil_descriptor, sizeof(struct terakan_depth_stencil_descriptor)) != 0;
      /* Make the depth / stencil buffer binding state pending regardless of whether the binding was
       * modified because rectangles may have different base array layers.
       */
      terakan_state_draw_set_pending(&command_writer->state_draw,
                                     TERAKAN_STATE_DRAW_INDEX_DB_DEPTH_STENCIL_BUFFER);
      if (depth_stencil_binding_modified) {
         command_writer->hw_state_draw.db_depth_stencil_buffer.bo =
            command_writer->state_draw.db_depth_stencil_buffer.bo;
         command_writer->hw_state_draw.db_depth_stencil_buffer.descriptor =
            depth_stencil_descriptor;
      }
      terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                    TERAKAN_HW_STATE_DRAW_INDEX_DB_DEPTH_STENCIL_BUFFER,
                                    depth_stencil_binding_modified);

      uint32_t db_depth_control = 0;

      if (depth_stencil_clear_aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
         /* Make sure the clear isn't discarded (which may also result in the color clear skipped
          * too) due to a NaN vertex position, and flush denormals so the value is surely the same
          * in every part of the pipeline it passes.
          */
         if (unlikely(isnan(depth_stencil_clear_value.depth) ||
                      fabsf(depth_stencil_clear_value.depth) < 0x1.0p-126)) {
            depth_stencil_clear_value.depth = 0.0f;
         }

         terakan_state_draw_set_pending(&command_writer->state_draw,
                                        TERAKAN_STATE_DRAW_INDEX_VIEWPORT);
         struct terakan_hw_state_draw_viewport * const viewport =
            &command_writer->hw_state_draw.viewports[0];
         bool viewport_z_modified = false;
         if (memcmp(&viewport->pa_cl_vport_xyz_scale_offset[2][1], &depth_stencil_clear_value.depth,
                    sizeof(float)) != 0) {
            command_writer->state_draw.viewport.viewports_pending.pa_cl_vport_z_scale_offset |=
               BITFIELD_BIT(0);
            viewport_z_modified = true;
            viewport->pa_cl_vport_xyz_scale_offset[2][1] = depth_stencil_clear_value.depth;
         }
         if (memcmp(&viewport->pa_sc_vport_z_min_max[0], &depth_stencil_clear_value.depth,
                    sizeof(float)) != 0 ||
             memcmp(&viewport->pa_sc_vport_z_min_max[1], &depth_stencil_clear_value.depth,
                    sizeof(float)) != 0) {
            command_writer->state_draw.viewport.viewports_pending.pa_sc_vport_z_min_max |=
               BITFIELD_BIT(0);
            viewport_z_modified = true;
            viewport->pa_sc_vport_z_min_max[0] = depth_stencil_clear_value.depth;
            viewport->pa_sc_vport_z_min_max[1] = depth_stencil_clear_value.depth;
         }
         terakan_hw_state_draw_update_viewports(
            &command_writer->hw_state_draw, 1,
            viewport_z_modified ? 0 : ARRAY_SIZE(command_writer->hw_state_draw.viewports),
            ARRAY_SIZE(command_writer->hw_state_draw.viewports));

         db_depth_control |= S_028800_Z_ENABLE(1) | S_028800_Z_WRITE_ENABLE(1) |
                             S_028800_ZFUNC((uint32_t)VK_COMPARE_OP_ALWAYS);
      }

      if (depth_stencil_clear_aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
         uint32_t const db_stencilrefmask = S_028430_STENCILREF(depth_stencil_clear_value.stencil) |
                                            S_028430_STENCILWRITEMASK(UINT8_MAX);
         /* Not using the values for back faces, but the hw_state_draw item must be initialized
          * fully.
          */
         bool const db_stencilrefmask_modified =
            command_writer->hw_state_draw.db_stencilrefmask_front_back[0] != db_stencilrefmask ||
            command_writer->hw_state_draw.db_stencilrefmask_front_back[1] != db_stencilrefmask;
         if (db_stencilrefmask_modified) {
            terakan_state_draw_set_pending(&command_writer->state_draw,
                                           TERAKAN_STATE_DRAW_INDEX_DB_STENCILREFMASK);
            command_writer->hw_state_draw.db_stencilrefmask_front_back[0] = db_stencilrefmask;
            command_writer->hw_state_draw.db_stencilrefmask_front_back[1] = db_stencilrefmask;
         }
         terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                       TERAKAN_HW_STATE_DRAW_INDEX_DB_STENCILREFMASK,
                                       db_stencilrefmask_modified);

         db_depth_control |= S_028800_STENCIL_ENABLE(1) |
                             S_028800_STENCILFUNC(V_028800_STENCILFUNC_ALWAYS) |
                             S_028800_STENCILFAIL(V_028800_STENCIL_REPLACE) |
                             S_028800_STENCILZPASS(V_028800_STENCIL_REPLACE) |
                             S_028800_STENCILZFAIL(V_028800_STENCIL_REPLACE);
      }

      terakan_meta_set_db_depth_control(command_writer, db_depth_control);
   }

   terakan_meta_begin_2d_immediate_rects(
      command_writer,
      TERAKAN_META_PA_CL_VTE_CNTL_2D |
         (depth_stencil_clear_aspects & VK_IMAGE_ASPECT_DEPTH_BIT ? S_028818_VPORT_Z_OFFSET_ENA(1)
                                                                  : 0),
      TERAKAN_META_DB_RENDER_OVERRIDE_DEFAULT, false);

   terakan_meta_set_vs(command_writer, TERAKAN_META_SHADER_POSITION_AND_LAYER_FROM_INDEX_VS);

   /* TODO(Triang3l): Clear CMask and update the stored fast clear color for COPY_DW into the
    * registers if clearing the entire image.
    */

   command_writer->push_constants_state.up_to_date_push_constants_bound_to_stages &=
      ~VK_SHADER_STAGE_FRAGMENT_BIT;

   uint32_t constants[TERAKAN_META_CLEAR_COLOR_CONSTS_COUNT] = {};
   struct terakan_bo const * constants_bo = NULL;
   uint32_t constants_va_lines;

   for (uint32_t attachment_index = 0; attachment_index < attachmentCount; ++attachment_index) {
      VkClearAttachment const * const attachment = &pAttachments[attachment_index];
      if (!(attachment->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT)) {
         continue;
      }

      if (memcmp(&constants[TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE],
                 attachment->clearValue.color.uint32, sizeof(uint32_t) * 4) != 0) {
         constants_bo = NULL;
         memcpy(&constants[TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE],
                attachment->clearValue.color.uint32, sizeof(uint32_t) * 4);
      }

      if (constants_bo == NULL) {
         void * const constants_mapping = terakan_push_buffer_allocate_kcache(
            command_writer->base.command_buffer, sizeof(constants), &constants_bo,
            &constants_va_lines);
         if (unlikely(constants_mapping == NULL)) {
            return;
         }
         memcpy(constants_mapping, constants, sizeof(constants));
         terakan_hw_state_sqc_set_kcache_fs(
            &command_writer->hw_state_sqc, TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
            DIV_ROUND_UP(sizeof(constants), TERAKAN_KCACHE_HW_LINE_BYTES), constants_bo,
            constants_va_lines);
      }

      terakan_meta_set_ps(command_writer, TERAKAN_META_SHADER_CLEAR_COLOR_PS, false);

      terakan_meta_begin_cb(command_writer, 0xF, 0b1);

      struct terakan_state_draw_cb_color const * const attachment_cb_color =
         &command_writer->state_draw.cb_color_rtv.attachments[attachment->colorAttachment];
      struct terakan_color_descriptor cb_color = attachment_cb_color->color;
      uint32_t const attachment_base_layer = G_028C6C_SLICE_START(cb_color.view);
      cb_color.view = (cb_color.view & C_028C6C_SLICE_START) |
                      S_028C6C_SLICE_START(attachment_base_layer + pRects[0].baseArrayLayer);
      terakan_hw_state_draw_set_cb_color(&command_writer->hw_state_draw, 0, attachment_cb_color->bo,
                                         &cb_color, &attachment_cb_color->meta, false);
      terakan_meta_set_db_shader_control_with_rtv(
         command_writer, terakan_meta_clear_color_ps.stage.ps.db_shader_control, cb_color.info);

      if (!depth_stencil_clear_aspects) {
         /* No depth or stencil to clear, or previously cleared alongside color.
          * Disable depth / stencil clearing, including disabling the viewport Z offset.
          * After this, the draws for this attachment must not be aborted for reasons other than
          * those causing the command buffer to enter an error state, or the depth / stencil clear
          * won't happen at all as it's already disabled in the hardware state.
          */
         terakan_meta_set_pa_cl_vte_cntl(command_writer, TERAKAN_META_PA_CL_VTE_CNTL_2D);
         terakan_meta_set_db_depth_control(command_writer, 0);
      }

      for (uint32_t rect_index = 0; rect_index < rectCount; ++rect_index) {
         VkClearRect const * const rect = &pRects[rect_index];
         if (rect_index != 0 && rect->baseArrayLayer != pRects[rect_index - 1].baseArrayLayer) {
            if (depth_stencil_clear_aspects) {
               command_writer->hw_state_draw.db_depth_stencil_buffer.descriptor.view =
                  (command_writer->hw_state_draw.db_depth_stencil_buffer.descriptor.view &
                   C_028008_SLICE_START) |
                  S_028008_SLICE_START(depth_stencil_base_layer + rect->baseArrayLayer);
               terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                             TERAKAN_HW_STATE_DRAW_INDEX_DB_DEPTH_STENCIL_BUFFER,
                                             true);
            }
            cb_color.view = (cb_color.view & C_028C6C_SLICE_START) |
                            S_028C6C_SLICE_START(attachment_base_layer + rect->baseArrayLayer);
            terakan_hw_state_draw_set_cb_color(&command_writer->hw_state_draw, 0,
                                               attachment_cb_color->bo, &cb_color,
                                               &attachment_cb_color->meta, false);
         }
         terakan_meta_emit_rect_3_vertices_draw(command_writer, &rect->rect, rect->layerCount);
      }

      /* If there was depth or stencil to clear, it's cleared now alongside the color attachment. */
      depth_stencil_clear_aspects = 0;
   }

   if (depth_stencil_clear_aspects) {
      /* Depth/stencil was not cleared alongside color. Clear it now. */
      terakan_meta_set_ps(command_writer, TERAKAN_META_SHADER_EMPTY_OPAQUE_PS, true);
      terakan_meta_begin_cb(command_writer, 0x0, 0b0);
      for (uint32_t rect_index = 0; rect_index < rectCount; ++rect_index) {
         VkClearRect const * const rect = &pRects[rect_index];
         if (rect_index != 0 && rect->baseArrayLayer != pRects[rect_index - 1].baseArrayLayer) {
            command_writer->hw_state_draw.db_depth_stencil_buffer.descriptor.view =
               (command_writer->hw_state_draw.db_depth_stencil_buffer.descriptor.view &
                C_028008_SLICE_START) |
               S_028008_SLICE_START(depth_stencil_base_layer + rect->baseArrayLayer);
            terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                          TERAKAN_HW_STATE_DRAW_INDEX_DB_DEPTH_STENCIL_BUFFER,
                                          true);
         }
         terakan_meta_emit_rect_3_vertices_draw(command_writer, &rect->rect, rect->layerCount);
      }
   }
}
