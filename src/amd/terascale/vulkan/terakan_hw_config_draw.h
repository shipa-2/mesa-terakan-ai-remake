/*
 * Copyright © 2026 Vitaliy Triang3l Kuzmin
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

#ifndef TERAKAN_HW_CONFIG_DRAW_H
#define TERAKAN_HW_CONFIG_DRAW_H

#include "terakan_bo.h"
#include "terakan_descriptor.h"
#include "terakan_shader.h"

#include "gallium/drivers/r600/evergreend.h"
#include "util/bitset.h"
#include "util/macros.h"
#include "util/u_endian.h"
#include "util/u_math.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bottom-level emission of packets setting graphics context registers.
 *
 * Also contains helpers for building the graphics context register values.
 */

struct terakan_hw_config_draw_vgt_dma_index_buffer {
   /* Unbound if `bo` is NULL or `size_indices` is 0. */
   uint64_t va;
   struct terakan_bo const * bo;
   uint32_t size_indices;
};

static inline bool
terakan_hw_config_draw_vgt_dma_index_buffer_is_bound(
   struct terakan_hw_config_draw_vgt_dma_index_buffer const index_buffer)
{
   return index_buffer.size_indices != 0 && index_buffer.bo != NULL;
}

#if UTIL_ARCH_BIG_ENDIAN
#define TERAKAN_HW_CONFIG_DRAW_VGT_DMA_INDEX_TYPE_16_HOST_ENDIAN                                   \
   (VGT_INDEX_16 | VGT_DMA_SWAP_16_BIT)
#define TERAKAN_HW_CONFIG_DRAW_VGT_DMA_INDEX_TYPE_32_HOST_ENDIAN                                   \
   (VGT_INDEX_32 | VGT_DMA_SWAP_32_BIT)
#else
#define TERAKAN_HW_CONFIG_DRAW_VGT_DMA_INDEX_TYPE_16_HOST_ENDIAN VGT_INDEX_16
#define TERAKAN_HW_CONFIG_DRAW_VGT_DMA_INDEX_TYPE_32_HOST_ENDIAN VGT_INDEX_32
#endif

/* `evergreen_fix_scissor_coordinates` in xf86-video-ati, which applies hardware bug workarounds,
 * sets the top and left edges to 1 if the bottom and right edges respectively are 0.
 */
#define TERAKAN_HW_CONFIG_DRAW_PA_SC_SCISSOR_EMPTY_TL(window_offset_disable)                       \
   (S_028250_TL_X(1) | S_028250_TL_Y(1) | S_028250_WINDOW_OFFSET_DISABLE(window_offset_disable))
#define TERAKAN_HW_CONFIG_DRAW_PA_SC_SCISSOR_EMPTY_BR 0

#define TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT 16

#define TERAKAN_HW_CONFIG_DRAW_PA_CL_VTE_CNTL_3D                                                   \
   (S_028818_VPORT_X_SCALE_ENA(true) | S_028818_VPORT_X_OFFSET_ENA(true) |                         \
    S_028818_VPORT_Y_SCALE_ENA(true) | S_028818_VPORT_Y_OFFSET_ENA(true) |                         \
    S_028818_VPORT_Z_SCALE_ENA(true) | S_028818_VPORT_Z_OFFSET_ENA(true) |                         \
    S_028818_VTX_W0_FMT(true))

#define TERAKAN_HW_CONFIG_DRAW_PA_SC_MODE_CNTL_1_CONSTANT                                          \
   (EG_S_028A4C_FORCE_EOV_CNTDWN_ENABLE(true) | EG_S_028A4C_FORCE_EOV_REZ_ENABLE(true))

extern uint8_t const terakan_hw_config_draw_pa_sc_aa_standard_sample_locs[5][16][4];
extern uint8_t const terakan_hw_config_draw_pa_sc_aa_standard_max_sample_dists[5];

struct terakan_hw_config_draw_pa_su_poly_offset {
   /* For both front and back faces (separate configuration is not needed currently). */
   float slope_scale_per_16th_subpixel;
   float constant_offset;
   float clamp;
};

static inline int
terakan_hw_config_draw_pa_sc_aa_sample_loc_signed_for_tl_0_to_br_1(float const tl_offset_pixels)
{
   if (unlikely(isnan(tl_offset_pixels))) {
      return 0;
   }
   float const tl_offset_subpixels = CLAMP(tl_offset_pixels, 0.0f, 1.0f - 0x1.0p-4f) * 0x1.0p4f;
   /* From the GL_ARB_sample_locations specification:
    *
    *     "Sample locations are rounded on use to the precision indicated by the value of
    *     SAMPLE_LOCATION_SUBPIXEL_BITS_ARB (i.e. rounded to the nearest 2^{-subpixelbits})."
    *
    * From the GL_AMD_sample_positions specification:
    *
    *     "Given two sample positions (x0, y0) and (x1, y1), one can make sure they don't fall in
    *     the same subpixel if
    *
    *             abs(x0-x1) >= ssd and abs(y0-y1) >= ssd,
    *
    *     where ssd is the float value returned when querying SUBSAMPLE_DISTANCE_AMD."
    *
    * Rounding N.5 up specifically (exactly, not using `(int)(x + 0.5f)`, which rounds twice), as
    * with rounding to the nearest even, both 1.5/16 and 2.5/16 would've been rounded to 2, making
    * an `ssd` of 1/16 insufficient.
    */
   int const tl_offset_subpixels_floored = (int)tl_offset_subpixels;
   return tl_offset_subpixels_floored +
          (int)(tl_offset_subpixels - (float)tl_offset_subpixels_floored >= 0.5f) - 8;
}

static inline uint8_t
terakan_hw_config_draw_pa_sc_aa_sample_loc_for_tl_0_to_br_1(float const l_offset_pixels,
                                                            float const t_offset_pixels)
{
   int const sample_loc_x =
      terakan_hw_config_draw_pa_sc_aa_sample_loc_signed_for_tl_0_to_br_1(l_offset_pixels);
   int const sample_loc_y =
      terakan_hw_config_draw_pa_sc_aa_sample_loc_signed_for_tl_0_to_br_1(t_offset_pixels);
   return (uint8_t)((sample_loc_x & 0xF) | (sample_loc_y << 4));
}

static inline uint8_t
terakan_hw_config_draw_pa_sc_aa_sample_max_dist(uint8_t const sample_loc_xy)
{
   unsigned const abs_x = (unsigned)abs((int8_t)(sample_loc_xy << 4) >> 4);
   unsigned const abs_y = (unsigned)abs((int8_t)sample_loc_xy >> 4);
   return (uint8_t)MAX2(abs_x, abs_y);
}

#define TERAKAN_HW_CONFIG_DRAW_PA_SC_AA_CONFIG_FIELDS_R8XX                                         \
   ((uint32_t) ~(C_028C04_MSAA_NUM_SAMPLES & C_028C04_AA_MASK_CENTROID_DTMN &                      \
                 C_028C04_MAX_SAMPLE_DIST))

