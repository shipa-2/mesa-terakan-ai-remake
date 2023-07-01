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

#include "terakan_hw_state.h"
#include "terakan_command_buffer.h"
#include "terakan_physical_device.h"

#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/macros.h"
#include "vk_device.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TERAKAN_MAKE_SAMPLE_LOCS(s0x, s0y, s1x, s1y, s2x, s2y, s3x, s3y)                           \
   (((uint32_t)(s0x)&0xF) | (((uint32_t)(s0y)&0xF) << 4) | (((uint32_t)(s1x)&0xF) << 8) |          \
    (((uint32_t)(s1y)&0xF) << 12) | (((uint32_t)(s2x)&0xF) << 16) |                                \
    (((uint32_t)(s2y)&0xF) << 20) | (((uint32_t)(s3x)&0xF) << 24) | (((uint32_t)(s3y)&0xF) << 28))

uint32_t const terakan_standard_sample_locs[5][16 / 4] = {
   {
      TERAKAN_MAKE_SAMPLE_LOCS(0, 0, 0, 0, 0, 0, 0, 0),
   },
   {
      TERAKAN_MAKE_SAMPLE_LOCS(4, 4, -4, -4, 0, 0, 0, 0),
   },
   {
      TERAKAN_MAKE_SAMPLE_LOCS(-2, -6, 6, -2, -6, 2, 2, 6),
   },
   {
      TERAKAN_MAKE_SAMPLE_LOCS(1, -3, -1, 3, 5, 1, -3, -5),
      TERAKAN_MAKE_SAMPLE_LOCS(-5, 5, -7, -1, 3, 7, 7, -7),
   },
   {
      TERAKAN_MAKE_SAMPLE_LOCS(1, 1, -1, -3, -3, 2, 4, -1),
      TERAKAN_MAKE_SAMPLE_LOCS(-5, -2, 2, 5, 5, 3, 3, -5),
      TERAKAN_MAKE_SAMPLE_LOCS(-2, 6, 0, -7, -4, -6, -6, 4),
      TERAKAN_MAKE_SAMPLE_LOCS(-8, 0, 7, -4, 6, 7, -7, -8),
   },
};

uint32_t const terakan_standard_sample_max_dists[5] = {0, 4, 6, 7, 8};

typedef void (*terakan_hw_state_draw_emit_function)(
   struct terakan_gfx_command_writer * command_writer,
   enum terakan_hw_state_draw_index state_index);

static void
terakan_hw_state_draw_emit_vgt_index_type(struct terakan_gfx_command_writer * const command_writer,
                                          UNUSED enum terakan_hw_state_draw_index const state_index)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(command_writer, 2, 0, 0);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_INDEX_TYPE, 1 - 1, 0);
   *packet++ = command_writer->hw_state_draw.vgt_index_type;
}

static void
terakan_hw_state_draw_emit_vgt_index_buffer(
   struct terakan_gfx_command_writer * const command_writer,
   UNUSED enum terakan_hw_state_draw_index const state_index)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(command_writer, 3 + 2, 1, 2);
   if (unlikely(packet == NULL)) {
      return;
   }

   *packet++ = PKT3(EG_PKT3_INDEX_BASE, 2 - 1, 0);
   *packet++ = (uint32_t)command_writer->hw_state_draw.vgt_index_buffer.base;
   *packet++ = (uint32_t)(command_writer->hw_state_draw.vgt_index_buffer.base >> 32);
   *packet++ = PKT3(PKT3_NOP, 0, 0);
   *packet++ = terakan_bo_reference_writer_add_reference(
      &command_writer->base.bo_reference_writer, command_writer->hw_state_draw.vgt_index_buffer.bo,
      true, false, TERAKAN_WINSYS_CS_BO_PRIORITY_INDEX_BUFFER);

   *packet++ = PKT3(EG_PKT3_INDEX_BUFFER_SIZE, 1 - 1, 0);
   *packet++ = command_writer->hw_state_draw.vgt_index_buffer.size;
}

static void
terakan_hw_state_draw_emit_vgt_primitive_type(
   struct terakan_gfx_command_writer * const command_writer,
   UNUSED enum terakan_hw_state_draw_index const state_index)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(command_writer, 2 + 1, 0, 0);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
   *packet++ = TERAKAN_CONFIG_REG_OFFSET(R_008958_VGT_PRIMITIVE_TYPE);
   *packet++ = command_writer->hw_state_draw.vgt_primitive_type;
}

