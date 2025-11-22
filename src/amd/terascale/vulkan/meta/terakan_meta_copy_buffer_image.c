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

#include "terakan_bo.h"
#include "terakan_buffer.h"
#include "terakan_command_buffer.h"
#include "terakan_entrypoints.h"
#include "terakan_image.h"
#include "terakan_physical_device.h"

#include "util/macros.h"
#include "vk_format.h"

#include <assert.h>
#include <string.h>

/* Buffer to image copying by drawing to a color attachment.
 * Not using an image UAV because "Tex2D UAV on cypress will fail/hang if tile mode is linear"
 * according to the R800 AddrLib.
 *
 * Image to buffer copying by drawing and writing to a UAV.
 */

enum {
   /* All values are in blocks. */

   /* The offsets are signed. */
   TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET,
   TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_Y,

   /* The pitches are unsigned. */
   TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Y_PITCH,
   TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Z_PITCH,

   TERAKAN_META_COPY_BUFFER_IMAGE_CONSTS_COUNT,
};

/* clang-format off */

#define TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_R8XX                                         \
   /* +0-2: Convert from image XY to buffer XY, and apply the layer pitch to the layer index.      \
    *                                                                                              \
    *     PV.X = SUB_INT R0.X, CB[push].image_offset_x_minus_buffer_offset                         \
    *     PV.Y = SUB_INT R0.Y, CB[push].image_offset_y                                             \
    * (T) PS (Z) = MULLO_UINT CB[push].buffer_z_pitch, R0.Z                                        \
    * Cycle 0: X = R0, Y = R0, T constant.                                                         \
    * Cycle 1: Z = R0.                                                                             \
    */                                                                                             \
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(                                                                \
      0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET) |                \
      TERAKAN_SHADER_OP2_NW(false, 'X', SUB_INT, EG, 0, 'X', 0, 0, VEC_012),                       \
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_Y) |       \
      TERAKAN_SHADER_OP2_NW(false, 'Y', SUB_INT, EG, 0, 'Y', 0, 0, VEC_012),                       \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Z_PITCH) |       \
      TERAKAN_SHADER_OP2_NW(true, 'Z', MULLO_UINT, EG, 0, 0, 0, 'Z', SCL_210),                     \
                                                                                                   \
   /* +3-4: Apply the layer offset to the address, and apply the row pitch to the row index.       \
    *                                                                                              \
    *     PV.X = ADD_INT PV.X, PS                                                                  \
    * (T) PS (Y) = MULLO_UINT CB[push].buffer_y_pitch, PV.Y                                        \
    * Cycle 0: T constant.                                                                         \
    */                                                                                             \
   TERAKAN_SHADER_OP2_NW(false, 'X', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'X', V_SQ_ALU_SRC_PS, 0,        \
                         VEC_012),                                                                 \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Y_PITCH) |       \
      TERAKAN_SHADER_OP2_NW(true, 'Y', MULLO_UINT, EG, 0, 0, V_SQ_ALU_SRC_PV, 'Y', SCL_210),       \
                                                                                                   \
   /* +5: Apply the row offset to the address written to R0.X.                                     \
    *                                                                                              \
    * (v) R0.X = ADD_INT PV.X, PS                                                                  \
    */                                                                                             \
   TERAKAN_SHADER_OP2(true, 0, 'X', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'X', V_SQ_ALU_SRC_PS, 0, VEC_012)

/* clang-format on */

#define TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_COUNT_R8XX 5

static uint32_t const terakan_meta_copy_buffer_to_image_ps_r8xx[] = {
   /* 0: Address calculation. */
   S_SQ_CF_WORD0_ADDR(6) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(TERAKAN_KCACHE_DWORD_LINE(
      TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET)) |
      S_SQ_CF_ALU_WORD1_COUNT(TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_COUNT_R8XX) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch from the source buffer. */
   S_SQ_CF_WORD0_ADDR(4),
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

   /* 3 (alignment padding), 4-5: Vertex-fetch from the source buffer to R0. */
   0,
   0,
   S_SQ_VTX_WORD0_FETCH_TYPE(SQ_VTX_FETCH_NO_INDEX_OFFSET) |
      S_SQ_VTX_WORD0_BUFFER_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_VTX_WORD0_SRC_GPR(0) | S_SQ_VTX_WORD0_SRC_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_VTX_WORD0_MEGA_FETCH_COUNT(16 - 1),
   S_SQ_VTX_WORD1_GPR_DST_GPR(0) | S_SQ_VTX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_VTX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_VTX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_VTX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W) | S_SQ_VTX_WORD1_USE_CONST_FIELDS(true),
   S_SQ_VTX_WORD2_MEGA_FETCH(true),
   0,

   /* 6: Address calculation ALU clause. */
   TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_R8XX,
};