#define TERAKAN_HW_CONFIG_DRAW_PA_CL_GB_MIN -0x1.0p15f
/* Don't allow clipping to produce vertices out of the 16.8 signed fixed-point range.
 * Specifying the maximum as 1 subpixel away from the bottom and right edges rather than 0.5
 * subpixels, because a vertex exactly 0.5 subpixels away from edge needs to be rounded towards the
 * edge (to even), which can't be represented by a 16-bit signed integer.
 * Also, `MAX + MIN`, the maximum viewport size, can be represented exactly as 32-bit floating-point
 * if `MAX` is 2^15 - 2^-8, but not if it's 2^15 - 2^-9.
 */
#define TERAKAN_HW_CONFIG_DRAW_PA_CL_GB_MAX (0x1.0p15f - 0x1.0p-8f)

#define TERAKAN_HW_CONFIG_DRAW_DB_EQAA_CONSTANT                                                    \
   (S_028804_HIGH_QUALITY_INTERSECTIONS(true) | S_028804_INCOHERENT_EQAA_READS(true) |             \
    S_028804_STATIC_ANCHOR_ASSOCIATIONS(true))

/* Returns -1 if sample shading is not needed for this minumum sample shading value,
 * log2(maximum number of samples per shader invocation) (>= 0) if sample shading is required.
 */
static inline int8_t
terakan_hw_config_draw_db_eqaa_ps_iter_max_invocation_samples_log2(float const min_sample_shading)
{
   if (!isgreater(min_sample_shading, 0.0f)) {
      /* Sample shading not requested. Also treat NaN as 0. */
      return -1;
   }
   /* Subtract 1 ULP before getting the exponent, because 2 samples per fragment - 0.5 sample
    * shading - is precisely sufficient for (0.25, 0.5] minimum sample shading, but the exponent -2
    * means [0.25, 0.5) instead, and similarly for other exponents.
    */
   int const max_invocation_samples_log2_unclamped =
      (0x7F - 1) - (int)((fui(min_sample_shading) - 1) >> 23);
   /* 2^3 samples per invocation is the most coarse sample shading possible for 2^4 samples. */
   return CLAMP(max_invocation_samples_log2_unclamped, 0, 3);
}

#define TERAKAN_HW_CONFIG_DRAW_DB_ALPHA_TO_MASK_OFFSETS_CLEAR_MASK                                 \
   (C_028B70_ALPHA_TO_MASK_OFFSET0 & C_028B70_ALPHA_TO_MASK_OFFSET1 &                              \
    C_028B70_ALPHA_TO_MASK_OFFSET2 & C_028B70_ALPHA_TO_MASK_OFFSET3 & C_028B70_OFFSET_ROUND)
#define TERAKAN_HW_CONFIG_DRAW_DB_ALPHA_TO_MASK_OFFSETS_DITHERED                                   \
   (S_028B70_ALPHA_TO_MASK_OFFSET0(3) | S_028B70_ALPHA_TO_MASK_OFFSET1(1) |                        \
    S_028B70_ALPHA_TO_MASK_OFFSET2(0) | S_028B70_ALPHA_TO_MASK_OFFSET3(2) |                        \
    S_028B70_OFFSET_ROUND(true))
#define TERAKAN_HW_CONFIG_DRAW_DB_ALPHA_TO_MASK_OFFSETS_REGULAR                                    \
   (S_028B70_ALPHA_TO_MASK_OFFSET0(2) | S_028B70_ALPHA_TO_MASK_OFFSET1(2) |                        \
    S_028B70_ALPHA_TO_MASK_OFFSET2(2) | S_028B70_ALPHA_TO_MASK_OFFSET3(2))

static inline bool
terakan_hw_config_draw_cb_blend_control_comb_fcn_uses_factors(uint32_t const comb_fcn)
{
   /* MIN is 0b10, MAX is 0b11, no other functions have bit 1 set. */
   return !(comb_fcn & 0b10);
}

#define TERAKAN_HW_CONFIG_DRAW_CB_BLEND_CONTROL_FACTORS_CONST_COLOR                                \
   (BITFIELD_BIT(V_028780_BLEND_CONST_COLOR) | BITFIELD_BIT(V_028780_BLEND_ONE_MINUS_CONST_COLOR))
#define TERAKAN_HW_CONFIG_DRAW_CB_BLEND_CONTROL_FACTORS_CONST_ALPHA                                \
   (BITFIELD_BIT(V_028780_BLEND_CONST_ALPHA) | BITFIELD_BIT(V_028780_BLEND_ONE_MINUS_CONST_ALPHA))
#define TERAKAN_HW_CONFIG_DRAW_CB_BLEND_CONTROL_FACTORS_CONST                                      \
   (TERAKAN_HW_CONFIG_DRAW_CB_BLEND_CONTROL_FACTORS_CONST_COLOR |                                  \
    TERAKAN_HW_CONFIG_DRAW_CB_BLEND_CONTROL_FACTORS_CONST_ALPHA)
#define TERAKAN_HW_CONFIG_DRAW_CB_BLEND_CONTROL_FACTORS_SRC1                                       \
   (BITFIELD_BIT(V_028780_BLEND_SRC1_COLOR) | BITFIELD_BIT(V_028780_BLEND_INV_SRC1_COLOR) |        \
    BITFIELD_BIT(V_028780_BLEND_SRC1_ALPHA) | BITFIELD_BIT(V_028780_BLEND_INV_SRC1_ALPHA))

/* Remaps alpha factors to color factors, and the rest as identity, for normalization of alpha blend
 * factors.
 */
extern uint8_t const terakan_hw_config_draw_cb_blend_control_color_factors_for_color_alpha[0x20];

/* Can be serialized in the pipeline cache. */
enum terakan_hw_config_draw_cb_color_control_rop3 : uint8_t {
   TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_CLEAR = 0x00,
   TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_NOR = 0x11,
   TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_AND_INVERTED = 0x22,
   TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_COPY_INVERTED = 0x33,
   TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_AND_REVERSE = 0x44,
   TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_INVERT = 0x55,
   TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_XOR = 0x66,
   TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_NAND = 0x77,
   TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_AND = 0x88,
   TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_EQUIVALENT = 0x99,
   TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_NO_OP = 0xAA,
   TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_OR_INVERTED = 0xBB,
   TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_COPY = 0xCC,
   TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_OR_REVERSE = 0xDD,
   TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_OR = 0xEE,
   TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_SET = 0xFF,
};

/* Safe defaults for registers, based on the values expected when the respective state is
 * zero-initialized in Vulkan structures or commands, or when the corresponding Vulkan optional
 * features or pipeline stages are disabled, or where there's no such default for Vulkan, on the
 * default OpenGL state, falling back to the Direct3D 11 default if not defined.
 */

/* vkCmdDraw* firstVertex or vertexOffset = 0 */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_VGT_INDEX_OFFSET 0

/* vkCmdBindIndexBuffer2KHR size = 0 */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_VGT_DMA_INDEX_BUFFER                                        \
   ((struct terakan_hw_config_draw_vgt_dma_index_buffer){})