static void
terakan_hw_state_draw_emit_vgt_index_offset(
   struct terakan_gfx_command_writer * const command_writer,
   UNUSED enum terakan_hw_state_draw_index const state_index)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(command_writer, 2 + 1, 0, 0);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028408_VGT_INDX_OFFSET);
   *packet++ = command_writer->hw_state_draw.vgt_index_offset;
}

static void
terakan_hw_state_draw_emit_sq_vtx_start_inst_loc(
   struct terakan_gfx_command_writer * const command_writer,
   UNUSED enum terakan_hw_state_draw_index const state_index)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(command_writer, 2 + 1, 0, 0);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_CTL_CONST, 1, 0);
   *packet++ = TERAKAN_CTL_CONST_OFFSET(R_03CFF4_SQ_VTX_START_INST_LOC);
   *packet++ = command_writer->hw_state_draw.sq_vtx_start_inst_loc;
}

static void
terakan_hw_state_draw_emit_pa_cl_clip_cntl(struct terakan_gfx_command_writer * const command_writer,
                                           UNUSED enum terakan_hw_state_draw_index const state_index)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(command_writer, 2 + 1, 0, 0);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028810_PA_CL_CLIP_CNTL);
   *packet++ = command_writer->hw_state_draw.pa_cl_clip_cntl;
}

static void
terakan_hw_state_draw_emit_pa_su_sc_mode_cntl(
   struct terakan_gfx_command_writer * const command_writer,
   UNUSED enum terakan_hw_state_draw_index const state_index)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(command_writer, 2 + 1, 0, 0);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028814_PA_SU_SC_MODE_CNTL);
   *packet++ = command_writer->hw_state_draw.pa_su_sc_mode_cntl;
}

static void
terakan_hw_state_draw_emit_pa_sc_aa_samples(
   struct terakan_gfx_command_writer * const command_writer,
   UNUSED enum terakan_hw_state_draw_index const state_index)
{
   bool const is_r9xx = container_of(command_writer->base.command_buffer->vk.base.device->physical,
                                     struct terakan_physical_device const, vk)
                           ->winsys->gpu_info.gfx_level >= CAYMAN;

   uint32_t const num_samples_log2 =
      command_writer->hw_state_draw.pa_sc_aa_samples.num_samples_log2;
   assert(num_samples_log2 <= (is_r9xx ? 4 : 3));

   uint32_t pa_sc_aa_config =
      S_028BE0_MSAA_NUM_SAMPLES(num_samples_log2) |
      S_028BE0_MAX_SAMPLE_DIST(terakan_standard_sample_max_dists[num_samples_log2]);

   uint32_t const num_sample_loc_dwords = (((uint32_t)1 << num_samples_log2) + 3) / 4;
   uint32_t const * const sample_locs = terakan_standard_sample_locs[num_samples_log2];

   if (is_r9xx) {
      pa_sc_aa_config |= S_028BE0_MSAA_EXPOSED_SAMPLES(num_samples_log2);

      uint32_t * packet = terakan_gfx_command_writer_emit(
         command_writer, (2 + 2) + (2 + 1) + (2 + num_sample_loc_dwords) * 4, 0, 0);
      if (unlikely(packet == NULL)) {
         return;
      }

      uint64_t const standard_centroid_priorities[] = {
         UINT64_C(0x0000000000000000), UINT64_C(0x1010101010101010), UINT64_C(0x3210321032103210),
         UINT64_C(0x7654321076543210), UINT64_C(0xFEDCBA9876543210),
      };
      uint64_t const centroid_priority = standard_centroid_priorities[num_samples_log2];
      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 2, 0);
      *packet++ = TERAKAN_CONTEXT_REG_OFFSET(CM_R_028BD4_PA_SC_CENTROID_PRIORITY_0);
      *packet++ = (uint32_t)centroid_priority;
      *packet++ = (uint32_t)(centroid_priority >> 32);

      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
      *packet++ = TERAKAN_CONTEXT_REG_OFFSET(CM_R_028BE0_PA_SC_AA_CONFIG);
      *packet++ = pa_sc_aa_config;

      /* Pixels in the quad have a constant stride of 4 dwords. */
      for (uint32_t quad_pixel_index = 0; quad_pixel_index < 4; ++quad_pixel_index) {
         *packet++ = PKT3(PKT3_SET_CONTEXT_REG, num_sample_loc_dwords, 0);
         *packet++ = TERAKAN_CONTEXT_REG_OFFSET(CM_R_028BF8_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0 +
                                                sizeof(uint32_t) * 4 * quad_pixel_index);
         memcpy(packet, sample_locs, sizeof(uint32_t) * num_sample_loc_dwords);
         packet += num_sample_loc_dwords;
      }
   } else {
      uint32_t * packet = terakan_gfx_command_writer_emit(
         command_writer, (2 + 1) + (2 + num_sample_loc_dwords * 4), 0, 0);
      if (unlikely(packet == NULL)) {
         return;
      }

      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
      *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028C04_PA_SC_AA_CONFIG);
      *packet++ = pa_sc_aa_config;

      /* Pixels in the quad have a stride of 1 dword at 1x/2x/4x, 2 dwords at 8x. */
      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 4 * num_sample_loc_dwords, 0);
      *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028C1C_PA_SC_AA_SAMPLE_LOCS_0);
      for (uint32_t quad_pixel_index = 0; quad_pixel_index < 4; ++quad_pixel_index) {
         memcpy(packet, sample_locs, sizeof(uint32_t) * num_sample_loc_dwords);
         packet += num_sample_loc_dwords;
      }
   }
}