static uint32_t const terakan_meta_copy_image_to_buffer_ps_r8xx[] = {
   /* 0: Loading the array layer index (may exceed the maximum number of attachment layers for
    * images without attachment usage enabled).
    */
   S_SQ_CF_WORD0_ADDR(5),
   S_SQ_CF_ALU_WORD1_COUNT(0) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch from the source texture. */
   S_SQ_CF_WORD0_ADDR(6),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Address calculation. */
   S_SQ_CF_WORD0_ADDR(8) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(TERAKAN_KCACHE_DWORD_LINE(
      TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET)) |
      S_SQ_CF_ALU_WORD1_COUNT(TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_COUNT_R8XX) |
      S_SQ_CF_ALU_WORD1_BARRIER(true) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 3: Write to the UAV. */
   TERAKAN_SHADER_CF_UAV(false, STORE_TYPED, 0, 0, 1, 0xF, true),

   /* 4: Perform a dummy export and end the program. */
   TERAKAN_SHADER_CF_PS_DUMMY_EXPORT_DONE_AND_END_R8XX,

   /* 5: ALU clause. */

   /* +0: Load the array layer index.
    *
    * (V) INTERP_LOAD_P0 R0.Z, Param0.X
    */
   TERAKAN_SHADER_OP1(true, 0, 'Z', INTERP_LOAD_P0, EG, V_SQ_ALU_SRC_PARAM_BASE, 'X', VEC_012),

   /* 6-7: Fetch from the source texture to R1. */
   S_SQ_TEX_WORD0_TEX_INST(3) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(1) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W),
   S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_0),
   0,

   /* 8: Address calculation ALU clause. */
   TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_R8XX,
};

/* clang-format off */

