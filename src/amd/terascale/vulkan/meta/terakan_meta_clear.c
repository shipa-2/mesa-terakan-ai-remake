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

#include "terakan_command_buffer.h"
#include "terakan_device.h"
#include "terakan_entrypoints.h"
#include "terakan_image.h"
#include "terakan_vk_state.h"

#include "util/macros.h"
#include "util/u_endian.h"
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

static_assert(
   (TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE & 1) == 0,
   "The color clear value must be accessible via kcache read ports available to one instruction "
   "group (two XY or ZW pairs on R8xx / R9xx).");

enum {
   TERAKAN_META_CLEAR_DEPTH_CONST_CLEAR_VALUE,

   TERAKAN_META_CLEAR_DEPTH_CONSTS_COUNT,
};

/* Like `terakan_meta_position_and_layer_from_index_vs`, but also copies the depth from the
 * constant.
 */

static uint32_t const terakan_meta_clear_depth_vs_r8xx[] = {
   /* 0: Export the instance ID as the first parameter. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PARAM) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 1: Export the instance ID as the render target array layer index. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_POS) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(61) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_MASK) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_MASK) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_MASK) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT,

   /* 2: Vertex position calculation. */
   S_SQ_CF_WORD0_ADDR(4) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(
      TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_CLEAR_DEPTH_CONST_CLEAR_VALUE)) |
      S_SQ_CF_ALU_WORD1_COUNT(6) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 3: Export the position in R0.XYZ1 and end the program. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_POS) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(60) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_1) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) | S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 4: ALU clause. */

   /* +0-2: Extract bits 15:0 of the vertex index (to be used as position) to PV.X and bits 31:16 to
    * PV.Y.
    * Cycle 0: X = R0.
    */
   TERAKAN_SHADER_OP3_NW(false, 'X', BFE_UINT, EG, 0, 'X', V_SQ_ALU_SRC_0, 0, V_SQ_ALU_SRC_LITERAL,
                         'X', VEC_012),
   TERAKAN_SHADER_OP3_NW(true, 'Y', BFE_UINT, EG, 0, 'X', V_SQ_ALU_SRC_LITERAL, 'X',
                         V_SQ_ALU_SRC_LITERAL, 'X', VEC_012),
   16,
   0,

   /* +3-4: Convert the vertex position X from int to float to R0.X, and pass PV.Y further
    * (INT_TO_FLT can be executed only on the transcendental unit).
    */
   TERAKAN_SHADER_OP1_NW(false, 'Y', MOV, EG, V_SQ_ALU_SRC_PV, 'Y', VEC_012),
   TERAKAN_SHADER_OP1(true, 0, 'X', INT_TO_FLT, EG, V_SQ_ALU_SRC_PV, 'X', SCL_210),

   /* +5-6: Convert the vertex position Y from int to float to R0.Y, and copy the depth to R0.Z. */
   TERAKAN_KCACHE_DWORD_WORD0_SRC01(0, TERAKAN_META_CLEAR_DEPTH_CONST_CLEAR_VALUE) |
      TERAKAN_SHADER_OP1(false, 0, 'Z', MOV, EG, 0, 0, VEC_012),
   TERAKAN_SHADER_OP1(true, 0, 'Y', INT_TO_FLT, EG, V_SQ_ALU_SRC_PV, 'Y', SCL_210),
};

