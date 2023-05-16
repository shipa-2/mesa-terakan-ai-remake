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
#include "terakan_physical_device.h"
#include "terakan_sync_bo_wait_idle.h"
#include "terakan_queue.h"
#include "winsys/terakan_winsys.h"

#include "c11/threads.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_enum_to_str.h"
#include "vk_log.h"
#include "vk_sync.h"
#include "vk_sync_dummy.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static VkResult
terakan_queue_submit(struct vk_queue * const queue_base, struct vk_queue_submit * const submit)
{
   struct terakan_queue * const queue = container_of(queue_base, struct terakan_queue, vk);

   struct terakan_winsys * const winsys =
      container_of(queue->device->vk.physical, struct terakan_physical_device const, vk)->winsys;

   /* GFX command buffers must be padded to a multiple of 8 dwords with NOPs. */
   uint32_t const sync_indirect_buffer_max_dwords =
      ARRAY_SIZE(queue->sync_indirect_buffer) & ~(uint32_t)7;

   /* Submit synchronization waits. */
   uint32_t const sync_wait_reg_mem_and_mem_write_dwords = 7 + 2 + 5 + 2;
   for (uint32_t sync_waits_submitted = 0; sync_waits_submitted < submit->wait_count;) {
      terakan_bo_reference_writer_reset(
         &queue->sync_bo_reference_writer, queue->sync_bo_references);
      uint32_t sync_indirect_buffer_dwords = 0;

      uint32_t sync_wait_index;
      for (sync_wait_index = sync_waits_submitted; sync_wait_index < submit->wait_count;
           ++sync_wait_index) {
         struct vk_sync_wait const * const sync_wait_base = &submit->waits[sync_wait_index];
         if (sync_wait_base->sync->type == &vk_sync_dummy_type) {
            continue;
         }
         assert(sync_wait_base->sync->type == &terakan_sync_bo_wait_idle_type);

         if (sync_indirect_buffer_max_dwords - sync_indirect_buffer_dwords <
             sync_wait_reg_mem_and_mem_write_dwords) {
            break;
         }

         struct terakan_sync_bo_wait_idle const * const sync_wait =
            container_of(sync_wait_base->sync, struct terakan_sync_bo_wait_idle const, vk);

         uint32_t const sync_wait_bo_reference_index = terakan_bo_reference_writer_add_reference(
            &queue->sync_bo_reference_writer, sync_wait->bo, true, true,
            TERAKAN_WINSYS_CS_BO_PRIORITY_FENCE_TRACE);
         if (sync_wait_bo_reference_index == UINT32_MAX) {
            break;
         }

         /* Await the semaphore in ME. */
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] =
            PKT3(PKT3_WAIT_REG_MEM, 6 - 1, 0);
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] =
            WAIT_REG_MEM_MEMORY | WAIT_REG_MEM_GEQUAL;
         /* Lower address bits and endian swap. */
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] = 0;
         /* Higher address bits. */
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] = 0;
         /* Reference (>= 1). */
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] = 1;
         /* Mask. */
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] = UINT32_MAX;
         /* Polling interval. */
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] = 4;
         /* Relocation. */
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] = PKT3(PKT3_NOP, 1 - 1, 0);
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] = sync_wait_bo_reference_index;

         /* Reset the semaphore in ME. */
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] =
            PKT3(PKT3_MEM_WRITE, 4 - 1, 0);
         /* Lower address bits and endian swap. */
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] = 0;
         /* Flags and higher address bits. */
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] = MEM_WRITE_32_BITS;
         /* Data. */
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] = 1;
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] = 0;
         /* Relocation. */
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] = PKT3(PKT3_NOP, 1 - 1, 0);
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] = sync_wait_bo_reference_index;

         assert(sync_indirect_buffer_dwords <= sync_indirect_buffer_max_dwords);
      }

      if (sync_indirect_buffer_dwords != 0) {
         /* Pad the GFX ring indirect buffer to a multiple of 8 dwords with NOPs. */
         while ((sync_indirect_buffer_dwords & 7) != 0) {
            queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] = PKT_TYPE_S(2);
         }
         VkResult const sync_wait_submit_result = winsys->cs_fn->submit(
            winsys, queue->ip_type, queue->sync_bo_reference_writer.reference_count,
            queue->sync_bo_reference_writer.references, sync_indirect_buffer_dwords,
            queue->sync_indirect_buffer, false);
         if (sync_wait_submit_result != VK_SUCCESS) {
            /* Lose the device regardless of the actual result for this specific command buffer
             * because a part of the queue submission might have already been done, don't leave the
             * device in an indeterminate state.
             */
            return vk_device_set_lost(
               &queue->device->vk,
               "Synchronization wait command buffer submission failed with result %s",
               vk_Result_to_str(sync_wait_submit_result));
         }
      }

      sync_waits_submitted = sync_wait_index;
   }

   /* TODO(Triang3l): Submit the command buffers. */
   /* TODO(Triang3l): End of frame flag. */

   /* Submit synchronization signals. */
   uint32_t const sync_event_write_eop_dwords = 6 + 2;
   for (uint32_t sync_signals_submitted = 0; sync_signals_submitted < submit->signal_count;) {
      terakan_bo_reference_writer_reset(
         &queue->sync_bo_reference_writer, queue->sync_bo_references);
      uint32_t sync_indirect_buffer_dwords = 0;

      uint32_t sync_signal_index;
      for (sync_signal_index = sync_signals_submitted; sync_signal_index < submit->signal_count;
           ++sync_signal_index) {
         struct vk_sync_signal const * const sync_signal_base = &submit->signals[sync_signal_index];
         if (sync_signal_base->sync->type == &vk_sync_dummy_type) {
            continue;
         }
         assert(sync_signal_base->sync->type == &terakan_sync_bo_wait_idle_type);

         if (sync_indirect_buffer_max_dwords - sync_indirect_buffer_dwords <
             sync_event_write_eop_dwords) {
            break;
         }

         struct terakan_sync_bo_wait_idle const * const sync_signal =
            container_of(sync_signal_base->sync, struct terakan_sync_bo_wait_idle const, vk);

         uint32_t const sync_signal_bo_reference_index = terakan_bo_reference_writer_add_reference(
            &queue->sync_bo_reference_writer, sync_signal->bo, false, true,
            TERAKAN_WINSYS_CS_BO_PRIORITY_FENCE_TRACE);
         if (sync_signal_bo_reference_index == UINT32_MAX) {
            break;
         }

         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] =
            PKT3(PKT3_EVENT_WRITE_EOP, 5 - 1, 0);
         /* TODO(Triang3l): Correct event type. */
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] =
            EVENT_INDEX(5) | EVENT_TYPE(EVENT_TYPE_CACHE_FLUSH_AND_INV_TS_EVENT);
         /* Lower address bits. */
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] = 0;
         /* Data selection, interrupt selection, and higher address bits. */
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] =
            EOP_DATA_SEL(EOP_DATA_SEL_VALUE_32BIT);
         /* Data. */
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] = 1;
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] = 0;
         /* Relocation. */
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] = PKT3(PKT3_NOP, 1 - 1, 0);
         queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] =
            sync_signal_bo_reference_index;

         assert(sync_indirect_buffer_dwords <= sync_indirect_buffer_max_dwords);
      }

      if (sync_indirect_buffer_dwords != 0) {
         /* Pad the GFX ring indirect buffer to a multiple of 8 dwords with NOPs. */
         while ((sync_indirect_buffer_dwords & 7) != 0) {
            queue->sync_indirect_buffer[sync_indirect_buffer_dwords++] = PKT_TYPE_S(2);
         }
         VkResult const sync_wait_submit_result = winsys->cs_fn->submit(
            winsys, queue->ip_type, queue->sync_bo_reference_writer.reference_count,
            queue->sync_bo_reference_writer.references, sync_indirect_buffer_dwords,
            queue->sync_indirect_buffer, false);
         if (sync_wait_submit_result != VK_SUCCESS) {
            /* Lose the device regardless of the actual result for this specific command buffer
             * because a part of the queue submission might have already been done, don't leave the
             * device in an indeterminate state.
             */
            return vk_device_set_lost(
               &queue->device->vk,
               "Synchronization wait command buffer submission failed with result %s",
               vk_Result_to_str(sync_wait_submit_result));
         }

         /* Wake up waits-before-signals after the successful submission of signals. */
         for (uint32_t sync_wake_signal_index = sync_signals_submitted;
              sync_wake_signal_index < sync_signal_index; ++sync_wake_signal_index) {
            struct vk_sync_signal const * const sync_wake_signal_base =
               &submit->signals[sync_wake_signal_index];
            if (sync_wake_signal_base->sync->type == &vk_sync_dummy_type) {
               continue;
            }
            assert(sync_wake_signal_base->sync->type == &terakan_sync_bo_wait_idle_type);
            struct terakan_sync_bo_wait_idle * const sync_wake_signal =
               container_of(sync_wake_signal_base->sync, struct terakan_sync_bo_wait_idle, vk);
            mtx_lock(&sync_wake_signal->scheduled_mutex);
            sync_wake_signal->scheduled = true;
            mtx_unlock(&sync_wake_signal->scheduled_mutex);
            cnd_broadcast(&sync_wake_signal->scheduled_condition);
         }
      }

      sync_signals_submitted = sync_signal_index;
   }

   return VK_SUCCESS;
}

