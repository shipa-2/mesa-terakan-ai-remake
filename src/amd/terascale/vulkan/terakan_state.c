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

#include "terakan_state.h"

#include "terakan_bo.h"
#include "terakan_command_buffer.h"
#include "terakan_descriptor.h"
#include "terakan_device.h"
#include "terakan_hw_state.h"
#include "terakan_image.h"
#include "terakan_shader.h"
#include "terakan_state_rasterization.h"

#include "gallium/drivers/r600/evergreend.h"
#include "util/bitscan.h"
#include "util/macros.h"
#include "util/u_endian.h"
#include "util/u_math.h"

#include <assert.h>
#include <float.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void
terakan_state_translate_window_rect_unpacked(VkRect2D const * const rect, uint16_t tl_br_xy_out[4])
{
   if (unlikely(rect->offset.x > TERAKAN_IMAGE_MAX_WIDTH_HEIGHT_1D_SLICES - 1 ||
                rect->offset.y > TERAKAN_IMAGE_MAX_WIDTH_HEIGHT_1D_SLICES - 1)) {
      /* For top-left, the maximum value in the register is 2^n-1, not 2^n. */
      memset(tl_br_xy_out, 0, sizeof(*tl_br_xy_out) * 4);
      return;
   }

   tl_br_xy_out[0] = CLAMP(rect->offset.x, 0, TERAKAN_IMAGE_MAX_WIDTH_HEIGHT_1D_SLICES - 1);
   tl_br_xy_out[1] = CLAMP(rect->offset.y, 0, TERAKAN_IMAGE_MAX_WIDTH_HEIGHT_1D_SLICES - 1);
   tl_br_xy_out[2] =
      CLAMP(rect->offset.x + rect->extent.width, 0, TERAKAN_IMAGE_MAX_WIDTH_HEIGHT_1D_SLICES);
   tl_br_xy_out[3] =
      CLAMP(rect->offset.y + rect->extent.height, 0, TERAKAN_IMAGE_MAX_WIDTH_HEIGHT_1D_SLICES);
}

typedef void (*terakan_state_draw_apply_function)(
   struct terakan_gfx_command_writer * command_writer);

static void
terakan_state_draw_apply_vgt_index_type(struct terakan_gfx_command_writer * const command_writer)
{
   bool const modified =
      command_writer->hw_state_draw.vgt_index_type != command_writer->state_draw.vgt_index_type;
   command_writer->hw_state_draw.vgt_index_type = command_writer->state_draw.vgt_index_type;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_VGT_INDEX_TYPE, modified);
}

static void
terakan_state_draw_apply_vgt_primitive_type(struct terakan_gfx_command_writer * const command_writer)
{
   bool const modified = command_writer->hw_state_draw.vgt_primitive_type !=
                         command_writer->state_draw.vgt_primitive_type;
   command_writer->hw_state_draw.vgt_primitive_type = command_writer->state_draw.vgt_primitive_type;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_VGT_PRIMITIVE_TYPE, modified);
}

static void
terakan_state_draw_apply_vgt_index_offset(struct terakan_gfx_command_writer * const command_writer)
{
   bool const modified =
      command_writer->hw_state_draw.vgt_index_offset != command_writer->state_draw.vgt_index_offset;
   command_writer->hw_state_draw.vgt_index_offset = command_writer->state_draw.vgt_index_offset;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_VGT_INDEX_OFFSET, modified);
}

