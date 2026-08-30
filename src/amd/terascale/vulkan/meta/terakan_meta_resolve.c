/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_meta_impl.h"

#include "terakan_barrier.h"
#include "terakan_entrypoints.h"
#include "terakan_image.h"

#include "util/macros.h"
#include "util/u_math.h"

#include <stdio.h>
#include <stdlib.h>

/* R8xx shader fallback for 2x color resolve. Fixed-point pixel coordinates arrive in R0.XY.
 * Fetch sample 0 to R2 and sample 1 to R3, then export their arithmetic mean.
 */
static uint32_t const terakan_meta_resolve_2x_ps_r8xx[] = {
   S_SQ_CF_WORD0_ADDR(4),
   S_SQ_CF_ALU_WORD1_COUNT(5) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   S_SQ_CF_WORD0_ADDR(10),
   S_SQ_CF_WORD1_COUNT(1) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   S_SQ_CF_WORD0_ADDR(14),
   S_SQ_CF_ALU_WORD1_COUNT(7) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(2),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) | S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   TERAKAN_SHADER_OP1(false, 0, 'Z', MOV, EG, V_SQ_ALU_SRC_0, 0, VEC_012),
   TERAKAN_SHADER_OP1(true, 0, 'W', MOV, EG, V_SQ_ALU_SRC_0, 0, VEC_012),
   TERAKAN_SHADER_OP1(false, 1, 'X', MOV, EG, 0, 'X', VEC_012),
   TERAKAN_SHADER_OP1(false, 1, 'Y', MOV, EG, 0, 'Y', VEC_012),
   TERAKAN_SHADER_OP1(false, 1, 'Z', MOV, EG, V_SQ_ALU_SRC_0, 0, VEC_012),
   TERAKAN_SHADER_OP1(true, 1, 'W', MOV, EG, V_SQ_ALU_SRC_1_INT, 0, VEC_012),

   S_SQ_TEX_WORD0_TEX_INST(SQ_TEX_INST_LD) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(2) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W),
   S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_W),
   0,
   S_SQ_TEX_WORD0_TEX_INST(SQ_TEX_INST_LD) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(1),
   S_SQ_TEX_WORD1_DST_GPR(3) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W),
   S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_W),
   0,

   TERAKAN_SHADER_OP2(false, 2, 'X', ADD, EG, 2, 'X', 3, 'X', VEC_012),
   TERAKAN_SHADER_OP2(false, 2, 'Y', ADD, EG, 2, 'Y', 3, 'Y', VEC_012),
   TERAKAN_SHADER_OP2(false, 2, 'Z', ADD, EG, 2, 'Z', 3, 'Z', VEC_012),
   TERAKAN_SHADER_OP2(true, 2, 'W', ADD, EG, 2, 'W', 3, 'W', VEC_012),
   TERAKAN_SHADER_OP2(false, 2, 'X', MUL_IEEE, EG, 2, 'X', V_SQ_ALU_SRC_0_5, 0, VEC_012),
   TERAKAN_SHADER_OP2(false, 2, 'Y', MUL_IEEE, EG, 2, 'Y', V_SQ_ALU_SRC_0_5, 0, VEC_012),
   TERAKAN_SHADER_OP2(false, 2, 'Z', MUL_IEEE, EG, 2, 'Z', V_SQ_ALU_SRC_0_5, 0, VEC_012),
   TERAKAN_SHADER_OP2(true, 2, 'W', MUL_IEEE, EG, 2, 'W', V_SQ_ALU_SRC_0_5, 0, VEC_012),
};

/* Depth resolve in VK_RESOLVE_MODE_SAMPLE_ZERO_BIT. Fixed-point pixel coordinates arrive in R0.XY,
 * the same way the color resolve above receives them. Fetch sample 0 of the multisample depth
 * source into R1 and export it as the pixel depth.
 *
 * Depth is exported through an ordinary pixel export with array base 61, taking the value from the
 * X component and masking the rest, matching what the shader compiler emits for
 * `FRAG_RESULT_DEPTH`. Reading multisample depth needs no decompression: Terakan does not
 * implement HTILE, so depth is stored uncompressed, and `terakan_depth_msaa_fetch` verifies the
 * per-sample fetch on hardware.
 *
 * Layout, in dwords: 3 control flow instructions (0-5), 3 ALU instructions (6-11), then the
 * texture instruction at 12, which keeps it on the required four-dword boundary. Control flow
 * addresses count eight-byte slots, and the counts are one less than the instruction count.
 */
static uint32_t const terakan_meta_resolve_depth_sample_zero_ps_r8xx[] = {
   S_SQ_CF_WORD0_ADDR(3),
   S_SQ_CF_ALU_WORD1_COUNT(2) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   S_SQ_CF_WORD0_ADDR(6),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(61) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(1),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_MASK) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_MASK) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_MASK) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) | S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* The fetch takes the array slice from Z and the sample index from W, so both must be zeroed
    * rather than left holding whatever the interpolator produced. The third instruction only pads
    * the block so the texture instruction stays four-dword aligned.
    */
   TERAKAN_SHADER_OP1(false, 0, 'Z', MOV, EG, V_SQ_ALU_SRC_0, 0, VEC_012),
   TERAKAN_SHADER_OP1(true, 0, 'W', MOV, EG, V_SQ_ALU_SRC_0, 0, VEC_012),
   TERAKAN_SHADER_OP1(true, 1, 'X', MOV, EG, V_SQ_ALU_SRC_0, 0, VEC_012),

   S_SQ_TEX_WORD0_TEX_INST(SQ_TEX_INST_LD) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(1) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W),
   S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_W),
   0,
};

#define TERAKAN_META_RESOLVE_DEPTH_SAMPLE_ZERO_PS_STATIC_REGISTERS                                 \
   {                                                                                               \
      .sq_pgm_resources =                                                                          \
         {                                                                                         \
            S_028844_NUM_GPRS(2) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,                           \
            TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,                                                \
         },                                                                                        \
      .stage = {.ps = {                                                                            \
         /* Depth only: no color is exported, so the target mask stays empty. */                   \
         .sq_pgm_exports_ps = S_02884C_EXPORT_Z(1),                                                \
         .spi_ps_in_control =                                                                      \
            {                                                                                      \
               S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),                           \
               S_0286D0_FIXED_PT_POSITION_ENA(1) | S_0286D0_FIXED_PT_POSITION_ADDR(0),             \
            },                                                                                     \
         .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),                                          \
         .cb_shader_mask = 0,                                                                      \
      }},                                                                                          \
   }

/* Stencil resolve in VK_RESOLVE_MODE_SAMPLE_ZERO_BIT. Identical to the depth shader above except
 * that the fetched value leaves through the export's Y slot rather than X, which is where DB
 * expects stencil, matching the `FRAG_RESULT_STENCIL` swizzle in the shader compiler.
 */