/* vkCmdBindIndexBuffer indexType = VK_INDEX_TYPE_UINT16 */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_VGT_DMA_INDEX_TYPE                                          \
   TERAKAN_HW_CONFIG_DRAW_VGT_DMA_INDEX_TYPE_16_HOST_ENDIAN
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_VGT_MULTI_PRIM_IB_RESET_INDEX                               \
   (TERAKAN_HW_CONFIG_DRAW_DEFAULT_VGT_DMA_INDEX_TYPE & VGT_INDEX_32 ? 0xFFFFFFFFu : 0xFFFFu)

/* VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
 * VK_SHADER_STAGE_GEOMETRY_BIT not bound
 * No transform feedback, no VkPipelineRasterizationLineStateCreateInfo
 */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_IA_MULTI_VGT_PARAM S_028AA8_PRIMGROUP_SIZE(128 - 1)

/* VkPipelineInputAssemblyStateCreateInfo topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST
 * VK_SHADER_STAGE_GEOMETRY_BIT not bound
 */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_VGT_SHADER_STAGES_EN S_028B54_VS_EN(V_028B54_VS_STAGE_REAL)

/* Both are ignored while the LS and HS stages are disabled, so the reset value only needs to be
 * something defined rather than a meaningful tessellator setup.
 */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_VGT_LS_HS_CONFIG 0
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_VGT_TF_PARAM 0

/* No VkPipelineViewportDepthClipControlCreateInfoEXT
 * VkPipelineRasterizationStateCreateInfo:
 * - depthClampEnable = VK_FALSE
 * - rasterizerDiscardEnable = VK_FALSE
 * No VkPipelineRasterizationDepthClipStateCreateInfoEXT
 */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_CL_CLIP_CNTL                                             \
   (S_028810_DX_CLIP_SPACE_DEF(true) | S_028810_DX_LINEAR_ATTR_CLIP_ENA(true))

/* VkPipelineRasterizationStateCreateInfo:
 * - polygonMode = VK_POLYGON_MODE_FILL
 * - cullMode = VK_CULL_MODE_NONE
 * - frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
 * - depthBiasEnable = VK_FALSE
 * No VkPipelineRasterizationProvokingVertexStateCreateInfoEXT
 */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SU_SC_MODE_CNTL                                          \
   (S_028814_POLY_MODE(V_028814_X_DISABLE_POLY_MODE) |                                             \
    S_028814_POLYMODE_FRONT_PTYPE(V_028814_X_DRAW_TRIANGLES) |                                     \
    S_028814_POLYMODE_BACK_PTYPE(V_028814_X_DRAW_TRIANGLES))

#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_CL_VTE_CNTL TERAKAN_HW_CONFIG_DRAW_PA_CL_VTE_CNTL_3D

/* VkPipelineRasterizationLineStateCreateInfo:
 * - lineStippleFactor = min (1)
 * - lineStipplePattern = 0
 * VkPipelineInputAssemblyStateCreateInfo topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
 * VK_SHADER_STAGE_GEOMETRY_BIT not bound (enabling resetting at each primitive only for line lists)
 */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SC_LINE_STIPPLE S_028A0C_AUTO_RESET_CNTL(2)

/* VkPipelineRasterizationStateCreateInfo lineWidth = 1.0. The field counts eighths of a pixel. */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SU_LINE_CNTL S_028A08_WIDTH(1u << 3)

/* VkPipelineMultisampleStateCreateInfo rasterizationSamples = min (VK_SAMPLE_COUNT_1_BIT)
 * No VkPipelineRasterizationLineStateCreateInfo, no VkPipelineSampleLocationsStateCreateInfoEXT
 */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SC_MODE_CNTL_0 S_028A48_VPORT_SCISSOR_ENABLE(true)

/* VkPipelineMultisampleStateCreateInfo rasterizationSamples = min (VK_SAMPLE_COUNT_1_BIT) */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SC_MODE_CNTL_1                                           \
   TERAKAN_HW_CONFIG_DRAW_PA_SC_MODE_CNTL_1_CONSTANT

/* No VkDepthBiasRepresentationInfoEXT, no depth attachment (undefined case, but treating as 32-bit
 * floating-point to match the precision of gl_FragCoord.z in fragment shaders)
 */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SU_POLY_OFFSET_DB_FMT_CNTL                               \
   (S_028B78_POLY_OFFSET_NEG_NUM_DB_BITS(-23) | S_028B78_POLY_OFFSET_DB_IS_FLOAT_FMT(true))

/* VkPipelineRasterizationStateCreateInfo:
 * - depthBiasConstantFactor = 0.0f
 * - depthBiasClamp = 0.0f
 * - depthBiasSlopeFactor = 0.0f
 */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SU_POLY_OFFSET                                           \
   ((struct terakan_hw_config_draw_pa_su_poly_offset){})

/* No VkPipelineRasterizationLineStateCreateInfo */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SC_LINE_CNTL S_028BDC_DX10_DIAMOND_TEST_ENA(1)

/* VkPipelineMultisampleStateCreateInfo rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
 * No VkPipelineSampleLocationsStateCreateInfoEXT
 * Same on R8xx and R9xx (bit masking is not needed for R8xx)
 */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SC_AA_CONFIG 0

/* VkPipelineMultisampleStateCreateInfo:
 * - rasterizationSamples = min (VK_SAMPLE_COUNT_1_BIT)
 * - pSampleMask = NULL
 */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_PA_SC_AA_MASK 0b1

/* No active VK_QUERY_TYPE_OCCLUSION */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_COUNT_CONTROL S_028004_ZPASS_INCREMENT_DISABLE(true)

#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_RENDER_CONTROL 0

/* VK_EXT_depth_range_unrestricted not enabled */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_RENDER_OVERRIDE 0

#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_RENDER_OVERRIDE2 0

/* VkStencilOpState:
 * - compareMask = 0
 * - writeMask = 0
 * - reference = 0
 */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_STENCILREFMASK 0

/* VkPipelineDepthStencilStateCreateInfo:
 * - depthTestEnable = VK_FALSE
 * - depthWriteEnable = VK_FALSE
 * - depthCompareOp = VK_COMPARE_OP_NEVER
 * - stencilTestEnable = VK_FALSE
 * VkStencilOpState:
 * - failOp = VK_STENCIL_OP_KEEP
 * - passOp = VK_STENCIL_OP_KEEP
 * - depthFailOp = VK_STENCIL_OP_KEEP
 * - compareOp = VK_COMPARE_OP_NEVER
 */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_DEPTH_CONTROL 0

/* VkPipelineMultisampleStateCreateInfo rasterizationSamples = min (VK_SAMPLE_COUNT_1_BIT) */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_EQAA TERAKAN_HW_CONFIG_DRAW_DB_EQAA_CONSTANT

#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_SHADER_CONTROL TERAKAN_SHADER_DB_SHADER_CONTROL_IDENTITY

/* VkPipelineMultisampleStateCreateInfo alphaToCoverageEnable = VK_FALSE */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_ALPHA_TO_MASK 0

/* VkPipelineColorBlendAttachmentState blendEnable = VK_FALSE */
#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_CB_BLEND_CONTROL 0

#define TERAKAN_HW_CONFIG_DRAW_DEFAULT_CB_COLOR_CONTROL                                            \
   S_028808_ROP3(TERAKAN_HW_CONFIG_DRAW_CB_COLOR_CONTROL_ROP3_COPY)