static void
terakan_hw_state_draw_emit_cb_blend_rgba(struct terakan_gfx_command_writer * const command_writer,
                                         UNUSED enum terakan_hw_state_draw_index const state_index)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(command_writer, 2 + 4, 0, 0);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 4, 0);
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028414_CB_BLEND_RED);
   memcpy(packet, command_writer->hw_state_draw.cb_blend_rgba, sizeof(float) * 4);
}

static void
terakan_hw_state_draw_emit_color(struct terakan_gfx_command_writer * const command_writer,
                                 enum terakan_hw_state_draw_index const state_index)
{
   uint32_t const color_index =
      (uint32_t)state_index - (uint32_t)TERAKAN_HW_STATE_DRAW_CB_COLOR_FIRST;
   uint32_t const color_register_offset =
      (R_028C9C_CB_COLOR1_BASE - R_028C60_CB_COLOR0_BASE) * color_index;

   struct terakan_winsys_bo const * const color_bo =
      command_writer->hw_state_draw.cb_color_bo[color_index];
   struct terakan_color_descriptor const * const color_descriptor =
      &command_writer->hw_state_draw.cb_color[color_index];
   if (color_bo != NULL && G_028C70_FORMAT(color_descriptor->info) != V_028C70_COLOR_INVALID) {
      uint32_t const cb_color_descriptor_dwords =
         sizeof(struct terakan_color_descriptor) / sizeof(uint32_t);
      uint32_t const cb_color_meta_descriptor_dwords =
         sizeof(struct terakan_color_meta_descriptor) / sizeof(uint32_t);

      /* Relocations needed for:
       * R_028C60_CB_COLOR0_BASE
       * R_028C74_CB_COLOR0_ATTRIB
       * R_028C7C_CB_COLOR0_CMASK
       * R_028C84_CB_COLOR0_FMASK
       */
      uint32_t const cb_color_relocation_count = 4;

      uint32_t * packet = terakan_gfx_command_writer_emit(
         command_writer, 2 + cb_color_descriptor_dwords + cb_color_meta_descriptor_dwords, 1,
         2 * cb_color_relocation_count);
      if (unlikely(packet == NULL)) {
         return;
      }

      *packet++ = PKT3(PKT3_SET_CONTEXT_REG,
                       cb_color_descriptor_dwords + cb_color_meta_descriptor_dwords, 0);
      *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028C60_CB_COLOR0_BASE + color_register_offset);

      /* TODO(Triang3l): Higher priority for multisampled color buffers (possibly pass the sample
       * count via the view not only on R9xx, but on R8xx too, but mask it away here on R8xx - using
       * the presence of FMask for this purpose is possibly more complicated and not always
       * reliable).
       */
      uint32_t const color_bo_reference = terakan_bo_reference_writer_add_reference(
         &command_writer->base.bo_reference_writer, color_bo, true, true,
         G_028C70_RAT(color_descriptor->info) ? TERAKAN_WINSYS_CS_BO_PRIORITY_SHADER_RW_IMAGE
                                              : TERAKAN_WINSYS_CS_BO_PRIORITY_COLOR_BUFFER);

