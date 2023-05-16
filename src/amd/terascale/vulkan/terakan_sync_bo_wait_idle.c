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

#include "terakan_physical_device.h"
#include "terakan_sync_bo_wait_idle.h"
#include "winsys/terakan_winsys.h"

#include "c11/threads.h"
#include "util/macros.h"
#include "util/timespec.h"
#include "vk_device.h"
#include "vk_log.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static VkResult
terakan_sync_bo_wait_idle_signal(
   struct vk_device * const device, struct vk_sync * const sync_base, uint64_t const value)
{
   struct terakan_sync_bo_wait_idle * const sync =
      container_of(sync_base, struct terakan_sync_bo_wait_idle, vk);

   *sync->bo_mapping = 1;

   mtx_lock(&sync->scheduled_mutex);
   sync->scheduled = true;
   mtx_unlock(&sync->scheduled_mutex);
   cnd_broadcast(&sync->scheduled_condition);

   return VK_SUCCESS;
}

static VkResult
terakan_sync_bo_wait_idle_reset(struct vk_device * const device, struct vk_sync * const sync_base)
{
   struct terakan_sync_bo_wait_idle * const sync =
      container_of(sync_base, struct terakan_sync_bo_wait_idle, vk);

   mtx_lock(&sync->scheduled_mutex);
   sync->scheduled = false;
   mtx_unlock(&sync->scheduled_mutex);

   *sync->bo_mapping = 0;

   return VK_SUCCESS;
}

static VkResult
terakan_sync_bo_wait_idle_move(
   struct vk_device * const device, struct vk_sync * destination_base, struct vk_sync * source_base)
{
   struct terakan_winsys * const winsys =
      container_of(device->physical, struct terakan_physical_device const, vk)->winsys;

