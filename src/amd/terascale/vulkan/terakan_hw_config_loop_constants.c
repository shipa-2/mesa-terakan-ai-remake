/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_hw_config_loop_constants.h"

#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600d_common.h"

uint32_t *
terakan_hw_config_loop_constants_write(uint32_t * packet, uint32_t const compute_packet_flag)
{
   for (uint32_t stage_index = 0; stage_index < TERAKAN_HW_CONFIG_LOOP_CONSTANT_STAGE_COUNT;
        ++stage_index) {
      uint32_t const stage_packet_flag =
         stage_index + 1 == TERAKAN_HW_CONFIG_LOOP_CONSTANT_STAGE_COUNT ? compute_packet_flag : 0;
      *packet++ = PKT3(PKT3_SET_LOOP_CONST, 1, 0) | stage_packet_flag;
      /* Each shader stage owns a range of 32 SQ_LOOP_CONST registers. */
      *packet++ = stage_index * 32;
      /* 0 initial value, +1 increment, 4095 maximum: at most 4096 loop iterations. */
      *packet++ = TERAKAN_HW_CONFIG_LOOP_CONSTANT_VALUE;
   }

   return packet;
}