static uint32_t const terakan_meta_resolve_stencil_sample_zero_ps_r8xx[] = {
   S_SQ_CF_WORD0_ADDR(3),
   S_SQ_CF_ALU_WORD1_COUNT(2) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   S_SQ_CF_WORD0_ADDR(6),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(61) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(1),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_MASK) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_MASK) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_MASK) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) | S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   TERAKAN_SHADER_OP1(false, 0, 'Z', MOV, EG, V_SQ_ALU_SRC_0, 0, VEC_012),
   TERAKAN_SHADER_OP1(true, 0, 'W', MOV, EG, V_SQ_ALU_SRC_0, 0, VEC_012),
   TERAKAN_SHADER_OP1(true, 1, 'X', MOV, EG, V_SQ_ALU_SRC_0, 0, VEC_012),

   S_SQ_TEX_WORD0_TEX_INST(SQ_TEX_INST_LD) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(1) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W),
   S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_W),
   0,
};

#define TERAKAN_META_RESOLVE_STENCIL_SAMPLE_ZERO_PS_STATIC_REGISTERS                               \
   {                                                                                               \
      .sq_pgm_resources =                                                                          \
         {                                                                                         \
            S_028844_NUM_GPRS(2) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,                           \
            TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,                                                \
         },                                                                                        \
      .stage = {.ps = {                                                                            \
         .sq_pgm_exports_ps = S_02884C_EXPORT_Z(1),                                                \
         .spi_ps_in_control =                                                                      \
            {                                                                                      \
               S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),                           \
               S_0286D0_FIXED_PT_POSITION_ENA(1) | S_0286D0_FIXED_PT_POSITION_ADDR(0),             \
            },                                                                                     \
         .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),                                          \
         .cb_shader_mask = 0,                                                                      \
      }},                                                                                          \
   }

/* Depth and stencil resolve in the reducing modes: VK_RESOLVE_MODE_MIN_BIT and
 * VK_RESOLVE_MODE_MAX_BIT. Every sample is fetched and the results are combined in the shader.
 *
 * The sample indices are literals rather than push constants, so there is one program per sample
 * count. Reading them from the constant cache would have allowed a single program to serve every
 * count by clamping the indices on the CPU, but constants do not reach this shader: whatever it
 * reads back is neither the uploaded values nor zeroes, and the fetches then land on sample zero
 * or out of range. Fetching out of range returns zero, which a maximum would survive but a minimum
 * would not, so the counts are kept separate instead of over-fetching.
 *
 * The fetch takes x and y from the coordinate register, the array slice from the swizzle's
 * constant zero, and the sample index from the register's W, matching how the r600 compiler lowers
 * a multisample texel fetch. Each fetch writes a whole register of its own, and each ALU
 * instruction is its own group: grouping copies of different channels aliases one source channel
 * onto another on this hardware, the same effect the image blit shader documents for its position
 * copy.
 *
 * An ALU clause's count is in eight-byte slots, not instructions, so each pair of literal dwords
 * counts as one slot of its own alongside the instruction that reads it.
 */
#define TERAKAN_META_RESOLVE_REDUCE_COORDINATES(gpr, sample)                                       \
   TERAKAN_SHADER_OP1(true, gpr, 'X', MOV, EG, 0, 'X', VEC_012),                                   \
      TERAKAN_SHADER_OP1(true, gpr, 'Y', MOV, EG, 0, 'Y', VEC_012),                                \
      TERAKAN_SHADER_OP1(true, gpr, 'W', MOV, EG, V_SQ_ALU_SRC_LITERAL, 'X', VEC_012), (sample), 0

#define TERAKAN_META_RESOLVE_REDUCE_FETCH(gpr, destination_gpr)                                    \
   S_SQ_TEX_WORD0_TEX_INST(SQ_TEX_INST_LD) |                                                       \
         S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |       \
         S_SQ_TEX_WORD0_SRC_GPR(gpr),                                                              \
      S_SQ_TEX_WORD1_DST_GPR(destination_gpr) |                                                    \
         S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |                                           \
         S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |                                           \
         S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) |                                           \
         S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W),                                            \
      S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) |                                              \
         S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |                                           \
         S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_0) |                                           \
         S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_W),                                            \
      0

/* The export slot DB reads the aspect from, and the register the reduction leaves its result in.
 */
#define TERAKAN_META_RESOLVE_REDUCE_EXPORT(result_gpr, export_x, export_y)                         \
   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |                   \
         S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(61) |                                               \
         S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(result_gpr),                                            \
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_##export_x) |                        \
         S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_##export_y) |                     \
         S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_MASK) |                           \
         S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_MASK) |                           \
         S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) |                                                \
         S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(true) |                                         \
         EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE

/* Two samples. Slots: 4 control flow (0-3), 6 ALU preparing coordinates (4-11, the literal of each
 * sample index taking a slot of its own), 2 texture instructions (12-15), 1 ALU reducing (16).
 */
#define TERAKAN_META_RESOLVE_REDUCE_PS_PROGRAM_2X(inst, export_x, export_y)                        \
   S_SQ_CF_WORD0_ADDR(4),                                                                          \
      S_SQ_CF_ALU_WORD1_COUNT(7) | S_SQ_CF_ALU_WORD1_BARRIER(true) |                               \
         EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,                                                      \
      S_SQ_CF_WORD0_ADDR(12),                                                                      \
      S_SQ_CF_WORD1_COUNT(1) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,      \
      S_SQ_CF_WORD0_ADDR(16),                                                                      \
      S_SQ_CF_ALU_WORD1_COUNT(0) | S_SQ_CF_ALU_WORD1_BARRIER(true) |                               \
         EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,                                                      \
      TERAKAN_META_RESOLVE_REDUCE_EXPORT(3, export_x, export_y),                                   \
                                                                                                   \
      TERAKAN_META_RESOLVE_REDUCE_COORDINATES(1, 0),                                               \
      TERAKAN_META_RESOLVE_REDUCE_COORDINATES(2, 1),                                               \
                                                                                                   \
      TERAKAN_META_RESOLVE_REDUCE_FETCH(1, 3),                                                     \
      TERAKAN_META_RESOLVE_REDUCE_FETCH(2, 4),                                                     \
                                                                                                   \
      TERAKAN_SHADER_OP2(true, 3, 'X', inst, EG, 3, 'X', 4, 'X', VEC_012)