enum terakan_hw_config_draw_entry {
   /* Generally ordered roughly by the location of the hardware unit in the pipeline, and within
    * each unit, by register address.
    */

   TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_INDEX_OFFSET,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_MULTI_PRIM_IB_RESET_INDEX,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_MULTI_PRIM_IB_RESET_EN,
   /* Separate from `VGT_DMA_INDEX_BUFFER` because it's needed for `DRAW_INDEX_IMMED` too. */
   TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_DMA_INDEX_TYPE,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_IA_MULTI_VGT_PARAM,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_SHADER_STAGES_EN,
   /* Tessellator configuration. Only consulted by the hardware while `VGT_SHADER_STAGES_EN`
    * enables the LS and HS stages.
    */
   TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_LS_HS_CONFIG,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_TF_PARAM,

   TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_PGM_FS,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_PGM_VS,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_PGM_PS,
   /* Vertex pipeline stages preceding the hardware VS. Unlike the VS, these have no parameter
    * export or clip/cull configuration of their own, so only the program address and the resource
    * registers are emitted for them.
    */
   TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_PGM_LS,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_PGM_HS,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_PGM_ES,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_PGM_GS,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_RING_ITEMSIZE,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_BOOL_CONST_VSES,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_BOOL_CONST_LS,

   TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_VPORT_SCISSOR,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_VPORT_ZMIN_ZMAX,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_CL_VPORT_SCALE_OFFSET,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_CL_CLIP_CNTL,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SU_SC_MODE_CNTL,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_CL_VTE_CNTL,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SU_LINE_CNTL,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_LINE_STIPPLE,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_MODE_CNTL_0,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_MODE_CNTL_1,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SU_POLY_OFFSET_DB_FMT_CNTL,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SU_POLY_OFFSET,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_LINE_CNTL,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_AA_CONFIG_SAMPLE_LOCS,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_CL_GB,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_AA_MASK,

   TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_RENDER_CONTROL,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_COUNT_CONTROL,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_RENDER_OVERRIDE,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_RENDER_OVERRIDE2,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_DEPTH_STENCIL_BUFFER,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_STENCILREFMASK,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_DEPTH_CONTROL,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_EQAA,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_SHADER_CONTROL,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_ALPHA_TO_MASK,

   TERAKAN_HW_CONFIG_DRAW_ENTRY_CB_TARGET_MASK,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_CB_BLEND_CONSTANTS,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_CB_BLEND_CONTROL,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_CB_COLOR_CONTROL,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_CB_IMMED,
   TERAKAN_HW_CONFIG_DRAW_ENTRY_CB_COLOR,

   /* The VGT DMA index buffer must be set after all other configuration is emitted, because in DRM
    * Radeon 2.50.0 and 2.51.0, an `INDEX_BASE` packet spuriously invokes binding validation.
    */
   TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_DMA_INDEX_BUFFER,

   TERAKAN_HW_CONFIG_DRAW_ENTRY_COUNT,
};

struct terakan_hw_config_draw {
   /* Whether each entry has been modified and needs to be emitted before the next draw. */
   BITSET_DECLARE(entries_modified_, TERAKAN_HW_CONFIG_DRAW_ENTRY_COUNT);

   /* Vertex grouping and tessellation. */

   uint32_t vgt_index_offset_;

   uint32_t vgt_multi_prim_ib_reset_index_;

   bool vgt_multi_prim_ib_reset_en_;

   struct terakan_hw_config_draw_vgt_dma_index_buffer vgt_dma_index_buffer_;

   uint32_t vgt_dma_index_type_;

   uint32_t ia_multi_vgt_param_;

   uint32_t vgt_shader_stages_en_;

   uint32_t vgt_ls_hs_config_;
   uint32_t vgt_tf_param_;

   /* Sequencer. */

   struct {
      struct terakan_bo const * bo;
      uint32_t va_shr8;
   } sq_pgm_fs_;

   struct terakan_shader_static const * sq_pgm_vs_;

   struct terakan_shader_static const * sq_pgm_ps_;

   struct terakan_shader_static const * sq_pgm_ls_;
   struct terakan_shader_static const * sq_pgm_hs_;
   struct terakan_shader_static const * sq_pgm_es_;
   struct terakan_shader_static const * sq_pgm_gs_;

   struct {
      uint32_t modified_bits;
      uint16_t itemsize_dwords[TERAKAN_SHADER_RING_INDEX_COUNT];
   } sq_ring_itemsize_;

   uint32_t sq_bool_const_vses_;

   uint32_t sq_bool_const_ls_;

   /* Primitive assembly and scan conversion. */

   struct {
      uint8_t needed_count;
      uint16_t modified_bits;
      uint32_t tl_br[TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT][2];
   } pa_sc_vport_scissor_;

   struct {
      uint8_t needed_count;
      uint16_t modified_bits;
      float zmin_zmax[TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT][2];
   } pa_sc_vport_zmin_zmax_;

   struct {
      uint8_t needed_count;
      uint16_t modified_bits;
      float scale_offset[TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT][3][2];
   } pa_cl_vport_scale_offset_;

   uint32_t pa_cl_clip_cntl_;

   uint32_t pa_su_sc_mode_cntl_;

   uint32_t pa_cl_vte_cntl_;

   uint32_t pa_su_line_cntl_;
   uint32_t pa_sc_line_stipple_;

   uint32_t pa_sc_mode_cntl_0_;

   uint32_t pa_sc_mode_cntl_1_;

   uint32_t pa_su_poly_offset_db_fmt_cntl_;

   struct terakan_hw_config_draw_pa_su_poly_offset pa_su_poly_offset_;

   uint32_t pa_sc_line_cntl_;

   struct {
      /* Fields not supported by the architecture are masked out during emission. */
      uint32_t config;
      /* [sample][2 * y + x]
       * Emission may be skipped for samples beyond `MSAA_NUM_SAMPLES` in the `config`, so the
       * sample locations must be fully emitted again if the sample count has been changed,
       * regardless of whether they've been set to a different value since the last emission.
       */
      uint8_t sample_locs[16][4];
   } pa_sc_aa_config_sample_locs_;

   struct {
      /* [Vertical, horizontal][clip, discard] (same as the register sequence). */
      float vert_horz_clip_disc_adj[2][2];
   } pa_cl_gb_;

   /* Replicated for all pixels in a quad. */
   uint16_t pa_sc_aa_mask_;

   /* Depth / stencil buffer. */

   uint32_t db_render_control_;

   uint32_t db_count_control_;

   uint32_t db_render_override_;

   uint32_t db_render_override2_;

   struct {
      struct terakan_bo const * bo;
      struct terakan_depth_stencil_descriptor descriptor;
   } db_depth_stencil_buffer_;

   struct {
      uint32_t front;
      uint32_t back;
   } db_stencilrefmask_;

   uint32_t db_depth_control_;

   uint32_t db_eqaa_;

   uint32_t db_shader_control_;

   uint32_t db_alpha_to_mask_;

   /* Color buffer. */

   uint32_t cb_target_mask_;