static uint32_t const terakan_meta_clear_depth_vs_r9xx[] = {
   /* 0: Export the instance ID as the first parameter. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PARAM) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 1: Export the instance ID as the render target array layer index. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_POS) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(61) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_MASK) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_MASK) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_MASK) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT,

   /* 2: Vertex position calculation. */
   S_SQ_CF_WORD0_ADDR(5) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(
      TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_CLEAR_DEPTH_CONST_CLEAR_VALUE)) |
      S_SQ_CF_ALU_WORD1_COUNT(5) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 3: Export the position in R0.XYZ1. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_POS) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(60) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_1) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 4: End the program. */
   TERAKAN_SHADER_CF_END_R9XX,

   /* 5: ALU clause. */

   /* +0-2: Extract bits 15:0 of the vertex index (to be used as position) to PV.X and bits 31:16 to
    * PV.Y.
    * Cycle 0: X = R0.
    */
   TERAKAN_SHADER_OP3_NW(false, 'X', BFE_UINT, EG, 0, 'X', V_SQ_ALU_SRC_0, 0, V_SQ_ALU_SRC_LITERAL,
                         'X', VEC_012),
   TERAKAN_SHADER_OP3_NW(true, 'Y', BFE_UINT, EG, 0, 'X', V_SQ_ALU_SRC_LITERAL, 'X',
                         V_SQ_ALU_SRC_LITERAL, 'X', VEC_012),
   16,
   0,

   /* +3-5: Convert the vertex XY position from int to float to R0.XY, and copy the depth to R0.Z.
    */
   TERAKAN_SHADER_OP1(false, 0, 'X', INT_TO_FLT, EG, V_SQ_ALU_SRC_PV, 'X', VEC_012),
   TERAKAN_SHADER_OP1(false, 0, 'Y', INT_TO_FLT, EG, V_SQ_ALU_SRC_PV, 'Y', VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC01(0, TERAKAN_META_CLEAR_DEPTH_CONST_CLEAR_VALUE) |
      TERAKAN_SHADER_OP1(true, 0, 'Z', MOV, EG, 0, 0, VEC_012),
};

struct terakan_meta_shader const terakan_meta_clear_depth_vs = {
   .r8xx =
      {
         .program = terakan_meta_clear_depth_vs_r8xx,
         .program_size_bytes = sizeof(terakan_meta_clear_depth_vs_r8xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     S_028860_NUM_GPRS(1) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                     TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                  },
               .stage =
                  {
                     .vs =
                        {
                           .pa_cl_vs_out_cntl = S_02881C_USE_VTX_RENDER_TARGET_INDX(1) |
                                                S_02881C_VS_OUT_MISC_VEC_ENA(1),
                        },
                  },
            },
      },
   .r9xx =
      {
         .program = terakan_meta_clear_depth_vs_r9xx,
         .program_size_bytes = sizeof(terakan_meta_clear_depth_vs_r9xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     S_028860_NUM_GPRS(1) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                     TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                  },
               .stage =
                  {
                     .vs =
                        {
                           .pa_cl_vs_out_cntl = S_02881C_USE_VTX_RENDER_TARGET_INDX(1) |
                                                S_02881C_VS_OUT_MISC_VEC_ENA(1),
                        },
                  },
            },
      },
   .kcache_used = BITFIELD_BIT(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS),
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
   .kcache_used = BITFIELD_BIT(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS),
};

