/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TERAKAN_HW_CONFIG_LOOP_CONSTANTS_H
#define TERAKAN_HW_CONFIG_LOOP_CONSTANTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TERAKAN_HW_CONFIG_LOOP_CONSTANT_STAGE_COUNT 6
#define TERAKAN_HW_CONFIG_LOOP_CONSTANT_DWORDS                                            \
   (TERAKAN_HW_CONFIG_LOOP_CONSTANT_STAGE_COUNT * 3)
#define TERAKAN_HW_CONFIG_LOOP_CONSTANT_VALUE 0x01000FFFu

uint32_t * terakan_hw_config_loop_constants_write(uint32_t * packet,
                                                  uint32_t compute_packet_flag);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_HW_CONFIG_LOOP_CONSTANTS_H */
