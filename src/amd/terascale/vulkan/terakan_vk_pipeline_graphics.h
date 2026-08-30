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

#ifndef TERAKAN_VK_PIPELINE_GRAPHICS_H
#define TERAKAN_VK_PIPELINE_GRAPHICS_H

#include "terakan_app_config_draw.h"
#include "terakan_bo.h"
#include "terakan_hw_config_draw.h"
#include "terakan_shader.h"
#include "terakan_vertex_input.h"
#include "terakan_vk_state.h"

#include "util/bitset.h"
#include "vk_graphics_state.h"
#include "vk_pipeline.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* TODO(Triang3l): Maybe don't have this header at all since nothing outside the pipeline functions
 * themselves uses it.
 */

enum terakan_vk_pipeline_graphics_vertex_input_static {
   TERAKAN_VK_PIPELINE_GRAPHICS_VERTEX_INPUT_STATIC_VGT_PRIMITIVE_TYPE,
   TERAKAN_VK_PIPELINE_GRAPHICS_VERTEX_INPUT_STATIC_SQ_PGM_FETCH,

   TERAKAN_VK_PIPELINE_GRAPHICS_VERTEX_INPUT_STATIC_COUNT,
};

struct terakan_vk_pipeline_graphics_vertex_input {
   BITSET_DECLARE(static_state, TERAKAN_VK_PIPELINE_GRAPHICS_VERTEX_INPUT_STATIC_COUNT);

   uint32_t vgt_primitive_type;

   struct {
      uint32_t static_stride_needed_for_bindings_bits;
      /* 2048 stride specified as 2048 regardless of whether the #2048StrideAs1024 workaround is
       * used.
       */
      uint16_t stride[TERAKAN_VK_STATE_MAX_VERTEX_BINDINGS];
   } sq_resources_fetch_stride;

   struct terakan_vertex_input_fs sq_pgm_fetch;
};

enum terakan_vk_pipeline_graphics_pre_rasterization_static {
   TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_CL_CLIP_CNTL_DX_RASTERIZATION_KILL,
   TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_CL_CLIP_CNTL_DX_CLIP_SPACE_DEF,
   TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_CL_CLIP_CNTL_Z_CLAMP_ENABLE,
   TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_CL_CLIP_CNTL_Z_CLIP_ENABLE_OVERRIDE,
   TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_VPORT_USER_DEFINED_ZMIN_ZMAX,
   TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_VPORT_VPORT_COUNT,
   TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_VPORT_VPORTS,
   TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_VPORT_EXPLICIT_SCISSOR_COUNT,
   TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_VPORT_EXPLICIT_SCISSORS,
   TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_SC_LINE_STIPPLE_PATTERN,
   TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_SU_LINE_WIDTH,
   TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_SC_LINE_STIPPLE_ENABLE,
   TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_PA_SU_POLY_OFFSET,

   TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_COUNT,
};

struct terakan_vk_pipeline_graphics_pre_rasterization {
   BITSET_DECLARE(static_state, TERAKAN_VK_PIPELINE_GRAPHICS_PRE_RASTERIZATION_STATIC_COUNT);

   /* Whether rasterizer discard is static and enabled. */
   unsigned char pa_cl_clip_cntl_dx_rasterization_kill : 1;
   unsigned char pa_cl_clip_cntl_dx_clip_space_def : 1;
   unsigned char pa_cl_clip_cntl_z_clamp_enable : 1;
   signed char pa_cl_clip_cntl_z_clip_enable_override : 2;
   unsigned char pa_vport_user_defined_zmin_zmax_enable : 1;
   unsigned char pa_sc_line_stipple_enable : 1;
   unsigned char pa_su_poly_offset_exact : 1;

   uint8_t pa_vport_vport_count;
   uint8_t pa_vport_explicit_scissor_count;

   float pa_vport_user_defined_zmin_zmax[2];

   struct terakan_app_config_draw_pa_vport pa_vport_vports[TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT];
   struct terakan_screen_rect pa_vport_explicit_scissors[TERAKAN_HW_CONFIG_DRAW_PA_VPORT_COUNT];

   uint32_t pa_su_sc_mode_cntl_keep, pa_su_sc_mode_cntl;

   /* LINE_PATTERN, REPEAT_COUNT, PATTERN_BIT_ORDER. */
   uint32_t pa_sc_line_stipple_pattern;
   float pa_su_line_width;

   struct {
      struct terakan_hw_config_draw_pa_su_poly_offset offset;
      enum terakan_app_config_draw_poly_offset_representation representation;
   } pa_su_poly_offset;
};

