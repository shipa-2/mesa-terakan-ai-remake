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

   /* Submit the command buffers. */
   /* TODO(Triang3l): Implement inheritance in secondary command buffers.
    *
    * Instead of writing dwords with the real register values and relocations to the secondary
    * indirect buffers for inherited bindings, write dword-sized substitution tokens in place of the
    * real indirect buffer dwords that form a linked list inside the indirect buffer submission.
    *
    * A substitution token should contain:
    * - The value of which field (including relocations) of which view the dword should be replaced
    *   with.
    * - The attachment number within the subpass the value needs to be taken from (for color
    *   targets, it may be different from the CB_COLOR# index due to MRT and RAT indices being
    *   compacted skipping those not used in the fragment shader).
    * - Offset to the next substitution token. It can be stored in 16 bits since 2^16 dwords is
    *   essentially the maximum indirect buffer size on Linux Radeon 2.50.0 (when using virtual
    *   memory), and in this case to make sure the dword 0xFFFF can be addressed too, an offset
    *   relative to the current dword can be stored, so that 0 will be the terminator.
    *
    * During queue submission, the secondary indirect buffer should be copied to a temporary
    * indirect buffer in the queue (the same indirect buffer that is used for synchronization), and
    * references to inherited BOs also need to be added to the submission.
    *
    * It must be guaranteed that during submission, there will be free space for all the attachment
    * references in the BO list - either more space needs to be allocated in the temporary BO list,
    * or some needs to be reserved in the secondary indirect buffer submission (however, BO
    * references from the secondary command buffer still must be copied into the temporary buffer,
    * because reading / writing flags and priorities of _existing_ BO references in the secondary
    * command buffer may be touched too by the execution depending on whether the secondary indirect
    * buffer already references something that's inherited but possibly in a different way, and that
    * depends on the location where it's executed, and in general a secondary command buffer can be
    * executed in any queue and thus without external synchronization, it just happens that at the
    * moment this comment is written Terakan has only one queue per type).
    *
    * Because some BOs that may be inherited at the execution location may also happen to be
    * referenced by the secondary indirect buffer itself directly and possibly with different
    * reading / writing flags and priority, the BO reference hash map also needs to be preserved or
    * reconstructed for secondary indirect buffers referencing any inherited BOs.
    *
    * Note that if virtual memory is used, there won't be relocations, so BO references need to be
    * created not only by substitution tokens for relocations, but also by tokens that will be
    * replaced with virtual addresses.
    */
   for (uint32_t command_buffer_index = 0; command_buffer_index < submit->command_buffer_count;
        ++command_buffer_index) {
      struct terakan_command_buffer const * const command_buffer = container_of(
         submit->command_buffers[command_buffer_index], struct terakan_command_buffer const, vk);
      list_for_each_entry(
         struct terakan_command_buffer_submission, command_buffer_submission_base,
         &command_buffer->submissions, command_buffer_submission_link) {
         struct terakan_command_buffer_submission_indirect_buffer const *
         command_buffer_indirect_buffer;
         if (command_buffer_submission_base->is_secondary_execution) {
            struct terakan_command_buffer_submission_secondary_execution const *
            command_buffer_submission = container_of(
               command_buffer_submission_base,
               struct terakan_command_buffer_submission_secondary_execution const, base);
            command_buffer_indirect_buffer = command_buffer_submission->indirect_buffer;
         } else {
            command_buffer_indirect_buffer = container_of(
               command_buffer_submission_base,
               struct terakan_command_buffer_submission_indirect_buffer const, base);
         }
         /* TODO(Triang3l): End of frame flag. */
         VkResult const command_buffer_submit_result = winsys->cs_fn->submit(
            winsys, queue->ip_type, command_buffer_indirect_buffer->bo_reference_count,
            command_buffer_indirect_buffer->bo_references,
            command_buffer_indirect_buffer->indirect_buffer_size_dwords,
            command_buffer_indirect_buffer->indirect_buffer, false);
         if (command_buffer_submit_result != VK_SUCCESS) {
            /* Lose the device regardless of the actual result for this specific command buffer
             * because a part of the queue submission might have already been done, don't leave the
             * device in an indeterminate state.
             */
            return vk_device_set_lost(
               &queue->device->vk, "Command buffer submission failed with result %s",
               vk_Result_to_str(command_buffer_submit_result));
         }
      }
   }

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