static void
terakan_state_draw_apply_sq_pgm_fs(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_state_draw const * const state = &command_writer->state_draw;

   struct terakan_vertex_input_static_state const * const static_vi = state->sq_pgm_fs.static_state;

   struct terakan_bo const * program_bo;
   uint32_t program_start;

   static_assert(
      BITSET_WORDBITS >= TERAKAN_RESOURCE_HW_COUNT_FETCH,
      "Assuming that the bindings mask as a single integer can fit into one bitset word.");
   BITSET_DECLARE(resources_needed, TERAKAN_RESOURCE_HW_COUNT_FETCH);
   resources_needed[0] =
      static_vi != NULL ? static_vi->bindings_needed_by_attributes_and_provided
                        : state->sq_pgm_fs.dynamic_state.bindings_needed_by_attributes_and_provided;

   uint32_t const bindings_with_2048_stride_workaround =
      state->sq_pgm_fs.bindings_with_2048_stride_workaround;
   if (static_vi != NULL &&
       !((static_vi->bindings_with_2048_stride_workaround ^ bindings_with_2048_stride_workaround) &
         static_vi->bindings_needed_by_attributes_and_provided)) {
      /* Use the static vertex input state fetch shader. */
      program_bo = static_vi->program_bo;
      program_start = static_vi->program_start;
   } else {
      /* Dynamically create the fetch shader, allocated alongside push constants. */
      struct terakan_device const * const device =
         terakan_gfx_command_writer_device(command_writer);
      bool const is_r9xx = terakan_device_physical_device(device)->chip_family_info.is_r9xx;
      uint32_t fs_alu_qword_count, fs_alu_clause_count, fs_fetch_count;
      uint32_t fs_alu[2 * TERAKAN_VERTEX_INPUT_FS_MAX_ALU_QWORDS];
      uint8_t fs_alu_clause_qwords[TERAKAN_VERTEX_INPUT_FS_MAX_ALU_CLAUSES];
      uint32_t fs_fetch[4 * TERAKAN_VERTEX_INPUT_MAX_ATTRIBUTES];
      terakan_vertex_input_create_fs_alu_and_fetches(
         is_r9xx,
         static_vi != NULL ? static_vi->attributes_needed_and_provided
                           : state->sq_pgm_fs.dynamic_state.attributes_provided,
         static_vi != NULL ? static_vi->attributes : state->sq_pgm_fs.dynamic_state.attributes,
         static_vi != NULL ? static_vi->instance_bindings
                           : state->sq_pgm_fs.dynamic_state.instance_bindings,
         static_vi != NULL ? static_vi->instance_binding_divisors
                           : state->sq_pgm_fs.dynamic_state.instance_binding_divisors,
         bindings_with_2048_stride_workaround, &fs_alu_qword_count, fs_alu, &fs_alu_clause_count,
         fs_alu_clause_qwords, &fs_fetch_count, fs_fetch);
      static_assert(
         TERAKAN_SHADER_PROGRAM_ALIGNMENT_LOG2 == TERAKAN_KCACHE_HW_LINE_BYTES_LOG2,
         "Assuming that kcache buffer and shader addresses have the same number of lower bits "
         "dropped in the binding registers.");
      void * const fs_mapping = terakan_command_buffer_allocate_push_constants(
         command_writer->base.command_buffer,
         terakan_vertex_input_fs_byte_count(fs_alu_qword_count, fs_alu_clause_count,
                                            fs_fetch_count),
         &program_bo, &program_start);
      if (unlikely(fs_mapping == NULL)) {
         /* Fall back to the empty fetch shader to avoid drawing with an uninitialized fetch shader
          * address.
          */
         program_bo = device->empty_vertex_input.program_bo;
         program_start = device->empty_vertex_input.program_start;
         BITSET_ZERO(resources_needed);
      } else {
         terakan_vertex_input_create_fs_program(is_r9xx, fs_alu_qword_count, fs_alu,
                                                fs_alu_clause_count, fs_alu_clause_qwords,
                                                fs_fetch_count, fs_fetch, fs_mapping);
      }
   }

   bool const program_modified = command_writer->hw_state_draw.sq_pgm_fs.bo != program_bo ||
                                 command_writer->hw_state_draw.sq_pgm_fs.start != program_start;
   command_writer->hw_state_draw.sq_pgm_fs.bo = program_bo;
   command_writer->hw_state_draw.sq_pgm_fs.start = program_start;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_SQ_PGM_FS, program_modified);

   terakan_hw_state_draw_set_sq_constants_needed_by_vi(&command_writer->hw_state_draw,
                                                       resources_needed);
}

static void
terakan_state_draw_apply_sq_resources_fs(struct terakan_gfx_command_writer * const command_writer)
{
   bool const is_r9xx =
      terakan_gfx_command_writer_physical_device(command_writer)->chip_family_info.is_r9xx;
   uint32_t resource[8] = {
      [3] = S_03000C_DST_SEL_X(V_03000C_SQ_SEL_X) | S_03000C_DST_SEL_Y(V_03000C_SQ_SEL_Y) |
            S_03000C_DST_SEL_Z(V_03000C_SQ_SEL_Z) | S_03000C_DST_SEL_W(V_03000C_SQ_SEL_W),
      [7] = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER),
      [TERAKAN_RESOURCE_BUFFER_PRIORITY_WORD] = TERAKAN_BO_PRIORITY_VERTEX_BUFFER,
   };
   unsigned buffers_remaining = command_writer->state_draw.sq_resources_fs_pending;
   while (buffers_remaining) {
      int const buffer_index = u_bit_scan(&buffers_remaining);
      struct terakan_state_draw_sq_resource_fs const * const buffer =
         &command_writer->state_draw.sq_resources_fs[buffer_index];
      resource[0] = (uint32_t)buffer->bo_offset;
      resource[1] = buffer->size_bytes_minus_1;
      /* The stride field in the descriptor is 11 bits wide on R8xx, 12 bits wide on R9xx.
       * To support 2048 stride, which is mandatory on Vulkan and Direct3D, the fetch shader
       * multiplies the index by 2 on R8xx, so the stride in the descriptor should be divided by 2.
       */
      resource[2] =
         S_030008_BASE_ADDRESS_HI(buffer->bo_offset >> 32) |
         ((uint32_t)((buffer->stride >> (is_r9xx && buffer->stride >= 0x800 ? 1 : 0)) & 0xFFF)
          << 8);
      terakan_hw_state_draw_set_sq_resource_vi(&command_writer->hw_state_draw, buffer_index,
                                               buffer->bo, resource);
   }
   command_writer->state_draw.sq_resources_fs_pending = 0;
}