/* Four samples. Slots: 4 control flow (0-3), 12 ALU (4-19), 4 texture (20-27), 3 ALU (28-30). */
#define TERAKAN_META_RESOLVE_REDUCE_PS_PROGRAM_4X(inst, export_x, export_y)                        \
   S_SQ_CF_WORD0_ADDR(4),                                                                          \
      S_SQ_CF_ALU_WORD1_COUNT(15) | S_SQ_CF_ALU_WORD1_BARRIER(true) |                              \
         EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,                                                      \
      S_SQ_CF_WORD0_ADDR(20),                                                                      \
      S_SQ_CF_WORD1_COUNT(3) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,      \
      S_SQ_CF_WORD0_ADDR(28),                                                                      \
      S_SQ_CF_ALU_WORD1_COUNT(2) | S_SQ_CF_ALU_WORD1_BARRIER(true) |                               \
         EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,                                                      \
      TERAKAN_META_RESOLVE_REDUCE_EXPORT(5, export_x, export_y),                                   \
                                                                                                   \
      TERAKAN_META_RESOLVE_REDUCE_COORDINATES(1, 0),                                               \
      TERAKAN_META_RESOLVE_REDUCE_COORDINATES(2, 1),                                               \
      TERAKAN_META_RESOLVE_REDUCE_COORDINATES(3, 2),                                               \
      TERAKAN_META_RESOLVE_REDUCE_COORDINATES(4, 3),                                               \
                                                                                                   \
      TERAKAN_META_RESOLVE_REDUCE_FETCH(1, 5),                                                     \
      TERAKAN_META_RESOLVE_REDUCE_FETCH(2, 6),                                                     \
      TERAKAN_META_RESOLVE_REDUCE_FETCH(3, 7),                                                     \
      TERAKAN_META_RESOLVE_REDUCE_FETCH(4, 8),                                                     \
                                                                                                   \
      TERAKAN_SHADER_OP2(true, 5, 'X', inst, EG, 5, 'X', 6, 'X', VEC_012),                         \
      TERAKAN_SHADER_OP2(true, 5, 'X', inst, EG, 5, 'X', 7, 'X', VEC_012),                         \
      TERAKAN_SHADER_OP2(true, 5, 'X', inst, EG, 5, 'X', 8, 'X', VEC_012)

/* Eight samples. Slots: 4 control flow (0-3), 24 ALU (4-35), 8 texture (36-51), 7 ALU (52-58). */
#define TERAKAN_META_RESOLVE_REDUCE_PS_PROGRAM_8X(inst, export_x, export_y)                        \
   S_SQ_CF_WORD0_ADDR(4),                                                                          \
      S_SQ_CF_ALU_WORD1_COUNT(31) | S_SQ_CF_ALU_WORD1_BARRIER(true) |                              \
         EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,                                                      \
      S_SQ_CF_WORD0_ADDR(36),                                                                      \
      S_SQ_CF_WORD1_COUNT(7) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,      \
      S_SQ_CF_WORD0_ADDR(52),                                                                      \
      S_SQ_CF_ALU_WORD1_COUNT(6) | S_SQ_CF_ALU_WORD1_BARRIER(true) |                               \
         EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,                                                      \
      TERAKAN_META_RESOLVE_REDUCE_EXPORT(9, export_x, export_y),                                   \
                                                                                                   \
      TERAKAN_META_RESOLVE_REDUCE_COORDINATES(1, 0),                                               \
      TERAKAN_META_RESOLVE_REDUCE_COORDINATES(2, 1),                                               \
      TERAKAN_META_RESOLVE_REDUCE_COORDINATES(3, 2),                                               \
      TERAKAN_META_RESOLVE_REDUCE_COORDINATES(4, 3),                                               \
      TERAKAN_META_RESOLVE_REDUCE_COORDINATES(5, 4),                                               \
      TERAKAN_META_RESOLVE_REDUCE_COORDINATES(6, 5),                                               \
      TERAKAN_META_RESOLVE_REDUCE_COORDINATES(7, 6),                                               \
      TERAKAN_META_RESOLVE_REDUCE_COORDINATES(8, 7),                                               \
                                                                                                   \
      TERAKAN_META_RESOLVE_REDUCE_FETCH(1, 9),                                                     \
      TERAKAN_META_RESOLVE_REDUCE_FETCH(2, 10),                                                    \
      TERAKAN_META_RESOLVE_REDUCE_FETCH(3, 11),                                                    \
      TERAKAN_META_RESOLVE_REDUCE_FETCH(4, 12),                                                    \
      TERAKAN_META_RESOLVE_REDUCE_FETCH(5, 13),                                                    \
      TERAKAN_META_RESOLVE_REDUCE_FETCH(6, 14),                                                    \
      TERAKAN_META_RESOLVE_REDUCE_FETCH(7, 15),                                                    \
      TERAKAN_META_RESOLVE_REDUCE_FETCH(8, 16),                                                    \
                                                                                                   \
      TERAKAN_SHADER_OP2(true, 9, 'X', inst, EG, 9, 'X', 10, 'X', VEC_012),                        \
      TERAKAN_SHADER_OP2(true, 9, 'X', inst, EG, 9, 'X', 11, 'X', VEC_012),                        \
      TERAKAN_SHADER_OP2(true, 9, 'X', inst, EG, 9, 'X', 12, 'X', VEC_012),                        \
      TERAKAN_SHADER_OP2(true, 9, 'X', inst, EG, 9, 'X', 13, 'X', VEC_012),                        \
      TERAKAN_SHADER_OP2(true, 9, 'X', inst, EG, 9, 'X', 14, 'X', VEC_012),                        \
      TERAKAN_SHADER_OP2(true, 9, 'X', inst, EG, 9, 'X', 15, 'X', VEC_012),                        \
      TERAKAN_SHADER_OP2(true, 9, 'X', inst, EG, 9, 'X', 16, 'X', VEC_012)

/* The interpolated position, one coordinate register per sample, and one result register per
 * sample.
 */
#define TERAKAN_META_RESOLVE_REDUCE_PS_STATIC_REGISTERS(gpr_count)                                 \
   {                                                                                               \
      .sq_pgm_resources =                                                                          \
         {                                                                                         \
            S_028844_NUM_GPRS(gpr_count) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,                   \
            TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,                                                \
         },                                                                                        \
      .stage = {.ps = {                                                                            \
         .sq_pgm_exports_ps = S_02884C_EXPORT_Z(1),                                                \
         .spi_ps_in_control =                                                                      \
            {                                                                                      \
               S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),                           \
               S_0286D0_FIXED_PT_POSITION_ENA(1) | S_0286D0_FIXED_PT_POSITION_ADDR(0),             \
            },                                                                                     \
         .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),                                          \
         .cb_shader_mask = 0,                                                                      \
      }},                                                                                          \
   }