enum terakan_vk_pipeline_graphics_multisample_static {
   TERAKAN_VK_PIPELINE_GRAPHICS_MULTISAMPLE_STATIC_PA_SC_AA_MASK,
   TERAKAN_VK_PIPELINE_GRAPHICS_MULTISAMPLE_STATIC_PA_SC_AA_CONFIG_MSAA_NUM_SAMPLES,
   TERAKAN_VK_PIPELINE_GRAPHICS_MULTISAMPLE_STATIC_PA_SC_AA_SAMPLE_LOCS_CUSTOM_ENABLE,
   TERAKAN_VK_PIPELINE_GRAPHICS_MULTISAMPLE_STATIC_PA_SC_AA_SAMPLE_LOCS_CUSTOM,
   TERAKAN_VK_PIPELINE_GRAPHICS_MULTISAMPLE_STATIC_DB_ALPHA_TO_MASK_ENABLE,

   TERAKAN_VK_PIPELINE_GRAPHICS_MULTISAMPLE_STATIC_COUNT,
};

struct terakan_vk_pipeline_graphics_multisample {
   BITSET_DECLARE(static_state, TERAKAN_VK_PIPELINE_GRAPHICS_MULTISAMPLE_STATIC_COUNT);

   /* Placed here for more compact packing. */
   uint16_t pa_sc_aa_mask;

   uint8_t pa_sc_aa_config_msaa_num_samples_log2;

   bool pa_sc_aa_sample_locs_custom_enable;

   uint8_t pa_sc_aa_sample_locs_custom[16][4];

   bool db_alpha_to_mask_enable;
};

struct terakan_vk_pipeline_graphics_fragment_shading {
   struct {
      uint32_t keep_mask;
      uint32_t front;
      uint32_t back;
   } db_stencilrefmask;

   uint32_t db_depth_control_keep, db_depth_control;

   int8_t db_eqaa_ps_iter_max_invocation_samples_log2;
};

enum terakan_vk_pipeline_graphics_fragment_output_static {
   TERAKAN_VK_PIPELINE_GRAPHICS_FRAGMENT_OUTPUT_STATIC_CB_ROP3_ENABLE,
   TERAKAN_VK_PIPELINE_GRAPHICS_FRAGMENT_OUTPUT_STATIC_CB_ROP3,
   TERAKAN_VK_PIPELINE_GRAPHICS_FRAGMENT_OUTPUT_STATIC_CB_COLOR_RTV_WRITE_ENABLE_MASK,
   TERAKAN_VK_PIPELINE_GRAPHICS_FRAGMENT_OUTPUT_STATIC_CB_COLOR_RTV_WRITE_COMPONENT_MASK,
   TERAKAN_VK_PIPELINE_GRAPHICS_FRAGMENT_OUTPUT_STATIC_CB_BLEND_CONSTANTS,

   TERAKAN_VK_PIPELINE_GRAPHICS_FRAGMENT_OUTPUT_STATIC_COUNT,
};

struct terakan_vk_pipeline_graphics_fragment_output {
   BITSET_DECLARE(static_state, TERAKAN_VK_PIPELINE_GRAPHICS_FRAGMENT_OUTPUT_STATIC_COUNT);

   bool cb_rop3_enable;
   enum terakan_hw_config_draw_cb_color_control_rop3 cb_rop3;

   /* If `CB_COLOR_RTV_WRITE_ENABLE_MASK` (Vulkan runtime `CB_COLOR_WRITE_ENABLES`) is static, this
    * is also used as the write enable mask.
    */
   uint8_t cb_color_rtv_write_potentially_enabled_mask;

   /* Initialized only for the attachments in `cb_color_rtv_write_potentially_enabled_mask`. */
   uint32_t cb_color_rtv_write_component_mask;

   uint32_t cb_blend_control_keep;
   /* Initialized only for the attachments in `cb_color_rtv_write_potentially_enabled_mask`. */
   uint32_t cb_blend_control[TERAKAN_VK_STATE_MAX_COLOR_ATTACHMENTS];

   float cb_blend_constants[4];
};

struct terakan_vk_pipeline_graphics {
   struct vk_pipeline vk;

   BITSET_DECLARE(dynamic_state, MESA_VK_DYNAMIC_GRAPHICS_STATE_ENUM_MAX);

   struct terakan_bo * shader_bo;
   VkShaderStageFlags shader_stages;
   bool diagnostic_skip_fragment;
   struct terakan_shader_impl shaders[MESA_SHADER_FRAGMENT + 1];

   struct terakan_vk_pipeline_graphics_vertex_input vertex_input;

   struct terakan_vk_pipeline_graphics_pre_rasterization pre_rasterization;

   /* Normally a part of the fragment output state, but if the fragment shading part is created with
    * `sample_shading_enable = true` or with a render pass object providing the attachment info, the
    * multisample state is also filled when creating the fragment shading part so it can be used by
    * fragment shader compilation.
    */
   struct terakan_vk_pipeline_graphics_multisample multisample;

   struct terakan_vk_pipeline_graphics_fragment_shading fragment_shading;

   struct terakan_vk_pipeline_graphics_fragment_output fragment_output;
};

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_VK_PIPELINE_GRAPHICS_H */