static void
terakan_state_draw_apply_pa_cl_vport_xy_scale_offset(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_state_draw_ensure_viewport_count(&command_writer->hw_state_draw,
                                               command_writer->state_draw.viewport_count);
   for (uint32_t viewport_index = 0; viewport_index < command_writer->state_draw.viewport_count;
        ++viewport_index) {
      if (memcmp(
             command_writer->hw_state_draw.viewports[viewport_index].pa_cl_vport_xy_scale_offset,
             command_writer->state_draw.viewports[viewport_index].pa_cl_vport_xy_scale_offset,
             sizeof(command_writer->state_draw.viewports[viewport_index]
                       .pa_cl_vport_xy_scale_offset)) != 0) {
         memcpy(
            command_writer->hw_state_draw.viewports[viewport_index].pa_cl_vport_xy_scale_offset,
            command_writer->state_draw.viewports[viewport_index].pa_cl_vport_xy_scale_offset,
            sizeof(
               command_writer->state_draw.viewports[viewport_index].pa_cl_vport_xy_scale_offset));
         terakan_hw_state_draw_viewport_modified(
            &command_writer->hw_state_draw, viewport_index,
            TERAKAN_HW_STATE_DRAW_VIEWPORT_PA_CL_VPORT_XY_SCALE_OFFSET);
      }
   }
}

static void
terakan_state_draw_apply_pa_cl_vport_z_scale_offset(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_state_draw_ensure_viewport_count(&command_writer->hw_state_draw,
                                               command_writer->state_draw.viewport_count);
   bool const dx_clip_space_def =
      G_028810_DX_CLIP_SPACE_DEF(command_writer->hw_state_draw.pa_cl_clip_cntl) != 0;
   for (uint32_t viewport_index = 0; viewport_index < command_writer->state_draw.viewport_count;
        ++viewport_index) {
      struct terakan_state_draw_viewport const * const viewport =
         &command_writer->state_draw.viewports[viewport_index];
      float const * const z_scale_offset =
         &viewport->pa_cl_vport_z_gl_dx_scale_offset[(int)dx_clip_space_def][0];
      if (memcmp(command_writer->hw_state_draw.viewports[viewport_index].pa_cl_vport_z_scale_offset,
                 z_scale_offset, sizeof(float) * 2) != 0) {
         memcpy(command_writer->hw_state_draw.viewports[viewport_index].pa_cl_vport_z_scale_offset,
                z_scale_offset, sizeof(float) * 2);
         terakan_hw_state_draw_viewport_modified(
            &command_writer->hw_state_draw, viewport_index,
            TERAKAN_HW_STATE_DRAW_VIEWPORT_PA_CL_VPORT_Z_SCALE_OFFSET);
      }
   }
}

static void
terakan_state_draw_apply_pa_cl_gb(struct terakan_gfx_command_writer * const command_writer)
{
   float pa_cl_gb_vert_horz_clip_disc_adj[][2] = {{FLT_MAX, 1.0f}, {FLT_MAX, 1.0f}};
   for (uint32_t viewport_index = 0; viewport_index < command_writer->state_draw.viewport_count;
        ++viewport_index) {
      for (int axis = 0; axis < 2; ++axis) {
         pa_cl_gb_vert_horz_clip_disc_adj[axis][0] = MIN2(
            command_writer->state_draw.viewports[viewport_index].pa_cl_gb_vert_horz_clip_adj[axis],
            pa_cl_gb_vert_horz_clip_disc_adj[axis][0]);
      }
   }
   /* TODO(Triang3l): Discard rectangle ratio for points and lines. */
   bool const modified =
      memcmp(command_writer->hw_state_draw.pa_cl_gb_vert_horz_clip_disc_adj,
             pa_cl_gb_vert_horz_clip_disc_adj, sizeof(pa_cl_gb_vert_horz_clip_disc_adj)) != 0;
   memcpy(command_writer->hw_state_draw.pa_cl_gb_vert_horz_clip_disc_adj,
          pa_cl_gb_vert_horz_clip_disc_adj, sizeof(pa_cl_gb_vert_horz_clip_disc_adj));
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_PA_CL_GB, modified);
}