#define TERAKAN_META_RESOLVE_REDUCE_SHADER(name, samples, gpr_count, inst, export_x, export_y)     \
   static uint32_t const terakan_meta_resolve_##name##_ps_r8xx[] = {                               \
      TERAKAN_META_RESOLVE_REDUCE_PS_PROGRAM_##samples(inst, export_x, export_y),                  \
   };                                                                                              \
   struct terakan_meta_shader const terakan_meta_resolve_##name##_ps = {                           \
      .r8xx =                                                                                      \
         {                                                                                         \
            .program = terakan_meta_resolve_##name##_ps_r8xx,                                      \
            .program_size_bytes = sizeof(terakan_meta_resolve_##name##_ps_r8xx),                   \
            .static_registers = TERAKAN_META_RESOLVE_REDUCE_PS_STATIC_REGISTERS(gpr_count),        \
         },                                                                                        \
      .r9xx =                                                                                      \
         {                                                                                         \
            .program = terakan_meta_resolve_##name##_ps_r8xx,                                      \
            .program_size_bytes = sizeof(terakan_meta_resolve_##name##_ps_r8xx),                   \
            .static_registers = TERAKAN_META_RESOLVE_REDUCE_PS_STATIC_REGISTERS(gpr_count),        \
         },                                                                                        \
      .primary_meta_resource_used = true,                                                          \
   }

TERAKAN_META_RESOLVE_REDUCE_SHADER(depth_min_2x, 2X, 5, MIN_DX10, X, MASK);
TERAKAN_META_RESOLVE_REDUCE_SHADER(depth_max_2x, 2X, 5, MAX_DX10, X, MASK);
TERAKAN_META_RESOLVE_REDUCE_SHADER(depth_min_4x, 4X, 9, MIN_DX10, X, MASK);
TERAKAN_META_RESOLVE_REDUCE_SHADER(depth_max_4x, 4X, 9, MAX_DX10, X, MASK);
TERAKAN_META_RESOLVE_REDUCE_SHADER(depth_min_8x, 8X, 17, MIN_DX10, X, MASK);
TERAKAN_META_RESOLVE_REDUCE_SHADER(depth_max_8x, 8X, 17, MAX_DX10, X, MASK);
TERAKAN_META_RESOLVE_REDUCE_SHADER(stencil_min_2x, 2X, 5, MIN_UINT, MASK, X);
TERAKAN_META_RESOLVE_REDUCE_SHADER(stencil_max_2x, 2X, 5, MAX_UINT, MASK, X);
TERAKAN_META_RESOLVE_REDUCE_SHADER(stencil_min_4x, 4X, 9, MIN_UINT, MASK, X);
TERAKAN_META_RESOLVE_REDUCE_SHADER(stencil_max_4x, 4X, 9, MAX_UINT, MASK, X);
TERAKAN_META_RESOLVE_REDUCE_SHADER(stencil_min_8x, 8X, 17, MIN_UINT, MASK, X);
TERAKAN_META_RESOLVE_REDUCE_SHADER(stencil_max_8x, 8X, 17, MAX_UINT, MASK, X);

struct terakan_meta_shader const terakan_meta_resolve_stencil_sample_zero_ps = {
   .r8xx = {
      .program = terakan_meta_resolve_stencil_sample_zero_ps_r8xx,
      .program_size_bytes = sizeof(terakan_meta_resolve_stencil_sample_zero_ps_r8xx),
      .static_registers = TERAKAN_META_RESOLVE_STENCIL_SAMPLE_ZERO_PS_STATIC_REGISTERS,
   },
   .r9xx = {
      .program = terakan_meta_resolve_stencil_sample_zero_ps_r8xx,
      .program_size_bytes = sizeof(terakan_meta_resolve_stencil_sample_zero_ps_r8xx),
      .static_registers = TERAKAN_META_RESOLVE_STENCIL_SAMPLE_ZERO_PS_STATIC_REGISTERS,
   },
   .primary_meta_resource_used = true,
};

struct terakan_meta_shader const terakan_meta_resolve_depth_sample_zero_ps = {
   .r8xx = {
      .program = terakan_meta_resolve_depth_sample_zero_ps_r8xx,
      .program_size_bytes = sizeof(terakan_meta_resolve_depth_sample_zero_ps_r8xx),
      .static_registers = TERAKAN_META_RESOLVE_DEPTH_SAMPLE_ZERO_PS_STATIC_REGISTERS,
   },
   .r9xx = {
      .program = terakan_meta_resolve_depth_sample_zero_ps_r8xx,
      .program_size_bytes = sizeof(terakan_meta_resolve_depth_sample_zero_ps_r8xx),
      .static_registers = TERAKAN_META_RESOLVE_DEPTH_SAMPLE_ZERO_PS_STATIC_REGISTERS,
   },
   .primary_meta_resource_used = true,
};

struct terakan_meta_shader const terakan_meta_resolve_2x_ps = {
   .r8xx = {
      .program = terakan_meta_resolve_2x_ps_r8xx,
      .program_size_bytes = sizeof(terakan_meta_resolve_2x_ps_r8xx),
      .static_registers = {
         .sq_pgm_resources = {
            S_028844_NUM_GPRS(4) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
            TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
         },
         .stage = {.ps = {
            .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
            .spi_ps_in_control = {
               S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),
               S_0286D0_FIXED_PT_POSITION_ENA(1) | S_0286D0_FIXED_PT_POSITION_ADDR(0),
            },
            .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
            .cb_shader_mask = 0xF,
         }},
      },
   },
   .r9xx = {
      .program = terakan_meta_resolve_2x_ps_r8xx,
      .program_size_bytes = sizeof(terakan_meta_resolve_2x_ps_r8xx),
      .static_registers = {
         .sq_pgm_resources = {
            S_028844_NUM_GPRS(4) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
            TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
         },
         .stage = {.ps = {
            .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
            .spi_ps_in_control = {
               S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),
               S_0286D0_FIXED_PT_POSITION_ENA(1) | S_0286D0_FIXED_PT_POSITION_ADDR(0),
            },
            .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
            .cb_shader_mask = 0xF,
         }},
      },
   },
   .primary_meta_resource_used = true,
};

