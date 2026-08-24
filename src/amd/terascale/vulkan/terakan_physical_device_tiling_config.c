/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_physical_device_tiling_config.h"

struct terakan_physical_device_tiling_config_info
terakan_physical_device_decode_tiling_config(uint32_t const tiling_config,
                                             bool const is_terascale_1)
{
   struct terakan_physical_device_tiling_config_info info = {
      /* See the comment on this function's declaration: neither generation's real tiling
       * algorithm has a bank interleave concept at all, so this is not something either
       * generation's tiling_config bits are decoded into.
       */
      .bank_interleave_log2 = 0,
   };

   if (is_terascale_1) {
      /* r6_init_hw_info() in libdrm's radeon/radeon_surface.c. Written as the same switch shape
       * the reference uses, not an arithmetic shortcut, because the legal value ranges are
       * narrower than R8xx/R9xx's (2 bank sizes here vs. 3 there) and the bit positions are not a
       * uniform shift of the R8xx/R9xx ones (the pipe field moves by 1 bit, the group-bytes field
       * moves by 2 bits), so there is no single formula that covers both without restating each
       * one's actual position.
       */
      switch ((tiling_config >> 1) & 0x7) {
      case 0:
         info.pipes_log2 = 0; /* 1 pipe. */
         break;
      case 1:
         info.pipes_log2 = 1; /* 2 pipes. */
         break;
      case 2:
         info.pipes_log2 = 2; /* 4 pipes. */
         break;
      default:
         info.pipes_log2 = 3; /* 8 pipes: case 3, and the reference's fallback for cases 4-7. */
         break;
      }

      switch ((tiling_config >> 4) & 0x3) {
      case 0:
         info.banks_log2 = 2; /* 4 banks. */
         break;
      default:
         info.banks_log2 = 3; /* 8 banks: case 1, and the reference's fallback for cases 2-3. */
         break;
      }

      switch ((tiling_config >> 6) & 0x3) {
      case 0:
         info.pipe_interleave_bytes_log2 = 8; /* 256 bytes. */
         break;
      default:
         /* 512 bytes: case 1, and the reference's fallback for cases 2-3. */
         info.pipe_interleave_bytes_log2 = 9;
         break;
      }

      /* No row size / TILE_SPLIT concept on this hardware at all -- see the comment on this
       * function's declaration. 0 rather than a guessed nonzero value, so a caller that starts
       * treating this as a real value before TeraScale 1 rendering is implemented fails loudly
       * (a zero tile split is a very visible bug) instead of silently using a fabricated one.
       */
      info.row_bytes_log2 = 0;
      return info;
   }

   /* eg_init_hw_info() in the same reference file. Confirmed to match every case its four switch
    * statements handle: each is exactly `constant + ((tiling_config >> shift) & 0xF)` for the
    * legal input range (cases 0-2 for banks and row size, 0-3 for pipes and group bytes), which is
    * what R8xx/R9xx callers of this struct already relied on before this function existed to
    * isolate it.
    */
   info.pipes_log2 = (uint8_t)(tiling_config & 0xF);
   info.banks_log2 = (uint8_t)(2 + ((tiling_config >> 4) & 0xF));
   info.pipe_interleave_bytes_log2 = (uint8_t)(8 + ((tiling_config >> 8) & 0xF));
   info.row_bytes_log2 = (uint8_t)(10 + ((tiling_config >> 12) & 0xF));
   return info;
}
