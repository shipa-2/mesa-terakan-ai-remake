/*
 * Copyright © 2023 Vitaliy Triang3l Kuzmin
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

#include "gallium/drivers/r600/eg_sq.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600_opcodes.h"

#include <stdint.h>

static uint32_t const terakan_meta_position_from_index_vs_r8xx[] = {
   /* Control flow. */

   /* 0: Vertex position calculation. */

   S_SQ_CF_WORD0_ADDR(3),
   S_SQ_CF_ALU_WORD1_COUNT(8 - 3) | S_SQ_CF_ALU_WORD1_BARRIER(1) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Export the position in R0.XY01. */

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_POS) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(60) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(V_03000C_SQ_SEL_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(V_03000C_SQ_SEL_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(V_03000C_SQ_SEL_0) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(V_03000C_SQ_SEL_1) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(1) | EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 2: Export an unused parameter as at least one parameter export is required and end the
    * program.
    */

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PARAM) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(V_03000C_SQ_SEL_0) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(V_03000C_SQ_SEL_0) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(V_03000C_SQ_SEL_0) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(V_03000C_SQ_SEL_0) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(1) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* ALU clause. */

   /* Extract bits 15:0 of the vertex index (to be used as position) to PV.X and bits 31:16 to PV.Y.
    *
    * 3:     PV.X = BFE_INT R0.X, 0, 16
    * 4: (V) PV.Y = BFE_INT R0.X, 16, 16
    * 5: Literal X = 16, Y = unused
    * Cycle 0: GPR.X = R0
    * Cycle 1: 2 constants
    * Cycle 2: 2 constants
    */

   S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_0),
   S_SQ_ALU_WORD1_OP3_SRC2_SEL(V_SQ_ALU_SRC_LITERAL) | S_SQ_ALU_WORD1_OP3_SRC2_CHAN(0) |
      S_SQ_ALU_WORD1_DST_GPR(0x7F - 4) | S_SQ_ALU_WORD1_DST_CHAN(0) |
      S_SQ_ALU_WORD1_OP3_ALU_INST(EG_V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_BFE_INT),

   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_LITERAL) | S_SQ_ALU_WORD0_SRC1_CHAN(0),
   S_SQ_ALU_WORD1_OP3_SRC2_SEL(V_SQ_ALU_SRC_LITERAL) | S_SQ_ALU_WORD1_OP3_SRC2_CHAN(0) |
      S_SQ_ALU_WORD1_DST_GPR(0x7F - 4) | S_SQ_ALU_WORD1_DST_CHAN(1) |
      S_SQ_ALU_WORD1_OP3_ALU_INST(EG_V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_BFE_INT),

   16,
   0,

   /* Convert the vertex position X from int to float.
    *
    * 6:     PV.Y = PV.Y, unused PV.Y
    * 7: (T) R0.X = INT_TO_FLT PV.X, unused PV.X
    * No GPR/constant loads
    */

   S_SQ_ALU_WORD0_SRC0_SEL(V_SQ_ALU_SRC_PV) | S_SQ_ALU_WORD0_SRC0_CHAN(1) |
      S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_PV) | S_SQ_ALU_WORD0_SRC1_CHAN(1),
   S_SQ_ALU_WORD1_DST_GPR(0x7F - 4) | S_SQ_ALU_WORD1_DST_CHAN(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV),

   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(V_SQ_ALU_SRC_PV) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_PV) | S_SQ_ALU_WORD0_SRC1_CHAN(0),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(0) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INT_TO_FLT),

   /* Convert the vertex position Y from int to float.
    *
    * 8: (T) R0.Y = INT_TO_FLT PV.Y, unused PV.Y
    * No GPR/constant loads
    */

   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(V_SQ_ALU_SRC_PV) | S_SQ_ALU_WORD0_SRC0_CHAN(1) |
      S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_PV) | S_SQ_ALU_WORD0_SRC1_CHAN(1),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(1) | S_SQ_ALU_WORD1_OP2_WRITE_MASK(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INT_TO_FLT),
};

