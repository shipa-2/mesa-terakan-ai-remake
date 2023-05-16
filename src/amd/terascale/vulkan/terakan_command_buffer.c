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
   /* Check if already added, and update the existing reference in this case. */
   uint32_t const hash = bo->creation_number & TERAKAN_BO_REFERENCE_HASH_MASK;
   if (BITSET_TEST(writer->map_entries_used, hash)) {
      uint32_t hash_reference_index = writer->map[hash];
      if (writer->reference_bos[hash_reference_index] != bo) {
         /* Hash collision. Search linearly starting from the most recently added, and thus recently
          * used. Ending the search at unsigned overflow.
          */
         for (hash_reference_index = writer->reference_count - 1;
              hash_reference_index != UINT32_MAX; --hash_reference_index) {
            if (writer->reference_bos[hash_reference_index] == bo) {
               /* Update the hash map entry to the most recently used BO with the hash so there
                * won't be a collision if it's referenced again.
                */
               writer->map[hash] = hash_reference_index;
               break;
            }
         }
      }
      if (hash_reference_index != UINT32_MAX) {
         bo->winsys->cs_fn->update_bo_reference(
            (char *)writer->references +
            bo->winsys->gpu_info.cs_bo_reference_size * hash_reference_index,
            bo, is_reading, is_writing, priority);
         return hash_reference_index;
      }
   }

   /* Add the new reference. */
   if (writer->reference_count >= TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT) {
      return UINT32_MAX;
   }
   uint32_t const new_reference_index = writer->reference_count++;
   bo->winsys->cs_fn->create_bo_reference(
      (char *)writer->references + bo->winsys->gpu_info.cs_bo_reference_size * new_reference_index,
      bo, is_reading, is_writing, priority);
   writer->map[hash] = new_reference_index;
   BITSET_SET(writer->map_entries_used, hash);
   return new_reference_index;
}