   float cb_blend_constants_[4];

   struct {
      uint8_t modified_bits;

      uint32_t blend_control[TERAKAN_COLOR_HW_RTV_COUNT];
   } cb_blend_control_;

   uint32_t cb_color_control_;

   struct {
      uint16_t modified_bits;

      /* 3 bits with a value in the [0, 4] interval for each UAV. */
      uint64_t uav_bytes_per_element_log2;
   } cb_immed_;

   struct {
      uint16_t modified_bits;

      /* The BO must not be NULL if the format in the descriptor is not `INVALID`.
       * This is an explicit deviation from the usual pattern in Terakan to indicate that setting
       * the BO to NULL is not a shortcut for having an unbound color target in
       * `terakan_hw_config_draw`, because the `INFO` register in the descriptor still must be
       * configured, specifically `SOURCE_FORMAT` of the target 1 for dual-source blending.
       */
      struct terakan_bo const * bo[TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT];
      /* If the format is `INVALID`, this color target is unbound (the DRM Radeon driver assumes the
       * same), and only `INFO` is emitted, other fields are ignored (only initialized for defined
       * comparison behavior).
       * This is normalized to either an RTV or a UAV hardware descriptor when changing it, to avoid
       * re-emission if only irrelevant fields were changed. However, `NUM_SAMPLES` is needed by the
       * emission logic internally regardless of the architecture generation, so it's always stored,
       * and so is `NUM_FRAGMENTS` to avoid depending on the architecture generation in the setter.
       */
      struct terakan_color_descriptor color[TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT];
      struct terakan_color_meta_descriptor meta[TERAKAN_COLOR_HW_RTV_COUNT];
   } cb_color_;
};

/* For floating-point modification checks, `memcmp` must be used instead of `!=`, to consider +0 and
 * -0 different values, and to prevent NaN from making an entry considered modified again and again.
 */
static inline bool
terakan_hw_config_draw_float_equal(float const a, float const b)
{
   return memcmp(&a, &b, sizeof(a)) == 0;
}

static inline void
terakan_hw_config_draw_set_single_register_(struct terakan_hw_config_draw * const config,
                                            enum terakan_hw_config_draw_entry const entry_index,
                                            uint32_t * const entry_register, uint32_t const value)
{
   if (*entry_register == value) {
      return;
   }
   *entry_register = value;
   BITSET_SET(config->entries_modified_, entry_index);
}

static inline void
terakan_hw_config_draw_set_vgt_index_offset(struct terakan_hw_config_draw * const config,
                                            uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_INDEX_OFFSET, &config->vgt_index_offset_, value);
}

/* If the primitive reset index is wider than the index type, it doesn't cause resets. */
static inline void
terakan_hw_config_draw_set_vgt_multi_prim_ib_reset_index(
   struct terakan_hw_config_draw * const config, uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_MULTI_PRIM_IB_RESET_INDEX,
      &config->vgt_multi_prim_ib_reset_index_, value);
}

static inline void
terakan_hw_config_draw_set_vgt_multi_prim_ib_reset_en(struct terakan_hw_config_draw * const config,
                                                      bool const value)
{
   if (config->vgt_multi_prim_ib_reset_en_ == value) {
      return;
   }
   config->vgt_multi_prim_ib_reset_en_ = value;
   BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_MULTI_PRIM_IB_RESET_EN);
}

/* The passed `vgt_dma_index_type` may be ignored if the implementation determines that indices
 * aren't going to be read from this buffer.
 * #MemoryIntegrity is handled internally.
 */
void terakan_hw_config_draw_set_vgt_dma_index_buffer(
   struct terakan_hw_config_draw * config,
   struct terakan_hw_config_draw_vgt_dma_index_buffer index_buffer, uint32_t index_type);

/* Must be used only for `DRAW_INDEX_IMMED`, with `terakan_hw_config_draw_set_vgt_dma_index_buffer`
 * called afterwards before drawing with a DMA index buffer next time, to make sure it's not
 * possible to switch from 16-bit to 32-bit indices without the #MemoryIntegrity checks.
 */
static inline void
terakan_hw_config_draw_set_vgt_dma_index_type_for_immediate(
   struct terakan_hw_config_draw * const config, uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_DMA_INDEX_TYPE, &config->vgt_dma_index_type_, value);
}

static inline void
terakan_hw_config_draw_set_ia_multi_vgt_param(struct terakan_hw_config_draw * const config,
                                              uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_IA_MULTI_VGT_PARAM, &config->ia_multi_vgt_param_, value);
}

static inline void
terakan_hw_config_draw_set_vgt_shader_stages_en(struct terakan_hw_config_draw * const config,
                                                uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(config,
                                               TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_SHADER_STAGES_EN,
                                               &config->vgt_shader_stages_en_, value);
}

static inline void
terakan_hw_config_draw_set_vgt_ls_hs_config(struct terakan_hw_config_draw * const config,
                                            uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_LS_HS_CONFIG, &config->vgt_ls_hs_config_, value);
}

static inline void
terakan_hw_config_draw_set_vgt_tf_param(struct terakan_hw_config_draw * const config,
                                        uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_VGT_TF_PARAM, &config->vgt_tf_param_, value);
}

/* Binding a NULL BO causes a return-only shader to be used. */
static inline void
terakan_hw_config_draw_set_sq_pgm_fs(struct terakan_hw_config_draw * const config,
                                     struct terakan_bo const * const bo, uint32_t const va_shr8)
{
   if (config->sq_pgm_fs_.bo == bo && (bo == NULL || config->sq_pgm_fs_.va_shr8 == va_shr8)) {
      return;
   }
   config->sq_pgm_fs_.bo = bo;
   config->sq_pgm_fs_.va_shr8 = va_shr8;
   BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_PGM_FS);
}

static inline void
terakan_hw_config_draw_set_sq_pgm_(struct terakan_hw_config_draw * const config,
                                   enum terakan_hw_config_draw_entry const entry_index,
                                   struct terakan_shader_static const ** const entry_shader,
                                   struct terakan_shader_static const * const value)
{
   if (*entry_shader == value) {
      return;
   }
   *entry_shader = value;
   BITSET_SET(config->entries_modified_, entry_index);
}

/* Binding a NULL shader causes `TERAKAN_META_SHADER_DUMMY_NAN_VS` to be used. */
static inline void
terakan_hw_config_draw_set_sq_pgm_vs(struct terakan_hw_config_draw * const config,
                                     struct terakan_shader_static const * const value)
{
   terakan_hw_config_draw_set_sq_pgm_(config, TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_PGM_VS,
                                      &config->sq_pgm_vs_, value);
}

/* Binding a NULL shader causes `TERAKAN_META_SHADER_DUMMY_OPAQUE_PS` to be used. */
static inline void
terakan_hw_config_draw_set_sq_pgm_ps(struct terakan_hw_config_draw * const config,
                                     struct terakan_shader_static const * const value)
{
   terakan_hw_config_draw_set_sq_pgm_(config, TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_PGM_PS,
                                      &config->sq_pgm_ps_, value);
}