#define TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_R9XX                                         \
   /* +0-1: Convert from image XY to buffer XY, and apply the layer pitch to the layer index.      \
    *                                                                                              \
    * R0.X = SUB_INT R0.X, CB[push].image_offset_x_minus_buffer_offset                             \
    * PV.Y = SUB_INT R0.Y, CB[push].image_offset_y                                                 \
    * Cycle 0: X = R0, Y = R0.                                                                     \
    */                                                                                             \
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(                                                                \
      0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET) |                \
      TERAKAN_SHADER_OP2(false, 0, 'X', SUB_INT, EG, 0, 'X', 0, 0, VEC_012),                       \
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_Y) |       \
      TERAKAN_SHADER_OP2_NW(true, 'Y', SUB_INT, EG, 0, 'Y', 0, 0, VEC_012),                        \
                                                                                                   \
   /* +2-5: Apply the row pitch to the row index.                                                  \
    *                                                                                              \
    * MULLO_UINT uses 4 slots.                                                                     \
    * PV.X = MULLO_UINT CB[push].buffer_y_pitch, PV.Y                                              \
    * R0.Y = MULLO_UINT CB[push].buffer_y_pitch, PV.Y                                              \
    * PV.Z = MULLO_UINT CB[push].buffer_y_pitch, PV.Y                                              \
    * PV.W = MULLO_UINT CB[push].buffer_y_pitch, PV.Y                                              \
    */                                                                                             \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Y_PITCH) |       \
      TERAKAN_SHADER_OP2_NW(false, 'X', MULLO_UINT, EG, 0, 0, V_SQ_ALU_SRC_PV, 'Y', VEC_012),      \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Y_PITCH) |       \
      TERAKAN_SHADER_OP2(false, 0, 'Y', MULLO_UINT, EG, 0, 0, V_SQ_ALU_SRC_PV, 'Y', VEC_012),      \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Y_PITCH) |       \
      TERAKAN_SHADER_OP2_NW(false, 'Z', MULLO_UINT, EG, 0, 0, V_SQ_ALU_SRC_PV, 'Y', VEC_012),      \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Y_PITCH) |       \
      TERAKAN_SHADER_OP2_NW(true, 'W', MULLO_UINT, EG, 0, 0, V_SQ_ALU_SRC_PV, 'Y', VEC_012),       \
                                                                                                   \
   /* +6-9: Apply the layer pitch to the layer index.                                              \
    *                                                                                              \
    * MULLO_UINT uses 4 slots.                                                                     \
    * PV.X = MULLO_UINT CB[push].buffer_z_pitch, R0.Z                                              \
    * PV.Y = MULLO_UINT CB[push].buffer_z_pitch, R0.Z                                              \
    * PV.Z = MULLO_UINT CB[push].buffer_z_pitch, R0.Z                                              \
    * PV.W = MULLO_UINT CB[push].buffer_z_pitch, R0.Z                                              \
    */                                                                                             \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Z_PITCH) |       \
      TERAKAN_SHADER_OP2_NW(false, 'X', MULLO_UINT, EG, 0, 0, 0, 'Z', VEC_012),                    \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Z_PITCH) |       \
      TERAKAN_SHADER_OP2_NW(false, 'Y', MULLO_UINT, EG, 0, 0, 0, 'Z', VEC_012),                    \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Z_PITCH) |       \
      TERAKAN_SHADER_OP2_NW(false, 'Z', MULLO_UINT, EG, 0, 0, 0, 'Z', VEC_012),                    \
   TERAKAN_KCACHE_DWORD_WORD0_SRC0(0, TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Z_PITCH) |       \
      TERAKAN_SHADER_OP2_NW(true, 'W', MULLO_UINT, EG, 0, 0, 0, 'Z', VEC_012),                     \
                                                                                                   \
   /* +10: Apply the layer offset to the address.                                                  \
    *                                                                                              \
    * PV.X = ADD_INT R0.X, PV.Z                                                                    \
    * Cycle 0: X = R0.                                                                             \
    */                                                                                             \
   TERAKAN_SHADER_OP2_NW(true, 'X', ADD_INT, EG, 0, 'X', V_SQ_ALU_SRC_PV, 'Z', VEC_012),           \
                                                                                                   \
   /* +11: Apply the row offset to the address written to R0.X.                                    \
    *                                                                                              \
    * R0.X = ADD_INT PV.X, R0.Y                                                                    \
    * Cycle 1: Y = R0.                                                                             \
    */                                                                                             \
   TERAKAN_SHADER_OP2(true, 0, 'X', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'X', 0, 'Y', VEC_012)

/* clang-format off */

#define TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_COUNT_R9XX 11

static uint32_t const terakan_meta_copy_buffer_to_image_ps_r9xx[] = {
   /* 0: Address calculation. */
   S_SQ_CF_WORD0_ADDR(6) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(TERAKAN_KCACHE_DWORD_LINE(
      TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET)) |
      S_SQ_CF_ALU_WORD1_COUNT(TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_COUNT_R9XX) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch from the source buffer. */
   S_SQ_CF_WORD0_ADDR(4),
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

   /* 4-5: Vertex-fetch from the source buffer to R0. */
   S_SQ_VTX_WORD0_FETCH_TYPE(SQ_VTX_FETCH_NO_INDEX_OFFSET) |
      S_SQ_VTX_WORD0_BUFFER_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_VTX_WORD0_SRC_GPR(0) | S_SQ_VTX_WORD0_SRC_SEL_X(TERASCALE_SWIZZLE_X),
   S_SQ_VTX_WORD1_GPR_DST_GPR(0) | S_SQ_VTX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_VTX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_VTX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_VTX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W) | S_SQ_VTX_WORD1_USE_CONST_FIELDS(true),
   0,
   0,

   /* 6: Address calculation ALU clause. */
   TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_R9XX,
};