      memcpy(packet, color_descriptor, sizeof(*color_descriptor));
      packet += sizeof(*color_descriptor) / sizeof(uint32_t);

      struct terakan_color_meta_descriptor const * const color_meta_descriptor =
         &command_writer->hw_state_draw.cb_color_meta[color_index];
      memcpy(packet, color_meta_descriptor, sizeof(*color_meta_descriptor));
      packet += sizeof(*color_meta_descriptor) / sizeof(uint32_t);

      for (uint32_t cb_color_relocation_index = 0;
           cb_color_relocation_index < cb_color_relocation_count; ++cb_color_relocation_index) {
         *packet++ = PKT3(PKT3_NOP, 0, 0);
         *packet++ = color_bo_reference;
      }
   } else {
      /* Set the format to invalid, not requiring any relocations. */
      uint32_t * packet = terakan_gfx_command_writer_emit(command_writer, 2 + 1, 0, 0);
      if (unlikely(packet == NULL)) {
         return;
      }
      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
      *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028C70_CB_COLOR0_INFO + color_register_offset);
      *packet++ = 0;
   }
}

static void
terakan_hw_state_draw_emit_color_rat_only(struct terakan_gfx_command_writer * const command_writer,
                                          enum terakan_hw_state_draw_index const state_index)
{
   uint32_t const color_rat_only_index =
      (uint32_t)state_index - ((uint32_t)TERAKAN_HW_STATE_DRAW_CB_COLOR_FIRST + 8);
   uint32_t const color_register_offset =
      (R_028E5C_CB_COLOR9_BASE - R_028E40_CB_COLOR8_BASE) * color_rat_only_index;

   struct terakan_winsys_bo const * const color_bo =
      command_writer->hw_state_draw.cb_color_bo[8 + color_rat_only_index];
   struct terakan_color_descriptor const * const color_descriptor =
      &command_writer->hw_state_draw.cb_color[8 + color_rat_only_index];
   if (color_bo != NULL && G_028C70_FORMAT(color_descriptor->info) != V_028C70_COLOR_INVALID) {
      uint32_t const cb_color_descriptor_dwords =
         sizeof(struct terakan_color_descriptor) / sizeof(uint32_t);

      /* Relocations needed for:
       * R_028E40_CB_COLOR8_BASE
       * R_028E54_CB_COLOR8_ATTRIB
       */
      uint32_t const cb_color_relocation_count = 2;

      uint32_t * packet = terakan_gfx_command_writer_emit(
         command_writer, 2 + cb_color_descriptor_dwords, 1, 2 * cb_color_relocation_count);
      if (unlikely(packet == NULL)) {
         return;
      }

      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, cb_color_descriptor_dwords, 0);
      *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028E40_CB_COLOR8_BASE + color_register_offset);

      uint32_t const color_bo_reference = terakan_bo_reference_writer_add_reference(
         &command_writer->base.bo_reference_writer, color_bo, true, true,
         TERAKAN_WINSYS_CS_BO_PRIORITY_SHADER_RW_IMAGE);

      memcpy(packet, color_descriptor, sizeof(*color_descriptor));
      packet += sizeof(*color_descriptor) / sizeof(uint32_t);

      for (uint32_t cb_color_relocation_index = 0;
           cb_color_relocation_index < cb_color_relocation_count; ++cb_color_relocation_index) {
         *packet++ = PKT3(PKT3_NOP, 0, 0);
         *packet++ = color_bo_reference;
      }
   } else {
      /* Set the format to invalid, not requiring any relocations. */
      uint32_t * packet = terakan_gfx_command_writer_emit(command_writer, 2 + 1, 0, 0);
      if (unlikely(packet == NULL)) {
         return;
      }
      *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
      *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028E50_CB_COLOR8_INFO + color_register_offset);
      *packet++ = 0;
   }
}