void
terakan_queue_destroy(struct terakan_queue * const queue)
{
   vk_free(&queue->device->vk.alloc, queue->sync_bo_references);

   vk_queue_finish(&queue->vk);

   vk_free(&queue->device->vk.alloc, queue);
}

VkResult
terakan_queue_create(
   struct terakan_device * const device, VkDeviceQueueCreateInfo const * const create_info,
   uint32_t const index_in_family, struct terakan_queue * * const queue_out)
{
   VkResult result;

   struct terakan_queue * const queue =
      vk_alloc(&device->vk.alloc, sizeof(*queue), alignof(*queue),
      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (queue == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   result = vk_queue_init(&queue->vk, &device->vk, create_info, index_in_family);
   if (result != VK_SUCCESS) {
      goto fail_alloc;
   }

   queue->device = device;

   queue->ip_type = AMD_IP_GFX;

   struct terakan_physical_device const * const physical_device =
      container_of(device->vk.physical, struct terakan_physical_device const, vk);

   queue->sync_bo_references = vk_alloc(
      &device->vk.alloc,
      physical_device->winsys->gpu_info.cs_bo_reference_size *
      TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT,
      physical_device->winsys->gpu_info.cs_bo_reference_alignment,
      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (queue->sync_bo_references == NULL) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_queue;
   }

   queue->vk.driver_submit = terakan_queue_submit;

   *queue_out = queue;

   return VK_SUCCESS;

fail_queue:
   vk_queue_finish(&queue->vk);
fail_alloc:
   vk_free(&device->vk.alloc, queue);
   return result;
}
