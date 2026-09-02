/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_cp_dma_limits.h"

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

int
main(void)
{
   /* Exact COMMAND dword used by r600_cp_dma_copy_buffer() for the largest R700 legal packet. */
   uint32_t const r700_fill = terakan_cp_dma_get_chunk_byte_count(
      true, TERAKAN_CP_DMA_TERASCALE_1_MAX_BYTE_COUNT + sizeof(uint32_t), sizeof(uint32_t));
   CHECK(r700_fill == 0x001ffff8);

   /* The same value with the pre-existing all-ones cap would be 0x1ffffc. This negative control
    * makes the test sensitive to accidentally reusing the Evergreen path on TeraScale 1.
    */
   uint32_t const old_evergreen_fill = terakan_cp_dma_get_chunk_byte_count(
      false, TERAKAN_CP_DMA_TERASCALE_1_MAX_BYTE_COUNT + sizeof(uint32_t), sizeof(uint32_t));
   CHECK(old_evergreen_fill == 0x001ffffc);
   CHECK(r700_fill != old_evergreen_fill);

   /* Copy packets use 32-byte optimal chunks, for which both caps round down to the same legal
    * 0x1fffe0. That equality is intentional and must not be mistaken for evidence about fills.
    */
   CHECK(terakan_cp_dma_get_chunk_byte_count(true, UINT64_C(0x200000),
                                             TERAKAN_CP_DMA_COPY_OPTIMAL_ALIGNMENT) == 0x001fffe0);
   CHECK(terakan_cp_dma_get_chunk_byte_count(false, UINT64_C(0x200000),
                                             TERAKAN_CP_DMA_COPY_OPTIMAL_ALIGNMENT) == 0x001fffe0);
   return EXIT_SUCCESS;
}