static uint32_t const terakan_meta_copy_image_to_buffer_ps_r9xx[] = {
   /* 0: Loading the array layer index (may exceed the maximum number of attachment layers for
    * images without attachment usage enabled).
    */
   S_SQ_CF_WORD0_ADDR(6),
   S_SQ_CF_ALU_WORD1_COUNT(0) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch from the source texture. */
   S_SQ_CF_WORD0_ADDR(8),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Address calculation. */
   S_SQ_CF_WORD0_ADDR(10) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(TERAKAN_KCACHE_DWORD_LINE(
      TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET)) |
      S_SQ_CF_ALU_WORD1_COUNT(TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_COUNT_R9XX) |
      S_SQ_CF_ALU_WORD1_BARRIER(true) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 3: Write to the UAV. */
   TERAKAN_SHADER_CF_UAV(false, STORE_TYPED, 0, 0, 1, 0xF, true),

   /* 4: Perform a dummy export. */
   TERAKAN_SHADER_CF_PS_DUMMY_EXPORT_DONE_R9XX,

   /* 5: End the program. */
   TERAKAN_SHADER_CF_END_R9XX,

   /* 6: ALU clause. */

   /* +0: Load the array layer index.
    *
    * INTERP_LOAD_P0 R0.Z, Param0.X, unused 0
    */
   TERAKAN_SHADER_OP1(true, 0, 'Z', INTERP_LOAD_P0, EG, V_SQ_ALU_SRC_PARAM_BASE, 'X', VEC_012),

   /* 7 (alignment padding), 8-9: Fetch from the source texture to R1. */
   0,
   0,
   S_SQ_TEX_WORD0_TEX_INST(3) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(1) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W),
   S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_0),
   0,

   /* 10: Address calculation ALU clause. */
   TERAKAN_META_COPY_IMAGE_TO_BUFFER_ADDRESS_ALU_R9XX,
};

