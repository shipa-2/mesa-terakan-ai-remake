/*
 * Copyright © 2023 Vitaliy Triang3l Kuzmin
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

#ifndef TERAKAN_COMMAND_BUFFER_H
#define TERAKAN_COMMAND_BUFFER_H

#include "winsys/terakan_winsys.h"

#include "util/bitset.h"

#include <stdbool.h>
#include <stdint.h>

/* Given that Terakan exposes more sampled image bindings than the Gallium R600 driver due to
 * separate images and samplers, the number of bindings may be much bigger, and thus, also taking
 * into account that each binding is set one by one, the sizes are larger than in the Gallium R600
 * driver.
 */

/* Must be large enough to hold all the necessary setup, including up to 1024 resources (up to 14
 * dwords per resource - 2 for the SET_RESOURCE header, 8 for the constant, and 4 dwords for 2
 * relocations for textures), for at least one draw / dispatch command.
 * Command buffers using virtual memory on Linux must not be larger than RADEON_INFO_IB_VM_MAX_SIZE
 * dwords reported by the kernel driver, however.
 * Twice the size in the Gallium R600 driver as of May 2023.
 */
#define TERAKAN_MAX_INDIRECT_BUFFER_SIZE_DWORDS ((uint32_t)1 << 15)

/* Must be large enough to hold all bindings for a single command even if they all point to
 * different BOs.
 * Assuming that new references may be needed every 8 dwords on average (resource constants are
 * 2 SET_RESOURCE dwords plus 10-12 dwords, DRAW_INDEX_2 is 6 dwords plus 2 dwords for the
 * relocation).
 */
#define TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT_LOG2 12
#define TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT \
   ((uint32_t)1 << TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT_LOG2)

/* Larger than the reference count to reduce the likelihood of hash collisions.
 * Twice the size of the Gallium R600 driver relocation hash table as of May 2023.
 */
#define TERAKAN_BO_REFERENCE_HASH_BITS (TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT_LOG2 + 1)
#define TERAKAN_BO_REFERENCE_HASH_MASK (((uint32_t)1 << TERAKAN_BO_REFERENCE_HASH_BITS) - 1)

struct terakan_bo_reference_writer {
   struct terakan_winsys const * winsys;
   void * references;

   uint32_t reference_count;

   struct terakan_winsys_bo const * reference_bos[TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT];

   /* Which elements of the map are used, faster to clear than the array itself. */
   BITSET_DECLARE(map_entries_used, TERAKAN_BO_REFERENCE_HASH_MASK + 1);
   uint32_t map[TERAKAN_BO_REFERENCE_HASH_MASK + 1];
};

/* bo_references must point to `terakan_gpu_info::cs_bo_reference_size *
 * TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT` references that will be passed to the winsys during
 * submission.
 */
void terakan_bo_reference_writer_reset(
   struct terakan_bo_reference_writer * writer, void * bo_references);

/* Returns the reference index to use in the relocation, or UINT32_MAX if too many references. */
uint32_t terakan_bo_reference_writer_add_reference(
   struct terakan_bo_reference_writer * writer, struct terakan_winsys_bo const * bo,
   bool is_reading, bool is_writing, enum terakan_winsys_cs_bo_priority priority);

#endif /* TERAKAN_COMMAND_BUFFER_H */
