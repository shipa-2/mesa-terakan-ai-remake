/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_hw_config_loop_constants.h"

#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600d_common.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                                           \
   do {                                                                                            \
      if (!(condition)) {                                                                          \
         fprintf(stderr, "CHECK failed at %s:%u: %s\n", __FILE__, __LINE__, #condition);           \
         abort();                                                                                  \
      }                                                                                            \
   } while (0)

#define CANARY 0xA5A5A5A5u
#define COMPUTE_PACKET_FLAG 0b10u

static void
test_loop_constant_packets(void)
{
   uint32_t guarded_packets[TERAKAN_HW_CONFIG_LOOP_CONSTANT_DWORDS + 2];
   guarded_packets[0] = CANARY;
   guarded_packets[TERAKAN_HW_CONFIG_LOOP_CONSTANT_DWORDS + 1] = CANARY;

   uint32_t * const packets = &guarded_packets[1];
   uint32_t * const end =
      terakan_hw_config_loop_constants_write(packets, COMPUTE_PACKET_FLAG);

   CHECK(end == packets + TERAKAN_HW_CONFIG_LOOP_CONSTANT_DWORDS);
   CHECK(guarded_packets[0] == CANARY);
   CHECK(guarded_packets[TERAKAN_HW_CONFIG_LOOP_CONSTANT_DWORDS + 1] == CANARY);

   for (uint32_t stage_index = 0;
        stage_index < TERAKAN_HW_CONFIG_LOOP_CONSTANT_STAGE_COUNT; ++stage_index) {
      uint32_t const packet_offset = stage_index * 3;
      uint32_t const expected_stage_flag =
         stage_index + 1 == TERAKAN_HW_CONFIG_LOOP_CONSTANT_STAGE_COUNT
            ? COMPUTE_PACKET_FLAG
            : 0;
      CHECK(packets[packet_offset] ==
            (PKT3(PKT3_SET_LOOP_CONST, 1, 0) | expected_stage_flag));
      CHECK(packets[packet_offset + 1] == stage_index * 32);
      CHECK(packets[packet_offset + 2] == TERAKAN_HW_CONFIG_LOOP_CONSTANT_VALUE);
   }
}

int
main(void)
{
   test_loop_constant_packets();
   return 0;
}
