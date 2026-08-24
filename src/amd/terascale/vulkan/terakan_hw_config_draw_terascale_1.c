/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Kept in its own translation unit including only r600d.h/r600d_common.h, never evergreend.h -- see
 * the comment at the top of terakan_hw_config_shared_terascale_1.c for why that separation matters
 * even for registers, like the two here, whose field layout happens to be identical between the two
 * headers: the offset arithmetic still has to use R600_CONTEXT_REG_OFFSET, and mixing both headers
 * in one file is not something to bet real hardware state on for the registers that were not
 * individually checked.
 */

#include "terakan_hw_config_draw_terascale_1.h"

#include "gallium/drivers/r600/r600d.h"
#include "gallium/drivers/r600/r600d_common.h"

uint32_t *
terakan_hw_config_draw_terascale_1_write_db_depth_control(uint32_t * packet, uint32_t const value)
{
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
   *packet++ = (R_028800_DB_DEPTH_CONTROL - R600_CONTEXT_REG_OFFSET) >> 2;
   *packet++ = value;
   return packet;
}

uint32_t *
terakan_hw_config_draw_terascale_1_write_cb_target_mask(uint32_t * packet, uint32_t const value)
{
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
   *packet++ = (R_028238_CB_TARGET_MASK - R600_CONTEXT_REG_OFFSET) >> 2;
   *packet++ = value;
   return packet;
}
