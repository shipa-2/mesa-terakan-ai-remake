/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TERAKAN_CP_DMA_LIMITS_H
#define TERAKAN_CP_DMA_LIMITS_H

#include "util/macros.h"

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

/* r600_cp_dma_copy_buffer() caps the R600/R700 BYTE_COUNT field at 2^21 - 8, rather than the
 * nominal 21-bit all-ones value. Keep that generation-specific limit in the packet dword itself;
 * the R8xx/R9xx behavior is left at the pre-existing all-ones cap until it has its own reason to
 * change.
 */
#define TERAKAN_CP_DMA_TERASCALE_1_MAX_BYTE_COUNT (((VkDeviceSize)1 << 21) - 8)
#define TERAKAN_CP_DMA_EVERGREEN_MAX_BYTE_COUNT (((VkDeviceSize)1 << 21) - 1)

/* See the copy alignment requirements documented with terakan_cp_dma_copy(). */
#define TERAKAN_CP_DMA_COPY_OPTIMAL_ALIGNMENT ((VkDeviceSize)1 << 5)

static inline uint32_t
terakan_cp_dma_get_chunk_byte_count(bool const is_terascale_1, VkDeviceSize const remaining,
                                    VkDeviceSize const alignment)
{
   VkDeviceSize const max_byte_count = is_terascale_1
                                          ? TERAKAN_CP_DMA_TERASCALE_1_MAX_BYTE_COUNT
                                          : TERAKAN_CP_DMA_EVERGREEN_MAX_BYTE_COUNT;
   return (uint32_t)MIN2(remaining, max_byte_count & ~(alignment - 1));
}

#endif /* TERAKAN_CP_DMA_LIMITS_H */