/* The stages preceding the hardware VS are only run when `VGT_SHADER_STAGES_EN` enables them, so
 * unlike the VS and the PS, a NULL shader is not substituted with a dummy: the registers simply
 * keep whatever was last written while the stage is disabled.
 */
static inline void
terakan_hw_config_draw_set_sq_pgm_ls(struct terakan_hw_config_draw * const config,
                                     struct terakan_shader_static const * const value)
{
   terakan_hw_config_draw_set_sq_pgm_(config, TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_PGM_LS,
                                      &config->sq_pgm_ls_, value);
}

static inline void
terakan_hw_config_draw_set_sq_pgm_hs(struct terakan_hw_config_draw * const config,
                                     struct terakan_shader_static const * const value)
{
   terakan_hw_config_draw_set_sq_pgm_(config, TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_PGM_HS,
                                      &config->sq_pgm_hs_, value);
}

static inline void
terakan_hw_config_draw_set_sq_pgm_es(struct terakan_hw_config_draw * const config,
                                     struct terakan_shader_static const * const value)
{
   terakan_hw_config_draw_set_sq_pgm_(config, TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_PGM_ES,
                                      &config->sq_pgm_es_, value);
}

static inline void
terakan_hw_config_draw_set_sq_pgm_gs(struct terakan_hw_config_draw * const config,
                                     struct terakan_shader_static const * const value)
{
   terakan_hw_config_draw_set_sq_pgm_(config, TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_PGM_GS,
                                      &config->sq_pgm_gs_, value);
}

static inline void
terakan_hw_config_draw_set_sq_ring_itemsize_dwords(struct terakan_hw_config_draw * const config,
                                                   enum terakan_shader_ring_index const ring_index,
                                                   uint16_t const value)
{
   assert(ring_index < TERAKAN_SHADER_RING_INDEX_COUNT);
   uint16_t * const ring_itemsize = &config->sq_ring_itemsize_.itemsize_dwords[ring_index];
   if (*ring_itemsize == value) {
      return;
   }
   *ring_itemsize = value;
   config->sq_ring_itemsize_.modified_bits |= BITFIELD_BIT(ring_index);
   BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_RING_ITEMSIZE);
}

static inline void
terakan_hw_config_draw_set_sq_bool_const_vses(struct terakan_hw_config_draw * const config,
                                              uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_BOOL_CONST_VSES, &config->sq_bool_const_vses_, value);
}

static inline void
terakan_hw_config_draw_set_sq_bool_const_ls(struct terakan_hw_config_draw * const config,
                                            uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_BOOL_CONST_LS, &config->sq_bool_const_ls_, value);
}

static inline void
terakan_hw_config_draw_set_pa_vport_needed_count_(
   struct terakan_hw_config_draw * const config,
   enum terakan_hw_config_draw_entry const entry_index, uint8_t * const needed_count_field,
   unsigned const new_needed_count, uint16_t const modified_bits)
{
   assert(new_needed_count <= TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
   *needed_count_field = new_needed_count;
   if (modified_bits & BITFIELD_MASK(new_needed_count)) {
      BITSET_SET(config->entries_modified_, entry_index);
   } else {
      BITSET_CLEAR(config->entries_modified_, entry_index);
   }
}

static inline void
terakan_hw_config_draw_set_pa_sc_vport_scissor_needed_count(
   struct terakan_hw_config_draw * const config, unsigned const value)
{
   terakan_hw_config_draw_set_pa_vport_needed_count_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_VPORT_SCISSOR,
      &config->pa_sc_vport_scissor_.needed_count, value,
      config->pa_sc_vport_scissor_.modified_bits);
}

static inline void
terakan_hw_config_draw_set_pa_sc_vport_scissor(struct terakan_hw_config_draw * const config,
                                               unsigned const vport_index, uint32_t const tl,
                                               uint32_t const br)
{
   assert(vport_index < TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
   uint32_t * const vport_scissor = config->pa_sc_vport_scissor_.tl_br[vport_index];
   if (vport_scissor[0] == tl && vport_scissor[1] == br) {
      return;
   }
   vport_scissor[0] = tl;
   vport_scissor[1] = br;
   config->pa_sc_vport_scissor_.modified_bits |= BITFIELD_BIT(vport_index);
   if (vport_index < config->pa_sc_vport_scissor_.needed_count) {
      BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_VPORT_SCISSOR);
   }
}

static inline void
terakan_hw_config_draw_set_pa_sc_vport_zmin_zmax_needed_count(
   struct terakan_hw_config_draw * const config, unsigned const value)
{
   terakan_hw_config_draw_set_pa_vport_needed_count_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_VPORT_ZMIN_ZMAX,
      &config->pa_sc_vport_zmin_zmax_.needed_count, value,
      config->pa_sc_vport_zmin_zmax_.modified_bits);
}

static inline void
terakan_hw_config_draw_set_pa_sc_vport_zmin_zmax(struct terakan_hw_config_draw * const config,
                                                 unsigned const vport_index, float const zmin,
                                                 float const zmax)
{
   assert(vport_index < TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
   float * const vport_zmin_zmax = config->pa_sc_vport_zmin_zmax_.zmin_zmax[vport_index];
   if (terakan_hw_config_draw_float_equal(vport_zmin_zmax[0], zmin) &&
       terakan_hw_config_draw_float_equal(vport_zmin_zmax[1], zmax)) {
      return;
   }
   vport_zmin_zmax[0] = zmin;
   vport_zmin_zmax[1] = zmax;
   config->pa_sc_vport_zmin_zmax_.modified_bits |= BITFIELD_BIT(vport_index);
   if (vport_index < config->pa_sc_vport_zmin_zmax_.needed_count) {
      BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_VPORT_ZMIN_ZMAX);
   }
}

static inline void
terakan_hw_config_draw_set_pa_cl_vport_scale_offset_needed_count(
   struct terakan_hw_config_draw * const config, unsigned const value)
{
   terakan_hw_config_draw_set_pa_vport_needed_count_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_CL_VPORT_SCALE_OFFSET,
      &config->pa_cl_vport_scale_offset_.needed_count, value,
      config->pa_cl_vport_scale_offset_.modified_bits);
}

/* `xy_scale_offset` is [x, y][scale, offset]. */
static inline void
terakan_hw_config_draw_set_pa_cl_vport_scale_offset(struct terakan_hw_config_draw * const config,
                                                    unsigned const vport_index,
                                                    float const * const xy_scale_offset,
                                                    float const z_scale, float const z_offset)
{
   assert(vport_index < TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT);
   float * const vport_scale_offset =
      config->pa_cl_vport_scale_offset_.scale_offset[vport_index][0];
   if (memcmp(vport_scale_offset, xy_scale_offset, sizeof(float) * 2 * 2) == 0 &&
       terakan_hw_config_draw_float_equal(vport_scale_offset[2 * 2], z_scale) &&
       terakan_hw_config_draw_float_equal(vport_scale_offset[2 * 2 + 1], z_offset)) {
      return;
   }
   memcpy(vport_scale_offset, xy_scale_offset, sizeof(float) * 2 * 2);
   vport_scale_offset[2 * 2] = z_scale;
   vport_scale_offset[2 * 2 + 1] = z_offset;
   config->pa_cl_vport_scale_offset_.modified_bits |= BITFIELD_BIT(vport_index);
   if (vport_index < config->pa_cl_vport_scale_offset_.needed_count) {
      BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_CL_VPORT_SCALE_OFFSET);
   }
}