static void
terakan_state_draw_apply_pa_sc_vport_scissor(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_state_draw_ensure_viewport_count(&command_writer->hw_state_draw,
                                               command_writer->state_draw.viewport_count);
   for (uint32_t viewport_index = 0; viewport_index < command_writer->state_draw.viewport_count;
        ++viewport_index) {
      uint16_t const * const viewport_scissor =
         command_writer->state_draw.viewports[viewport_index].pa_sc_vport_scissor_tl_br_xy[0];
      uint16_t const * const generic_scissor =
         command_writer->state_draw.pa_sc_vport_generic_scissor_tl_br_xy[viewport_index][0];
      uint16_t scissor[] = {
         MAX2(viewport_scissor[0], generic_scissor[0]),
         MAX2(viewport_scissor[1], generic_scissor[1]),
         MIN2(viewport_scissor[2], generic_scissor[2]),
         MIN2(viewport_scissor[3], generic_scissor[3]),
      };
      terakan_state_draw_finalize_scissor(scissor);
      uint32_t const pa_sc_vport_scissor[] = {
         S_028250_TL_X(scissor[0]) | S_028250_TL_Y(scissor[1]) | S_028250_WINDOW_OFFSET_DISABLE(1),
         S_028254_BR_X(scissor[2]) | S_028254_BR_Y(scissor[3]),
      };
      if (memcmp(command_writer->hw_state_draw.viewports[viewport_index].pa_sc_vport_scissor,
                 pa_sc_vport_scissor, sizeof(pa_sc_vport_scissor)) != 0) {
         memcpy(command_writer->hw_state_draw.viewports[viewport_index].pa_sc_vport_scissor,
                pa_sc_vport_scissor, sizeof(pa_sc_vport_scissor));
         terakan_hw_state_draw_viewport_modified(
            &command_writer->hw_state_draw, viewport_index,
            TERAKAN_HW_STATE_DRAW_VIEWPORT_PA_SC_VPORT_SCISSOR);
      }
   }
}

static void
terakan_state_draw_apply_pa_sc_vport_z_min_max(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_state_draw_ensure_viewport_count(&command_writer->hw_state_draw,
                                               command_writer->state_draw.viewport_count);
   if (command_writer->state_draw.pa_sc_vport_z_min_0_max_1) {
      float const zero_one[] = {0.0f, 1.0f};
      for (uint32_t viewport_index = 0; viewport_index < command_writer->state_draw.viewport_count;
           ++viewport_index) {
         if (memcmp(command_writer->hw_state_draw.viewports[viewport_index].pa_sc_vport_z_min_max,
                    &zero_one, sizeof(float) * 2) != 0) {
            memcpy(command_writer->hw_state_draw.viewports[viewport_index].pa_sc_vport_z_min_max,
                   &zero_one, sizeof(float) * 2);
            terakan_hw_state_draw_viewport_modified(
               &command_writer->hw_state_draw, viewport_index,
               TERAKAN_HW_STATE_DRAW_VIEWPORT_PA_SC_VPORT_Z_MIN_MAX);
         }
      }
   } else {
      for (uint32_t viewport_index = 0; viewport_index < command_writer->state_draw.viewport_count;
           ++viewport_index) {
         if (memcmp(command_writer->hw_state_draw.viewports[viewport_index].pa_sc_vport_z_min_max,
                    command_writer->state_draw.viewports[viewport_index].pa_sc_vport_z_min_max,
                    sizeof(float) * 2) != 0) {
            memcpy(command_writer->hw_state_draw.viewports[viewport_index].pa_sc_vport_z_min_max,
                   command_writer->state_draw.viewports[viewport_index].pa_sc_vport_z_min_max,
                   sizeof(float) * 2);
            terakan_hw_state_draw_viewport_modified(
               &command_writer->hw_state_draw, viewport_index,
               TERAKAN_HW_STATE_DRAW_VIEWPORT_PA_SC_VPORT_Z_MIN_MAX);
         }
      }
   }
}

static void
terakan_state_draw_apply_pa_cl_clip_cntl(struct terakan_gfx_command_writer * const command_writer)
{
   bool const modified =
      command_writer->hw_state_draw.pa_cl_clip_cntl != command_writer->state_draw.pa_cl_clip_cntl;
   command_writer->hw_state_draw.pa_cl_clip_cntl = command_writer->state_draw.pa_cl_clip_cntl;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_PA_CL_CLIP_CNTL, modified);
}

