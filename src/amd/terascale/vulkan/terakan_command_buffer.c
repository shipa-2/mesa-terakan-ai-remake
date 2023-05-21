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
#include "terakan_device.h"
#include "terakan_entrypoints.h"

#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_log.h"

#include <assert.h>
#include <stddef.h>
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

static void
terakan_command_buffer_release_resources(struct terakan_command_buffer * const command_buffer)
{
   struct terakan_command_pool * const command_pool =
      container_of(command_buffer->vk.pool, struct terakan_command_pool, vk);

   list_for_each_entry_safe(
      struct terakan_command_buffer_submission, submission_base, &command_buffer->submissions,
      command_buffer_submission_link) {
      list_del(&submission_base->command_buffer_submission_link);
      if (submission_base->is_secondary_execution) {
         struct terakan_command_buffer_submission_secondary_execution * const submission =
            container_of(
               submission_base, struct terakan_command_buffer_submission_secondary_execution, base);
         list_add(&submission->free_link, &command_pool->secondary_executions_free);
      } else {
         struct terakan_command_buffer_submission_indirect_buffer * const submission = container_of(
            submission_base, struct terakan_command_buffer_submission_indirect_buffer, base);
         list_add(&submission->free_link, &command_pool->indirect_buffers_free);
      }
   }
}

static void
terakan_command_buffer_reset(
   struct vk_command_buffer * const command_buffer_base,
   UNUSED VkCommandBufferResetFlags const flags)
{
   struct terakan_command_buffer * command_buffer =
      container_of(command_buffer_base, struct terakan_command_buffer, vk);

   vk_command_buffer_reset(&command_buffer->vk);

   terakan_command_buffer_release_resources(command_buffer);
}

static void
terakan_command_buffer_destroy(struct vk_command_buffer * const command_buffer_base)
{
   struct terakan_command_buffer * command_buffer =
      container_of(command_buffer_base, struct terakan_command_buffer, vk);

   terakan_command_buffer_release_resources(command_buffer);

   vk_command_buffer_finish(&command_buffer->vk);

   vk_free(&command_buffer->vk.pool->alloc, command_buffer);
}

static VkResult
terakan_command_buffer_create(struct vk_command_pool * const command_pool,
                              UNUSED VkCommandBufferLevel level,
                              struct vk_command_buffer ** const command_buffer_out)
{
   VkResult result;

   struct terakan_command_buffer * command_buffer = vk_alloc(
      &command_pool->alloc, sizeof(*command_buffer), alignof(*command_buffer),
      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (command_buffer == NULL) {
      return vk_error(command_pool->base.device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   result =
      vk_command_buffer_init(command_pool, &command_buffer->vk, &terakan_command_buffer_ops, 0);
   if (result != VK_SUCCESS) {
      vk_free(&command_pool->alloc, command_buffer);
      return result;
   }

   list_inithead(&command_buffer->submissions);

   *command_buffer_out = &command_buffer->vk;

   return VK_SUCCESS;
}

struct vk_command_buffer_ops const terakan_command_buffer_ops = {
   .create = terakan_command_buffer_create,
   .reset = terakan_command_buffer_reset,
   .destroy = terakan_command_buffer_destroy,
};

static void
terakan_command_pool_trim_resources(struct terakan_command_pool * const command_pool)
{
   list_for_each_entry_safe(
      struct terakan_command_buffer_submission_secondary_execution, submission,
      &command_pool->secondary_executions_free, free_link) {
      vk_free(&command_pool->vk.alloc, submission);
   }
   list_inithead(&command_pool->secondary_executions_free);

   list_for_each_entry_safe(
      struct terakan_command_buffer_submission_indirect_buffer, submission,
      &command_pool->indirect_buffers_free, free_link) {
      vk_free(&command_pool->vk.alloc, submission->indirect_buffer);
      vk_free(&command_pool->vk.alloc, submission->bo_references);
      vk_free(&command_pool->vk.alloc, submission);
   }
   list_inithead(&command_pool->indirect_buffers_free);
}

VKAPI_ATTR void VKAPI_CALL
terakan_TrimCommandPool(
   VkDevice const deviceHandle, VkCommandPool const commandPool,
   UNUSED VkCommandPoolTrimFlags const flags)
{
   struct terakan_command_pool * const command_pool = terakan_command_pool_from_handle(commandPool);

   vk_command_pool_trim(&command_pool->vk, flags);

   terakan_command_pool_trim_resources(command_pool);
}

VKAPI_ATTR void VKAPI_CALL
terakan_DestroyCommandPool(
   VkDevice const deviceHandle, VkCommandPool const commandPool,
   VkAllocationCallbacks const * const pAllocator)
{
   struct terakan_command_pool * const command_pool = terakan_command_pool_from_handle(commandPool);

   if (command_pool == NULL) {
      return;
   }

   /* Finish the command pool base before destroying their dependencies, as finishing the base
    * destroys all allocated command buffers, and resetting command buffers sends their dependencies
    * back to the free lists.
    */
   vk_command_pool_finish(&command_pool->vk);

   struct terakan_device const * const device = terakan_device_from_handle(deviceHandle);

   terakan_command_pool_trim_resources(command_pool);

   vk_free2(&device->vk.alloc, pAllocator, command_pool);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreateCommandPool(
   VkDevice const deviceHandle, VkCommandPoolCreateInfo const * const pCreateInfo,
   VkAllocationCallbacks const * const pAllocator, VkCommandPool * const pCommandPool)
{
   VkResult result;

   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   struct terakan_command_pool * const command_pool = vk_alloc2(
      &device->vk.alloc, pAllocator, sizeof(*command_pool), alignof(*command_pool),
      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (command_pool == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   list_inithead(&command_pool->indirect_buffers_free);

   list_inithead(&command_pool->secondary_executions_free);

   result = vk_command_pool_init(&device->vk, &command_pool->vk, pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, pAllocator, command_pool);
      return result;
   }

   *pCommandPool = terakan_command_pool_to_handle(command_pool);

   return VK_SUCCESS;
}
