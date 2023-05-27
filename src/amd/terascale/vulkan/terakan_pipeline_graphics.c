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

#include "terakan_command_buffer.h"
#include "terakan_pipeline_graphics.h"
#include "terakan_state_rasterization.h"

#include "vk_graphics_state.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static VkResult
terakan_pipeline_graphics_pre_rasterization_init(
   struct vk_graphics_pipeline_state const * const pipeline_state,
   struct terakan_pipeline_graphics_pre_rasterization * const pre_rasterization_out)
{
   /* PA_SU_SC_MODE_CNTL. */
   pre_rasterization_out->pa_su_sc_mode_cntl_clear = UINT32_MAX;
   pre_rasterization_out->pa_su_sc_mode_cntl = 0;
   if (!BITSET_TEST(pipeline_state->dynamic, MESA_VK_DYNAMIC_RS_POLYGON_MODE)) {
      pre_rasterization_out->pa_su_sc_mode_cntl_clear &=
         TERAKAN_STATE_DRAW_POLYGON_MODE_PA_SU_SC_MODE_CNTL_CLEAR;
      pre_rasterization_out->pa_su_sc_mode_cntl |=
         terakan_state_draw_polygon_mode_pa_su_sc_mode_cntl(pipeline_state->rs->polygon_mode);
   }
   if (!BITSET_TEST(pipeline_state->dynamic, MESA_VK_DYNAMIC_RS_CULL_MODE)) {
      pre_rasterization_out->pa_su_sc_mode_cntl_clear &=
         TERAKAN_STATE_DRAW_CULL_MODE_PA_SU_SC_MODE_CNTL_CLEAR;
      pre_rasterization_out->pa_su_sc_mode_cntl |=
         terakan_state_draw_cull_mode_pa_su_sc_mode_cntl(pipeline_state->rs->cull_mode);
   }
   if (!BITSET_TEST(pipeline_state->dynamic, MESA_VK_DYNAMIC_RS_FRONT_FACE)) {
      pre_rasterization_out->pa_su_sc_mode_cntl_clear &=
         TERAKAN_STATE_DRAW_FRONT_FACE_PA_SU_SC_MODE_CNTL_CLEAR;
      pre_rasterization_out->pa_su_sc_mode_cntl |=
         terakan_state_draw_front_face_pa_su_sc_mode_cntl(pipeline_state->rs->front_face);
   }
   if (!BITSET_TEST(pipeline_state->dynamic, MESA_VK_DYNAMIC_RS_PROVOKING_VERTEX)) {
      pre_rasterization_out->pa_su_sc_mode_cntl_clear &=
         TERAKAN_STATE_DRAW_PROVOKING_VERTEX_MODE_PA_SU_SC_MODE_CNTL_CLEAR;
      pre_rasterization_out->pa_su_sc_mode_cntl |=
         terakan_state_draw_provoking_vertex_mode_pa_su_sc_mode_cntl(
            pipeline_state->rs->provoking_vertex);
   }
   if (!BITSET_TEST(pipeline_state->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_ENABLE)) {
      pre_rasterization_out->pa_su_sc_mode_cntl_clear &=
         TERAKAN_STATE_DRAW_DEPTH_BIAS_ENABLE_PA_SU_SC_MODE_CNTL_CLEAR;
      pre_rasterization_out->pa_su_sc_mode_cntl |=
         terakan_state_draw_depth_bias_enable_pa_su_sc_mode_cntl(
            pipeline_state->rs->depth_bias.enable);
   }
   assert(
      !(pre_rasterization_out->pa_su_sc_mode_cntl &
        pre_rasterization_out->pa_su_sc_mode_cntl_clear));

   return VK_SUCCESS;
}

static void
terakan_pipeline_graphics_bind(
   struct terakan_command_writer * const command_writer,
   struct terakan_pipeline_graphics const * const pipeline)
{
   terakan_hw_state_draw_replace_fields(
      &command_writer->hw_state_draw, TERAKAN_HW_STATE_DRAW_PA_SU_SC_MODE_CNTL,
      &command_writer->hw_state_draw.pa_su_sc_mode_cntl, pipeline->pre_rasterization.pa_su_sc_mode_cntl_clear,
      pipeline->pre_rasterization.pa_su_sc_mode_cntl);
}
