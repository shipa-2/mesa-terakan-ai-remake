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

#include "gallium/drivers/r600/eg_sq.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600_opcodes.h"
#include "util/macros.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct terakan_meta_clear_color_push_constants {
   uint32_t clear_value[4];
};

static uint32_t const terakan_meta_clear_color_ps_r8xx[] = {
   /* Control flow. */

   /* 0: Clear value loading. */

   S_SQ_CF_WORD0_ADDR(2) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(0) | S_SQ_CF_ALU_WORD1_COUNT(5 - 2) |
      S_SQ_CF_ALU_WORD1_BARRIER(1) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Export the color and end the program. */

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(V_03000C_SQ_SEL_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(V_03000C_SQ_SEL_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(V_03000C_SQ_SEL_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(V_03000C_SQ_SEL_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(1) | S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(1) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* ALU clause. */

   /* Move the clear value from the kcache to the GPRs.
    *
    * 2: R0.X = MOV CB[push_constants][0].X
    * 3: R0.Y = MOV CB[push_constants][0].Y
    * 4: R0.Z = MOV CB[push_constants][0].Z
    * 5: R0.W = MOV CB[push_constants][0].W
    */

   S_SQ_ALU_WORD0_SRC0_SEL(0x80) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_0),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV),

   S_SQ_ALU_WORD0_SRC0_SEL(0x80) | S_SQ_ALU_WORD0_SRC0_CHAN(1) |
      S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_0),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV),

   S_SQ_ALU_WORD0_SRC0_SEL(0x80) | S_SQ_ALU_WORD0_SRC0_CHAN(2) |
      S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_0),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(2) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV),

   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(0x80) | S_SQ_ALU_WORD0_SRC0_CHAN(3) |
      S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_0),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(3) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV),
};

static uint32_t const terakan_meta_clear_color_ps_r9xx[] = {
   /* Control flow. */

   /* 0: Clear value loading. */

   S_SQ_CF_WORD0_ADDR(3) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(0) | S_SQ_CF_ALU_WORD1_COUNT(6 - 3) |
      S_SQ_CF_ALU_WORD1_BARRIER(1) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Export the color. */

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(V_03000C_SQ_SEL_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(V_03000C_SQ_SEL_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(V_03000C_SQ_SEL_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(V_03000C_SQ_SEL_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(1) | EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 2: End the program. */

   0,
   S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(1) | CM_V_SQ_CF_WORD1_SQ_CF_INST_END,

   /* ALU clause. */

   /* Move the clear value from the kcache to the GPRs.
    *
    * 3: R0.X = MOV CB[push_constants][0].X
    * 4: R0.Y = MOV CB[push_constants][0].Y
    * 5: R0.Z = MOV CB[push_constants][0].Z
    * 6: R0.W = MOV CB[push_constants][0].W
    */

   S_SQ_ALU_WORD0_SRC0_SEL(0x80) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_0),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV),

   S_SQ_ALU_WORD0_SRC0_SEL(0x80) | S_SQ_ALU_WORD0_SRC0_CHAN(1) |
      S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_0),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV),

   S_SQ_ALU_WORD0_SRC0_SEL(0x80) | S_SQ_ALU_WORD0_SRC0_CHAN(2) |
      S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_0),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(2) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV),

   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(0x80) | S_SQ_ALU_WORD0_SRC0_CHAN(3) |
      S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_0),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(3) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV),
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
};