static void
terakan_state_draw_apply_pa_su_sc_mode_cntl(struct terakan_gfx_command_writer * const command_writer)
{
   bool const modified = command_writer->hw_state_draw.pa_su_sc_mode_cntl !=
                         command_writer->state_draw.pa_su_sc_mode_cntl;
   command_writer->hw_state_draw.pa_su_sc_mode_cntl = command_writer->state_draw.pa_su_sc_mode_cntl;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_PA_SU_SC_MODE_CNTL, modified);
}

static void
terakan_state_draw_apply_pa_cl_vte_cntl(struct terakan_gfx_command_writer * const command_writer)
{
   uint32_t const pa_cl_vte_cntl = S_028818_VPORT_X_SCALE_ENA(1) | S_028818_VPORT_X_OFFSET_ENA(1) |
                                   S_028818_VPORT_Y_SCALE_ENA(1) | S_028818_VPORT_Y_OFFSET_ENA(1) |
                                   S_028818_VPORT_Z_SCALE_ENA(1) | S_028818_VPORT_Z_OFFSET_ENA(1) |
                                   S_028818_VTX_W0_FMT(1);
   bool const modified = command_writer->hw_state_draw.pa_cl_vte_cntl != pa_cl_vte_cntl;
   command_writer->hw_state_draw.pa_cl_vte_cntl = pa_cl_vte_cntl;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_PA_CL_VTE_CNTL, modified);
}

static void
terakan_state_draw_apply_pa_sc_mode_cntl_0(struct terakan_gfx_command_writer * const command_writer)
{
   /* TODO(Triang3l): MSAA_ENABLE, LINE_STIPPLE_ENABLE from a variable. */
   uint32_t const pa_sc_mode_cntl_0 = S_028A48_VPORT_SCISSOR_ENABLE(1);
   bool const modified = command_writer->hw_state_draw.pa_sc_mode_cntl_0 != pa_sc_mode_cntl_0;
   command_writer->hw_state_draw.pa_sc_mode_cntl_0 = pa_sc_mode_cntl_0;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_PA_SC_MODE_CNTL_0, modified);
}

static void
terakan_state_draw_apply_pa_sc_aa_mask(struct terakan_gfx_command_writer * const command_writer)
{
   bool const modified =
      command_writer->hw_state_draw.pa_sc_aa_mask != command_writer->state_draw.pa_sc_aa_mask;
   command_writer->hw_state_draw.pa_sc_aa_mask = command_writer->state_draw.pa_sc_aa_mask;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_PA_SC_AA_MASK, modified);
}

static void
terakan_state_draw_apply_db_render_override(struct terakan_gfx_command_writer * const command_writer)
{
   bool const modified = command_writer->hw_state_draw.db_render_override !=
                         command_writer->state_draw.db_render_override;
   command_writer->hw_state_draw.db_render_override = command_writer->state_draw.db_render_override;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_DB_RENDER_OVERRIDE, modified);
}

static void
terakan_state_draw_apply_color_attachment_usage(
   struct terakan_gfx_command_writer * const command_writer)
{
   TERAKAN_STATE_DRAW_ASSERT_DEPENDS_ON(CB_TARGET_MASK, COLOR_ATTACHMENT_USAGE);
   terakan_state_draw_set_pending(&command_writer->state_draw,
                                  TERAKAN_STATE_DRAW_INDEX_CB_TARGET_MASK);
   TERAKAN_STATE_DRAW_ASSERT_DEPENDS_ON(CB_COLOR_MRT, COLOR_ATTACHMENT_USAGE);
   terakan_state_draw_set_pending(&command_writer->state_draw,
                                  TERAKAN_STATE_DRAW_INDEX_CB_COLOR_MRT);
}

