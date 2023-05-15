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

#include "terakan_device.h"
#include "terakan_entrypoints.h"
#include "terakan_physical_device.h"

#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_log.h"
#include "wsi_common.h"

#include <stdbool.h>
#include <stddef.h>

VKAPI_ATTR void VKAPI_CALL
terakan_DestroyDevice(VkDevice const deviceHandle, VkAllocationCallbacks const * const pAllocator)
{
   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   if (device == NULL) {
      return;
   }

   vk_device_finish(&device->vk);

   vk_free(&device->vk.alloc, device);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreateDevice(
   VkPhysicalDevice const physicalDevice, VkDeviceCreateInfo const * const pCreateInfo,
   VkAllocationCallbacks const * const pAllocator, VkDevice * const pDevice)
{
   VkResult result;

   struct terakan_physical_device * const physical_device =
      terakan_physical_device_from_handle(physicalDevice);

   struct terakan_device * const device = vk_alloc2(
      &physical_device->vk.instance->alloc, pAllocator, sizeof(*device), alignof(*device),
      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (device == NULL) {
      return vk_error(physical_device->vk.instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   struct vk_device_dispatch_table dispatch_table;
   vk_device_dispatch_table_from_entrypoints(&dispatch_table, &terakan_device_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(&dispatch_table, &wsi_device_entrypoints, false);

   result =
      vk_device_init(&device->vk, &physical_device->vk, &dispatch_table, pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(&device->vk.alloc, device);
      return result;
   }

   *pDevice = terakan_device_to_handle(device);

   return VK_SUCCESS;
}
