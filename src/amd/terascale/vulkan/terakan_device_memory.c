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

#include "terakan_device_memory.h"

#include "winsys/terakan_winsys.h"
#include "terakan_device.h"
#include "terakan_entrypoints.h"
#include "terakan_physical_device.h"

#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_log.h"

VKAPI_ATTR VkResult VKAPI_CALL
terakan_FlushMappedMemoryRanges(VkDevice const device, uint32_t const memoryRangeCount,
                                VkMappedMemoryRange const * const pMemoryRanges)
{
   /* All host-visible memory types are host-coherent currently. */
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_InvalidateMappedMemoryRanges(VkDevice const device, uint32_t const memoryRangeCount,
                                     VkMappedMemoryRange const * const pMemoryRanges)
{
   /* All host-visible memory types are host-coherent currently. */
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_UnmapMemory2KHR(VkDevice const deviceHandle,
                        VkMemoryUnmapInfoKHR const * const pMemoryUnmapInfo)
{
   struct terakan_device_memory * const device_memory =
      terakan_device_memory_from_handle(pMemoryUnmapInfo->memory);

   terakan_winsys_bo_unmap(device_memory->bo);

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_MapMemory2KHR(VkDevice const deviceHandle, VkMemoryMapInfoKHR const * const pMemoryMapInfo,
                      void ** const ppData)
{
   struct terakan_device_memory * const device_memory =
      terakan_device_memory_from_handle(pMemoryMapInfo->memory);

   void * const mapping = terakan_winsys_bo_map(device_memory->bo);
   if (mapping == NULL) {
      return vk_error(terakan_device_from_handle(deviceHandle), VK_ERROR_MEMORY_MAP_FAILED);
   }

   *ppData = mapping;

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
terakan_FreeMemory(VkDevice const deviceHandle, VkDeviceMemory const deviceMemory,
                   VkAllocationCallbacks const * const pAllocator)
{
   struct terakan_device_memory * const device_memory =
      terakan_device_memory_from_handle(deviceMemory);

   if (device_memory == NULL) {
      return;
   }

   terakan_winsys_bo_free(device_memory->bo);

   vk_object_free(&terakan_device_from_handle(deviceHandle)->vk, pAllocator, device_memory);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_AllocateMemory(VkDevice const deviceHandle,
                       VkMemoryAllocateInfo const * const pAllocateInfo,
                       VkAllocationCallbacks const * pAllocator, VkDeviceMemory * const pMemory)
{
   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   struct terakan_device_memory * const device_memory = vk_object_alloc(
      &device->vk, pAllocator, sizeof(*device_memory), VK_OBJECT_TYPE_DEVICE_MEMORY);
   if (device_memory == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   struct terakan_physical_device const * const physical_device =
      container_of(device->vk.physical, struct terakan_physical_device const, vk);

   device_memory->bo = physical_device->winsys->bo_fn->allocate_device_memory(
      physical_device->winsys, pAllocateInfo->allocationSize,
      physical_device->winsys->gpu_info.buffer_image_bo_alignment,
      physical_device->memory_properties.memoryTypes[pAllocateInfo->memoryTypeIndex].propertyFlags);
   if (device_memory->bo == NULL) {
      vk_object_free(&device->vk, pAllocator, device_memory);
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }

   *pMemory = terakan_device_memory_to_handle(device_memory);

   return VK_SUCCESS;
}