static void
terakan_state_draw_apply_cb_target_mask(struct terakan_gfx_command_writer * const command_writer)
{
   uint32_t cb_target_mask = 0b0;

   {
      TERAKAN_STATE_DRAW_ASSERT_DEPENDS_ON(CB_TARGET_MASK, COLOR_ATTACHMENT_USAGE);
      unsigned attachments_remaining =
         command_writer->state_draw.color_attachment_usage.written_by_shader;
      uint32_t hw_color_index = 0;
      for (; attachments_remaining; ++hw_color_index) {
         unsigned const attachment_index = (unsigned)u_bit_scan(&attachments_remaining);
         if (!(command_writer->state_draw.color_attachment_usage.bound &
               ((uint8_t)1 << attachment_index))) {
            continue;
         }
         cb_target_mask |= 0b1111 << (4 * attachment_index);
         /* TODO(Triang3l): Blending state mask. */
         /* TODO(Triang3l): Force-enable channels that don't exist in the format (add a new field
          * with present channels under the CB_TARGET_MASK state configured in vkCmdBeginRendering),
          * and force-disable targets that have only missing channels enabled in the blending state.
          */
      }
   }

   /* TODO(Triang3l): RATs. */

   bool const modified = command_writer->hw_state_draw.cb_target_mask != cb_target_mask;
   command_writer->hw_state_draw.cb_target_mask = cb_target_mask;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_CB_TARGET_MASK, modified);

   bool const any_target_enabled = cb_target_mask != 0;
   TERAKAN_STATE_DRAW_ASSERT_DEPENDS_ON(CB_COLOR_CONTROL, CB_TARGET_MASK);
   if (command_writer->state_draw.cb_color_control.from_apply_cb_target_mask.any_target_enabled !=
       any_target_enabled) {
      command_writer->state_draw.cb_color_control.from_apply_cb_target_mask.any_target_enabled =
         any_target_enabled;
      terakan_state_draw_set_pending(&command_writer->state_draw,
                                     TERAKAN_STATE_DRAW_INDEX_CB_COLOR_CONTROL);
   }
}

static void
terakan_state_draw_apply_cb_color_control(struct terakan_gfx_command_writer * const command_writer)
{
   TERAKAN_STATE_DRAW_ASSERT_DEPENDS_ON(CB_COLOR_CONTROL, CB_TARGET_MASK);
   uint32_t const cb_color_control =
      S_028808_MODE(
         command_writer->state_draw.cb_color_control.from_apply_cb_target_mask.any_target_enabled
            ? V_028808_CB_NORMAL
            : V_028808_CB_DISABLE) |
      S_028808_ROP3(0xCC);
   bool const modified = command_writer->hw_state_draw.cb_color_control != cb_color_control;
   command_writer->hw_state_draw.cb_color_control = cb_color_control;
   terakan_hw_state_draw_written(&command_writer->hw_state_draw,
                                 TERAKAN_HW_STATE_DRAW_INDEX_CB_COLOR_CONTROL, modified);
}

static void
terakan_state_draw_apply_cb_color_mrt(struct terakan_gfx_command_writer * const command_writer)
{
   TERAKAN_STATE_DRAW_ASSERT_DEPENDS_ON(CB_COLOR_MRT, COLOR_ATTACHMENT_USAGE);
   unsigned attachments_remaining =
      command_writer->state_draw.color_attachment_usage.bound &
      command_writer->state_draw.color_attachment_usage.written_by_shader;
   while (attachments_remaining) {
      unsigned const attachment_index = (unsigned)u_bit_scan(&attachments_remaining);
      struct terakan_state_draw_cb_color const * const attachment =
         &command_writer->state_draw.attachment_cb_color[attachment_index];
      unsigned const hw_color_index =
         util_bitcount(command_writer->state_draw.color_attachment_usage.written_by_shader &
                       ((1u << attachment_index) - 1));
      bool const modified =
         command_writer->hw_state_draw.cb_color.bo[hw_color_index] != attachment->bo ||
         memcmp(&command_writer->hw_state_draw.cb_color.color[hw_color_index], &attachment->color,
                sizeof(struct terakan_color_descriptor)) != 0 ||
         memcmp(&command_writer->hw_state_draw.cb_color.meta[hw_color_index], &attachment->meta,
                sizeof(struct terakan_color_meta_descriptor)) != 0;
      command_writer->hw_state_draw.cb_color.bo[hw_color_index] = attachment->bo;
      memcpy(&command_writer->hw_state_draw.cb_color.color[hw_color_index], &attachment->color,
             sizeof(struct terakan_color_descriptor));
      memcpy(&command_writer->hw_state_draw.cb_color.meta[hw_color_index], &attachment->meta,
             sizeof(struct terakan_color_meta_descriptor));
      terakan_hw_state_draw_cb_color_written(&command_writer->hw_state_draw, hw_color_index,
                                             modified);
   }
}