VKAPI_ATTR void VKAPI_CALL
terakan_CmdClearAttachments(VkCommandBuffer const commandBuffer, uint32_t const attachmentCount,
                            VkClearAttachment const * const pAttachments, uint32_t const rectCount,
                            VkClearRect const * const pRects)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   terakan_meta_begin_2d_immediate_rects(command_writer, TERAKAN_META_DB_RENDER_OVERRIDE_DEFAULT);

   terakan_meta_set_vs(command_writer, TERAKAN_META_SHADER_POSITION_FROM_INDEX_VS);
   terakan_meta_set_ps(command_writer, TERAKAN_META_SHADER_CLEAR_COLOR_PS);

   /* TODO(Triang3l): Clear depth and stencil via the viewport Z min/max and stencil reference. */

   /* TODO(Triang3l): Clear CMask and update the stored fast clear color for COPY_DW into the
    * registers if clearing the entire image.
    */

   command_writer->push_constants_state.up_to_date_push_constants_bound_to_stages &=
      ~VK_SHADER_STAGE_FRAGMENT_BIT;

   struct terakan_meta_clear_color_push_constants push_constants = {};
   struct terakan_bo const * push_constants_bo = NULL;
   uint32_t push_constants_base;

   for (uint32_t attachment_index = 0; attachment_index < attachmentCount; ++attachment_index) {
      VkClearAttachment const * const attachment = &pAttachments[attachment_index];
      if (!(attachment->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT)) {
         continue;
      }

      assert(command_writer->state_draw.color_attachment_usage.bound &
             ((uint8_t)1 << attachment->colorAttachment));
      if (unlikely(!(command_writer->state_draw.color_attachment_usage.bound &
                     ((uint8_t)1 << attachment->colorAttachment)))) {
         continue;
      }

      if (memcmp(push_constants.clear_value, attachment->clearValue.color.uint32,
                 sizeof(uint32_t) * 4) != 0) {
         memcpy(push_constants.clear_value, attachment->clearValue.color.uint32,
                sizeof(uint32_t) * 4);
         push_constants_bo = NULL;
      }

      if (push_constants_bo == NULL) {
         void * const push_constants_mapping = terakan_command_buffer_allocate_push_constants(
            command_writer->base.command_buffer, sizeof(push_constants), &push_constants_bo,
            &push_constants_base);
         if (unlikely(push_constants_mapping == NULL)) {
            return;
         }
         memcpy(push_constants_mapping, &push_constants, sizeof(push_constants));
         terakan_hw_state_draw_set_sq_kcache_fs(
            &command_writer->hw_state_draw, TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
            (sizeof(push_constants) + (TERAKAN_KCACHE_HW_LINE_BYTES - 1)) /
               TERAKAN_KCACHE_HW_LINE_BYTES,
            push_constants_bo, push_constants_base);
      }

      terakan_meta_begin_cb(command_writer, 0b1111, V_028808_CB_NORMAL);

      struct terakan_state_draw_cb_color const * const attachment_cb_color =
         &command_writer->state_draw.attachment_cb_color[attachment->colorAttachment];
      struct terakan_color_descriptor cb_color = attachment_cb_color->color;
      uint32_t const attachment_base_layer = G_028C6C_SLICE_START(cb_color.view);
      cb_color.view = (cb_color.view & C_028C6C_SLICE_START) |
                      S_028C6C_SLICE_START(attachment_base_layer + pRects[0].baseArrayLayer);
      bool const cb_color_modified =
         command_writer->hw_state_draw.cb_color.bo[0] != attachment_cb_color->bo ||
         memcmp(&command_writer->hw_state_draw.cb_color.color[0], &cb_color, sizeof(cb_color)) !=
            0 ||
         memcmp(&command_writer->hw_state_draw.cb_color.meta[0], &attachment_cb_color->meta,
                sizeof(attachment_cb_color->meta)) != 0;
      if (cb_color_modified) {
         command_writer->hw_state_draw.cb_color.bo[0] = attachment_cb_color->bo;
         memcpy(&command_writer->hw_state_draw.cb_color.color[0], &cb_color, sizeof(cb_color));
         memcpy(&command_writer->hw_state_draw.cb_color.meta[0], &attachment_cb_color->meta,
                sizeof(attachment_cb_color->meta));
      }
      terakan_hw_state_draw_cb_color_written(&command_writer->hw_state_draw, 0, cb_color_modified);

      for (uint32_t rect_index = 0; rect_index < rectCount; ++rect_index) {
         VkClearRect const * const rect = &pRects[rect_index];
         if (rect_index != 0 && rect->baseArrayLayer != pRects[rect_index - 1].baseArrayLayer) {
            command_writer->hw_state_draw.cb_color.color[0].view =
               (cb_color.view & C_028C6C_SLICE_START) |
               S_028C6C_SLICE_START(attachment_base_layer + rect->baseArrayLayer);
            terakan_hw_state_draw_cb_color_written(&command_writer->hw_state_draw, 0, true);
         }
         terakan_meta_emit_rect_3_vertices_draw(command_writer, &rect->rect, rect->layerCount);
      }
   }
}
