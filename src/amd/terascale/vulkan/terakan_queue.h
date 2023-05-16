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

#ifndef TERAKAN_QUEUE_H
#define TERAKAN_QUEUE_H

#include "terakan_command_buffer.h"

#include "vk_queue.h"

#include <stddef.h>
#include <stdint.h>

struct terakan_device;

struct terakan_queue {
   struct vk_queue vk;

   struct terakan_device * device;

   enum amd_ip_type ip_type;

   void * sync_bo_references;

   /* The last so these don't leave a lot of space between other fields. */
   struct terakan_bo_reference_writer sync_bo_reference_writer;
   uint32_t sync_indirect_buffer[TERAKAN_MAX_INDIRECT_BUFFER_SIZE_DWORDS];
};

VK_DEFINE_HANDLE_CASTS(terakan_queue, vk.base, VkQueue, VK_OBJECT_TYPE_QUEUE)

void terakan_queue_destroy(struct terakan_queue * queue);

VkResult terakan_queue_create(
   struct terakan_device * device, VkDeviceQueueCreateInfo const * create_info,
   uint32_t index_in_family, struct terakan_queue * * queue_out);

#endif /* TERAKAN_QUEUE_H */
