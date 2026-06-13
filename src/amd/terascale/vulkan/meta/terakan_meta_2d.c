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
   TERAKAN_SHADER_OP3_NW(false, 'X', BFE_UINT, EG, 0, 'X', V_SQ_ALU_SRC_0, 0, V_SQ_ALU_SRC_LITERAL,
                         'X', VEC_012),
   TERAKAN_SHADER_OP3_NW(true, 'Y', BFE_UINT, EG, 0, 'X', V_SQ_ALU_SRC_LITERAL, 'X',
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
   TERAKAN_SHADER_OP3_NW(false, 'X', BFE_UINT, EG, 0, 'X', V_SQ_ALU_SRC_0, 0, V_SQ_ALU_SRC_LITERAL,
                         'X', VEC_012),
   TERAKAN_SHADER_OP3_NW(true, 'Y', BFE_UINT, EG, 0, 'X', V_SQ_ALU_SRC_LITERAL, 'X',
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