void
terakan_meta_resolve_depth_stencil(struct terakan_gfx_command_writer * const command_writer,
                                   struct terakan_image const * const src_image,
                                   struct terakan_image const * const dst_image,
                                   VkImageSubresourceLayers const * const src_subresource,
                                   VkImageSubresourceLayers const * const dst_subresource,
                                   VkRect2D const * const area,
                                   VkImageAspectFlags const aspects,
                                   VkResolveModeFlagBits const depth_mode,
                                   VkResolveModeFlagBits const stencil_mode)
{
   enum terascale_r8xx_depth_format dst_depth_format = TERASCALE_R8XX_DEPTH_FORMAT_INVALID;
   bool dst_has_stencil = false;
   if (unlikely(!terascale_get_r8xx_depth_stencil_format(
          vk_format_to_pipe_format(dst_image->vk.format), &dst_depth_format, &dst_has_stencil))) {
      return;
   }
   if (unlikely(area->extent.width == 0 || area->extent.height == 0)) {
      return;
   }
   VkImageAspectFlags const resolved_aspects =
      aspects & (VK_IMAGE_ASPECT_DEPTH_BIT |
                 (dst_has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0));
   if (unlikely(resolved_aspects == 0)) {
      return;
   }

   /* The source was just written as a depth attachment and is about to be read as a texture. */
   terakan_barrier_emit_actions_unconditionally(
      command_writer, TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_DATA |
                         TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_META |
                         TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS |
                         TERAKAN_BARRIER_ACTION_INV_TC);

   struct terakan_meta_config_draw_begin_options const begin_options = {
      .vgt_primitive_type = V_008958_DI_PT_RECTLIST,
      .cb_and_db_shader_control_mode = TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_DYNAMIC,
      .rasterization =
         {
            .enable = true,
            .db_explicit = true,
         },
   };
   terakan_meta_config_draw_begin(command_writer, &begin_options);
   terakan_meta_config_draw_set_cb_color_control_for_mode(command_writer, V_028808_CB_DISABLE);
   terakan_meta_config_draw_set_sq_pgm_vs(command_writer,
                                          TERAKAN_META_SHADER_POSITION_AND_LAYER_FROM_INDEX_VS);
   /* The stencil reference is what a REPLACE operation writes, but with STENCIL_EXPORT_ENABLE the
    * shader's exported value takes its place, so only the write mask matters here.
    */
   terakan_meta_config_draw_set_db_stencilrefmask(command_writer, false,
                                                  S_028430_STENCILWRITEMASK(0xFF));

   uint32_t const layer_count =
      MIN2(src_subresource->layerCount, dst_subresource->layerCount);

   /* Each reducing shader fetches exactly as many samples as the source has, so the sample count
    * selects among three programs per aspect and mode.
    */
   unsigned const reduce_sample_count_index = util_logbase2((uint32_t)src_image->vk.samples) - 1u;

   /* One draw per aspect: they export through different slots and need different DB state. */
   u_foreach_bit (aspect_bit_index, resolved_aspects) {
   VkImageAspectFlags const aspect = (VkImageAspectFlags)1 << aspect_bit_index;
   bool const aspect_is_depth = aspect == VK_IMAGE_ASPECT_DEPTH_BIT;

   /* DB must expect the matching export from the pixel shader. Everything else matches the
    * identity control the other depth-writing meta draws use.
    */
   terakan_meta_config_draw_set_db_shader_control(
      command_writer, TERAKAN_SHADER_DB_SHADER_CONTROL_IDENTITY |
                         (aspect_is_depth ? S_02880C_Z_EXPORT_ENABLE(1)
                                          : S_02880C_STENCIL_EXPORT_ENABLE(1)));
   /* Indexed by [aspect is depth][mode is maximum][log2 of the sample count minus one]. */
   static enum terakan_meta_shader_index const reduce_shaders[2][2][3] = {
      {
         {TERAKAN_META_SHADER_RESOLVE_STENCIL_MIN_2X_PS,
          TERAKAN_META_SHADER_RESOLVE_STENCIL_MIN_4X_PS,
          TERAKAN_META_SHADER_RESOLVE_STENCIL_MIN_8X_PS},
         {TERAKAN_META_SHADER_RESOLVE_STENCIL_MAX_2X_PS,
          TERAKAN_META_SHADER_RESOLVE_STENCIL_MAX_4X_PS,
          TERAKAN_META_SHADER_RESOLVE_STENCIL_MAX_8X_PS},
      },
      {
         {TERAKAN_META_SHADER_RESOLVE_DEPTH_MIN_2X_PS,
          TERAKAN_META_SHADER_RESOLVE_DEPTH_MIN_4X_PS,
          TERAKAN_META_SHADER_RESOLVE_DEPTH_MIN_8X_PS},
         {TERAKAN_META_SHADER_RESOLVE_DEPTH_MAX_2X_PS,
          TERAKAN_META_SHADER_RESOLVE_DEPTH_MAX_4X_PS,
          TERAKAN_META_SHADER_RESOLVE_DEPTH_MAX_8X_PS},
      },
   };
   VkResolveModeFlagBits const aspect_mode = aspect_is_depth ? depth_mode : stencil_mode;
   bool const aspect_reduces =
      (aspect_mode & (VK_RESOLVE_MODE_MIN_BIT | VK_RESOLVE_MODE_MAX_BIT)) != 0 &&
      reduce_sample_count_index < ARRAY_SIZE(reduce_shaders[0][0]);
   enum terakan_meta_shader_index const aspect_shader =
      aspect_reduces
         ? reduce_shaders[aspect_is_depth][aspect_mode == VK_RESOLVE_MODE_MAX_BIT]
                         [reduce_sample_count_index]
         : (aspect_is_depth ? TERAKAN_META_SHADER_RESOLVE_DEPTH_SAMPLE_ZERO_PS
                            : TERAKAN_META_SHADER_RESOLVE_STENCIL_SAMPLE_ZERO_PS);
   terakan_meta_config_draw_set_sq_pgm_ps(command_writer, aspect_shader);
   /* Every resolved pixel must be written regardless of what the destination already holds, and
    * only the aspect being resolved may be touched.
    */
   terakan_meta_config_draw_set_db_depth_control(
      command_writer,
      aspect_is_depth ? (S_028800_Z_ENABLE(true) | S_028800_Z_WRITE_ENABLE(true) |
                         S_028800_ZFUNC(V_028800_STENCILFUNC_ALWAYS))
                      : (S_028800_STENCIL_ENABLE(1) |
                         S_028800_STENCILFUNC(V_028800_STENCILFUNC_ALWAYS) |
                         S_028800_STENCILFAIL(V_028800_STENCIL_REPLACE) |
                         S_028800_STENCILZPASS(V_028800_STENCIL_REPLACE) |
                         S_028800_STENCILZFAIL(V_028800_STENCIL_REPLACE)));

   for (uint32_t layer_index = 0; layer_index < layer_count; ++layer_index) {
      struct terakan_image_descriptor_subresource_range src_range = {
         .base_mip_level = src_subresource->mipLevel,
         .max_level_count = 1,
         .base_z_or_array_layer = src_subresource->baseArrayLayer + layer_index,
         .max_depth_or_layer_count = 1,
      };
      if (unlikely(
             !terakan_image_descriptor_subresource_range_sanitize(src_image, &src_range, false))) {
         continue;
      }
      unsigned const src_aspect_index =
         terakan_format_aspect_index(src_image->format_info.aspect_map, aspect, 0);
      struct terascale_format_info src_view_format =
         src_image->format_info.aspect_formats[src_aspect_index];
      if (!aspect_is_depth) {
         src_view_format = terakan_image_stencil_aspect_sampled_format(src_view_format);
      }
      struct terakan_image_descriptor_create_info const src_descriptor_info = {
         .image = src_image,
         .view_format = src_view_format,
         .image_aspect_index = src_aspect_index,
         .subresource_range = src_range,
      };
      struct terakan_resource_descriptor src_resource;
      if (unlikely(!terakan_image_create_resource_descriptor(
             &src_descriptor_info, V_030000_SQ_TEX_DIM_2D_ARRAY_MSAA, NULL, &src_resource))) {
         continue;
      }

      struct terakan_image_descriptor_subresource_range dst_range = {
         .base_mip_level = dst_subresource->mipLevel,
         .max_level_count = 1,
         .base_z_or_array_layer = dst_subresource->baseArrayLayer + layer_index,
         .max_depth_or_layer_count = 1,
      };
      if (unlikely(
             !terakan_image_descriptor_subresource_range_sanitize(dst_image, &dst_range, false))) {
         continue;
      }
      struct terakan_depth_stencil_descriptor dst_descriptor;
      if (unlikely(!terakan_image_create_depth_stencil_descriptor(
             dst_image, dst_depth_format, dst_has_stencil, &dst_range, &dst_descriptor))) {
         continue;
      }

      terakan_hw_config_sqk_set_resource_fs(&command_writer->hw_config_sqk,
                                            TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META,
                                            src_image->bo, &src_resource);
      terakan_meta_config_draw_set_db_depth_stencil_buffer(command_writer, dst_image->bo,
                                                           &dst_descriptor);

      struct terakan_screen_rect const screen_bounds = {
         .bounds = {
            [1] = {
               u_minify(dst_image->vk.extent.width, dst_range.base_mip_level),
               u_minify(dst_image->vk.extent.height, dst_range.base_mip_level),
            },
         },
      };
      terakan_meta_draw_rect(command_writer,
                             terakan_vk_rect_to_screen_rect(*area, screen_bounds), 1);
   }
   }

   /* The resolved depth is normally sampled or transferred right after the render pass ends. */
   terakan_barrier_emit_actions_unconditionally(
      command_writer, TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_DATA |
                         TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_META |
                         TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS |
                         TERAKAN_BARRIER_ACTION_INV_TC);
}

