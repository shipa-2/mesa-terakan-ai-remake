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