static terakan_state_draw_apply_function const
   terakan_state_draw_apply_functions[TERAKAN_STATE_DRAW_INDEX_COUNT] = {
      [TERAKAN_STATE_DRAW_INDEX_VGT_INDEX_TYPE] = terakan_state_draw_apply_vgt_index_type,
      [TERAKAN_STATE_DRAW_INDEX_VGT_PRIMITIVE_TYPE] = terakan_state_draw_apply_vgt_primitive_type,
      [TERAKAN_STATE_DRAW_INDEX_VGT_INDEX_OFFSET] = terakan_state_draw_apply_vgt_index_offset,
      [TERAKAN_STATE_DRAW_INDEX_SQ_PGM_FS] = terakan_state_draw_apply_sq_pgm_fs,
      [TERAKAN_STATE_DRAW_INDEX_SQ_RESOURCES_FS] = terakan_state_draw_apply_sq_resources_fs,
      [TERAKAN_STATE_DRAW_INDEX_PA_CL_VPORT_XY_SCALE_OFFSET] =
         terakan_state_draw_apply_pa_cl_vport_xy_scale_offset,
      [TERAKAN_STATE_DRAW_INDEX_PA_CL_VPORT_Z_SCALE_OFFSET] =
         terakan_state_draw_apply_pa_cl_vport_z_scale_offset,
      [TERAKAN_STATE_DRAW_INDEX_PA_CL_GB] = terakan_state_draw_apply_pa_cl_gb,
      [TERAKAN_STATE_DRAW_INDEX_PA_SC_VPORT_SCISSOR] = terakan_state_draw_apply_pa_sc_vport_scissor,
      [TERAKAN_STATE_DRAW_INDEX_PA_SC_VPORT_Z_MIN_MAX] =
         terakan_state_draw_apply_pa_sc_vport_z_min_max,
      [TERAKAN_STATE_DRAW_INDEX_PA_CL_CLIP_CNTL] = terakan_state_draw_apply_pa_cl_clip_cntl,
      [TERAKAN_STATE_DRAW_INDEX_PA_SU_SC_MODE_CNTL] = terakan_state_draw_apply_pa_su_sc_mode_cntl,
      [TERAKAN_STATE_DRAW_INDEX_PA_CL_VTE_CNTL] = terakan_state_draw_apply_pa_cl_vte_cntl,
      [TERAKAN_STATE_DRAW_INDEX_PA_SC_MODE_CNTL_0] = terakan_state_draw_apply_pa_sc_mode_cntl_0,
      [TERAKAN_STATE_DRAW_INDEX_PA_SC_AA_MASK] = terakan_state_draw_apply_pa_sc_aa_mask,
      [TERAKAN_STATE_DRAW_INDEX_DB_RENDER_OVERRIDE] = terakan_state_draw_apply_db_render_override,
      [TERAKAN_STATE_DRAW_INDEX_COLOR_ATTACHMENT_USAGE] =
         terakan_state_draw_apply_color_attachment_usage,
      [TERAKAN_STATE_DRAW_INDEX_CB_TARGET_MASK] = terakan_state_draw_apply_cb_target_mask,
      [TERAKAN_STATE_DRAW_INDEX_CB_COLOR_CONTROL] = terakan_state_draw_apply_cb_color_control,
      [TERAKAN_STATE_DRAW_INDEX_CB_COLOR_MRT] = terakan_state_draw_apply_cb_color_mrt,
};

void
terakan_state_draw_apply_pending(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_state_draw * const state = &command_writer->state_draw;
   /* Manual loop instead of BITSET_FOREACH_SET, reading the pending bits at each iteration so
    * application of state items with lower indices can make state items with higher indices pending
    * if the latter depend on the former.
    */
   unsigned next_state_index = 0;
   while (next_state_index < TERAKAN_STATE_DRAW_INDEX_COUNT) {
      unsigned const next_state_word_index = BITSET_BITWORD(next_state_index);
      BITSET_WORD const next_state_word_remaining =
         state->state_pending[next_state_word_index] & ~(BITSET_BIT(next_state_index) - 1);
      if (!next_state_word_remaining) {
         next_state_index = BITSET_WORDBITS * (next_state_word_index + 1);
         continue;
      }
      unsigned const state_index =
         BITSET_WORDBITS * next_state_word_index + (ffs(next_state_word_remaining) - 1);
      if (state_index >= TERAKAN_STATE_DRAW_INDEX_COUNT) {
         /* Ignore the bits beyond the end of the bitset. */
         break;
      }
      terakan_state_draw_apply_functions[state_index](command_writer);
      BITSET_CLEAR(state->state_pending, state_index);
      next_state_index = state_index + 1;
   }
}

void
terakan_state_draw_reset(struct terakan_state_draw * const state,
                         struct terakan_device const * device)
{
   /* Initialize the state to the default values, corresponding to one of the following that's
    * applicable:
    * - For optional features, the feature is not enabled.
    * - For state configured via a structure in a pNext chain, the structure is missing.
    * - The field in the original structure is zero (like if it was zeroed via memset or {}).
    *
    * While Vulkan drivers are not required to do this because complete state for all enabled
    * features must be configured before a draw (via a pipeline object or dynamic state), do this
    * for simplicity:
    * - Certain state items correspond to hardware registers containing multiple fields, and those
    *   fields are configured individually separately by state setters. In this case the rest of the
    *   fields (the ones not exposed to the application) in those registers must be set to their
    *   default values.
    * - Optional features not enabled on the device don't need to be handled explicitly, especially
    *   when the application uses only dynamic state and never calls the setters for certain state
    *   items.
    * - There may be conditional logic based on the latest values of the state items in some
    *   places, make sure it doesn't read uninitialized variables.
    */