static uint32_t const terakan_meta_position_from_index_vs_r9xx[] = {
   /* Control flow. */

   /* 0: Vertex position calculation. */

   S_SQ_CF_WORD0_ADDR(4),
   S_SQ_CF_ALU_WORD1_COUNT(8 - 4) | S_SQ_CF_ALU_WORD1_BARRIER(1) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Export the position in R0.XY01. */

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_POS) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(60) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(V_03000C_SQ_SEL_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(V_03000C_SQ_SEL_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(V_03000C_SQ_SEL_0) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(V_03000C_SQ_SEL_1) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(1) | EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 2: Export an unused parameter as at least one parameter export is required. */

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PARAM) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(V_03000C_SQ_SEL_0) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(V_03000C_SQ_SEL_0) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(V_03000C_SQ_SEL_0) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(V_03000C_SQ_SEL_0) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* 3: End the program. */

   0,
   S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(1) | CM_V_SQ_CF_WORD1_SQ_CF_INST_END,

   /* ALU clause. */

   /* Extract bits 15:0 of the vertex index (to be used as position) to PV.X and bits 31:16 to PV.Y.
    *
    * 4: PV.X = BFE_INT R0.X, 0, 16
    * 5: PV.Y = BFE_INT R0.X, 16, 16
    * 6: Literal X = 16, Y = unused
    * Cycle 0: GPR.X = R0
    * Cycle 1: 2 constants
    * Cycle 2: 2 constants
    */

   S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_0),
   S_SQ_ALU_WORD1_OP3_SRC2_SEL(V_SQ_ALU_SRC_LITERAL) | S_SQ_ALU_WORD1_OP3_SRC2_CHAN(0) |
      S_SQ_ALU_WORD1_DST_GPR(0x7F - 4) | S_SQ_ALU_WORD1_DST_CHAN(0) |
      S_SQ_ALU_WORD1_OP3_ALU_INST(EG_V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_BFE_INT),

   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(0) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_LITERAL) | S_SQ_ALU_WORD0_SRC1_CHAN(0),
   S_SQ_ALU_WORD1_OP3_SRC2_SEL(V_SQ_ALU_SRC_LITERAL) | S_SQ_ALU_WORD1_OP3_SRC2_CHAN(0) |
      S_SQ_ALU_WORD1_DST_GPR(0x7F - 4) | S_SQ_ALU_WORD1_DST_CHAN(1) |
      S_SQ_ALU_WORD1_OP3_ALU_INST(EG_V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_BFE_INT),

   16,
   0,

   /* Convert the vertex position from int to float.
    *
    * 7: R0.X = INT_TO_FLT PV.X, unused PV.X
    * 8: R0.Y = INT_TO_FLT PV.Y, unused PV.Y
    * No GPR/constant loads
    */

   S_SQ_ALU_WORD0_SRC0_SEL(V_SQ_ALU_SRC_PV) | S_SQ_ALU_WORD0_SRC0_CHAN(0) |
      S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_PV) | S_SQ_ALU_WORD0_SRC1_CHAN(0),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(0) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INT_TO_FLT),

   S_SQ_ALU_WORD0_LAST(1) | S_SQ_ALU_WORD0_SRC0_SEL(V_SQ_ALU_SRC_PV) | S_SQ_ALU_WORD0_SRC0_CHAN(1) |
      S_SQ_ALU_WORD0_SRC1_SEL(V_SQ_ALU_SRC_PV) | S_SQ_ALU_WORD0_SRC1_CHAN(1),
   S_SQ_ALU_WORD1_DST_GPR(0) | S_SQ_ALU_WORD1_DST_CHAN(1) |
      S_SQ_ALU_WORD1_OP2_ALU_INST(EG_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_INT_TO_FLT),
};

static struct terakan_meta_shader const terakan_meta_position_from_index_vs = {
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

struct terakan_meta_shader const * const terakan_meta_shaders[TERAKAN_META_SHADER_COUNT] = {
   [TERAKAN_META_SHADER_POSITION_FROM_INDEX_VS] = &terakan_meta_position_from_index_vs,
};
