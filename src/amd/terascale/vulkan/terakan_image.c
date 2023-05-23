/*
 * Copyright © 2023 Vitaliy Triang3l Kuzmin
 *
 * Based in part on r600_texture.c which is:
 * Copyright 2010 Jerome Glisse <glisse@freedesktop.org>
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
#include "terakan_device_memory.h"
#include "terakan_entrypoints.h"
#include "terakan_image.h"
#include "terakan_physical_device.h"
#include "winsys/terakan_winsys.h"

#include "gallium/drivers/r600/evergreend.h"
#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_format.h"
#include "vk_log.h"

#include <assert.h>
#include <stddef.h>

uint32_t
terakan_image_get_optimal_tiling_array_mode(VkImageCreateInfo const * const image_create_info)
{
   /* Multisampled images must be 2D-tiled. */
   if (image_create_info->samples > VK_SAMPLE_COUNT_1_BIT) {
      return V_028C70_ARRAY_2D_TILED_THIN1;
   }

   /* Handle common candidates for the linear mode.
    *
    * Depth/stencil and compressed images must be tiled.
    *
    * The "Tex2D UAV on cypress will fail/hang if tile mode is linear" note from the R800 AddrLib
    * must be taken into account if there's possibility of this image being used as a random access
    * target.
    *
    * 1D images must be linear - fixes storage image operations on 1D according to the Gallium R600
    * driver.
    *
    * Tiling doesn't work with the 422 (SUBSAMPLED) formats according to the Gallium R600 driver.
    */
   if (!vk_format_is_depth_or_stencil(image_create_info->format) &&
       !vk_format_is_compressed(image_create_info->format) &&
       (image_create_info->imageType == VK_IMAGE_TYPE_1D ||
        vk_format_description(image_create_info->format)->layout ==
        UTIL_FORMAT_LAYOUT_SUBSAMPLED)) {
      return V_028C70_ARRAY_LINEAR_ALIGNED;
   }

   /* Make small textures 1D-tiled, similar to how that's done in the Gallium R600 driver. */
   /* TODO(Triang3l): Debug flag for disabling 2D tiling. */
   if (image_create_info->extent.width <= 16 || image_create_info->extent.height <= 16) {
      return V_028C70_ARRAY_1D_TILED_THIN1;
   }

   /* Surface computation will switch to 1D if needed. */
   return V_028C70_ARRAY_2D_TILED_THIN1;
}

VKAPI_ATTR void VKAPI_CALL
terakan_GetDeviceImageMemoryRequirements(
   VkDevice const deviceHandle, VkDeviceImageMemoryRequirements const * const pInfo,
   VkMemoryRequirements2 * const pMemoryRequirements)
{
   struct terakan_device const * const device = terakan_device_from_handle(deviceHandle);
   struct terakan_physical_device const * const physical_device =
      container_of(device->vk.physical, struct terakan_physical_device const, vk);

   struct radeon_surf surface;
   if (physical_device->winsys->surface_fn->translate_image_create_info(
          physical_device->winsys, pInfo->pCreateInfo, &surface)) {
      pMemoryRequirements->memoryRequirements.size = surface.total_size;
      pMemoryRequirements->memoryRequirements.alignment = (VkDeviceSize)1 << surface.alignment_log2;
   } else {
      assert(!"Failed to translate the image creation info into surface info");
      pMemoryRequirements->memoryRequirements.size = 1;
      pMemoryRequirements->memoryRequirements.alignment = 1;
   }

   pMemoryRequirements->memoryRequirements.memoryTypeBits =
      ((uint32_t)1 << physical_device->memory_properties.memoryTypeCount) - 1;
}

/* Skipping the translation into the surface structure if it has already been done. */
VKAPI_ATTR void VKAPI_CALL
terakan_GetImageMemoryRequirements2(
   VkDevice const deviceHandle, VkImageMemoryRequirementsInfo2 const * const pInfo,
   VkMemoryRequirements2 * const pMemoryRequirements)
{
   struct terakan_image const * const image = terakan_image_from_handle(pInfo->image);
   pMemoryRequirements->memoryRequirements.size = image->surface.total_size;
   pMemoryRequirements->memoryRequirements.alignment =
      (VkDeviceSize)1 << image->surface.alignment_log2;

   struct terakan_device const * const device = terakan_device_from_handle(deviceHandle);
   struct terakan_physical_device const * const physical_device =
      container_of(device->vk.physical, struct terakan_physical_device const, vk);
   pMemoryRequirements->memoryRequirements.memoryTypeBits =
      ((uint32_t)1 << physical_device->memory_properties.memoryTypeCount) - 1;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_BindImageMemory2(
   VkDevice const device, uint32_t const bindInfoCount,
   VkBindImageMemoryInfo const * const pBindInfos)
{
   for (uint32_t bind_info_index = 0; bind_info_index < bindInfoCount; ++bind_info_index) {
      VkBindImageMemoryInfo const * const bind_info = &pBindInfos[bind_info_index];
      struct terakan_image * const image = terakan_image_from_handle(bind_info->image);
      image->bo = terakan_device_memory_from_handle(bind_info->memory)->bo;
      image->bo_offset = bind_info->memoryOffset;
   }

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
terakan_DestroyImage(
   VkDevice const deviceHandle, VkImage const imageHandle,
   VkAllocationCallbacks const * const pAllocator)
{
   struct terakan_image * const image = terakan_image_from_handle(imageHandle);

   if (image == NULL) {
      return;
   }

   struct terakan_device const * const device = terakan_device_from_handle(deviceHandle);

   vk_image_finish(&image->vk);

   vk_free2(&device->vk.alloc, pAllocator, image);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreateImage(
   VkDevice const deviceHandle, VkImageCreateInfo const * const pCreateInfo,
   VkAllocationCallbacks const * const pAllocator, VkImage * const pImage)
{
   VkResult result;

   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   struct terakan_image * const image = vk_alloc2(
      &device->vk.alloc, pAllocator, sizeof(*image), alignof(*image),
      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (image == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   vk_image_init(&device->vk, &image->vk, pCreateInfo);

   struct terakan_winsys const * const winsys =
      container_of(device->vk.physical, struct terakan_physical_device const, vk)->winsys;
   if (!winsys->surface_fn->translate_image_create_info(winsys, pCreateInfo, &image->surface)) {
      result = vk_errorf(
         device, VK_ERROR_UNKNOWN, "Failed to translate the image creation info into surface info");
      goto fail_image;
   }

   image->bo = NULL;
   image->bo_offset = 0;

   *pImage = terakan_image_to_handle(image);

   return VK_SUCCESS;

fail_image:
   vk_image_finish(&image->vk);
   vk_free2(&device->vk.alloc, pAllocator, image);
   return result;
}
