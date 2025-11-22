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
#include "terakan_draw.h"

#include "amd/terascale/common/terascale_wddm.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/macros.h"
#include "util/u_math.h"

#include <assert.h>
#include <stddef.h>

static uint32_t const terakan_meta_position_from_index_vs_r8xx[] = {
   /* 0: Export the instance ID as the first parameter. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PARAM) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 1: Vertex position calculation. */
   S_SQ_CF_WORD0_ADDR(3),
   S_SQ_CF_ALU_WORD1_COUNT(5) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 2: Export the position in R0.XY01 and end the program. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_POS) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(60) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_0) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_1) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) | S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 3: ALU clause. */

   /* +0-2: Extract bits 15:0 of the vertex index (to be used as position) to PV.X and bits 31:16 to
    * PV.Y.
    * Cycle 0: X = R0.
    */
   TERAKAN_SHADER_OP3_NW(false, 'X', BFE_INT, EG, 0, 'X', V_SQ_ALU_SRC_0, 0, V_SQ_ALU_SRC_LITERAL,
                         'X', VEC_012),
   TERAKAN_SHADER_OP3_NW(true, 'Y', BFE_INT, EG, 0, 'X', V_SQ_ALU_SRC_LITERAL, 'X',
                         V_SQ_ALU_SRC_LITERAL, 'X', VEC_012),
   16,
   0,

   /* +3-4: Convert the vertex position X from int to float to R0.X, and pass PV.Y further
    * (INT_TO_FLT can be executed only on the transcendental unit).
    */
   TERAKAN_SHADER_OP1_NW(false, 'Y', MOV, EG, V_SQ_ALU_SRC_PV, 'Y', VEC_012),
   TERAKAN_SHADER_OP1(true, 0, 'X', INT_TO_FLT, EG, V_SQ_ALU_SRC_PV, 'X', SCL_210),

   /* +5: Convert the vertex position Y from int to float to R0.Y. */
   TERAKAN_SHADER_OP1(true, 0, 'Y', INT_TO_FLT, EG, V_SQ_ALU_SRC_PV, 'Y', SCL_210),
};

static uint32_t const terakan_meta_position_and_layer_from_index_vs_r8xx[] = {
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
   S_SQ_CF_WORD0_ADDR(4),
   S_SQ_CF_ALU_WORD1_COUNT(5) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 3: Export the position in R0.XY01 and end the program. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_POS) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(60) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_0) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_1) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) | S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 4: ALU clause. */

   /* +0-2: Extract bits 15:0 of the vertex index (to be used as position) to PV.X and bits 31:16 to
    * PV.Y.
    * Cycle 0: X = R0.
    */
   TERAKAN_SHADER_OP3_NW(false, 'X', BFE_INT, EG, 0, 'X', V_SQ_ALU_SRC_0, 0, V_SQ_ALU_SRC_LITERAL,
                         'X', VEC_012),
   TERAKAN_SHADER_OP3_NW(true, 'Y', BFE_INT, EG, 0, 'X', V_SQ_ALU_SRC_LITERAL, 'X',
                         V_SQ_ALU_SRC_LITERAL, 'X', VEC_012),
   16,
   0,

   /* +3-4: Convert the vertex position X from int to float to R0.X, and pass PV.Y further
    * (INT_TO_FLT can be executed only on the transcendental unit).
    */
   TERAKAN_SHADER_OP1_NW(false, 'Y', MOV, EG, V_SQ_ALU_SRC_PV, 'Y', VEC_012),
   TERAKAN_SHADER_OP1(true, 0, 'X', INT_TO_FLT, EG, V_SQ_ALU_SRC_PV, 'X', SCL_210),

   /* +5: Convert the vertex position Y from int to float to R0.Y. */
   TERAKAN_SHADER_OP1(true, 0, 'Y', INT_TO_FLT, EG, V_SQ_ALU_SRC_PV, 'Y', SCL_210),
};

static uint32_t const terakan_meta_position_from_index_vs_r9xx[] = {
   /* 0: Export the instance ID as the first parameter. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PARAM) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 1: Vertex position calculation. */
   S_SQ_CF_WORD0_ADDR(4),
   S_SQ_CF_ALU_WORD1_COUNT(4) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 2: Export the position in R0.XY01. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_POS) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(60) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_0) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_1) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 3: End the program. */
   TERAKAN_SHADER_CF_END_R9XX,

   /* 4: ALU clause. */

   /* +0-2: Extract bits 15:0 of the vertex index (to be used as position) to PV.X and bits 31:16 to
    * PV.Y.
    * Cycle 0: X = R0.
    */
   TERAKAN_SHADER_OP3_NW(false, 'X', BFE_INT, EG, 0, 'X', V_SQ_ALU_SRC_0, 0, V_SQ_ALU_SRC_LITERAL,
                         'X', VEC_012),
   TERAKAN_SHADER_OP3_NW(true, 'Y', BFE_INT, EG, 0, 'X', V_SQ_ALU_SRC_LITERAL, 'X',
                         V_SQ_ALU_SRC_LITERAL, 'X', VEC_012),
   16,
   0,

   /* +3-4: Convert the vertex position from int to float to R0.XY. */
   TERAKAN_SHADER_OP1(false, 0, 'X', INT_TO_FLT, EG, V_SQ_ALU_SRC_PV, 'X', VEC_012),
   TERAKAN_SHADER_OP1(true, 0, 'Y', INT_TO_FLT, EG, V_SQ_ALU_SRC_PV, 'Y', VEC_012),
};

