/*
 * Copyright © 2025 Vitaliy Triang3l Kuzmin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef TERAKAN_QUERY_H
#define TERAKAN_QUERY_H

#include "terakan_bo.h"

#include "vk_query_pool.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum terakan_query_sample_index {
   TERAKAN_QUERY_SAMPLE_INDEX_ZPASS,

   TERAKAN_QUERY_SAMPLE_INDEX_PIPELINESTAT,

   TERAKAN_QUERY_SAMPLE_INDEX_STREAMOUTSTATS_0,
   TERAKAN_QUERY_SAMPLE_INDEX_STREAMOUTSTATS_1,
   TERAKAN_QUERY_SAMPLE_INDEX_STREAMOUTSTATS_2,
   TERAKAN_QUERY_SAMPLE_INDEX_STREAMOUTSTATS_3,

   TERAKAN_QUERY_SAMPLE_INDEX_COUNT,
};

enum terakan_query_pipelinestat_hw_counter {
   /* Written by the SAMPLE_PIPELINESTAT event in this order. */
   TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_FS_INVOCATIONS,
   TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_CLIPPING_PRIMITIVES,
   TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_CLIPPING_INVOCATIONS,
   TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_VS_INVOCATIONS,
   TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_GS_INVOCATIONS,
   TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_GS_PRIMITIVES,
   TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_IA_PRIMITIVES,
   TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_IA_VERTICES,
   TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_TCS_PATCHES,
   TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_TES_INVOCATIONS,
   TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_CS_INVOCATIONS,

   TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_COUNT,
};

struct terakan_query_pool {
   struct vk_query_pool vk;

   /* Accessed frequently, and for occlusion queries, depends on the physical device render backend
    * count, so stored explicitly.
    */
   uint8_t samples_size_counters;

   uint8_t copy_dst_result_size_counters;

   uint16_t pipelinestat_hw_counters;
   /* Per-counter destination offsets passed to the copy meta shader constants, with the meaning of
    * the values being internal to the copy shaders. See `terakan_meta_query_copy_init_offsets`.
    */
   int8_t pipelinestat_copy_offsets_32_bit[TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_COUNT];
   int8_t pipelinestat_copy_offsets_64_bit[TERAKAN_QUERY_PIPELINESTAT_HW_COUNTER_COUNT];

   /* Stores an array of 32-bit query availabilities, followed by an array of samples. */
   struct terakan_bo * bo;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(terakan_query_pool, vk.base, VkQueryPool, VK_OBJECT_TYPE_QUERY_POOL)

unsigned terakan_query_pool_vk_result_size_counters(struct terakan_query_pool const * query_pool);

/* #MemoryIntegrity */
static inline void
terakan_query_pool_clamp_range(struct terakan_query_pool const * const query_pool,
                               uint32_t * const first_query, uint32_t * const query_count)
{
   *first_query = MIN2(*first_query, query_pool->vk.query_count);
   *query_count = MIN2(*query_count, query_pool->vk.query_count - *first_query);
}

static inline VkDeviceSize
terakan_query_pool_samples_offset_bytes(struct terakan_query_pool const * const query_pool,
                                        uint32_t const query)
{
   return (VkDeviceSize)(sizeof(uint64_t) * query_pool->samples_size_counters) * query;
}

static inline VkDeviceSize
terakan_query_pool_availability_offset_bytes(struct terakan_query_pool const * const query_pool,
                                             uint32_t const query)
{
   /* Preceded by the samples array (the availabilities need an alignment smaller than the samples).
    */
   return (VkDeviceSize)(sizeof(uint64_t) * query_pool->samples_size_counters) *
             query_pool->vk.query_count +
          (VkDeviceSize)sizeof(uint32_t) * query;
}

struct terakan_gfx_command_writer;

void
terakan_query_sample_in_new_indirect_buffer(struct terakan_gfx_command_writer * command_writer);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_QUERY_H */
