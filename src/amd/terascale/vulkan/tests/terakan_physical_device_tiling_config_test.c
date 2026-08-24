/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_physical_device_tiling_config.h"

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
check(uint32_t const tiling_config, bool const is_terascale_1, uint8_t const pipes_log2,
      uint8_t const banks_log2, uint8_t const pipe_interleave_bytes_log2,
      uint8_t const row_bytes_log2)
{
   struct terakan_physical_device_tiling_config_info const info =
      terakan_physical_device_decode_tiling_config(tiling_config, is_terascale_1);
   CHECK(info.pipes_log2 == pipes_log2);
   CHECK(info.banks_log2 == banks_log2);
   CHECK(info.pipe_interleave_bytes_log2 == pipe_interleave_bytes_log2);
   CHECK(info.bank_interleave_log2 == 0);
   CHECK(info.row_bytes_log2 == row_bytes_log2);
}

/* Values taken from r6_init_hw_info()'s switch statements in libdrm's radeon/radeon_surface.c
 * (see the comment on terakan_physical_device_decode_tiling_config()'s declaration for how this
 * was cross-checked). The RV710 in the R700 dual-GPU test machine has a single shader engine pipe
 * and 4 memory banks, which is what the "typical RV710" case below encodes.
 */
static void
test_terascale_1(void)
{
   /* pipes field (bits [3:1]) case 0 -> 1 pipe, banks field (bits [5:4]) case 0 -> 4 banks,
    * group bytes field (bits [7:6]) case 0 -> 256 bytes: the typical RV710 configuration.
    */
   check(0x00000000u, true, 0, 2, 8, 0);

   /* pipes case 1 -> 2 pipes, banks case 1 (and the 2-3 fallback) -> 8 banks, group bytes case 1
    * (and the 2-3 fallback) -> 512 bytes.
    */
   check((1u << 1) | (1u << 4) | (1u << 6), true, 1, 3, 9, 0);

   /* pipes case 2 -> 4 pipes. */
   check(2u << 1, true, 2, 2, 8, 0);

   /* pipes case 3 -> 8 pipes; cases 4-7 fall back to the same 8-pipe value (unused encodings, but
    * the reference's switch has a default rather than being undefined for them).
    */
   check(3u << 1, true, 3, 2, 8, 0);
   check(7u << 1, true, 3, 2, 8, 0);

   /* banks case 2/3 fall back to 8 banks like case 1 does. */
   check(3u << 4, true, 0, 3, 8, 0);

   /* group bytes case 2/3 fall back to 512 bytes like case 1 does. */
   check(3u << 6, true, 0, 2, 9, 0);
}

/* Values taken from eg_init_hw_info()'s switch statements in the same reference file. These match
 * the arithmetic-shortcut formulas the R8xx/R9xx code path already relied on before this function
 * existed to isolate it, checked here directly against a Cedar-like (case 0 throughout) and an
 * Aruba/Cayman-like (higher case) configuration.
 */
static void
test_r8xx_r9xx(void)
{
   /* All fields case 0: 1 pipe, 4 banks, 256-byte pipe interleave, 1024-byte row size. */
   check(0x00000000u, false, 0, 2, 8, 10);

   /* pipes = 3 (8 pipes), banks = 3 (32 banks), pipe interleave = 3 (2048 bytes), row size = 3
    * (8192 bytes) -- a Cayman-class configuration exercising every field's high bits.
    */
   check(3u | (3u << 4) | (3u << 8) | (3u << 12), false, 3, 5, 11, 13);
}

int
main(void)
{
   test_terascale_1();
   test_r8xx_r9xx();
   return 0;
}