static inline void
terakan_hw_config_draw_set_pa_cl_clip_cntl(struct terakan_hw_config_draw * const config,
                                           uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(config, TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_CL_CLIP_CNTL,
                                               &config->pa_cl_clip_cntl_, value);
}

static inline void
terakan_hw_config_draw_set_pa_su_sc_mode_cntl(struct terakan_hw_config_draw * const config,
                                              uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SU_SC_MODE_CNTL, &config->pa_su_sc_mode_cntl_, value);
}

static inline void
terakan_hw_config_draw_set_pa_cl_vte_cntl(struct terakan_hw_config_draw * const config,
                                          uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(config, TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_CL_VTE_CNTL,
                                               &config->pa_cl_vte_cntl_, value);
}

static inline void
terakan_hw_config_draw_set_pa_su_line_cntl(struct terakan_hw_config_draw * const config,
                                           uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SU_LINE_CNTL, &config->pa_su_line_cntl_, value);
}

static inline void
terakan_hw_config_draw_set_pa_sc_line_stipple(struct terakan_hw_config_draw * const config,
                                              uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_LINE_STIPPLE, &config->pa_sc_line_stipple_, value);
}

static inline void
terakan_hw_config_draw_set_pa_sc_mode_cntl_0(struct terakan_hw_config_draw * const config,
                                             uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_MODE_CNTL_0, &config->pa_sc_mode_cntl_0_, value);
}

static inline void
terakan_hw_config_draw_set_pa_sc_mode_cntl_1(struct terakan_hw_config_draw * const config,
                                             uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_MODE_CNTL_1, &config->pa_sc_mode_cntl_1_, value);
}

static inline void
terakan_hw_config_draw_set_pa_su_poly_offset_db_fmt_cntl(
   struct terakan_hw_config_draw * const config, uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SU_POLY_OFFSET_DB_FMT_CNTL,
      &config->pa_su_poly_offset_db_fmt_cntl_, value);
}

static inline void
terakan_hw_config_draw_set_pa_su_poly_offset(
   struct terakan_hw_config_draw * const config,
   struct terakan_hw_config_draw_pa_su_poly_offset const value)
{
   /* Depth bias clamp is optional in Vulkan and is unlikely to change frequently. */
   if (terakan_hw_config_draw_float_equal(config->pa_su_poly_offset_.slope_scale_per_16th_subpixel,
                                          value.slope_scale_per_16th_subpixel) &&
       terakan_hw_config_draw_float_equal(config->pa_su_poly_offset_.constant_offset,
                                          value.constant_offset) &&
       terakan_hw_config_draw_float_equal(config->pa_su_poly_offset_.clamp, value.clamp)) {
      return;
   }
   config->pa_su_poly_offset_ = value;
   BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SU_POLY_OFFSET);
}

static inline void
terakan_hw_config_draw_set_pa_sc_line_cntl(struct terakan_hw_config_draw * const config,
                                           uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(config, TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_LINE_CNTL,
                                               &config->pa_sc_line_cntl_, value);
}

/* `sample_locs` is [1 << G_028BE0_MSAA_NUM_SAMPLES(pa_sc_aa_config)][4]. */
static inline void
terakan_hw_config_draw_set_pa_sc_aa_config_sample_locs(struct terakan_hw_config_draw * const config,
                                                       uint32_t const pa_sc_aa_config,
                                                       uint8_t const * const sample_locs)
{
   unsigned const sample_count = 1u << G_028BE0_MSAA_NUM_SAMPLES(pa_sc_aa_config);
   assert(sample_count <= 16);
   unsigned const sample_locs_bytes = (unsigned)sizeof(uint8_t) * 4 * sample_count;
   if (config->pa_sc_aa_config_sample_locs_.config == pa_sc_aa_config &&
       memcmp(config->pa_sc_aa_config_sample_locs_.sample_locs, sample_locs, sample_locs_bytes) ==
          0) {
      return;
   }
   config->pa_sc_aa_config_sample_locs_.config = pa_sc_aa_config;
   memcpy(config->pa_sc_aa_config_sample_locs_.sample_locs, sample_locs, sample_locs_bytes);
   BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_AA_CONFIG_SAMPLE_LOCS);
}

/* Replicated for all pixels in a quad. */
static inline void
terakan_hw_config_draw_set_pa_sc_aa_mask(struct terakan_hw_config_draw * const config,
                                         uint16_t const value)
{
   if (config->pa_sc_aa_mask_ == value) {
      return;
   }
   config->pa_sc_aa_mask_ = value;
   BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_SC_AA_MASK);
}

static inline void
terakan_hw_config_draw_set_pa_cl_gb_clip_adj(struct terakan_hw_config_draw * const config,
                                             float const vertical, float const horizontal)
{
   if (terakan_hw_config_draw_float_equal(config->pa_cl_gb_.vert_horz_clip_disc_adj[0][0],
                                          vertical) &&
       terakan_hw_config_draw_float_equal(config->pa_cl_gb_.vert_horz_clip_disc_adj[1][0],
                                          horizontal)) {
      return;
   }
   config->pa_cl_gb_.vert_horz_clip_disc_adj[0][0] = vertical;
   config->pa_cl_gb_.vert_horz_clip_disc_adj[1][0] = horizontal;
   BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_CL_GB);
}

static inline void
terakan_hw_config_draw_set_pa_cl_gb_disc_adj(struct terakan_hw_config_draw * const config,
                                             float const vertical, float const horizontal)
{
   if (terakan_hw_config_draw_float_equal(config->pa_cl_gb_.vert_horz_clip_disc_adj[0][1],
                                          vertical) &&
       terakan_hw_config_draw_float_equal(config->pa_cl_gb_.vert_horz_clip_disc_adj[1][1],
                                          horizontal)) {
      return;
   }
   config->pa_cl_gb_.vert_horz_clip_disc_adj[0][1] = vertical;
   config->pa_cl_gb_.vert_horz_clip_disc_adj[1][1] = horizontal;
   BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_DRAW_ENTRY_PA_CL_GB);
}

static inline void
terakan_hw_config_draw_set_db_render_control(struct terakan_hw_config_draw * const config,
                                             uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_RENDER_CONTROL, &config->db_render_control_, value);
}

static inline void
terakan_hw_config_draw_set_db_count_control(struct terakan_hw_config_draw * const config,
                                            uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_COUNT_CONTROL, &config->db_count_control_, value);
}

static inline void
terakan_hw_config_draw_set_db_render_override(struct terakan_hw_config_draw * const config,
                                              uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_RENDER_OVERRIDE, &config->db_render_override_, value);
}

static inline void
terakan_hw_config_draw_set_db_render_override2(struct terakan_hw_config_draw * const config,
                                               uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(config,
                                               TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_RENDER_OVERRIDE2,
                                               &config->db_render_override2_, value);
}