static uint32_t const terakan_meta_position_and_layer_from_index_vs_r9xx[] = {
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
   S_SQ_CF_WORD0_ADDR(5),
   S_SQ_CF_ALU_WORD1_COUNT(4) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 3: Export the position in R0.XY01. */
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_POS) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(60) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_0) |
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
   TERAKAN_SHADER_OP3_NW(false, 'X', BFE_INT, EG, 0, 'X', V_SQ_ALU_SRC_0, 0, V_SQ_ALU_SRC_LITERAL,
                         'X', VEC_012),
   TERAKAN_SHADER_OP3_NW(true, 'Y', BFE_INT, EG, 0, 'X', V_SQ_ALU_SRC_LITERAL, 'X',
                         V_SQ_ALU_SRC_LITERAL, 'X', VEC_012),
   16,
   0,

   /* +3-4: Convert the vertex position from int to float to R0.XY. */
   TERAKAN_SHADER_OP1(false, 0, 'X', INT_TO_FLT, EG, V_SQ_ALU_SRC_PV, 'X', VEC_012),
   TERAKAN_SHADER_OP1(true, 0, 'Y', INT_TO_FLT, EG, V_SQ_ALU_SRC_PV, 'Y', VEC_012),
};

struct terakan_meta_shader const terakan_meta_position_from_index_vs = {
   .r8xx =
      {
         .program = terakan_meta_position_from_index_vs_r8xx,
         .program_size_bytes = sizeof(terakan_meta_position_from_index_vs_r8xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     S_028860_NUM_GPRS(1) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                     TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                  },
            },
      },
   .r9xx =
      {
         .program = terakan_meta_position_from_index_vs_r9xx,
         .program_size_bytes = sizeof(terakan_meta_position_from_index_vs_r9xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     S_028860_NUM_GPRS(1) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                     TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                  },
            },
      },
};

struct terakan_meta_shader const terakan_meta_position_and_layer_from_index_vs = {
   .r8xx =
      {
         .program = terakan_meta_position_and_layer_from_index_vs_r8xx,
         .program_size_bytes = sizeof(terakan_meta_position_and_layer_from_index_vs_r8xx),
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
         .program = terakan_meta_position_and_layer_from_index_vs_r9xx,
         .program_size_bytes = sizeof(terakan_meta_position_and_layer_from_index_vs_r9xx),
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
};

void
terakan_meta_begin_2d(struct terakan_gfx_command_writer * const command_writer,
                      uint32_t const pa_cl_vte_cntl)
{
   terakan_meta_modify_state_draw_dword(command_writer, TERAKAN_STATE_DRAW_INDEX_PA_CL_CLIP_CNTL,
                                        TERAKAN_HW_STATE_DRAW_INDEX_PA_CL_CLIP_CNTL,
                                        &command_writer->hw_state_draw.pa_cl_clip_cntl,
                                        S_028810_CLIP_DISABLE(1));

   terakan_meta_modify_state_draw_dword(command_writer, TERAKAN_STATE_DRAW_INDEX_PA_SU_SC_MODE_CNTL,
                                        TERAKAN_HW_STATE_DRAW_INDEX_PA_SU_SC_MODE_CNTL,
                                        &command_writer->hw_state_draw.pa_su_sc_mode_cntl, 0);

   terakan_meta_set_pa_cl_vte_cntl(command_writer, pa_cl_vte_cntl);

   uint32_t const num_samples_log2 = 0;
   /* TODO(Triang3l): Make what depends on the sample count pending in state_draw. */

   terakan_meta_modify_state_draw_dword(command_writer, TERAKAN_STATE_DRAW_INDEX_PA_SC_MODE_CNTL_0,
                                        TERAKAN_HW_STATE_DRAW_INDEX_PA_SC_MODE_CNTL_0,
                                        &command_writer->hw_state_draw.pa_sc_mode_cntl_0,
                                        S_028A48_MSAA_ENABLE(num_samples_log2 > 0));

   bool const pa_sc_aa_samples_modified =
      command_writer->hw_state_draw.pa_sc_aa_samples.num_samples_log2 != num_samples_log2;
   command_writer->hw_state_draw.pa_sc_aa_samples.num_samples_log2 = num_samples_log2;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_PA_SC_AA_SAMPLES,
                                 pa_sc_aa_samples_modified);