   /* WAIT_REG_MEM requires 16-byte alignment. */
   struct terakan_winsys_bo * new_bo = winsys->bo_fn->allocate_device_memory(
      winsys, sizeof(uint32_t), sizeof(uint32_t) * 4,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
      VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
   if (new_bo == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }

   uint32_t volatile * const new_bo_mapping = winsys->bo_fn->map(new_bo);
   if (new_bo_mapping == NULL) {
      winsys->bo_fn->free(new_bo);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   /* Initialize to unsignaled. */
   *new_bo_mapping = 0;

   struct terakan_sync_bo_wait_idle * const destination =
      container_of(destination_base, struct terakan_sync_bo_wait_idle, vk);
   struct terakan_sync_bo_wait_idle * const source =
      container_of(source_base, struct terakan_sync_bo_wait_idle, vk);

   winsys->bo_fn->free(destination->bo);
   destination->bo = source->bo;
   destination->bo_mapping = source->bo_mapping;
   destination->scheduled = source->scheduled;

   source->bo = new_bo;
   source->bo_mapping = new_bo_mapping;
   source->scheduled = false;

   return VK_SUCCESS;
}

static VkResult
terakan_sync_bo_wait_idle_wait(
   struct vk_device * const device, struct vk_sync * const sync_base, uint64_t const wait_value,
   enum vk_sync_wait_flags const wait_flags, uint64_t const abs_timeout_ns)
{
   struct terakan_sync_bo_wait_idle * const sync =
      container_of(sync_base, struct terakan_sync_bo_wait_idle, vk);

   /* Quick early-out. */
   if (*sync->bo_mapping != 0) {
      return VK_SUCCESS;
   }

   /* Wait until a GPU signal is queued or a CPU signal is made. */

   struct timespec abs_timeout_timespec;
   timespec_from_nsec(&abs_timeout_timespec, abs_timeout_ns);

   mtx_lock(&sync->scheduled_mutex);
   while (true) {
      if (sync->scheduled) {
         break;
      }
      if (abs_timeout_ns == 0) {
         return VK_TIMEOUT;
      }
      int scheduled_condition_wait_result;
      if (abs_timeout_ns == OS_TIMEOUT_INFINITE) {
         scheduled_condition_wait_result =
            cnd_wait(&sync->scheduled_condition, &sync->scheduled_mutex);
      } else {
         scheduled_condition_wait_result = cnd_timedwait(
            &sync->scheduled_condition, &sync->scheduled_mutex, &abs_timeout_timespec);
         if (scheduled_condition_wait_result == thrd_timedout) {
            return VK_TIMEOUT;
         }
      }
      if (scheduled_condition_wait_result != thrd_success) {
         return vk_device_set_lost(
            device, "Failed to await the signal scheduled condition variable");
      }
   }
   mtx_unlock(&sync->scheduled_mutex);

   if (wait_flags & VK_SYNC_WAIT_PENDING) {
      return VK_SUCCESS;
   }

   /* If signaled on the CPU, the value will be set before the setting the scheduled flag - quick
    * early-out.
    */
   if (*sync->bo_mapping != 0) {
      return VK_SUCCESS;
   }

   /* Wait until the signaling command buffer has been completed. */

   struct terakan_winsys const * const winsys =
      container_of(device->physical, struct terakan_physical_device const, vk)->winsys;

   switch (winsys->bo_fn->wait_idle(sync->bo, abs_timeout_ns)) {
   case 0:
      if (*sync->bo_mapping == 0) {
         return vk_device_set_lost(
            device,
            "A synchronization buffer is not potentially used on the GPU, but the GPU has not "
            "signaled the synchronization object (or it has been reset before having been awaited "
            "by the application)");
      }
      return VK_SUCCESS;
   case -EBUSY:
      assert(abs_timeout_ns != OS_TIMEOUT_INFINITE);
      return VK_TIMEOUT;
   default:
      return vk_device_set_lost(
         device,
         "Failed to wait for the synchronization buffer not to be potentially used on the GPU");
   }
}

static void
terakan_sync_bo_wait_idle_finish(struct vk_device * const device, struct vk_sync * const sync_base)
{
   struct terakan_sync_bo_wait_idle * const sync =
      container_of(sync_base, struct terakan_sync_bo_wait_idle, vk);

   struct terakan_winsys const * const winsys =
      container_of(device->physical, struct terakan_physical_device const, vk)->winsys;

   winsys->bo_fn->free(sync->bo);
}

static VkResult
terakan_sync_bo_wait_idle_init(
   struct vk_device * const device, struct vk_sync * const sync_base, uint64_t const initial_value)
{
   VkResult result;

   struct terakan_sync_bo_wait_idle * const sync =
      container_of(sync_base, struct terakan_sync_bo_wait_idle, vk);

   struct terakan_winsys * const winsys =
      container_of(device->physical, struct terakan_physical_device const, vk)->winsys;

   /* WAIT_REG_MEM requires 16-byte alignment. */
   sync->bo = winsys->bo_fn->allocate_device_memory(
      winsys, sizeof(uint32_t), sizeof(uint32_t) * 4,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
      VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
   if (sync->bo == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }

   sync->bo_mapping = winsys->bo_fn->map(sync->bo);
   if (sync->bo_mapping == NULL) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_bo;
   }

   *sync->bo_mapping = (uint32_t)(initial_value != 0);

   if (mtx_init(&sync->scheduled_mutex, mtx_plain) != thrd_success) {
      result = vk_errorf(
         device, VK_ERROR_OUT_OF_HOST_MEMORY, "Failed to initialize the signal scheduled mutex");
      goto fail_bo;
   }
   if (cnd_init(&sync->scheduled_condition) != thrd_success) {
      result = vk_errorf(
         device, VK_ERROR_OUT_OF_HOST_MEMORY,
         "Failed to initialize the signal scheduled condition variable");
      goto fail_scheduled_mutex;
   }

   sync->scheduled = false;

   return VK_SUCCESS;

fail_scheduled_mutex:
   mtx_destroy(&sync->scheduled_mutex);
fail_bo:
   winsys->bo_fn->free(sync->bo);
   return result;
}

struct vk_sync_type const terakan_sync_bo_wait_idle_type = {
   .size = sizeof(struct terakan_sync_bo_wait_idle),
   .features =
      VK_SYNC_FEATURE_BINARY |
      VK_SYNC_FEATURE_GPU_WAIT |
      VK_SYNC_FEATURE_GPU_MULTI_WAIT |
      VK_SYNC_FEATURE_CPU_WAIT |
      VK_SYNC_FEATURE_CPU_RESET |
      VK_SYNC_FEATURE_CPU_SIGNAL |
      VK_SYNC_FEATURE_WAIT_PENDING,
   .init = terakan_sync_bo_wait_idle_init,
   .finish = terakan_sync_bo_wait_idle_finish,
   .signal = terakan_sync_bo_wait_idle_signal,
   .reset = terakan_sync_bo_wait_idle_reset,
   .move = terakan_sync_bo_wait_idle_move,
   .wait = terakan_sync_bo_wait_idle_wait,
};
