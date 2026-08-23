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
                                   VkImageAspectFlags const aspects)
{
   /* Only VK_RESOLVE_MODE_SAMPLE_ZERO_BIT so far, which the specification requires whenever
    * VK_KHR_depth_stencil_resolve is exposed at all.
    */
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
   terakan_meta_config_draw_set_sq_pgm_ps(
      command_writer, aspect_is_depth ? TERAKAN_META_SHADER_RESOLVE_DEPTH_SAMPLE_ZERO_PS
                                      : TERAKAN_META_SHADER_RESOLVE_STENCIL_SAMPLE_ZERO_PS);
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
      struct terakan_image_descriptor_create_info const src_descriptor_info = {
         .image = src_image,
         .view_format = src_image->format_info.aspect_formats[src_aspect_index],
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

   /* CB_RESOLVE operates on matching source and destination coordinates and surface dimensions.
    * Scissoring the draw makes same-offset subrectangles safe without rebasing either surface.
    */
   return src_extent.width == dst_extent.width && src_extent.height == dst_extent.height &&
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
   /* CB_RESOLVE averages samples. Vulkan integer resolves select one sample instead. */
   if (unlikely(src_format.number_type == TERASCALE_FORMAT_NUMBER_TYPE_UINT ||
                src_format.number_type == TERASCALE_FORMAT_NUMBER_TYPE_SINT ||
                dst_format.number_type == TERASCALE_FORMAT_NUMBER_TYPE_UINT ||
                dst_format.number_type == TERASCALE_FORMAT_NUMBER_TYPE_SINT)) {
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(command_buffer_handle)->command_writer.gfx;
   /* Disabled: the initial R8xx TXF_MS fallback did not decode direct sample coordinates
    * correctly and corrupted the entire frame. Keep using CB_RESOLVE until the shader path has
    * an isolated conformance test.
    */
   bool const shader_resolve_2x = false;

   terakan_barrier_emit_actions_unconditionally(
      command_writer, TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
                         TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS);

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
      command_writer, shader_resolve_2x ? TERAKAN_META_SHADER_RESOLVE_2X_PS
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
