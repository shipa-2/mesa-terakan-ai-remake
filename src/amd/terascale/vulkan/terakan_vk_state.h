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

#ifndef TERAKAN_VK_STATE_H
#define TERAKAN_VK_STATE_H

#include "terakan_app_config_draw.h"
#include "terakan_descriptor.h"
#include "terakan_hw_config_draw.h"
#include "terakan_instance.h"
#include "terakan_physical_device.h"
#include "terakan_screen_rect.h"

#include "gallium/drivers/r600/evergreend.h"
#include "util/macros.h"
#include "vk_graphics_state.h"
#include "vk_limits.h"

#include <assert.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TERAKAN_VK_STATE_MAX_VERTEX_BINDINGS                                                       \
   MIN2(TERAKAN_RESOURCE_HW_COUNT_FETCH, MESA_VK_MAX_VERTEX_BINDINGS)
#define TERAKAN_VK_STATE_MAX_VERTEX_ATTRIBUTES                                                     \
   MIN2(TERAKAN_RESOURCE_HW_COUNT_FETCH, MESA_VK_MAX_VERTEX_ATTRIBUTES)

#define TERAKAN_VK_STATE_MAX_COLOR_ATTACHMENTS                                                     \
   MIN2(TERAKAN_COLOR_HW_RTV_COUNT, MESA_VK_MAX_COLOR_ATTACHMENTS)

uint32_t
terakan_vk_state_primitive_topology_vgt_primitive_type(VkPrimitiveTopology primitive_topology);

/* Whether the #2048StrideAs1024 workaround is needed on the specified physical device. */
static inline bool
terakan_vk_state_vertex_input_uses_2048_stride_as_1024(
   struct terakan_physical_device const * const physical_device)
{
   if (!physical_device->chip_info.is_r9xx) {
      return true;
   }
#ifdef TERAKAN_REGRESSION_TEST
   if (container_of(physical_device->vk.instance, struct terakan_instance const, vk)
          ->regression_test_flags &
       TERAKAN_REGRESSION_TEST_SHIFT_2048_VERTEX_STRIDE_WORKAROUND_ON_R9XX) {
      return true;
   }
#endif
   return false;
}

struct terakan_app_config_draw_pa_vport
terakan_vk_state_viewport_to_hw(VkViewport const * viewport);

static inline signed char
terakan_vk_state_depth_clip_enable_to_override(
   enum vk_mesa_depth_clip_enable const depth_clip_enable)
{
   switch (depth_clip_enable) {
   case VK_MESA_DEPTH_CLIP_ENABLE_FALSE:
      return 0;
   case VK_MESA_DEPTH_CLIP_ENABLE_TRUE:
      return 1;
   case VK_MESA_DEPTH_CLIP_ENABLE_NOT_CLAMP:
      return -1;
   default:
      assert(!"Unsupported depth clipping enablement state");
      return -1;
   }
}

#define TERAKAN_VK_STATE_POLYGON_MODE_PA_SU_SC_MODE_CNTL_CLEAR                                     \
   (C_028814_POLY_MODE & C_028814_POLYMODE_FRONT_PTYPE & C_028814_POLYMODE_BACK_PTYPE)

static inline uint32_t
terakan_vk_state_polygon_mode_pa_su_sc_mode_cntl(VkPolygonMode const polygon_mode)
{
   switch (polygon_mode) {
   case VK_POLYGON_MODE_FILL:
      return S_028814_POLYMODE_FRONT_PTYPE(V_028814_X_DRAW_TRIANGLES) |
             S_028814_POLYMODE_BACK_PTYPE(V_028814_X_DRAW_TRIANGLES);
      break;
   case VK_POLYGON_MODE_LINE:
      return S_028814_POLYMODE_FRONT_PTYPE(V_028814_X_DRAW_LINES) |
             S_028814_POLYMODE_BACK_PTYPE(V_028814_X_DRAW_LINES) | S_028814_POLY_MODE(1);
   case VK_POLYGON_MODE_POINT:
      return S_028814_POLYMODE_FRONT_PTYPE(V_028814_X_DRAW_POINTS) |
             S_028814_POLYMODE_BACK_PTYPE(V_028814_X_DRAW_POINTS) | S_028814_POLY_MODE(1);
      break;
   default:
      assert(!"Unsupported polygon mode");
      return S_028814_POLYMODE_FRONT_PTYPE(V_028814_X_DRAW_TRIANGLES) |
             S_028814_POLYMODE_BACK_PTYPE(V_028814_X_DRAW_TRIANGLES);
   }
}

