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
#include "terakan_format.h"
#include "terakan_gpu_info.h"
#include "terakan_image.h"
#include "terakan_physical_device.h"
#include "winsys/terakan_winsys.h"

#include "gallium/drivers/r600/evergreend.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "vk_alloc.h"
#include "vk_format.h"
#include "vk_log.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

uint32_t
terakan_image_get_optimal_tiling_array_mode(VkImageCreateInfo const * const image_create_info)
{
   if (terakan_format_is_linear_only(image_create_info->format)) {
      return V_028C70_ARRAY_LINEAR_ALIGNED;
   }

   /* Multisampled images must be 2D-tiled. */
   if (image_create_info->samples > VK_SAMPLE_COUNT_1_BIT) {
      return V_028C70_ARRAY_2D_TILED_THIN1;
   }

   /* Handle common candidates for the linear mode.
    *
    * Depth / stencil attachments must be tiled.
    *
    * The "Tex2D UAV on cypress will fail/hang if tile mode is linear" note from the R800 AddrLib
    * must be taken into account if there's possibility of this image being used as a random access
    * target.
    *
    * 1D storage images must be linear according to the Gallium R600 driver, and overall linear is
    * more compact for them (storage image usage is not supported in Terakan for formats that must
    * be tiled).
    */
   if (!(image_create_info->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
       !terakan_format_is_tiled_only(image_create_info->format) &&
       image_create_info->imageType == VK_IMAGE_TYPE_1D) {
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

VKAPI_ATTR void VKAPI_CALL
terakan_GetImageSubresourceLayout(
   VkDevice const device, VkImage const imageHandle, VkImageSubresource const * pSubresource,
   VkSubresourceLayout * const pLayout)
{
   struct terakan_image const * const image = terakan_image_from_handle(imageHandle);

   bool const is_stencil_layout =
      pSubresource->aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT &&
      terakan_image_ac_surface_has_separate_stencil_layout(image->vk.format);
   struct legacy_surf_level const * const level =
      is_stencil_layout
         ? &image->surface.u.legacy.zs.stencil_level[pSubresource->mipLevel]
         : &image->surface.u.legacy.level[pSubresource->mipLevel];

   VkDeviceSize const slice_size = sizeof(uint32_t) * (VkDeviceSize)level->slice_size_dw;

   pLayout->offset = 256 * (VkDeviceSize)level->offset_256B + slice_size * pSubresource->arrayLayer;

   pLayout->size = slice_size;
   if (image->vk.image_type == VK_IMAGE_TYPE_3D) {
      pLayout->size *= u_minify(image->vk.extent.depth, pSubresource->mipLevel);
   }

   /* nblk is expected to have already been aligned appropriately in the surface computation.
    * vkGetImageSubresourceLayout is for linear images only - no need to take MSAA samples into
    * account.
    */
   pLayout->rowPitch = (is_stencil_layout ? 1 : image->surface.bpe) * (VkDeviceSize)level->nblk_x;

   pLayout->arrayPitch = slice_size;
   pLayout->depthPitch = slice_size;
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

bool
terakan_image_create_color_descriptor(
   VkImageViewCreateInfo const * const image_view_create_info,
   struct terakan_color_descriptor * const descriptor_out,
   struct terakan_color_meta_descriptor * const meta_descriptor_out_opt)
{
   bool const is_stencil_aspect =
      image_view_create_info->subresourceRange.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT;

   uint32_t color_format, number_type, swap;
   if (is_stencil_aspect && vk_format_has_depth(image_view_create_info->format)) {
      /* The getters return the values for the depth aspect, not stencil. */
      color_format = V_028C70_COLOR_8;
      number_type = V_028C70_NUMBER_UINT;
      swap = V_028C70_SWAP_STD;
   } else {
      color_format = terakan_format_color_get_format(image_view_create_info->format);
      if (color_format == V_028C70_COLOR_INVALID) {
         return false;
      }
      number_type = terakan_format_color_get_number_type(image_view_create_info->format);
      if (number_type == UINT32_MAX) {
         return false;
      }
      swap = terakan_format_color_get_swap(image_view_create_info->format);
      if (swap == UINT32_MAX) {
         return false;
      }
   }

   struct terakan_image const * const image =
      terakan_image_from_handle(image_view_create_info->image);

   bool const is_stencil_layout =
      is_stencil_aspect && terakan_image_ac_surface_has_separate_stencil_layout(image->vk.format);
   struct legacy_surf_level const * const level =
      is_stencil_layout
         ? &image->surface.u.legacy.zs.stencil_level[
               image_view_create_info->subresourceRange.baseMipLevel]
         : &image->surface.u.legacy.level[image_view_create_info->subresourceRange.baseMipLevel];
   /* Only LINEAR_ALIGNED is currently supported for linear, not LINEAR_GENERAL. */
   assert(level->mode != (enum radeon_surf_mode)0);

   descriptor_out->base = image->bo_offset / 256 + level->offset_256B;

   /* nblk is expected to have already been aligned appropriately in the surface computation. */
   uint32_t pitch_elements = level->nblk_x * (uint32_t)image->vk.samples;
   descriptor_out->pitch = S_028C64_PITCH_TILE_MAX(pitch_elements / 8 - 1);

   /* Linear pitch is always at least 64 elements, micro-tiles are 8x8. */
   descriptor_out->slice = S_028C68_SLICE_TILE_MAX(pitch_elements * level->nblk_y / 64 - 1);

   descriptor_out->view =
      S_028C6C_SLICE_START(image_view_create_info->subresourceRange.baseArrayLayer) |
      S_028C6C_SLICE_MAX(
         (image_view_create_info->subresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS
             ? image->vk.array_layers
             : image_view_create_info->subresourceRange.baseArrayLayer +
               image_view_create_info->subresourceRange.layerCount) - 1);

   bool blend_clamp = false;
   uint32_t source_format = V_028C70_EXPORT_4C_32BPC;
   switch (number_type) {
   case V_028C70_NUMBER_UNORM:
   case V_028C70_NUMBER_SNORM:
   case V_028C70_NUMBER_SRGB:
      blend_clamp = true;
      if (TERAKAN_FORMAT_COLOR_16BPC_EXPORT_NORM_FORMATS & ((uint64_t)1 << color_format)) {
         source_format = V_028C70_EXPORT_4C_16BPC;
      }
      break;
   case V_028C70_NUMBER_FLOAT:
      if (TERAKAN_FORMAT_COLOR_16BPC_EXPORT_FLOAT_FORMATS & ((uint64_t)1 << color_format)) {
         source_format = V_028C70_EXPORT_4C_16BPC;
      }
      break;
   default:
      break;
   }
   descriptor_out->info =
      S_028C70_FORMAT(color_format) |
      S_028C70_ARRAY_MODE(terakan_image_array_mode_ac_to_hw(level->mode)) |
      S_028C70_NUMBER_TYPE(number_type) |
      S_028C70_COMP_SWAP(swap) |
      S_028C70_SIMPLE_FLOAT(1) |
      S_028C70_SOURCE_FORMAT(source_format);
   if (terakan_format_color_is_blendable(color_format, number_type)) {
      descriptor_out->info |= S_028C70_BLEND_CLAMP(blend_clamp);
   } else {
      descriptor_out->info |= S_028C70_BLEND_BYPASS(1);
   }

   struct terakan_gpu_info const * const gpu_info =
      &container_of(image->vk.base.device->physical, struct terakan_physical_device const, vk)->
          winsys->gpu_info;
   descriptor_out->attrib =
      S_028C74_NON_DISP_TILING_ORDER(
         level->mode <= RADEON_SURF_MODE_LINEAR_ALIGNED ||
         vk_format_is_depth_or_stencil(image->vk.format)) |
      S_028C74_TILE_SPLIT(
         terakan_image_tile_split_bytes_to_hw(image->surface.u.legacy.tile_split)) |
      S_028C74_NUM_BANKS(gpu_info->tile_banks_log2 - 1) |
      S_028C74_BANK_WIDTH(util_logbase2(image->surface.u.legacy.bankw)) |
      S_028C74_BANK_HEIGHT(util_logbase2(image->surface.u.legacy.bankh)) |
      S_028C74_MACRO_TILE_ASPECT(util_logbase2(image->surface.u.legacy.mtilea)) |
      S_028C74_FMASK_BANK_HEIGHT(util_logbase2(image->surface.u.legacy.bankh));
   if (gpu_info->gfx_level >= CAYMAN) {
      /* Cayman has EQAA, and additionally doesn't support displayable tiling for 128 bits per pixel
       * color targets.
       */
      unsigned const samples_log2 = util_logbase2((uint32_t)image->vk.samples);
      enum pipe_swizzle const alpha_swizzle =
         (enum pipe_swizzle)vk_format_description(image_view_create_info->format)->swizzle[3];
      descriptor_out->attrib |=
         S_028C74_NON_DISP_TILING_ORDER(image->surface.bpe >= 16) |
         S_028C74_NUM_SAMPLES(samples_log2) |
         S_028C74_NUM_FRAGMENTS(samples_log2) |
         S_028C74_FORCE_DST_ALPHA_1(
            alpha_swizzle == PIPE_SWIZZLE_1 || alpha_swizzle == PIPE_SWIZZLE_NONE);
   }

   /* Allowing uncompressed views of compressed textures. */
   descriptor_out->dim =
      S_028C78_WIDTH_MAX(
         (u_minify(image->vk.extent.width, image_view_create_info->subresourceRange.baseMipLevel) +
          (image->surface.blk_w - 1)) /
         image->surface.blk_w - 1) |
      S_028C78_HEIGHT_MAX(
         (u_minify(image->vk.extent.height, image_view_create_info->subresourceRange.baseMipLevel) +
          (image->surface.blk_h - 1)) /
         image->surface.blk_h - 1);

   if (meta_descriptor_out_opt != NULL) {
      meta_descriptor_out_opt->cmask = descriptor_out->base;
      meta_descriptor_out_opt->cmask_slice = S_028C80_TILE_MAX(0);
      meta_descriptor_out_opt->fmask = descriptor_out->base;
      meta_descriptor_out_opt->fmask_slice =
         S_028C88_TILE_MAX(G_028C68_SLICE_TILE_MAX(descriptor_out->slice));
   }

   /* TODO(Triang3l): CMask, FMask. */

   return true;
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

VKAPI_ATTR void VKAPI_CALL
terakan_DestroyImageView(
   VkDevice const deviceHandle, VkImageView const imageView,
   VkAllocationCallbacks const * const pAllocator)
{
   struct terakan_image_view * const image_view = terakan_image_view_from_handle(imageView);

   if (image_view == NULL) {
      return;
   }

   struct terakan_device const * const device = terakan_device_from_handle(deviceHandle);

   vk_image_view_finish(&image_view->vk);

   vk_free2(&device->vk.alloc, pAllocator, image_view);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreateImageView(
   VkDevice const deviceHandle, VkImageViewCreateInfo const * const pCreateInfo,
   VkAllocationCallbacks const * const pAllocator, VkImageView * const pView)
{
   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   struct terakan_image_view * const image_view = vk_alloc2(
      &device->vk.alloc, pAllocator, sizeof(*image_view), alignof(*image_view),
      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (image_view == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   vk_image_view_init(&device->vk, &image_view->vk, false, pCreateInfo);

   memset(&image_view->descriptor, 0, sizeof(image_view->descriptor));
   memset(&image_view->color_meta, 0, sizeof(image_view->color_meta));

   struct terakan_image const * const image = terakan_image_from_handle(pCreateInfo->image);

   image_view->descriptor.bo = image->bo;

   terakan_image_create_color_descriptor(
      pCreateInfo, &image_view->descriptor.color, &image_view->color_meta);

   /* TODO(Triang3l): Other descriptor types. */

   *pView = terakan_image_view_to_handle(image_view);

   return VK_SUCCESS;
}
