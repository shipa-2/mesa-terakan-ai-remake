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
#include "terakan_physical_device.h"

#include "gallium/drivers/r600/r600d_common.h"
#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_log.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void
terakan_bo_reference_writer_reset(struct terakan_bo_reference_writer * const writer,
                                  void * const bo_references)
{
   writer->references = bo_references;

   writer->reference_count = 0;

   BITSET_ZERO(writer->map_entries_used);
}

uint32_t
terakan_bo_reference_writer_add_reference(struct terakan_bo_reference_writer * const writer,
                                          struct terakan_winsys_bo const * const bo,
                                          bool const is_reading, bool const is_writing,
                                          enum terakan_winsys_cs_bo_priority const priority)
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
      uint32_t const check_hash = (hash + collisions) & TERAKAN_BO_REFERENCE_HASH_MASK;
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

static struct terakan_command_buffer_submission_indirect_buffer *
terakan_command_buffer_new_indirect_buffer(struct terakan_command_buffer * const command_buffer)
{
   if (vk_command_buffer_has_error(&command_buffer->vk)) {
      return NULL;
   }

   struct terakan_command_pool * const command_pool =
      container_of(command_buffer->vk.pool, struct terakan_command_pool, vk);

   struct terakan_command_buffer_submission_indirect_buffer * indirect_buffer;
   if (!list_is_empty(&command_pool->indirect_buffers_free)) {
      indirect_buffer =
         list_first_entry(&command_pool->indirect_buffers_free,
                          struct terakan_command_buffer_submission_indirect_buffer, free_link);
      list_del(&indirect_buffer->free_link);
   } else {
      indirect_buffer = vk_alloc(&command_pool->vk.alloc, sizeof(*indirect_buffer),
                                 alignof(*indirect_buffer), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (indirect_buffer == NULL) {
         vk_command_buffer_set_error(&command_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         return NULL;
      }

      indirect_buffer->base.is_secondary_execution = false;

      struct terakan_gpu_info const * const gpu_info =
         &container_of(command_buffer->vk.pool->base.device->physical,
                       struct terakan_physical_device const, vk)
             ->winsys->gpu_info;

      indirect_buffer->bo_references =
         vk_alloc(&command_pool->vk.alloc,
                  gpu_info->cs_bo_reference_size * TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT,
                  gpu_info->cs_bo_reference_alignment, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (indirect_buffer->bo_references == NULL) {
         vk_command_buffer_set_error(&command_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         vk_free(&command_pool->vk.alloc, indirect_buffer);
         return NULL;
      }

      indirect_buffer->indirect_buffer = vk_alloc(
         &command_pool->vk.alloc, sizeof(uint32_t) * TERAKAN_MAX_INDIRECT_BUFFER_SIZE_DWORDS,
         alignof(uint32_t), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (indirect_buffer->indirect_buffer == NULL) {
         vk_command_buffer_set_error(&command_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         vk_free(&command_pool->vk.alloc, indirect_buffer->bo_references);
         vk_free(&command_pool->vk.alloc, indirect_buffer);
         return NULL;
      }
   }

   indirect_buffer->bo_reference_count = 0;
   indirect_buffer->indirect_buffer_size_dwords = 0;

   list_addtail(&indirect_buffer->base.command_buffer_submission_link,
                &command_buffer->submissions);

   return indirect_buffer;
}

static void
terakan_command_buffer_release_resources(struct terakan_command_buffer * const command_buffer)
{
   struct terakan_command_pool * const command_pool =
      container_of(command_buffer->vk.pool, struct terakan_command_pool, vk);

   if (command_buffer->command_writer.gfx != NULL) {
      struct terakan_command_pool * const command_pool =
         container_of(command_buffer->vk.pool, struct terakan_command_pool, vk);
      list_add(&command_buffer->command_writer.gfx->base.free_link,
               &command_pool->command_writers_free);
      command_buffer->command_writer.gfx = NULL;
   }

   list_for_each_entry_safe(struct terakan_command_buffer_submission, submission_base,
                            &command_buffer->submissions, command_buffer_submission_link)
   {
      list_del(&submission_base->command_buffer_submission_link);
      if (submission_base->is_secondary_execution) {
         struct terakan_command_buffer_submission_secondary_execution * const submission =
            container_of(submission_base,
                         struct terakan_command_buffer_submission_secondary_execution, base);
         list_add(&submission->free_link, &command_pool->secondary_executions_free);
      } else {
         struct terakan_command_buffer_submission_indirect_buffer * const submission = container_of(
            submission_base, struct terakan_command_buffer_submission_indirect_buffer, base);
         list_add(&submission->free_link, &command_pool->indirect_buffers_free);
      }
   }
}

static void
terakan_command_buffer_reset(struct vk_command_buffer * const command_buffer_base,
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

   struct terakan_command_buffer * command_buffer =
      vk_alloc(&command_pool->alloc, sizeof(*command_buffer), alignof(*command_buffer),
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

   command_buffer->command_writer.gfx = NULL;

   *command_buffer_out = &command_buffer->vk;

   return VK_SUCCESS;
}

struct vk_command_buffer_ops const terakan_command_buffer_ops = {
   .create = terakan_command_buffer_create,
   .reset = terakan_command_buffer_reset,
   .destroy = terakan_command_buffer_destroy,
};

static void
terakan_gfx_command_writer_end_indirect_buffer(
   struct terakan_gfx_command_writer * const command_writer)
{
   if (command_writer->indirect_buffer == NULL) {
      return;
   }

   command_writer->indirect_buffer->bo_reference_count =
      command_writer->base.bo_reference_writer.reference_count;

   /* Pad the GFX ring indirect buffer to a multiple of 8 dwords with NOPs. */
   while ((command_writer->indirect_buffer->indirect_buffer_size_dwords & 7) != 0) {
      command_writer->indirect_buffer
         ->indirect_buffer[command_writer->indirect_buffer->indirect_buffer_size_dwords++] =
         PKT_TYPE_S(2);
   }

   command_writer->indirect_buffer = NULL;
}

static bool
terakan_gfx_command_writer_new_indirect_buffer(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_gfx_command_writer_end_indirect_buffer(command_writer);

   command_writer->indirect_buffer =
      terakan_command_buffer_new_indirect_buffer(command_writer->base.command_buffer);
   if (command_writer->indirect_buffer == NULL) {
      return false;
   }

   terakan_bo_reference_writer_reset(&command_writer->base.bo_reference_writer,
                                     command_writer->indirect_buffer->bo_references);

   command_writer->is_beginning_indirect_buffer = true;

   if (command_writer->indirect_buffer_ever_begun) {
      /* Re-emit the state from the previous indirect buffer. */
      terakan_hw_state_draw_emit_all(command_writer);
   }
   command_writer->indirect_buffer_ever_begun = true;

   command_writer->is_beginning_indirect_buffer = false;

   return !vk_command_buffer_has_error(&command_writer->base.command_buffer->vk);
}

uint32_t *
terakan_gfx_command_writer_emit(struct terakan_gfx_command_writer * const command_writer,
                                uint32_t const packet_dwords, uint32_t const bo_count,
                                uint32_t const relocation_packet_dwords)
{
   if (unlikely(vk_command_buffer_has_error(&command_writer->base.command_buffer->vk))) {
      return NULL;
   }

   uint32_t const total_packet_dwords = packet_dwords + relocation_packet_dwords;

   uint32_t const indirect_buffer_max_dwords =
      TERAKAN_MAX_INDIRECT_BUFFER_SIZE_DWORDS & ~((uint32_t)7);

   if (command_writer->indirect_buffer == NULL ||
       (indirect_buffer_max_dwords - command_writer->indirect_buffer->indirect_buffer_size_dwords) <
          total_packet_dwords ||
       (TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT -
        command_writer->base.bo_reference_writer.reference_count) < bo_count) {
      assert(!command_writer->is_beginning_indirect_buffer);
      if (unlikely(command_writer->is_beginning_indirect_buffer)) {
         /* Possibly a recursive overflow while moving to the new indirect buffer, if this happens,
          * it's a Terakan bug.
          */
         vk_command_buffer_set_error(&command_writer->base.command_buffer->vk,
                                     VK_ERROR_OUT_OF_HOST_MEMORY);
         return NULL;
      }
      if (!terakan_gfx_command_writer_new_indirect_buffer(command_writer)) {
         return NULL;
      }
   }

   if (unlikely((indirect_buffer_max_dwords -
                 command_writer->indirect_buffer->indirect_buffer_size_dwords) <
                   total_packet_dwords ||
                (TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT -
                 command_writer->base.bo_reference_writer.reference_count) < bo_count)) {
      assert(
         !"A single command emission is too large, no space even after moving to the new indirect "
          "buffer");
      vk_command_buffer_set_error(&command_writer->base.command_buffer->vk,
                                  VK_ERROR_OUT_OF_HOST_MEMORY);
      return NULL;
   }

   uint32_t * const indirect_buffer_allocation =
      command_writer->indirect_buffer->indirect_buffer +
      command_writer->indirect_buffer->indirect_buffer_size_dwords;
   command_writer->indirect_buffer->indirect_buffer_size_dwords += total_packet_dwords;
   return indirect_buffer_allocation;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_EndCommandBuffer(VkCommandBuffer const commandBuffer)
{
   struct terakan_command_buffer * const command_buffer =
      terakan_command_buffer_from_handle(commandBuffer);

   terakan_gfx_command_writer_end_indirect_buffer(command_buffer->command_writer.gfx);

   return vk_command_buffer_end(&command_buffer->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_BeginCommandBuffer(VkCommandBuffer const commandBuffer,
                           VkCommandBufferBeginInfo const * const pBeginInfo)
{
   struct terakan_command_buffer * const command_buffer =
      terakan_command_buffer_from_handle(commandBuffer);

   vk_command_buffer_begin(&command_buffer->vk, pBeginInfo);

   struct terakan_command_pool * const command_pool =
      container_of(command_buffer->vk.pool, struct terakan_command_pool, vk);

   assert(command_buffer->command_writer.gfx == NULL);
   if (!list_is_empty(&command_pool->command_writers_free)) {
      command_buffer->command_writer.gfx = list_first_entry(
         &command_pool->command_writers_free, struct terakan_gfx_command_writer, base.free_link);
      list_del(&command_buffer->command_writer.gfx->base.free_link);
   } else {
      command_buffer->command_writer.gfx =
         vk_alloc(&command_pool->vk.alloc, sizeof(*command_buffer->command_writer.gfx),
                  alignof(*command_buffer->command_writer.gfx), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (command_buffer->command_writer.gfx == NULL) {
         return vk_command_buffer_set_error(&command_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   }

   command_buffer->command_writer.gfx->base.command_buffer = command_buffer;

   command_buffer->command_writer.gfx->indirect_buffer_ever_begun = false;

   /* The first emission will request the first indirect buffer. */
   command_buffer->command_writer.gfx->is_beginning_indirect_buffer = false;

   terakan_hw_state_draw_reset(&command_buffer->command_writer.gfx->hw_state_draw);

   terakan_state_draw_reset(&command_buffer->command_writer.gfx->state_draw);

   return vk_command_buffer_get_record_result(&command_buffer->vk);
}

static void
terakan_command_pool_trim_resources(struct terakan_command_pool * const command_pool)
{
   list_for_each_entry_safe(struct terakan_gfx_command_writer, command_writer,
                            &command_pool->command_writers_free, base.free_link)
   {
      vk_free(&command_pool->vk.alloc, command_writer);
   }
   list_inithead(&command_pool->command_writers_free);

   list_for_each_entry_safe(struct terakan_command_buffer_submission_secondary_execution,
                            submission, &command_pool->secondary_executions_free, free_link)
   {
      vk_free(&command_pool->vk.alloc, submission);
   }
   list_inithead(&command_pool->secondary_executions_free);

   list_for_each_entry_safe(struct terakan_command_buffer_submission_indirect_buffer, submission,
                            &command_pool->indirect_buffers_free, free_link)
   {
      vk_free(&command_pool->vk.alloc, submission->indirect_buffer);
      vk_free(&command_pool->vk.alloc, submission->bo_references);
      vk_free(&command_pool->vk.alloc, submission);
   }
   list_inithead(&command_pool->indirect_buffers_free);
}

VKAPI_ATTR void VKAPI_CALL
terakan_TrimCommandPool(VkDevice const deviceHandle, VkCommandPool const commandPool,
                        UNUSED VkCommandPoolTrimFlags const flags)
{
   struct terakan_command_pool * const command_pool = terakan_command_pool_from_handle(commandPool);

   vk_command_pool_trim(&command_pool->vk, flags);

   terakan_command_pool_trim_resources(command_pool);
}

VKAPI_ATTR void VKAPI_CALL
terakan_DestroyCommandPool(VkDevice const deviceHandle, VkCommandPool const commandPool,
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
terakan_CreateCommandPool(VkDevice const deviceHandle,
                          VkCommandPoolCreateInfo const * const pCreateInfo,
                          VkAllocationCallbacks const * const pAllocator,
                          VkCommandPool * const pCommandPool)
{
   VkResult result;

   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   struct terakan_command_pool * const command_pool =
      vk_alloc2(&device->vk.alloc, pAllocator, sizeof(*command_pool), alignof(*command_pool),
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (command_pool == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   list_inithead(&command_pool->indirect_buffers_free);

   list_inithead(&command_pool->secondary_executions_free);

   list_inithead(&command_pool->command_writers_free);

   result = vk_command_pool_init(&device->vk, &command_pool->vk, pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, pAllocator, command_pool);
      return result;
   }

   *pCommandPool = terakan_command_pool_to_handle(command_pool);

   return VK_SUCCESS;
}