#define TERAKAN_VK_STATE_DEPTH_BIAS_ENABLE_PA_SU_SC_MODE_CNTL_CLEAR                                \
   (C_028814_POLY_OFFSET_FRONT_ENABLE & C_028814_POLY_OFFSET_BACK_ENABLE &                         \
    C_028814_POLY_OFFSET_PARA_ENABLE)
#define TERAKAN_VK_STATE_DEPTH_BIAS_ENABLE_PA_SU_SC_MODE_CNTL                                      \
   (S_028814_POLY_OFFSET_FRONT_ENABLE(1) | S_028814_POLY_OFFSET_BACK_ENABLE(1) |                   \
    S_028814_POLY_OFFSET_PARA_ENABLE(1))

static inline enum terakan_app_config_draw_poly_offset_representation
terakan_vk_state_depth_bias_poly_offset_representation(
   VkDepthBiasRepresentationEXT const depth_bias_representation)
{
   switch (depth_bias_representation) {
   case VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT:
      return TERAKAN_APP_CONFIG_DRAW_POLY_OFFSET_REPRESENTATION_FORMAT;
   case VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT:
      return TERAKAN_APP_CONFIG_DRAW_POLY_OFFSET_REPRESENTATION_FORCE_UNORM;
   case VK_DEPTH_BIAS_REPRESENTATION_FLOAT_EXT:
      return TERAKAN_APP_CONFIG_DRAW_POLY_OFFSET_REPRESENTATION_FLOAT;
   default:
      assert(!"Unsupported depth bias representation");
      return TERAKAN_APP_CONFIG_DRAW_POLY_OFFSET_REPRESENTATION_FORMAT;
   }
}

/* `hw_16_samples_2x2_locations_out` is [16][4], where the inner index is [2 * y + x] for the pixel
 * in a quad.
 */
void terakan_vk_state_sample_locations_to_hw(VkSampleCountFlagBits per_pixel, VkExtent2D grid_size,
                                             VkSampleLocationEXT const * locations,
                                             uint8_t * hw_16_samples_2x2_locations_out);

static inline void
terakan_vk_state_sample_locations_mesa_to_hw(
   struct vk_sample_locations_state const * const sample_locations_state,
   uint8_t * const hw_16_samples_2x2_locations_out)
{
   terakan_vk_state_sample_locations_to_hw(
      sample_locations_state->per_pixel, sample_locations_state->grid_size,
      sample_locations_state->locations, hw_16_samples_2x2_locations_out);
}

#define TERAKAN_VK_STATE_STENCIL_TEST_ENABLE_DB_DEPTH_CONTROL_CLEAR                                \
   (C_028800_STENCIL_ENABLE & C_028800_BACKFACE_ENABLE)
#define TERAKAN_VK_STATE_STENCIL_TEST_ENABLE_DB_DEPTH_CONTROL                                      \
   (S_028800_STENCIL_ENABLE(1) | S_028800_BACKFACE_ENABLE(1))

#define TERAKAN_VK_STATE_STENCIL_OP_DB_DEPTH_CONTROL_CLEAR                                         \
   (C_028800_STENCILFUNC & C_028800_STENCILFAIL & C_028800_STENCILZPASS & C_028800_STENCILZFAIL &  \
    C_028800_STENCILFUNC_BF & C_028800_STENCILFAIL_BF & C_028800_STENCILZPASS_BF &                 \
    C_028800_STENCILZFAIL_BF)

enum terakan_hw_config_draw_cb_color_control_rop3
terakan_vk_state_logic_op_rop3(VkLogicOp logic_op);

uint32_t terakan_vk_state_blend_factor_to_hw(VkBlendFactor blend_factor);
uint32_t terakan_vk_state_blend_op_to_hw(VkBlendOp blend_op);

void terakan_vk_state_dynamic_apply(struct terakan_gfx_command_writer * command_writer);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_VK_STATE_H */