VKAPI_ATTR void VKAPI_CALL
terakan_CmdClearColorImage(VkCommandBuffer const commandBuffer, VkImage const imageHandle,
                           UNUSED VkImageLayout const imageLayout,
                           VkClearColorValue const * const pColor, uint32_t const rangeCount,
                           VkImageSubresourceRange const * const pRanges)
{
   struct terakan_image const * const image = terakan_image_from_handle(imageHandle);

   /* VUID-vkCmdClearColorImage-aspectMask-02498: "The VkImageSubresourceRange::aspectMask members
    * of the elements of the pRanges array must each only include VK_IMAGE_ASPECT_COLOR_BIT"
    */
   unsigned const aspect_index =
      terakan_format_aspect_index(image->format_info.aspect_map, VK_IMAGE_ASPECT_COLOR_BIT, 0);

   struct terascale_format_info const format_info = image->format_info.aspect_formats[aspect_index];
   uint8_t const format_bytes_per_block = terascale_format_bytes_per_block[format_info.format];

   /* TODO(Triang3l): 3x-expanded format clearing. */
   if (unlikely(!IS_POT(format_bytes_per_block))) {
      return;
   }

   struct terakan_image_descriptor_create_info image_descriptor_create_info = {
      .image = image,
      .image_aspect_index = aspect_index,
      .view_format = terakan_meta_transfer_image_block_format_info(format_bytes_per_block),
      .subresource_range = {.max_level_count = 1},
   };

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   struct terakan_meta_config_draw_begin_options const meta_begin_options = {
      .vgt_primitive_type = V_008958_DI_PT_RECTLIST,
      .cb_and_db_shader_control_mode =
         TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_NORMAL_WITH_RTV_AND_DYNAMIC_DB_SHADER_CONTROL,
      .rasterization =
         {
            .enable = true,
            .msaa_num_samples_log2 = util_logbase2((uint32_t)image->vk.samples),
            .msaa_num_anchor_samples_log2 = util_logbase2((uint32_t)image->vk.samples),
         },
   };
   terakan_meta_config_draw_begin(command_writer, &meta_begin_options);
   terakan_meta_config_draw_set_sq_pgm_vs(command_writer,
                                          TERAKAN_META_SHADER_POSITION_AND_LAYER_FROM_INDEX_VS);
   terakan_meta_config_draw_set_sq_pgm_ps(command_writer, TERAKAN_META_SHADER_CLEAR_COLOR_PS);

   struct terakan_bo const * constants_bo;
   uint32_t constants_va_lines;
   uint32_t * const constants_mapping = terakan_push_buffer_allocate_kcache(
      command_writer->base.command_buffer, sizeof(uint32_t) * TERAKAN_META_CLEAR_COLOR_CONSTS_COUNT,
      &constants_bo, &constants_va_lines);
   if (unlikely(constants_mapping == NULL)) {
      return;
   }

   uint8_t clear_value[sizeof(uint32_t) * 4] = {};
   terascale_format_pack_color(&format_info, pColor->uint32, clear_value);
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

   terakan_meta_config_draw_set_kcache_push_constant_buffer_ps(
      command_writer,
      DIV_ROUND_UP(sizeof(uint32_t) * TERAKAN_META_CLEAR_COLOR_CONSTS_COUNT,
                   TERAKAN_KCACHE_HW_LINE_BYTES),
      constants_bo, constants_va_lines);

   bool const image_is_3d = image->vk.image_type == VK_IMAGE_TYPE_3D;

   for (uint32_t range_index = 0; range_index < rangeCount; ++range_index) {
      VkImageSubresourceRange const * const range = &pRanges[range_index];

      /* Perform `VK_REMAINING_*` handling and sanitization for the mip range, as well the array
       * layer range for non-3D images.
       */
      struct terakan_image_descriptor_subresource_range range_subresource_range = {
         .base_mip_level = range->baseMipLevel,
         .max_level_count = range->levelCount,
         .max_depth_or_layer_count = 1,
      };
      if (!image_is_3d) {
         range_subresource_range.base_z_or_array_layer = range->baseArrayLayer;
         range_subresource_range.max_depth_or_layer_count = range->layerCount;
      }
      if (unlikely(!terakan_image_descriptor_subresource_range_sanitize(
             image, &range_subresource_range, false))) {
         continue;
      }

      for (uint32_t level_base_relative_index = 0;
           level_base_relative_index < range_subresource_range.max_level_count;
           ++level_base_relative_index) {
         image_descriptor_create_info.subresource_range.base_mip_level =
            range_subresource_range.base_mip_level + level_base_relative_index;
         if (image_is_3d) {
            image_descriptor_create_info.subresource_range.base_z_or_array_layer = 0;
            image_descriptor_create_info.subresource_range.max_depth_or_layer_count =
               u_minify(image->vk.extent.depth,
                        image_descriptor_create_info.subresource_range.base_mip_level);
         } else {
            image_descriptor_create_info.subresource_range.base_z_or_array_layer =
               range_subresource_range.base_z_or_array_layer;
            image_descriptor_create_info.subresource_range.max_depth_or_layer_count =
               range_subresource_range.max_depth_or_layer_count;
         }

         do {
            struct terakan_color_descriptor image_descriptor;
            uint32_t const image_descriptor_slices = terakan_image_create_color_descriptor(
               &image_descriptor_create_info, V_028C70_TEXTURE2DARRAY, &image_descriptor, NULL);
            if (unlikely(image_descriptor_slices == 0)) {
               break;
            }
#if UTIL_ARCH_BIG_ENDIAN
            /* Reinterpreting as a raw format with the clear value pre-swapped. */
            image_descriptor.info &= C_028C70_ENDIAN;
#endif
            terakan_meta_config_draw_set_cb_rtvs_and_db_shader_control(
               command_writer, 0xF, &image->bo, &image_descriptor, NULL,
               TERAKAN_SHADER_DB_SHADER_CONTROL_IDENTITY);

            terakan_meta_draw_rect(
               command_writer,
               (struct terakan_screen_rect){
                  .bounds = {[1] = {G_028C78_WIDTH_MAX(image_descriptor.dim) + 1,
                                    G_028C78_HEIGHT_MAX(image_descriptor.dim) + 1}}},
               image_descriptor_slices);

            image_descriptor_create_info.subresource_range.base_z_or_array_layer +=
               image_descriptor_slices;
            image_descriptor_create_info.subresource_range.max_depth_or_layer_count -=
               image_descriptor_slices;
         } while (image_descriptor_create_info.subresource_range.max_depth_or_layer_count != 0);
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdClearDepthStencilImage(
   VkCommandBuffer const commandBuffer, VkImage const imageHandle,
   UNUSED VkImageLayout const imageLayout, VkClearDepthStencilValue const * const pDepthStencil,
   uint32_t const rangeCount, VkImageSubresourceRange const * const pRanges)
{
   struct terakan_image const * const image = terakan_image_from_handle(imageHandle);

   enum terascale_r8xx_depth_format depth_format = TERASCALE_R8XX_DEPTH_FORMAT_INVALID;
   bool image_has_stencil = false;
   if (unlikely(!terascale_get_r8xx_depth_stencil_format(
          vk_format_to_pipe_format(image->vk.format), &depth_format, &image_has_stencil))) {
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   struct terakan_meta_config_draw_begin_options const meta_begin_options = {
      .vgt_primitive_type = V_008958_DI_PT_RECTLIST,
      .cb_and_db_shader_control_mode = TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_DYNAMIC,
      .rasterization =
         {
            .enable = true,
            .db_explicit = true,
         },
   };
   terakan_meta_config_draw_begin(command_writer, &meta_begin_options);
   terakan_meta_config_draw_set_db_shader_control(command_writer,
                                                  TERAKAN_SHADER_DB_SHADER_CONTROL_IDENTITY);
   terakan_meta_config_draw_set_cb_color_control_for_mode(command_writer, V_028808_CB_DISABLE);
   terakan_meta_config_draw_set_sq_pgm_vs(command_writer, TERAKAN_META_SHADER_CLEAR_DEPTH_VS);
   terakan_meta_config_draw_set_sq_pgm_ps(command_writer, TERAKAN_META_SHADER_DUMMY_OPAQUE_PS);

   float depth_clear_value = pDepthStencil->depth;
   if (depth_format != TERASCALE_R8XX_DEPTH_FORMAT_32_FLOAT ||
       !terakan_gfx_command_writer_device(command_writer)
           ->vk.enabled_extensions.EXT_depth_range_unrestricted) {
      depth_clear_value = CLAMP(depth_clear_value, 0.0f, 1.0f);
   }
   float const depth_clear_constants[TERAKAN_META_CLEAR_DEPTH_CONSTS_COUNT] = {
      [TERAKAN_META_CLEAR_DEPTH_CONST_CLEAR_VALUE] = depth_clear_value,
   };
   terakan_meta_config_draw_set_kcache_push_constants(
      command_writer, sizeof(depth_clear_constants), depth_clear_constants, true, false);
   terakan_meta_config_draw_set_db_stencilrefmask(
      command_writer, false,
      S_028430_STENCILREF(pDepthStencil->stencil) | S_028430_STENCILWRITEMASK(0xFF));

   bool const image_is_3d = image->vk.image_type == VK_IMAGE_TYPE_3D;

   for (uint32_t range_index = 0; range_index < rangeCount; ++range_index) {
      VkImageSubresourceRange const * const range = &pRanges[range_index];
      VkImageAspectFlags aspects =
         range->aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
      if (!image_has_stencil) {
         aspects &= ~VK_IMAGE_ASPECT_STENCIL_BIT;
      }
      if (unlikely(aspects == 0)) {
         continue;
      }

      uint32_t db_depth_control = 0;
      if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
         db_depth_control |= S_028800_Z_ENABLE(true) | S_028800_Z_WRITE_ENABLE(true) |
                             S_028800_ZFUNC(V_028800_STENCILFUNC_ALWAYS);
      }
      if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
         db_depth_control |= S_028800_STENCIL_ENABLE(1) |
                             S_028800_STENCILFUNC(V_028800_STENCILFUNC_ALWAYS) |
                             S_028800_STENCILFAIL(V_028800_STENCIL_REPLACE) |
                             S_028800_STENCILZPASS(V_028800_STENCIL_REPLACE) |
                             S_028800_STENCILZFAIL(V_028800_STENCIL_REPLACE);
      }
      terakan_meta_config_draw_set_db_depth_control(command_writer, db_depth_control);

      struct terakan_image_descriptor_subresource_range subresource_range = {
         .base_mip_level = range->baseMipLevel,
         .max_level_count = range->levelCount,
         .max_depth_or_layer_count = 1,
      };
      if (!image_is_3d) {
         subresource_range.base_z_or_array_layer = range->baseArrayLayer;
         subresource_range.max_depth_or_layer_count = range->layerCount;
      }
      if (unlikely(!terakan_image_descriptor_subresource_range_sanitize(
             image, &subresource_range, false))) {
         continue;
      }

      uint32_t const level_count = subresource_range.max_level_count;
      for (uint32_t level_index = 0; level_index < level_count; ++level_index) {
         struct terakan_image_descriptor_subresource_range level_range = subresource_range;
         level_range.base_mip_level += level_index;
         level_range.max_level_count = 1;
         if (image_is_3d) {
            level_range.base_z_or_array_layer = 0;
            level_range.max_depth_or_layer_count =
               u_minify(image->vk.extent.depth, level_range.base_mip_level);
         }

         struct terakan_depth_stencil_descriptor descriptor;
         if (unlikely(!terakan_image_create_depth_stencil_descriptor(
                image, depth_format, image_has_stencil, &level_range, &descriptor))) {
            continue;
         }
         terakan_meta_config_draw_set_db_depth_stencil_buffer(command_writer, image->bo,
                                                              &descriptor);

         struct terakan_screen_rect const rect = {
            .bounds =
               {
                  {0, 0},
                  {
                     u_minify(image->vk.extent.width, level_range.base_mip_level),
                     u_minify(image->vk.extent.height, level_range.base_mip_level),
                  },
               },
         };
         terakan_meta_draw_rect(command_writer, rect, level_range.max_depth_or_layer_count);
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdClearAttachments(VkCommandBuffer const commandBuffer, uint32_t const attachmentCount,
                            VkClearAttachment const * const pAttachments, uint32_t const rectCount,
                            VkClearRect const * const pRects)
{
   /* TODO(Triang3l): HTile, CMask. If falling back to a rectangle, maybe export to all render
    * targets at once instead of performing one draw per color attachment.
    */

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   VkClearDepthStencilValue depth_stencil_clear_value = {};
   VkImageAspectFlags depth_stencil_clear_aspects = 0;

   VkClearColorValue color_clear_values[TERAKAN_VK_STATE_MAX_COLOR_ATTACHMENTS];
   uint8_t color_clear_attachments = 0b0;

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
      if ((attachment->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) &&
          likely(attachment->colorAttachment < TERAKAN_VK_STATE_MAX_COLOR_ATTACHMENTS)) {
         color_clear_values[attachment->colorAttachment] = attachment->clearValue.color;
         color_clear_attachments |= BITFIELD_BIT(attachment->colorAttachment);
      }
   }

   struct terakan_screen_rect const render_area =
      terakan_app_config_draw_get_pa_vport_render_area(&command_writer->app_config_draw);

   /* Load-op clears happen before a graphics pipeline is necessarily bound, so the current
    * rasterization sample count can't be taken from pipeline state. Derive it from the attachments
    * being cleared. Using one-sample rasterization for an MSAA attachment only clears sample zero,
    * leaving the other samples with old memory contents.
    */
   uint8_t clear_samples_log2 = 0;
   if (depth_stencil_clear_aspects) {
      struct terakan_bo const * depth_stencil_bo;
      struct terakan_depth_stencil_descriptor const * const depth_stencil_descriptor =
         terakan_app_config_draw_get_db_depth_stencil_buffer(&command_writer->app_config_draw,
                                                             &depth_stencil_bo);
      bool depth_bound, stencil_bound;
      terakan_depth_stencil_descriptor_is_bound(depth_stencil_bo, depth_stencil_descriptor,
                                                &depth_bound, &stencil_bound);
      if (depth_bound || stencil_bound) {
         clear_samples_log2 = MAX2(clear_samples_log2,
                                   (uint8_t)G_028040_NUM_SAMPLES(depth_stencil_descriptor->z_info));
      }
   }
   u_foreach_bit (color_attachment_index, color_clear_attachments) {
      struct terakan_app_config_draw_cb_color_rtv const * const render_pass_rtv =
         terakan_app_config_draw_get_cb_color_rtv(&command_writer->app_config_draw,
                                                  color_attachment_index);
      if (terakan_color_descriptor_is_bound(render_pass_rtv->bo, &render_pass_rtv->color)) {
         clear_samples_log2 =
            MAX2(clear_samples_log2, (uint8_t)G_028C74_NUM_SAMPLES(render_pass_rtv->color.attrib));
      }
   }

   struct terakan_meta_config_draw_begin_options const meta_begin_options = {
      .vgt_primitive_type = V_008958_DI_PT_RECTLIST,
      .cb_and_db_shader_control_mode = TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_DYNAMIC,
      .rasterization =
         {
            .enable = true,
            .db_explicit = true,
            .msaa_num_samples_log2 = clear_samples_log2,
            .msaa_num_anchor_samples_log2 = clear_samples_log2,
         },
   };
   terakan_meta_config_draw_begin(command_writer, &meta_begin_options);

   /* Depth / stencil attachment. */

   struct terakan_bo const * depth_stencil_bo = NULL;
   struct terakan_depth_stencil_descriptor depth_stencil_descriptor =
      *terakan_app_config_draw_get_db_depth_stencil_buffer(&command_writer->app_config_draw,
                                                           &depth_stencil_bo);
   bool depth_attachment_bound, stencil_attachment_bound;
   terakan_depth_stencil_descriptor_is_bound(depth_stencil_bo, &depth_stencil_descriptor,
                                             &depth_attachment_bound, &stencil_attachment_bound);
   if (!depth_attachment_bound) {
      depth_stencil_clear_aspects &= ~VK_IMAGE_ASPECT_DEPTH_BIT;
   }
   if (!stencil_attachment_bound) {
      depth_stencil_clear_aspects &= ~VK_IMAGE_ASPECT_STENCIL_BIT;
   }

   if (depth_stencil_clear_aspects) {
      uint32_t db_depth_control = 0;
      if (depth_stencil_clear_aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
         if (G_028040_FORMAT(depth_stencil_descriptor.z_info) !=
                TERASCALE_R8XX_DEPTH_FORMAT_32_FLOAT ||
             !terakan_gfx_command_writer_device(command_writer)
                 ->vk.enabled_extensions.EXT_depth_range_unrestricted) {
            depth_stencil_clear_value.depth = fmaxf(depth_stencil_clear_value.depth, 0.0f);
            depth_stencil_clear_value.depth = MIN2(depth_stencil_clear_value.depth, 1.0f);
         }
         db_depth_control |= S_028800_Z_ENABLE(true) | S_028800_Z_WRITE_ENABLE(true) |
                             S_028800_ZFUNC(V_028800_STENCILFUNC_ALWAYS);
      }
      if (depth_stencil_clear_aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
         terakan_meta_config_draw_set_db_stencilrefmask(
            command_writer, false,
            S_028430_STENCILREF(depth_stencil_clear_value.stencil) |
               S_028430_STENCILWRITEMASK(0xFF));
         db_depth_control |= S_028800_STENCIL_ENABLE(1) |
                             S_028800_STENCILFUNC(V_028800_STENCILFUNC_ALWAYS) |
                             S_028800_STENCILFAIL(V_028800_STENCIL_REPLACE) |
                             S_028800_STENCILZPASS(V_028800_STENCIL_REPLACE) |
                             S_028800_STENCILZFAIL(V_028800_STENCIL_REPLACE);
      }
      terakan_meta_config_draw_set_db_depth_control(command_writer, db_depth_control);

      terakan_meta_config_draw_set_db_shader_control(command_writer,
                                                     TERAKAN_SHADER_DB_SHADER_CONTROL_IDENTITY);
      terakan_meta_config_draw_set_cb_color_control_for_mode(command_writer, V_028808_CB_DISABLE);

      terakan_meta_config_draw_set_sq_pgm_vs(command_writer, TERAKAN_META_SHADER_CLEAR_DEPTH_VS);
      terakan_meta_config_draw_set_sq_pgm_ps(command_writer, TERAKAN_META_SHADER_DUMMY_OPAQUE_PS);

      float const depth_clear_constants[TERAKAN_META_CLEAR_DEPTH_CONSTS_COUNT] = {
         [TERAKAN_META_CLEAR_DEPTH_CONST_CLEAR_VALUE] = depth_stencil_clear_value.depth,
      };
      terakan_meta_config_draw_set_kcache_push_constants(
         command_writer, sizeof(depth_clear_constants), depth_clear_constants, true, false);

      uint32_t const depth_stencil_slice_start =
         G_028008_SLICE_START(depth_stencil_descriptor.view);
      uint32_t const depth_stencil_slice_count =
         G_028008_SLICE_MAX(depth_stencil_descriptor.view) - depth_stencil_slice_start + 1u;

      for (uint32_t rect_index = 0; rect_index < rectCount; ++rect_index) {
         VkClearRect const * const rect = &pRects[rect_index];
         if (unlikely(rect->baseArrayLayer >= depth_stencil_slice_count)) {
            continue;
         }
         uint32_t const rect_slice_count =
            MIN2(rect->layerCount, depth_stencil_slice_count - rect->baseArrayLayer);
         if (unlikely(rect_slice_count == 0)) {
            continue;
         }
         uint32_t const rect_slice_start = depth_stencil_slice_start + rect->baseArrayLayer;
         depth_stencil_descriptor.view =
            (depth_stencil_descriptor.view & (C_028008_SLICE_START & C_028008_SLICE_MAX)) |
            S_028008_SLICE_START(rect_slice_start) |
            S_028008_SLICE_MAX(rect_slice_start + (rect_slice_count - 1));
         terakan_meta_config_draw_set_db_depth_stencil_buffer(command_writer, depth_stencil_bo,
                                                              &depth_stencil_descriptor);
         terakan_meta_draw_rect(command_writer,
                                terakan_vk_rect_to_screen_rect(rect->rect, render_area),
                                rect_slice_count);
      }
   }

   /* Color attachments. */

   if (color_clear_attachments) {
      terakan_meta_config_draw_set_db_depth_control(command_writer, 0);

      terakan_meta_config_draw_set_cb_color_control_for_mode(command_writer, V_028808_CB_NORMAL);

      terakan_meta_config_draw_set_sq_pgm_vs(command_writer,
                                             TERAKAN_META_SHADER_POSITION_AND_LAYER_FROM_INDEX_VS);
      terakan_meta_config_draw_set_sq_pgm_ps(command_writer, TERAKAN_META_SHADER_CLEAR_COLOR_PS);

      u_foreach_bit (color_attachment_index, color_clear_attachments) {
         struct terakan_app_config_draw_cb_color_rtv const * const render_pass_rtv =
            terakan_app_config_draw_get_cb_color_rtv(&command_writer->app_config_draw,
                                                     color_attachment_index);
         if (unlikely(
                !terakan_color_descriptor_is_bound(render_pass_rtv->bo, &render_pass_rtv->color))) {
            continue;
         }

         uint32_t const * const color_clear_value =
            color_clear_values[color_attachment_index].uint32;
         uint32_t const color_clear_constants[TERAKAN_META_CLEAR_COLOR_CONSTS_COUNT] = {
            [TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE_R] = color_clear_value[0],
            [TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE_G] = color_clear_value[1],
            [TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE_B] = color_clear_value[2],
            [TERAKAN_META_CLEAR_COLOR_CONST_CLEAR_VALUE_A] = color_clear_value[3],
         };
         terakan_meta_config_draw_set_kcache_push_constants(
            command_writer, sizeof(color_clear_constants), color_clear_constants, false, true);

         uint32_t const render_pass_rtv_slice_start =
            G_028C6C_SLICE_START(render_pass_rtv->color.view);
         uint32_t const render_pass_rtv_slice_count =
            G_028C6C_SLICE_MAX(render_pass_rtv->color.view) - render_pass_rtv_slice_start + 1;

         struct terakan_color_descriptor rect_rtv_color = render_pass_rtv->color;

         for (uint32_t rect_index = 0; rect_index < rectCount; ++rect_index) {
            VkClearRect const * const rect = &pRects[rect_index];
            if (unlikely(rect->baseArrayLayer >= render_pass_rtv_slice_count)) {
               continue;
            }
            uint32_t const rect_slice_count =
               MIN2(rect->layerCount, render_pass_rtv_slice_count - rect->baseArrayLayer);
            if (unlikely(rect_slice_count == 0)) {
               continue;
            }
            uint32_t const rect_slice_start = render_pass_rtv_slice_start + rect->baseArrayLayer;
            rect_rtv_color.view =
               (rect_rtv_color.view & (C_028C6C_SLICE_START & C_028C6C_SLICE_MAX)) |
               S_028C6C_SLICE_START(rect_slice_start) |
               S_028C6C_SLICE_MAX(rect_slice_start + (rect_slice_count - 1));
            terakan_meta_config_draw_set_cb_rtvs_and_db_shader_control(
               command_writer, 0xF, &render_pass_rtv->bo, &rect_rtv_color, &render_pass_rtv->meta,
               TERAKAN_SHADER_DB_SHADER_CONTROL_IDENTITY);
            terakan_meta_draw_rect(command_writer,
                                   terakan_vk_rect_to_screen_rect(rect->rect, render_area),
                                   rect_slice_count);
         }
      }
   }

   /* ClearAttachments is implemented with internal draws. Their attachment writes must finish
    * before the next application draw restores and uses application state; command order alone
    * doesn't drain the asynchronous graphics pipeline on TeraScale.
    */
   if (depth_stencil_clear_aspects) {
      command_writer->pending_barrier_actions |=
         TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_DATA |
         TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_META |
         TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;
   }
   if (color_clear_attachments) {
      command_writer->pending_barrier_actions |=
         TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
         TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_META |
         TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;
   }
}
