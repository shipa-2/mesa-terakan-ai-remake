/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_hw_config_draw_terascale_1.h"

#include "gallium/drivers/r600/r600d.h"
#include "gallium/drivers/r600/r600d_common.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                                          \
   do {                                                                                            \
      if (!(condition)) {                                                                          \
         fprintf(stderr, "CHECK failed at %s:%u: %s\n", __FILE__, __LINE__, #condition);           \
         abort();                                                                                  \
      }                                                                                            \
   } while (0)

static void
test_db_depth_view(void)
{
   /* An arbitrary but representative value exercising both fields, matching the field layout
    * confirmed identical to R8xx/R9xx's own DB_DEPTH_VIEW (SLICE_START/SLICE_MAX, both 11 bits).
    */
   uint32_t const value = S_028004_SLICE_START(3) | S_028004_SLICE_MAX(5);

   uint32_t packets[3];
   uint32_t * const end = terakan_hw_config_draw_terascale_1_write_db_depth_view(packets, value);
   CHECK(end == packets + 3);
   CHECK(packets[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[1] == (R_028004_DB_DEPTH_VIEW - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[2] == value);
}

static void
test_db_render_control_override(void)
{
   uint32_t const db_render_control = S_028D0C_ZPASS_INCREMENT_DISABLE(1);
   uint32_t const db_render_override =
      S_028D10_FORCE_HIS_ENABLE0(V_028D10_FORCE_DISABLE) |
      S_028D10_FORCE_HIS_ENABLE1(V_028D10_FORCE_DISABLE);

   uint32_t packets[4];
   uint32_t * const end = terakan_hw_config_draw_terascale_1_write_db_render_control_override(
      packets, db_render_control, db_render_override);
   CHECK(end == packets + 4);
   CHECK(packets[0] == PKT3(PKT3_SET_CONTEXT_REG, 2, 0));
   CHECK(packets[1] == (R_028D0C_DB_RENDER_CONTROL - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[2] == db_render_control);
   CHECK(packets[3] == db_render_override);
}

static void
test_db_render_control_override_default_matches_r8xx_baseline(void)
{
   /* Every current R8xx/R9xx caller only ever passes the all-zero default -- see
    * TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_RENDER_CONTROL/_DB_RENDER_OVERRIDE in
    * terakan_hw_config_draw.h -- since this driver has no dynamic per-draw DB_RENDER_CONTROL/
    * DB_RENDER_OVERRIDE logic for either generation yet. Checked explicitly here so a future
    * R8xx/R9xx default change doesn't silently stop matching what TeraScale 1 assumes.
    */
   uint32_t packets[4];
   uint32_t * const end =
      terakan_hw_config_draw_terascale_1_write_db_render_control_override(packets, 0, 0);
   CHECK(end == packets + 4);
   CHECK(packets[2] == 0);
   CHECK(packets[3] == 0);
}

static void
test_db_depth_size(void)
{
   uint32_t const pitch_tile_max = 47, slice_tile_max = 12345;
   uint32_t packets[3];
   uint32_t * const end =
      terakan_hw_config_draw_terascale_1_write_db_depth_size(packets, pitch_tile_max,
                                                              slice_tile_max);
   CHECK(end == packets + 3);
   CHECK(packets[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[1] == (R_028000_DB_DEPTH_SIZE - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[2] ==
        (S_028000_PITCH_TILE_MAX(pitch_tile_max) | S_028000_SLICE_TILE_MAX(slice_tile_max)));
}

static void
test_db_depth_base_info(void)
{
   uint32_t const db_depth_base = 0x12345u;
   uint32_t const db_depth_info =
      S_028010_FORMAT(V_028010_DEPTH_8_24) | S_028010_ARRAY_MODE(2) /* 1D tiled thin1 */;

   uint32_t packets[4];
   uint32_t * const end = terakan_hw_config_draw_terascale_1_write_db_depth_base_info(
      packets, db_depth_base, db_depth_info);
   CHECK(end == packets + 4);
   CHECK(packets[0] == PKT3(PKT3_SET_CONTEXT_REG, 2, 0));
   CHECK(packets[1] == (R_02800C_DB_DEPTH_BASE - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[2] == db_depth_base);
   CHECK(packets[3] == db_depth_info);
}

static struct terakan_hw_config_draw_terascale_1_cb_color_input
representative_cb_color_input(void)
{
   return (struct terakan_hw_config_draw_terascale_1_cb_color_input){
      .base = 0x12345,
      .pitch_tile_max = 63,
      .slice_tile_max = 511,
      .slice_start = 3,
      .slice_max = 7,
      .endian = 0,
      .format = V_0280A0_COLOR_8_8_8_8,
      .array_mode = V_0280A0_ARRAY_2D_TILED_THIN1,
      .number_type = V_0280A0_NUMBER_UNORM,
      .comp_swap = V_0280A0_SWAP_ALT,
      .blend_clamp = true,
      .simple_float = true,
      .source_format = 1,
   };
}

static void
test_cb_color_encode(void)
{
   struct terakan_hw_config_draw_terascale_1_cb_color_input const input =
      representative_cb_color_input();
   struct terakan_hw_config_draw_terascale_1_cb_color color;
   CHECK(terakan_hw_config_draw_terascale_1_cb_color_encode(&input, &color));
   CHECK(color.base == input.base);
   CHECK(color.size == (S_028060_PITCH_TILE_MAX(63) | S_028060_SLICE_TILE_MAX(511)));
   CHECK(color.view == (S_028080_SLICE_START(3) | S_028080_SLICE_MAX(7)));
   CHECK(color.info ==
         (S_0280A0_FORMAT(V_0280A0_COLOR_8_8_8_8) |
          S_0280A0_ARRAY_MODE(V_0280A0_ARRAY_2D_TILED_THIN1) |
          S_0280A0_NUMBER_TYPE(V_0280A0_NUMBER_UNORM) |
          S_0280A0_COMP_SWAP(V_0280A0_SWAP_ALT) | S_0280A0_BLEND_CLAMP(1) |
          S_0280A0_SIMPLE_FLOAT(1) | S_0280A0_SOURCE_FORMAT(V_0280A0_EXPORT_NORM)));
   CHECK(color.fmask == input.base);
   CHECK(color.cmask == input.base);
   CHECK(color.mask == 0);
}

static void
test_cb_color_encode_rejects_unported_surfaces(void)
{
   struct terakan_hw_config_draw_terascale_1_cb_color_input input =
      representative_cb_color_input();
   struct terakan_hw_config_draw_terascale_1_cb_color color;

   input.is_uav = true;
   CHECK(!terakan_hw_config_draw_terascale_1_cb_color_encode(&input, &color));
   input.is_uav = false;
   input.is_multisampled = true;
   CHECK(!terakan_hw_config_draw_terascale_1_cb_color_encode(&input, &color));
   input.is_multisampled = false;
   input.metadata_enabled = true;
   CHECK(!terakan_hw_config_draw_terascale_1_cb_color_encode(&input, &color));
   input.metadata_enabled = false;
   input.source_format = 2;
   CHECK(!terakan_hw_config_draw_terascale_1_cb_color_encode(&input, &color));
   input.source_format = 1;
   input.pitch_tile_max = 0x400;
   CHECK(!terakan_hw_config_draw_terascale_1_cb_color_encode(&input, &color));
}

static void
test_cb_color_packets(void)
{
   struct terakan_hw_config_draw_terascale_1_cb_color_input const input =
      representative_cb_color_input();
   struct terakan_hw_config_draw_terascale_1_cb_color color;
   CHECK(terakan_hw_config_draw_terascale_1_cb_color_encode(&input, &color));

   uint32_t packets[21];
   uint32_t * const end =
      terakan_hw_config_draw_terascale_1_write_cb_color(packets, 2, &color);
   CHECK(end == packets + 21);
   uint32_t const registers[7] = {
      R_0280A0_CB_COLOR0_INFO + 2 * 4, R_028040_CB_COLOR0_BASE + 2 * 4,
      R_0280E0_CB_COLOR0_FRAG + 2 * 4, R_0280C0_CB_COLOR0_TILE + 2 * 4,
      R_028060_CB_COLOR0_SIZE + 2 * 4, R_028080_CB_COLOR0_VIEW + 2 * 4,
      R_028100_CB_COLOR0_MASK + 2 * 4,
   };
   uint32_t const values[7] = {
      color.info, color.base, color.fmask, color.cmask, color.size, color.view, color.mask,
   };
   for (uint32_t register_index = 0; register_index < 7; ++register_index) {
      uint32_t const packet_index = register_index * 3;
      CHECK(packets[packet_index] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
      CHECK(packets[packet_index + 1] ==
            (registers[register_index] - R600_CONTEXT_REG_OFFSET) >> 2);
      CHECK(packets[packet_index + 2] == values[register_index]);
   }

   uint32_t unbound_packet[3];
   CHECK(terakan_hw_config_draw_terascale_1_write_cb_color_unbound(unbound_packet, 7, 1) ==
         unbound_packet + 3);
   CHECK(unbound_packet[1] ==
         (R_0280A0_CB_COLOR0_INFO + 7 * 4 - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(unbound_packet[2] == S_0280A0_SOURCE_FORMAT(V_0280A0_EXPORT_NORM));
}

int
main(void)
{
   test_db_depth_view();
   test_db_render_control_override();
   test_db_render_control_override_default_matches_r8xx_baseline();
   test_db_depth_size();
   test_db_depth_base_info();
   test_cb_color_encode();
   test_cb_color_encode_rejects_unported_surfaces();
   test_cb_color_packets();
   return 0;
}