/* If `bo` is NULL, `descriptor` is ignored, and the depth / stencil buffer is unbound.
 *
 * `NUM_SAMPLES` must be provided regardless of the architecture generation for the BO priority.
 * It will be zeroed during emission if the hardware doesn't need it.
 *
 * #MemoryIntegrity is expected to be ensured before calling.
 */
void terakan_hw_config_draw_set_db_depth_stencil_buffer(
   struct terakan_hw_config_draw * config, struct terakan_bo const * bo,
   struct terakan_depth_stencil_descriptor const * descriptor);

static inline void
terakan_hw_config_draw_set_db_stencilrefmask(struct terakan_hw_config_draw * const config,
                                             bool const back, uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_STENCILREFMASK,
      back ? &config->db_stencilrefmask_.back : &config->db_stencilrefmask_.front, value);
}

static inline void
terakan_hw_config_draw_set_db_depth_control(struct terakan_hw_config_draw * const config,
                                            uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_DEPTH_CONTROL, &config->db_depth_control_, value);
}

static inline void
terakan_hw_config_draw_set_db_eqaa(struct terakan_hw_config_draw * const config,
                                   uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(config, TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_EQAA,
                                               &config->db_eqaa_, value);
}

static inline void
terakan_hw_config_draw_set_db_shader_control(struct terakan_hw_config_draw * const config,
                                             uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_SHADER_CONTROL, &config->db_shader_control_, value);
}

static inline void
terakan_hw_config_draw_set_db_alpha_to_mask(struct terakan_hw_config_draw * const config,
                                            uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_DB_ALPHA_TO_MASK, &config->db_alpha_to_mask_, value);
}

static inline void
terakan_hw_config_draw_set_cb_target_mask(struct terakan_hw_config_draw * const config,
                                          uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(config, TERAKAN_HW_CONFIG_DRAW_ENTRY_CB_TARGET_MASK,
                                               &config->cb_target_mask_, value);
}

static inline void
terakan_hw_config_draw_set_cb_blend_constants_rgb(struct terakan_hw_config_draw * const config,
                                                  float const value[3])
{
   if (memcmp(config->cb_blend_constants_, value, sizeof(float) * 3) == 0) {
      return;
   }
   memcpy(config->cb_blend_constants_, value, sizeof(float) * 3);
   BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_DRAW_ENTRY_CB_BLEND_CONSTANTS);
}

static inline void
terakan_hw_config_draw_set_cb_blend_constants_alpha(struct terakan_hw_config_draw * const config,
                                                    float const value)
{
   if (terakan_hw_config_draw_float_equal(config->cb_blend_constants_[3], value)) {
      return;
   }
   config->cb_blend_constants_[3] = value;
   BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_DRAW_ENTRY_CB_BLEND_CONSTANTS);
}

static inline void
terakan_hw_config_draw_set_cb_blend_control(struct terakan_hw_config_draw * const config,
                                            unsigned const color_index, uint32_t const value)
{
   assert(color_index < TERAKAN_COLOR_HW_RTV_COUNT);
   if (config->cb_blend_control_.blend_control[color_index] == value) {
      return;
   }
   config->cb_blend_control_.blend_control[color_index] = value;
   config->cb_blend_control_.modified_bits |= BITFIELD_BIT(color_index);
   BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_DRAW_ENTRY_CB_BLEND_CONTROL);
}

static inline void
terakan_hw_config_draw_set_cb_color_control(struct terakan_hw_config_draw * const config,
                                            uint32_t const value)
{
   terakan_hw_config_draw_set_single_register_(
      config, TERAKAN_HW_CONFIG_DRAW_ENTRY_CB_COLOR_CONTROL, &config->cb_color_control_, value);
}

static inline void
terakan_hw_config_draw_set_cb_immed(struct terakan_hw_config_draw * const config,
                                    unsigned const uav_index,
                                    unsigned const uav_bytes_per_element_log2)
{
   assert(uav_index < TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT);
   assert(uav_bytes_per_element_log2 <= 4);
   uint64_t const old_uavs_bytes_per_element_log2 = config->cb_immed_.uav_bytes_per_element_log2;
   unsigned const uav_shift = 3 * uav_index;
   config->cb_immed_.uav_bytes_per_element_log2 =
      (old_uavs_bytes_per_element_log2 & ~((uint64_t)0b111 << uav_shift)) |
      ((uint64_t)uav_bytes_per_element_log2 << uav_shift);
   if (config->cb_immed_.uav_bytes_per_element_log2 != old_uavs_bytes_per_element_log2) {
      config->cb_immed_.modified_bits |= BITFIELD_BIT(uav_index);
      BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_DRAW_ENTRY_CB_IMMED);
   }
}

/* It's desirable to explicitly unbind unused color targets so `SURFACE_SYNC` is clearly aware that
 * they're not used in the state context.
 */

/* For dual-source blending, the target 1 `SOURCE_FORMAT` must match that of the target 0.
 * In other cases, specify `V_028C70_EXPORT_4C_16BPC`.
 */
static inline void
terakan_hw_config_draw_set_cb_color_unbound(struct terakan_hw_config_draw * const config,
                                            unsigned const color_index,
                                            uint32_t const source_format)
{
   assert(color_index < TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT);

   uint32_t const color_info = S_028C70_SOURCE_FORMAT(source_format);

   if (config->cb_color_.color[color_index].info == color_info) {
      return;
   }

   config->cb_color_.color[color_index].info = color_info;
   /* Clear the BO pointer for quick comparison when a target is bound later. */
   config->cb_color_.bo[color_index] = NULL;

   config->cb_color_.modified_bits |= BITFIELD_BIT(color_index);
   BITSET_SET(config->entries_modified_, TERAKAN_HW_CONFIG_DRAW_ENTRY_CB_COLOR);
}

/* If `terakan_color_descriptor_is_bound` for the provided arguments is false, the color target is
 * unbound with the export format being `4C_16BPC`.
 *
 * `NUM_SAMPLES` must be provided regardless of the architecture generation for the WDDM patch
 * location ID and the BO priority, so whether the target is multisampled is known regardless of
 * whether it has FMASK enabled via `COMPRESSION`. It will be zeroed during emission if the hardware
 * doesn't use it, along with `NUM_FRAGMENTS`.
 *
 * If the meta descriptor pointer is NULL, the meta surfaces will be disabled for this target.
 *
 * #MemoryIntegrity is expected to be ensured before calling.
 */
void terakan_hw_config_draw_set_cb_color(struct terakan_hw_config_draw * config,
                                         unsigned color_index, struct terakan_bo const * bo,
                                         struct terakan_color_descriptor const * color,
                                         struct terakan_color_meta_descriptor const * meta);

struct terakan_gfx_command_writer;

/* Emits packets setting registers that never change. */
void terakan_hw_config_draw_emit_constant(struct terakan_gfx_command_writer * command_writer);

void terakan_hw_config_draw_emit_modified(struct terakan_gfx_command_writer * command_writer);

void terakan_hw_config_draw_set_all_modified(struct terakan_hw_config_draw * config);

void terakan_hw_config_draw_reset(struct terakan_hw_config_draw * config);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_HW_CONFIG_DRAW_H */