   /* No VK_DYNAMIC_STATE_DEPTH_CLIP_ENABLE_EXT,
    * no VkPipelineRasterizationDepthClipStateCreateInfoEXT */
   state->cmd_set_depth_clamp_enable_sets_depth_clip_enable = true;

   /* indexType = VK_INDEX_TYPE_UINT16 */
#if UTIL_ARCH_BIG_ENDIAN
   state->vgt_index_type = VGT_INDEX_16 | VGT_DMA_SWAP_16_BIT;
#else
   state->vgt_index_type = VGT_INDEX_16;
#endif

   /* topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST */
   state->vgt_primitive_type = S_008958_PRIM_TYPE(V_008958_DI_PT_POINTLIST);

   /* firstVertex or vertexOffset = 0 */
   state->vgt_index_offset = 0;

   /* vertexBindingDescriptionCount = 0, vertexAttributeDescriptionCount = 0 */
   memset(&state->sq_pgm_fs, 0, sizeof(state->sq_pgm_fs));
   state->sq_pgm_fs.static_state = &device->empty_vertex_input;

   /* pBuffers[...] = VK_NULL_HANDLE
    * Also make sure all vertex buffer bindings are cleared regardless of what's bound in
    * hw_state_draw at the time of this call.
    */
   memset(state->sq_resources_fs, 0, sizeof(state->sq_resources_fs));
   state->sq_resources_fs_pending = BITFIELD_MASK(TERAKAN_RESOURCE_HW_COUNT_FETCH);

   state->viewport_count = 0;
   memset(state->viewports, 0, sizeof(state->viewports));
   memset(state->pa_sc_vport_generic_scissor_tl_br_xy, 0,
          sizeof(state->pa_sc_vport_generic_scissor_tl_br_xy));

   /* depthClampEnable = VK_FALSE, must be initialized accurately because depthClamp is an optional
    * feature, when it's disabled, VK_FALSE must be assumed if never set by the application.
    */
   bool const depth_range_unrestricted = device->vk.enabled_extensions.EXT_depth_range_unrestricted;
   state->pa_sc_vport_z_min_0_max_1 = !depth_range_unrestricted;
   /* depthClampEnable = VK_FALSE */
   state->db_render_override = S_02800C_DISABLE_VIEWPORT_CLAMP(depth_range_unrestricted);

   state->pa_cl_clip_cntl =
      /* negativeOneToOne = VK_FALSE */
      S_028810_DX_CLIP_SPACE_DEF(1) |
      /* rasterizerDiscardEnable = VK_FALSE */
      S_028810_DX_RASTERIZATION_KILL(0) |
      /* Always enabled. */
      S_028810_DX_LINEAR_ATTR_CLIP_ENA(1) |
      /* depthClampEnable = VK_FALSE, no VkPipelineRasterizationDepthClipStateCreateInfoEXT */
      S_028810_ZCLIP_NEAR_DISABLE(0) | S_028810_ZCLIP_FAR_DISABLE(0);

   state->pa_su_sc_mode_cntl =
      /* cullMode = VK_CULL_MODE_NONE */
      S_028814_CULL_FRONT(0) | S_028814_CULL_BACK(0) |
      /* frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE */
      S_028814_FACE(0) |
      /* polygonMode = VK_POLYGON_MODE_FILL */
      S_028814_POLY_MODE(V_028814_X_DISABLE_POLY_MODE) |
      S_028814_POLYMODE_FRONT_PTYPE(V_028814_X_DRAW_TRIANGLES) |
      S_028814_POLYMODE_BACK_PTYPE(V_028814_X_DRAW_TRIANGLES) |
      /* depthBiasEnable = VK_FALSE */
      S_028814_POLY_OFFSET_FRONT_ENABLE(0) | S_028814_POLY_OFFSET_BACK_ENABLE(0) |
      S_028814_POLY_OFFSET_PARA_ENABLE(0) |
      /* provokingVertexMode = VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT */
      S_028814_PROVOKING_VTX_LAST(0);

   /* pSampleMask = NULL */
   state->pa_sc_aa_mask = UINT16_MAX;

   state->color_attachment_usage.written_by_shader = 0b0;
   state->color_attachment_usage.bound = 0b0;

   state->cb_color_control.from_apply_cb_target_mask.any_target_enabled = false;

   /* Make all state items pending so the defaults are applied before the first draw, even for
    * unsupported features.
    */
   BITSET_ONES(state->state_pending);
}