   terakan_state_draw_set_pending(&command_writer->state_draw,
                                  TERAKAN_STATE_DRAW_INDEX_PA_SC_AA_MASK);
   uint16_t const pa_sc_aa_mask = ~(uint16_t)0;
   bool const pa_sc_aa_mask_modified = command_writer->hw_state_draw.pa_sc_aa_mask != pa_sc_aa_mask;
   command_writer->hw_state_draw.pa_sc_aa_mask = pa_sc_aa_mask;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_PA_SC_AA_MASK, pa_sc_aa_mask_modified);
}

void
terakan_meta_begin_rects(struct terakan_gfx_command_writer * const command_writer)
{
   terakan_meta_modify_state_draw_dword(command_writer, TERAKAN_STATE_DRAW_INDEX_VGT_PRIMITIVE_TYPE,
                                        TERAKAN_HW_STATE_DRAW_INDEX_VGT_PRIMITIVE_TYPE,
                                        &command_writer->hw_state_draw.vgt_primitive_type,
                                        S_008958_PRIM_TYPE(V_008958_DI_PT_RECTLIST));
}

void
terakan_meta_begin_index_immediate_32(struct terakan_gfx_command_writer * const command_writer)
{
   terakan_meta_modify_state_draw_dword(command_writer, TERAKAN_STATE_DRAW_INDEX_VGT_INDEX_TYPE,
                                        TERAKAN_HW_STATE_DRAW_INDEX_VGT_INDEX_TYPE,
                                        &command_writer->hw_state_draw.vgt_index_type,
                                        VGT_INDEX_32);

   terakan_meta_modify_state_draw_dword(command_writer, TERAKAN_STATE_DRAW_INDEX_VGT_INDEX_OFFSET,
                                        TERAKAN_HW_STATE_DRAW_INDEX_VGT_INDEX_OFFSET,
                                        &command_writer->hw_state_draw.vgt_index_offset, 0);
}

void
terakan_meta_emit_rect_3_vertices_draw(struct terakan_gfx_command_writer * const command_writer,
                                       VkRect2D const * const rect, uint32_t const instance_count)
{
   terakan_hw_state_draw_set_vgt_num_instances(&command_writer->hw_state_draw, instance_count);

   terakan_before_hw_draw(command_writer);

   uint32_t * packet;

   uint32_t const vertices[] = {
      (uint16_t)rect->offset.x | ((uint32_t)rect->offset.y << 16),
      (uint16_t)rect->offset.x | ((uint32_t)(rect->offset.y + rect->extent.height) << 16),
      (uint16_t)(rect->offset.x + rect->extent.width) | ((uint32_t)rect->offset.y << 16)};

   if (instance_count > 1) {
      /* PKT3_DRAW_INDEX_IMMD with multiple instances causes a hang in this usage scenario (tested
       * on Barts with the firmware used by DRM Radeon 2.50.0).
       */
      struct terakan_bo const * index_buffer_bo;
      uint64_t index_buffer_va;
      uint32_t * const index_buffer_mapping =
         terakan_push_buffer_allocate(command_writer->base.command_buffer, sizeof(vertices),
                                      sizeof(uint32_t), &index_buffer_bo, &index_buffer_va);
      if (unlikely(index_buffer_mapping == NULL)) {
         return;
      }
      /* terakan_meta_begin_index_immediate_32 sets VGT_INDEX_TYPE to non-endian-swapped. */
      util_memcpy_cpu_to_le32(index_buffer_mapping, vertices, sizeof(vertices));

      packet = terakan_gfx_command_writer_emit_with_bo(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW, 5, 1, 0, 1);
      if (unlikely(packet == NULL)) {
         return;
      }
      *packet++ = PKT3(PKT3_DRAW_INDEX, 5 - 2, 0);
      uint32_t const * const packet_index_base = packet;
      *packet++ = (uint32_t)index_buffer_va;
      *packet++ = (index_buffer_va >> 32) & 0xFF;
      *packet++ = ARRAY_SIZE(vertices);
      *packet++ = S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_DMA);
      /* The exact WDDM patch location for PKT3_DRAW_INDEX isn't known, so reusing the one for
       * PKT3_INDEX_BASE.
       */
      terakan_gfx_command_writer_add_relocation_for_40_bits(
         command_writer, &packet, packet_index_base, packet_index_base + 1,
         TERASCALE_WDDM_PATCH_IDS_INDEX_BASE_LO, TERASCALE_WDDM_PATCH_IDS_INDEX_BASE_HI,
         terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                   index_buffer_bo, true, false,
                                                   TERAKAN_BO_PRIORITY_INDEX_BUFFER));
      terakan_gfx_command_writer_emit_done(command_writer, packet);

      return;
   }

   packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW, 3 + ARRAY_SIZE(vertices));
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_DRAW_INDEX_IMMD, 1 + ARRAY_SIZE(vertices), 0);
   *packet++ = ARRAY_SIZE(vertices);
   *packet++ = S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_IMMEDIATE);
   memcpy(packet, vertices, sizeof(vertices));
   packet += ARRAY_SIZE(vertices);
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}