/* Evergreen has one per-draw array-slice select shared by every bound colour buffer, not an
 * independent one per RTV, so a source and destination sitting on different array layers cannot
 * both be addressed through their own SLICE_START: the source's wins and the resolve lands on the
 * wrong layer of the destination (confirmed on real CAICOS hardware -- see
 * terakan_color_resolve_subresource_test).
 *
 * The destination can be moved to meet the source instead. Shift its base address by the layer
 * difference and let it select the source's slice: the shared select then lands on the layer the
 * caller asked for. Nothing changes when the layers already match, which is the case the fixed
 * function always handled.
 *
 * Only the destination can be moved, and only when it carries no colour metadata. CMASK and FMASK
 * are not addressed through a base the driver can shift -- terakan_image_create_color_descriptor
 * leaves their bases at the image and lets the same slice select index them through their own tile
 * strides -- so moving the colour base out from under them desynchronises the two. That rules out
 * the multisample source, which always has them, and any destination that has them. Such a region
 * is skipped, as it was before.
 */
static bool
terakan_meta_resolve_retarget_dst_slice(struct terakan_color_descriptor * const dst_color,
                                        struct terakan_color_meta_descriptor * const dst_meta,
                                        struct terakan_image const * const dst_image,
                                        unsigned const dst_aspect_index,
                                        uint32_t const dst_mip_level, uint32_t const src_slice)
{
   uint32_t const dst_slice = G_028C6C_SLICE_START(dst_color->view);
   if (dst_slice == src_slice) {
      return true;
   }
   if (terakan_image_surface_has_color_metadata(&dst_image->surface)) {
      return false;
   }
   uint32_t const slice_size_bytes_shr8 =
      dst_image->surface.aspects[dst_aspect_index].levels[dst_mip_level].slice_size_bytes_shr8;
   dst_color->base = (uint32_t)((int64_t)dst_color->base +
                                ((int64_t)dst_slice - (int64_t)src_slice) * slice_size_bytes_shr8);
   /* max_depth_or_layer_count is 1 here, so SLICE_MAX equals SLICE_START. */
   dst_color->view = S_028C6C_SLICE_START(src_slice) | S_028C6C_SLICE_MAX(src_slice);
   /* FMASK must equal BASE on a single-sampled surface, so rebuild the disabled descriptor from
    * the moved base rather than leaving it pointing at the old layer.
    */
   *dst_meta = terakan_color_meta_descriptor_create_disabled(dst_color);
   return true;
}

