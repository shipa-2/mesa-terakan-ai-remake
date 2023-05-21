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

#include "terakan_command_buffer.h"

#include "util/macros.h"

#include <assert.h>
#include <stdint.h>

void
terakan_bo_reference_writer_reset(
   struct terakan_bo_reference_writer * const writer, void * const bo_references)
{
   writer->references = bo_references;

   writer->reference_count = 0;

   BITSET_ZERO(writer->map_entries_used);
}

uint32_t
terakan_bo_reference_writer_add_reference(
   struct terakan_bo_reference_writer * const writer, struct terakan_winsys_bo const * const bo,
   bool const is_reading, bool const is_writing, enum terakan_winsys_cs_bo_priority const priority)
{
   /* Provide two slots per hash value for quick handling of collisions by effectively doing
    * separate chaining if there are only 2 BOs per hash in this open addressing scheme (though this
    * separate-chaining-like behavior is not guaranteed if BOs with another hash value have stomped
    * on the two entries for this hash value if there were many collisions for other hash values).
    */
   uint32_t const hash = (bo->creation_number << 1) & TERAKAN_BO_REFERENCE_HASH_MASK;

   /* Search for the hash map entry.
    * Expect that more recently created BOs are more likely to be used than older ones (in a
    * scenario of an application preferring dedicated allocations - in applications with
    * suballocation from large allocations, collisions are less likely overall as the total number
    * of allocations ever created is likely to be smaller). Thus, because the hash is the lower bits
    * of the BO creation number, by going forward from the most recently created BO, collision
    * resolution will immediately end up at a hash value for a very old BO.
    */
   uint32_t reference_index = UINT32_MAX;
   uint32_t collisions;
   for (collisions = 0; collisions <= TERAKAN_BO_REFERENCE_HASH_MASK; ++collisions) {
      uint32_t const check_hash =
         (hash + collisions) & TERAKAN_BO_REFERENCE_HASH_MASK;
      if (!BITSET_TEST(writer->map_entries_used, check_hash)) {
         /* Didn't find an existing BO. */
         break;
      }
      uint32_t const check_reference_index = writer->map[check_hash];
      if (writer->reference_bos[check_reference_index] == bo) {
         /* Found an existing BO. */
         reference_index = check_reference_index;
         break;
      }
   }
   if (unlikely(collisions > TERAKAN_BO_REFERENCE_HASH_MASK)) {
      /* No free space in the hash map.
       * External code may assume that this will never happen if there's space for BO references
       * themselves, so normally this should happen only if the capacity of the hash map matches the
       * total maximum number of BO references, and there's already no free space in the BO
       * reference array.
       * A hash map smaller than the maximum BO reference count is pointless anyway because it would
       * effectively clamp the maximum BO reference count.
       */
      assert(writer->reference_count >= TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT);
      return UINT32_MAX;
   }

   /* Create the new or update the existing reference. */
   size_t const reference_size = bo->winsys->gpu_info.cs_bo_reference_size;
   if (reference_index != UINT32_MAX) {
      bo->winsys->cs_fn->update_bo_reference(
         (char *)writer->references + reference_size * reference_index, bo, is_reading, is_writing,
         priority);
   } else {
      if (unlikely(writer->reference_count >= TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT)) {
         return UINT32_MAX;
      }
      reference_index = writer->reference_count++;
      writer->reference_bos[reference_index] = bo;
      bo->winsys->cs_fn->create_bo_reference(
         (char *)writer->references + reference_size * reference_index, bo, is_reading, is_writing,
         priority);
   }

   /* Insert the BO reference in the beginning of the collision chain for this hash, moving the
    * collisions forward by one (so they're ordered from the most recently used to the least
    * recently used), so when this BO is referenced again in the near future, it will be found
    * quickly.
    */
   for (uint32_t collision_index = 0; collision_index < collisions; ++collision_index) {
      writer->map[(hash + (collisions - collision_index)) & TERAKAN_BO_REFERENCE_HASH_MASK] =
         writer->map[(hash + (collisions - collision_index - 1)) & TERAKAN_BO_REFERENCE_HASH_MASK];
   }
   BITSET_SET(writer->map_entries_used, (hash + collisions) & TERAKAN_BO_REFERENCE_HASH_MASK);
   writer->map[hash] = reference_index;

   assert(reference_size % sizeof(uint32_t) == 0);
   return (uint32_t)(reference_size / sizeof(uint32_t)) * reference_index;
}