static terakan_hw_state_draw_emit_function const
   terakan_hw_state_draw_emit_functions[TERAKAN_HW_STATE_DRAW_COUNT] = {
      [TERAKAN_HW_STATE_DRAW_VGT_INDEX_TYPE] = terakan_hw_state_draw_emit_vgt_index_type,
      [TERAKAN_HW_STATE_DRAW_VGT_INDEX_BUFFER] = terakan_hw_state_draw_emit_vgt_index_buffer,
      [TERAKAN_HW_STATE_DRAW_VGT_PRIMITIVE_TYPE] = terakan_hw_state_draw_emit_vgt_primitive_type,
      [TERAKAN_HW_STATE_DRAW_VGT_INDEX_OFFSET] = terakan_hw_state_draw_emit_vgt_index_offset,
      [TERAKAN_HW_STATE_DRAW_SQ_VTX_START_INST_LOC] =
         terakan_hw_state_draw_emit_sq_vtx_start_inst_loc,
      [TERAKAN_HW_STATE_DRAW_PA_CL_CLIP_CNTL] = terakan_hw_state_draw_emit_pa_cl_clip_cntl,
      [TERAKAN_HW_STATE_DRAW_PA_SU_SC_MODE_CNTL] = terakan_hw_state_draw_emit_pa_su_sc_mode_cntl,
      [TERAKAN_HW_STATE_DRAW_PA_SC_AA_SAMPLES] = terakan_hw_state_draw_emit_pa_sc_aa_samples,
      [TERAKAN_HW_STATE_DRAW_CB_BLEND_RGBA] = terakan_hw_state_draw_emit_cb_blend_rgba,
      [TERAKAN_HW_STATE_DRAW_CB_COLOR_FIRST] = terakan_hw_state_draw_emit_color,
      [TERAKAN_HW_STATE_DRAW_CB_COLOR_FIRST + 1] = terakan_hw_state_draw_emit_color,
      [TERAKAN_HW_STATE_DRAW_CB_COLOR_FIRST + 2] = terakan_hw_state_draw_emit_color,
      [TERAKAN_HW_STATE_DRAW_CB_COLOR_FIRST + 3] = terakan_hw_state_draw_emit_color,
      [TERAKAN_HW_STATE_DRAW_CB_COLOR_FIRST + 4] = terakan_hw_state_draw_emit_color,
      [TERAKAN_HW_STATE_DRAW_CB_COLOR_FIRST + 5] = terakan_hw_state_draw_emit_color,
      [TERAKAN_HW_STATE_DRAW_CB_COLOR_FIRST + 6] = terakan_hw_state_draw_emit_color,
      [TERAKAN_HW_STATE_DRAW_CB_COLOR_FIRST + 7] = terakan_hw_state_draw_emit_color,
      [TERAKAN_HW_STATE_DRAW_CB_COLOR_FIRST + 8] = terakan_hw_state_draw_emit_color_rat_only,
      [TERAKAN_HW_STATE_DRAW_CB_COLOR_FIRST + 9] = terakan_hw_state_draw_emit_color_rat_only,
      [TERAKAN_HW_STATE_DRAW_CB_COLOR_FIRST + 10] = terakan_hw_state_draw_emit_color_rat_only,
      [TERAKAN_HW_STATE_DRAW_CB_COLOR_FIRST + 11] = terakan_hw_state_draw_emit_color_rat_only,
};

void
terakan_hw_state_draw_emit_all(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_state_draw * const state = &command_writer->hw_state_draw;
   BITSET_ZERO(state->state_modified);
   unsigned state_index;
   BITSET_FOREACH_SET(state_index, state->state_ever_written, TERAKAN_HW_STATE_DRAW_COUNT)
   {
      terakan_hw_state_draw_emit_functions[state_index](
         command_writer, (enum terakan_hw_state_draw_index)state_index);
   }
}

void
terakan_hw_state_draw_emit_modified(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_state_draw * const state = &command_writer->hw_state_draw;
   unsigned state_index;
   BITSET_FOREACH_SET(state_index, state->state_modified, TERAKAN_HW_STATE_DRAW_COUNT)
   {
      terakan_hw_state_draw_emit_functions[state_index](
         command_writer, (enum terakan_hw_state_draw_index)state_index);
      if (unlikely(!BITSET_TEST(state->state_modified, state_index))) {
         /* If state_modified was zeroed during an emit call, switched to another indirect buffer,
          * and all state has been applied.
          */
         return;
      }
      BITSET_CLEAR(state->state_modified, state_index);
   }
}

void
terakan_hw_state_draw_reset(struct terakan_hw_state_draw * const state)
{
   BITSET_ZERO(state->state_ever_written);
   BITSET_ZERO(state->state_modified);
}
