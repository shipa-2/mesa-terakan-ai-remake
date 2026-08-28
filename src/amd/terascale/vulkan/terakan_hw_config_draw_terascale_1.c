/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Kept in its own translation unit including only r600d.h/r600d_common.h, deliberately never
 * evergreend.h -- see the identical rationale in terakan_hw_config_shared_terascale_1.c.
 */

#include "terakan_hw_config_draw_terascale_1.h"

#include "gallium/drivers/r600/r600d.h"
#include "gallium/drivers/r600/r600d_common.h"

#include <assert.h>

static uint32_t *
write_context_reg(uint32_t * packet, uint32_t const reg, uint32_t const value)
{
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
   *packet++ = (reg - R600_CONTEXT_REG_OFFSET) >> 2;
   *packet++ = value;
   return packet;
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_db_depth_view(uint32_t * const packet,
                                                        uint32_t const value)
{
   return write_context_reg(packet, R_028004_DB_DEPTH_VIEW, value);
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_db_render_control_override(
   uint32_t * packet, uint32_t const db_render_control, uint32_t const db_render_override)
{
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 2, 0);
   *packet++ = (R_028D0C_DB_RENDER_CONTROL - R600_CONTEXT_REG_OFFSET) >> 2;
   *packet++ = db_render_control;
   *packet++ = db_render_override;
   return packet;
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_db_depth_size(uint32_t * const packet,
                                                        uint32_t const pitch_tile_max,
                                                        uint32_t const slice_tile_max)
{
   return write_context_reg(packet, R_028000_DB_DEPTH_SIZE,
                            S_028000_PITCH_TILE_MAX(pitch_tile_max) |
                               S_028000_SLICE_TILE_MAX(slice_tile_max));
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_db_depth_base_info(uint32_t * packet,
                                                             uint32_t const db_depth_base,
                                                             uint32_t const db_depth_info)
{
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 2, 0);
   *packet++ = (R_02800C_DB_DEPTH_BASE - R600_CONTEXT_REG_OFFSET) >> 2;
   *packet++ = db_depth_base;
   *packet++ = db_depth_info;
   return packet;
}

bool
terakan_hw_config_draw_terascale_1_cb_color_encode(
   struct terakan_hw_config_draw_terascale_1_cb_color_input const * const input,
   struct terakan_hw_config_draw_terascale_1_cb_color * const color_out)
{
   if (input->is_uav || input->is_multisampled || input->metadata_enabled || input->format == 0 ||
       input->pitch_tile_max > 0x3FF || input->slice_tile_max > 0xFFFFF ||
       input->slice_start > 0x7FF || input->slice_max > 0x7FF ||
       input->slice_start > input->slice_max || input->endian > 0x3 || input->format > 0x3F ||
       (input->array_mode != V_0280A0_ARRAY_LINEAR_ALIGNED &&
        input->array_mode != V_0280A0_ARRAY_1D_TILED_THIN1 &&
        input->array_mode != V_0280A0_ARRAY_2D_TILED_THIN1) ||
       input->number_type > 0x7 || input->comp_swap > 0x3 || input->source_format > 1) {
      return false;
   }

   color_out->base = input->base;
   color_out->size = S_028060_PITCH_TILE_MAX(input->pitch_tile_max) |
                     S_028060_SLICE_TILE_MAX(input->slice_tile_max);
   color_out->view =
      S_028080_SLICE_START(input->slice_start) | S_028080_SLICE_MAX(input->slice_max);
   color_out->info =
      S_0280A0_ENDIAN(input->endian) | S_0280A0_FORMAT(input->format) |
      S_0280A0_ARRAY_MODE(input->array_mode) | S_0280A0_NUMBER_TYPE(input->number_type) |
      S_0280A0_COMP_SWAP(input->comp_swap) | S_0280A0_BLEND_CLAMP(input->blend_clamp) |
      S_0280A0_BLEND_BYPASS(input->blend_bypass) | S_0280A0_SIMPLE_FLOAT(input->simple_float) |
      S_0280A0_SOURCE_FORMAT(input->source_format == 1 ? V_0280A0_EXPORT_NORM
                                                       : V_0280A0_EXPORT_FULL);
   /* With metadata disabled, classic r600_init_color_surface() points both metadata bases at the
    * color base and leaves CB_COLORn_MASK zero. This is not a claim that FMASK/CMASK addressing is
    * validated -- the early rejection above keeps every metadata-bearing surface out.
    */
   color_out->fmask = input->base;
   color_out->cmask = input->base;
   color_out->mask = 0;
   return true;
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_cb_color(
   uint32_t * packet, uint32_t const color_index,
   struct terakan_hw_config_draw_terascale_1_cb_color const * const color)
{
   assert(color_index < 8);
   packet = write_context_reg(packet, R_0280A0_CB_COLOR0_INFO + color_index * sizeof(uint32_t),
                              color->info);
   packet = write_context_reg(packet, R_028040_CB_COLOR0_BASE + color_index * sizeof(uint32_t),
                              color->base);
   packet = write_context_reg(packet, R_0280E0_CB_COLOR0_FRAG + color_index * sizeof(uint32_t),
                              color->fmask);
   packet = write_context_reg(packet, R_0280C0_CB_COLOR0_TILE + color_index * sizeof(uint32_t),
                              color->cmask);
   packet = write_context_reg(packet, R_028060_CB_COLOR0_SIZE + color_index * sizeof(uint32_t),
                              color->size);
   packet = write_context_reg(packet, R_028080_CB_COLOR0_VIEW + color_index * sizeof(uint32_t),
                              color->view);
   return write_context_reg(packet, R_028100_CB_COLOR0_MASK + color_index * sizeof(uint32_t),
                            color->mask);
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_cb_color_unbound(uint32_t * const packet,
                                                          uint32_t const color_index,
                                                          uint32_t const source_format)
{
   assert(color_index < 8);
   assert(source_format <= 1);
   return write_context_reg(packet, R_0280A0_CB_COLOR0_INFO + color_index * sizeof(uint32_t),
                            S_0280A0_SOURCE_FORMAT(source_format));
}
