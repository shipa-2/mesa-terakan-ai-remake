/*
 * Copyright © 2024 Vitaliy Triang3l Kuzmin
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

#include "terakan_device_wddm.h"
#include "terakan_queue.h"

#include "util/macros.h"

#include "vk_alloc.h"

#include <stddef.h>

static VkResult
terakan_queue_wddm_submit(UNUSED struct terakan_device * const device_base,
                          UNUSED enum amd_ip_type const ip_type,
                          UNUSED uint32_t const bo_reference_count,
                          UNUSED void const * const bo_references,
                          UNUSED uint32_t const indirect_buffer_size_dwords,
                          UNUSED uint32_t const * const indirect_buffer)
{
   /* TODO(Triang3l): Accept the queue object that contains the context pointers, and D3DKMTRender.
    */
   return VK_ERROR_UNKNOWN;
}

struct terakan_queue_completion_submission_wddm {
   struct terakan_queue_completion_submission base;
};

static VkResult
terakan_queue_completion_submission_wddm_submit(
   UNUSED struct terakan_queue_completion_submission * const submission_base,
   UNUSED uint32_t const signal_indirect_buffer_size_dwords,
   UNUSED uint32_t const * const signal_indirect_buffer)
{
   /* TODO(Triang3l): Implement submission completion signals. */
   return VK_ERROR_UNKNOWN;
}

static bool
terakan_queue_completion_submission_wddm_await(
   UNUSED struct terakan_queue_completion_submission * const submission_base)
{
   /* TODO(Triang3l): Implement submission completion signals. */
   return false;
}

static void
terakan_queue_completion_submission_wddm_finish_winsys_and_free(
   struct terakan_queue_completion_submission * const submission_base)
{
   struct terakan_queue_completion_submission_wddm * const submission =
      container_of(submission_base, struct terakan_queue_completion_submission_wddm, base);

   /* TODO(Triang3l): Implement submission completion signals. */

   vk_free(&submission->base.queue->vk.base.device->alloc, submission);
}

static VkResult
terakan_queue_completion_submission_wddm_alloc_and_init_winsys(
   struct terakan_queue * const queue,
   struct terakan_queue_completion_submission ** const submission_out)
{
   VkResult result;

   struct terakan_device * const device =
      container_of(queue->vk.base.device, struct terakan_device, vk);

   struct terakan_queue_completion_submission_wddm * const submission = vk_alloc(
      &device->vk.alloc, sizeof(struct terakan_queue_completion_submission_wddm),
      alignof(struct terakan_queue_completion_submission_wddm), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (submission == NULL) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   /* TODO(Triang3l): Implement submission completion signals. */

   *submission_out = &submission->base;
   return VK_SUCCESS;
}

struct terakan_queue_winsys_fn const terakan_queue_wddm_fn = {
   .submit = terakan_queue_wddm_submit,
   .completion_submission_submit = terakan_queue_completion_submission_wddm_submit,
   .completion_submission_await = terakan_queue_completion_submission_wddm_await,
   .completion_submission_finish_winsys_and_free =
      terakan_queue_completion_submission_wddm_finish_winsys_and_free,
   .completion_submission_alloc_and_init_winsys =
      terakan_queue_completion_submission_wddm_alloc_and_init_winsys,
};