struct terakan_meta_shader const terakan_meta_copy_buffer_to_image_ps = {
   .r8xx =
      {
         .program = terakan_meta_copy_buffer_to_image_ps_r8xx,
         .program_size_bytes = sizeof(terakan_meta_copy_buffer_to_image_ps_r8xx),
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
         .program = terakan_meta_copy_buffer_to_image_ps_r9xx,
         .program_size_bytes = sizeof(terakan_meta_copy_buffer_to_image_ps_r9xx),
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

struct terakan_meta_shader const terakan_meta_copy_image_to_buffer_ps = {
   .r8xx =
      {
         .program = terakan_meta_copy_image_to_buffer_ps_r8xx,
         .program_size_bytes = sizeof(terakan_meta_copy_image_to_buffer_ps_r8xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     S_028844_NUM_GPRS(2) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                     TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                  },
               .stage =
                  {
                     .ps =
                        {
                           .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                           .spi_ps_input_cntl =
                              {
                                 [0] = S_028644_FLAT_SHADE(1),
                              },
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
         .program = terakan_meta_copy_image_to_buffer_ps_r9xx,
         .program_size_bytes = sizeof(terakan_meta_copy_image_to_buffer_ps_r9xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     S_028844_NUM_GPRS(2) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                     TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                  },
               .stage =
                  {
                     .ps =
                        {
                           .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                           .spi_ps_input_cntl =
                              {
                                 [0] = S_028644_FLAT_SHADE(1),
                              },
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
               .db_shader_control = TERAKAN_META_DB_SHADER_CONTROL_PS_MEMORY_EXPORT,
            },
      },
};

static void
terakan_meta_copy_buffer_image_pitches_and_rect(struct terakan_image const * const image,
                                                VkBufferImageCopy2 const * const region,
                                                uint32_t * const buffer_y_pitch_out,
                                                uint32_t * const buffer_z_pitch_out,
                                                VkRect2D * const rect_out)
{
   unsigned const block_width = vk_format_get_blockwidth(image->vk.format);
   unsigned const block_height = vk_format_get_blockheight(image->vk.format);

   /* Buffer row length and image height must be block-aligned. However, safer to assume the
    * intention of rounding up in case of invalid (like simply copying imageExtent) usage.
    */
   uint32_t const buffer_y_pitch = DIV_ROUND_UP(
      region->bufferRowLength != 0 ? region->bufferRowLength : region->imageExtent.width,
      block_width);
   *buffer_y_pitch_out = buffer_y_pitch;
   *buffer_z_pitch_out =
      buffer_y_pitch * DIV_ROUND_UP(region->bufferImageHeight != 0 ? region->bufferImageHeight
                                                                   : region->imageExtent.height,
                                    block_height);

   /* The offset must be block-aligned, but offset + extent is limited to the extent of the
    * subresource, which is not block-aligned.
    */
   rect_out->offset.x = region->imageOffset.x / block_width;
   rect_out->offset.y = region->imageOffset.y / block_height;
   rect_out->extent.width = DIV_ROUND_UP(region->imageExtent.width, block_width);
   rect_out->extent.height = DIV_ROUND_UP(region->imageExtent.height, block_height);
}

/* The level count must be 1. */
static void
terakan_meta_copy_buffer_image_level_index_and_layer_range(
   struct terakan_image_descriptor_create_info * const image_descriptor_create_info,
   struct terakan_image const * const image, VkBufferImageCopy2 const * const region)
{
   image_descriptor_create_info->base_mip_level = region->imageSubresource.mipLevel;
   image_descriptor_create_info->level_count = 1;
   if (image->vk.image_type == VK_IMAGE_TYPE_3D) {
      image_descriptor_create_info->base_array_layer = (uint32_t)region->imageOffset.z;
      image_descriptor_create_info->layer_count = region->imageExtent.depth;
   } else {
      image_descriptor_create_info->base_array_layer = region->imageSubresource.baseArrayLayer;
      image_descriptor_create_info->layer_count =
         vk_image_subresource_layer_count(&image->vk, &region->imageSubresource);
   }
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdCopyBufferToImage2(VkCommandBuffer const commandBuffer,
                              VkCopyBufferToImageInfo2 const * const pCopyBufferToImageInfo)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   struct terakan_image const * const image =
      terakan_image_from_handle(pCopyBufferToImageInfo->dstImage);

   if (terakan_format_is_expand_3x(image->surface.aspects[0].bytes_per_block)) {
      terakan_meta_copy_expand_3x_buffer_to_image(command_writer, pCopyBufferToImageInfo);
      return;
   }

   struct terakan_image_descriptor_create_info image_descriptor_create_info = {
      .image = image,
      .view_type = terakan_meta_transfer_image_view_type(image->vk.image_type),
      .force_little_endian = true,
      .level_count = 1,
   };

   struct terakan_buffer const * const buffer =
      terakan_buffer_from_handle(pCopyBufferToImageInfo->srcBuffer);

   uint32_t buffer_resource[8] = {
      [3] = S_03000C_DST_SEL_X(TERASCALE_SWIZZLE_X) | S_03000C_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
            S_03000C_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_03000C_DST_SEL_W(TERASCALE_SWIZZLE_W),
      [7] = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER),
      [TERAKAN_RESOURCE_BUFFER_PRIORITY_WORD] = TERAKAN_BO_PRIORITY_SHADER_READ_BUFFER,
   };

   command_writer->post_color_image_copy_write_barrier_actions |=
      TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
      TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;

   terakan_meta_begin_2d_immediate_rects(command_writer, TERAKAN_META_PA_CL_VTE_CNTL_2D,
                                         TERAKAN_META_DB_RENDER_OVERRIDE_DEFAULT, true);

   terakan_meta_set_vs(command_writer, TERAKAN_META_SHADER_POSITION_AND_LAYER_FROM_INDEX_VS);
   terakan_meta_set_ps(command_writer, TERAKAN_META_SHADER_COPY_BUFFER_TO_IMAGE_PS, false);

   terakan_meta_begin_cb(command_writer, 0xF, 0b1);

   command_writer->push_constants_state.up_to_date_push_constants_bound_to_stages &=
      ~VK_SHADER_STAGE_FRAGMENT_BIT;

   uint32_t constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONSTS_COUNT] = {};
   struct terakan_bo const * constants_bo = NULL;
   uint32_t constants_va_lines;

   for (uint32_t region_index = 0; region_index < pCopyBufferToImageInfo->regionCount;
        ++region_index) {
      VkBufferImageCopy2 const * const region = &pCopyBufferToImageInfo->pRegions[region_index];

      uint32_t buffer_y_pitch, buffer_z_pitch;
      VkRect2D rect;
      terakan_meta_copy_buffer_image_pitches_and_rect(image, region, &buffer_y_pitch,
                                                      &buffer_z_pitch, &rect);

      if (constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET] !=
             (uint32_t)rect.offset.x ||
          constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_Y] !=
             (uint32_t)rect.offset.y ||
          constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Y_PITCH] != buffer_y_pitch ||
          constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Z_PITCH] != buffer_z_pitch) {
         constants_bo = NULL;
         constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET] =
            (uint32_t)rect.offset.x;
         constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_Y] = (uint32_t)rect.offset.y;
         constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Y_PITCH] = buffer_y_pitch;
         constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Z_PITCH] = buffer_z_pitch;
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

      image_descriptor_create_info.image_aspect_index = terakan_format_aspect_index(
         image->format_info.aspect_map, region->imageSubresource.aspectMask, 0);

      unsigned const region_bytes_per_block =
         image->surface.aspects[image_descriptor_create_info.image_aspect_index].bytes_per_block;

      image_descriptor_create_info.view_format =
         terakan_meta_transfer_image_block_format_info(region_bytes_per_block);

      terakan_meta_copy_buffer_image_level_index_and_layer_range(&image_descriptor_create_info,
                                                                 image, region);

      uint64_t buffer_va = buffer->va + region->bufferOffset;

      while (image_descriptor_create_info.layer_count > 0) {
         struct terakan_color_descriptor color_descriptor;
         struct terakan_color_meta_descriptor color_meta_descriptor;
         uint32_t const color_descriptor_layer_count = terakan_image_create_color_descriptor(
            &image_descriptor_create_info, &color_descriptor, &color_meta_descriptor);
         terakan_color_descriptor_image_view_to_color_attachment(&color_descriptor);
         terakan_hw_state_draw_set_cb_color(&command_writer->hw_state_draw, 0, image->bo,
                                            &color_descriptor, &color_meta_descriptor, false);
         terakan_meta_set_db_shader_control_with_rtv(
            command_writer, terakan_meta_copy_buffer_to_image_ps.stage.ps.db_shader_control,
            color_descriptor.info);

         VkDeviceSize const buffer_size_elements =
            (color_descriptor_layer_count - 1) * buffer_z_pitch +
            (rect.extent.height - 1) * buffer_y_pitch + rect.extent.width;
         buffer_resource[0] = (uint32_t)buffer_va;
         buffer_resource[1] = (uint32_t)(region_bytes_per_block * buffer_size_elements - 1);
         buffer_resource[2] =
            S_030008_BASE_ADDRESS_HI(buffer_va >> 32) | S_030008_STRIDE(region_bytes_per_block) |
            S_030008_DATA_FORMAT(image_descriptor_create_info.view_format.format) |
            S_030008_NUM_FORMAT_ALL(terascale_format_get_sq_num_format(
               (enum terascale_format_number_type)
                  image_descriptor_create_info.view_format.number_type)) |
            S_030008_ENDIAN_SWAP(G_028C70_ENDIAN(color_descriptor.info));
         buffer_resource[4] = (uint32_t)buffer_size_elements;
         terakan_hw_state_sqc_set_resource_fs(&command_writer->hw_state_sqc,
                                              TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META,
                                              buffer->bo, buffer_resource);

         terakan_meta_emit_rect_3_vertices_draw(command_writer, &rect,
                                                color_descriptor_layer_count);

         image_descriptor_create_info.base_array_layer += color_descriptor_layer_count;
         image_descriptor_create_info.layer_count -= color_descriptor_layer_count;
         buffer_va +=
            region_bytes_per_block * (VkDeviceSize)buffer_z_pitch * color_descriptor_layer_count;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdCopyImageToBuffer2(VkCommandBuffer const commandBuffer,
                              VkCopyImageToBufferInfo2 const * const pCopyImageToBufferInfo)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   struct terakan_image const * const image =
      terakan_image_from_handle(pCopyImageToBufferInfo->srcImage);

   if (terakan_format_is_expand_3x(image->surface.aspects[0].bytes_per_block)) {
      terakan_meta_copy_expand_3x_image_to_buffer(command_writer, pCopyImageToBufferInfo);
      return;
   }

   struct terakan_image_descriptor_create_info image_descriptor_create_info = {
      .image = image,
      .view_type = terakan_meta_transfer_image_view_type(image->vk.image_type),
      .force_little_endian = true,
      .level_count = 1,
   };

   struct terakan_buffer const * const buffer =
      terakan_buffer_from_handle(pCopyImageToBufferInfo->dstBuffer);

   struct terakan_color_descriptor buffer_uav = {
      .attrib = TERAKAN_COLOR_DESCRIPTOR_BUFFER_UAV_ATTRIB,
   };

   struct terakan_physical_device const * const physical_device =
      terakan_gfx_command_writer_physical_device(command_writer);

   command_writer->post_color_image_copy_write_barrier_actions |=
      TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_UAV | TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;

   terakan_meta_begin_2d_immediate_rects(command_writer, TERAKAN_META_PA_CL_VTE_CNTL_2D,
                                         TERAKAN_META_DB_RENDER_OVERRIDE_DEFAULT, true);

   terakan_meta_set_vs(command_writer, TERAKAN_META_SHADER_POSITION_FROM_INDEX_VS);
   terakan_meta_set_ps(command_writer, TERAKAN_META_SHADER_COPY_IMAGE_TO_BUFFER_PS, true);

   terakan_meta_begin_cb(command_writer, 0xF, 0b0);

   command_writer->push_constants_state.up_to_date_push_constants_bound_to_stages &=
      ~VK_SHADER_STAGE_FRAGMENT_BIT;

   uint32_t constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONSTS_COUNT] = {};
   struct terakan_bo const * constants_bo = NULL;
   uint32_t constants_va_lines;

   for (uint32_t region_index = 0; region_index < pCopyImageToBufferInfo->regionCount;
        ++region_index) {
      VkBufferImageCopy2 const * const region = &pCopyImageToBufferInfo->pRegions[region_index];

      uint32_t buffer_y_pitch, buffer_z_pitch;
      VkRect2D rect;
      terakan_meta_copy_buffer_image_pitches_and_rect(image, region, &buffer_y_pitch,
                                                      &buffer_z_pitch, &rect);

      terakan_meta_copy_buffer_image_level_index_and_layer_range(&image_descriptor_create_info,
                                                                 image, region);

      image_descriptor_create_info.image_aspect_index = terakan_format_aspect_index(
         image->format_info.aspect_map, region->imageSubresource.aspectMask, 0);

      unsigned const region_bytes_per_block =
         image->surface.aspects[image_descriptor_create_info.image_aspect_index].bytes_per_block;

      uint32_t buffer_uav_base_granularity_offset_bytes;
      terakan_color_descriptor_calculate_buffer_base_pitch_slice_dim_offset(
         &buffer_uav, buffer->va + region->bufferOffset,
         (image_descriptor_create_info.layer_count - 1) * buffer_z_pitch +
            (rect.extent.height - 1) * buffer_y_pitch + rect.extent.width,
         region_bytes_per_block, physical_device, &buffer_uav_base_granularity_offset_bytes);

      int32_t const image_offset_x_minus_buffer_offset =
         rect.offset.x -
         (int32_t)(buffer_uav_base_granularity_offset_bytes / region_bytes_per_block);

      if (constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET] !=
             (uint32_t)image_offset_x_minus_buffer_offset ||
          constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_Y] !=
             (uint32_t)rect.offset.y ||
          constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Y_PITCH] != buffer_y_pitch ||
          constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Z_PITCH] != buffer_z_pitch) {
         constants_bo = NULL;
         constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_X_MINUS_BUFFER_OFFSET] =
            (uint32_t)image_offset_x_minus_buffer_offset;
         constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_IMAGE_OFFSET_Y] = (uint32_t)rect.offset.y;
         constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Y_PITCH] = buffer_y_pitch;
         constants[TERAKAN_META_COPY_BUFFER_IMAGE_CONST_BUFFER_Z_PITCH] = buffer_z_pitch;
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

      image_descriptor_create_info.view_format =
         terakan_meta_transfer_image_block_format_info(region_bytes_per_block);

      buffer_uav.info = S_028C70_FORMAT(image_descriptor_create_info.view_format.format) |
                        S_028C70_NUMBER_TYPE(image_descriptor_create_info.view_format.number_type) |
                        TERAKAN_COLOR_DESCRIPTOR_BUFFER_UAV_INFO_CONST_FIELDS;

      terakan_hw_state_draw_set_cb_color(&command_writer->hw_state_draw, 0, buffer->bo, &buffer_uav,
                                         NULL, true);

      uint32_t image_resource[8];
      VkComponentMapping const identity_component_mapping = {};
      if (unlikely(!terakan_image_create_resource_descriptor(
             &image_descriptor_create_info, &identity_component_mapping, image_resource))) {
         assert(!"Invalid image descriptor create info");
         return;
      }
      terakan_hw_state_sqc_set_resource_fs(&command_writer->hw_state_sqc,
                                           TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META,
                                           image->bo, image_resource);

      terakan_meta_emit_rect_3_vertices_draw(command_writer, &rect,
                                             image_descriptor_create_info.layer_count);
   }
}