static bool
terakan_meta_resolve_region_is_fixed_function_compatible(
   struct terakan_image const * const src_image, struct terakan_image const * const dst_image,
   VkImageResolve2 const * const region)
{
   VkExtent3D const src_extent = {
      .width = u_minify(src_image->vk.extent.width, region->srcSubresource.mipLevel),
      .height = u_minify(src_image->vk.extent.height, region->srcSubresource.mipLevel),
      .depth = 1,
   };
   VkExtent3D const dst_extent = {
      .width = u_minify(dst_image->vk.extent.width, region->dstSubresource.mipLevel),
      .height = u_minify(dst_image->vk.extent.height, region->dstSubresource.mipLevel),
      .depth = 1,
   };

   /* CB_RESOLVE reads and writes one coordinate stream, so the two regions must sit at the same
    * offset; scissoring the draw makes a same-offset subrectangle safe without rebasing either
    * surface. Differing offsets would need the shader path, which is disabled elsewhere in this
    * file.
    *
    * The surfaces themselves do not have to match, though. This used to also require equal source
    * and destination surface dimensions, which was an assumption rather than a measurement: with it
    * dropped and only the region required to fit inside both, the whole
    * dEQP-VK.api.copy_and_blit.core.resolve_image.diff_image_size group goes from 9 passing and 18
    * failing to 27 passing and none failing.
    *
    * Differing array layers are handled by terakan_meta_resolve_fold_slice_into_base rather than by
    * being excluded here; see its comment for why the shared slice select made that necessary.
    */
   return region->dstOffset.x >= 0 && region->dstOffset.y >= 0 &&
          region->extent.width <= dst_extent.width &&
          region->extent.height <= dst_extent.height &&
          (uint32_t)region->dstOffset.x <= dst_extent.width - region->extent.width &&
          (uint32_t)region->dstOffset.y <= dst_extent.height - region->extent.height &&
          region->srcOffset.x >= 0 && region->srcOffset.y >= 0 && region->srcOffset.z == 0 &&
          region->srcOffset.x == region->dstOffset.x &&
          region->srcOffset.y == region->dstOffset.y && region->dstOffset.z == 0 &&
          region->extent.width != 0 && region->extent.height != 0 && region->extent.depth == 1 &&
          region->extent.width <= src_extent.width &&
          region->extent.height <= src_extent.height &&
          (uint32_t)region->srcOffset.x <= src_extent.width - region->extent.width &&
          (uint32_t)region->srcOffset.y <= src_extent.height - region->extent.height;
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdResolveImage2(VkCommandBuffer const command_buffer_handle,
                         VkResolveImageInfo2 const * const resolve_info)
{
   struct terakan_image const * const src_image =
      terakan_image_from_handle(resolve_info->srcImage);
   struct terakan_image const * const dst_image =
      terakan_image_from_handle(resolve_info->dstImage);
   if (getenv("TERAKAN_DEBUG_RENDER") != NULL) {
      fprintf(stderr,
              "[TERAKAN_RESOLVE] src=%p %ux%u samples=%u layout=%u dst=%p %ux%u samples=%u layout=%u regions=%u\n",
              (void *)src_image, src_image != NULL ? src_image->vk.extent.width : 0,
              src_image != NULL ? src_image->vk.extent.height : 0,
              src_image != NULL ? src_image->vk.samples : 0, resolve_info->srcImageLayout,
              (void *)dst_image, dst_image != NULL ? dst_image->vk.extent.width : 0,
              dst_image != NULL ? dst_image->vk.extent.height : 0,
              dst_image != NULL ? dst_image->vk.samples : 0, resolve_info->dstImageLayout,
              resolve_info->regionCount);
   }
   if (unlikely(src_image == NULL || dst_image == NULL ||
                src_image->vk.samples <= VK_SAMPLE_COUNT_1_BIT ||
                dst_image->vk.samples != VK_SAMPLE_COUNT_1_BIT)) {
      return;
   }
   unsigned const src_aspect_index =
      terakan_format_aspect_index(src_image->format_info.aspect_map,
                                  VK_IMAGE_ASPECT_COLOR_BIT, 0);
   unsigned const dst_aspect_index =
      terakan_format_aspect_index(dst_image->format_info.aspect_map,
                                  VK_IMAGE_ASPECT_COLOR_BIT, 0);
   struct terascale_format_info const src_format =
      src_image->format_info.aspect_formats[src_aspect_index];
   struct terascale_format_info const dst_format =
      dst_image->format_info.aspect_formats[dst_aspect_index];
   bool const debug_render = getenv("TERAKAN_DEBUG_RENDER") != NULL;
   /* CB_RESOLVE averages samples, and Vulkan resolves an integer format by selecting one instead,
    * so integer formats take the shader path and everything else keeps the fixed function. This
    * used to be a bare return -- an integer resolve did nothing at all, which is what made the
    * multisample integer cases of dEQP-VK.api.image_clearing fail: they read the image back by
    * resolving it.
    */
   bool const resolve_selects_sample_zero =
      src_format.number_type == TERASCALE_FORMAT_NUMBER_TYPE_UINT ||
      src_format.number_type == TERASCALE_FORMAT_NUMBER_TYPE_SINT ||
      dst_format.number_type == TERASCALE_FORMAT_NUMBER_TYPE_UINT ||
      dst_format.number_type == TERASCALE_FORMAT_NUMBER_TYPE_SINT;

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(command_buffer_handle)->command_writer.gfx;
   /* The 2x averaging shader stays disabled: the initial R8xx TXF_MS fallback did not decode
    * direct sample coordinates correctly and corrupted the entire frame, and the fixed function
    * does that case correctly anyway. The scaffolding it needed is what the sample-zero shader
    * uses, which is why the flag is shared.
    */
   bool const shader_resolve_2x = resolve_selects_sample_zero;

   /* The fixed function consumes the source through CB, which keeps the RTV metadata coherent on
    * its own. The shader path samples the source as a texture instead, bypassing CB entirely, so
    * the metadata has to reach memory first - otherwise the fetch reads whole tiles as they were
    * before the render pass, which is what made every integer format's renderpass resolve
    * inconsistent between attachments.
    */
   terakan_barrier_emit_actions_unconditionally(
      command_writer,
      TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
         TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS |
         (shader_resolve_2x ? TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_META : 0));

   struct terakan_meta_config_draw_begin_options const begin_options = {
      .vgt_primitive_type = V_008958_DI_PT_RECTLIST,
      .cb_and_db_shader_control_mode =
         shader_resolve_2x
            ? TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_NORMAL_WITH_RTV_AND_DYNAMIC_DB_SHADER_CONTROL
            : TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_DYNAMIC,
      .rasterization =
         {
            .enable = true,
            .msaa_num_samples_log2 =
               shader_resolve_2x ? 0 : util_logbase2((uint32_t)src_image->vk.samples),
            .msaa_num_anchor_samples_log2 =
               shader_resolve_2x ? 0 : util_logbase2((uint32_t)src_image->vk.samples),
         },
   };
   terakan_meta_config_draw_begin(command_writer, &begin_options);
   terakan_meta_config_draw_set_sq_pgm_vs(command_writer,
                                          TERAKAN_META_SHADER_POSITION_AND_LAYER_FROM_INDEX_VS);
   terakan_meta_config_draw_set_sq_pgm_ps(
      command_writer, shader_resolve_2x ? TERAKAN_META_SHADER_RESOLVE_SAMPLE_ZERO_PS
                                        : TERAKAN_META_SHADER_DUMMY_OPAQUE_PS);
   terakan_meta_config_draw_set_cb_color_control_for_mode(
      command_writer, shader_resolve_2x ? V_028808_CB_NORMAL : V_028808_CB_RESOLVE);

   for (uint32_t region_index = 0; region_index < resolve_info->regionCount; ++region_index) {
      VkImageResolve2 const * const region = &resolve_info->pRegions[region_index];
      if (debug_render) {
         fprintf(stderr,
                 "[TERAKAN_RESOLVE] region[%u] src=%d,%d dst=%d,%d extent=%ux%u compatible=%u srcfmt=%u dstfmt=%u\n",
                 region_index, region->srcOffset.x, region->srcOffset.y,
                 region->dstOffset.x, region->dstOffset.y, region->extent.width,
                 region->extent.height,
                 terakan_meta_resolve_region_is_fixed_function_compatible(src_image, dst_image,
                                                                           region),
                 src_format.format, dst_format.format);
      }

      /* Evergreen's fixed-function CB resolve requires matching source and destination surface
       * dimensions and coordinates. Differently offset regions need a shader fallback.
       */
      if (unlikely(region->srcSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
                   region->dstSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
                   !terakan_meta_resolve_region_is_fixed_function_compatible(src_image, dst_image,
                                                                             region))) {
         continue;
      }

      uint32_t const layer_count =
         MIN2(region->srcSubresource.layerCount, region->dstSubresource.layerCount);
      for (uint32_t layer_index = 0; layer_index < layer_count; ++layer_index) {
         struct terakan_image_descriptor_create_info descriptor_info[2] = {
            {
               .image = src_image,
               .view_format = src_format,
               .image_aspect_index = src_aspect_index,
               .subresource_range =
                  {
                     .base_mip_level = region->srcSubresource.mipLevel,
                     .max_level_count = 1,
                     .base_z_or_array_layer =
                        region->srcSubresource.baseArrayLayer + layer_index,
                     .max_depth_or_layer_count = 1,
                  },
            },
            {
               .image = dst_image,
               .view_format = dst_format,
               .image_aspect_index = dst_aspect_index,
               .subresource_range =
                  {
                     .base_mip_level = region->dstSubresource.mipLevel,
                     .max_level_count = 1,
                     .base_z_or_array_layer =
                        region->dstSubresource.baseArrayLayer + layer_index,
                     .max_depth_or_layer_count = 1,
                  },
            },
         };

         if (unlikely(!terakan_image_descriptor_subresource_range_sanitize(
                         src_image, &descriptor_info[0].subresource_range, false) ||
                      !terakan_image_descriptor_subresource_range_sanitize(
                         dst_image, &descriptor_info[1].subresource_range, false))) {
            continue;
         }

         struct terakan_color_descriptor color[2];
         struct terakan_color_meta_descriptor meta[2];
         uint32_t const color_resource_type[2] = {
            terakan_image_depth_or_array_layers(src_image, region->srcSubresource.mipLevel) > 1
               ? V_028C70_TEXTURE2DARRAY
               : V_028C70_TEXTURE2D,
            terakan_image_depth_or_array_layers(dst_image, region->dstSubresource.mipLevel) > 1
               ? V_028C70_TEXTURE2DARRAY
               : V_028C70_TEXTURE2D,
         };
         if (shader_resolve_2x) {
            if (unlikely(terakan_image_create_color_descriptor(
                            &descriptor_info[1], color_resource_type[1], &color[1], &meta[1]) != 1)) {
               continue;
            }
         } else if (unlikely(
                       terakan_image_create_color_descriptor(
                          &descriptor_info[0], color_resource_type[0], &color[0], &meta[0]) != 1 ||
                       terakan_image_create_color_descriptor(
                          &descriptor_info[1], color_resource_type[1], &color[1], &meta[1]) != 1)) {
            continue;
         }
         if (!shader_resolve_2x &&
             unlikely(!terakan_meta_resolve_retarget_dst_slice(
                         &color[1], &meta[1], dst_image, dst_aspect_index,
                         region->dstSubresource.mipLevel, G_028C6C_SLICE_START(color[0].view)))) {
            continue;
         }
         if (debug_render) {
            if (shader_resolve_2x) {
               fprintf(stderr,
                       "[TERAKAN_RESOLVE] shader2x dst_type=%u dst_info=0x%08x"
                       " dst_pitch=0x%08x dst_slice=0x%08x dst_attrib=0x%08x\n",
                       color_resource_type[1], color[1].info, color[1].pitch, color[1].slice,
                       color[1].attrib);
            } else {
               fprintf(stderr,
                       "[TERAKAN_RESOLVE] descriptors src_type=%u dst_type=%u"
                       " src_info=0x%08x dst_info=0x%08x"
                       " src_pitch=0x%08x dst_pitch=0x%08x"
                       " src_slice=0x%08x dst_slice=0x%08x"
                       " src_attrib=0x%08x dst_attrib=0x%08x\n",
                       color_resource_type[0], color_resource_type[1], color[0].info, color[1].info,
                       color[0].pitch, color[1].pitch, color[0].slice, color[1].slice,
                       color[0].attrib, color[1].attrib);
            }
         }

         if (shader_resolve_2x) {
            struct terakan_resource_descriptor src_resource;
            if (unlikely(!terakan_image_create_resource_descriptor(
                           &descriptor_info[0], V_030000_SQ_TEX_DIM_2D_ARRAY_MSAA, NULL,
                           &src_resource))) {
               continue;
            }
            terakan_meta_config_draw_set_cb_rtvs_and_db_shader_control(
               command_writer, 0xF, &dst_image->bo, &color[1], &meta[1],
               TERAKAN_SHADER_DB_SHADER_CONTROL_IDENTITY);
            terakan_hw_config_sqk_set_resource_fs(
               &command_writer->hw_config_sqk,
               TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META, src_image->bo, &src_resource);
         } else {
            struct terakan_bo const * const bos[2] = {src_image->bo, dst_image->bo};
            terakan_meta_config_draw_set_cb_rtvs_and_db_shader_control(
               command_writer, 0xFF, bos, color, meta, TERAKAN_SHADER_DB_SHADER_CONTROL_IDENTITY);
            /* CB_RESOLVE consumes RTV 0 as the source and routes the resolved value to RTV 1,
             * while only color export 0 is enabled.
             */
            terakan_hw_config_draw_set_cb_target_mask(&command_writer->hw_config_draw, 0xF);
         }

         struct terakan_screen_rect const screen_bounds = {
            .bounds = {
               [1] = {
                  G_028C78_WIDTH_MAX(color[shader_resolve_2x ? 1 : 0].dim) + 1,
                  G_028C78_HEIGHT_MAX(color[shader_resolve_2x ? 1 : 0].dim) + 1,
               },
            },
         };
         VkRect2D const rect = {
            .offset = {
               .x = region->dstOffset.x,
               .y = region->dstOffset.y,
            },
            .extent = {
               .width = region->extent.width,
               .height = region->extent.height,
            },
         };
         terakan_meta_draw_rect(command_writer,
                                terakan_vk_rect_to_screen_rect(rect, screen_bounds), 1);
      }
   }

   /* CB_RESOLVE writes through the color buffer and its result is commonly sampled immediately
    * afterwards. Make the resolve complete and invalidate TC here rather than relying solely on a
    * later application barrier to consume deferred copy-write actions. This matches the ordering
    * required by the Radeon CB resolve path and also covers applications that keep the image in a
    * general layout across the resolve and the following sampling pass.
    */
   terakan_barrier_emit_actions_unconditionally(
      command_writer, TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
                         TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS |
                         TERAKAN_BARRIER_ACTION_INV_TC);
}
