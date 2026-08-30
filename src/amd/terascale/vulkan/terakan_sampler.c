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

#include "terakan_sampler.h"

#include "terakan_device.h"
#include "terakan_entrypoints.h"
#include "terakan_physical_device.h"
#include "terakan_sampler_terascale_1.h"

#include "gallium/drivers/r600/evergreend.h"
#include "vk_log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

VKAPI_ATTR void VKAPI_CALL
terakan_DestroySampler(VkDevice const deviceHandle, VkSampler const samplerHandle,
                       VkAllocationCallbacks const * const pAllocator)
{
   struct terakan_sampler * const sampler = terakan_sampler_from_handle(samplerHandle);

   if (sampler == NULL) {
      return;
   }

   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   vk_sampler_destroy(&device->vk, pAllocator, &sampler->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreateSampler(VkDevice const deviceHandle, VkSamplerCreateInfo const * const pCreateInfo,
                      VkAllocationCallbacks const * const pAllocator, VkSampler * const pSampler)
{
   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   struct terakan_sampler * const sampler =
      vk_sampler_create(&device->vk, pCreateInfo, pAllocator, sizeof(struct terakan_sampler));
   if (sampler == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   if (terakan_device_physical_device(device)->chip_info.is_terascale_1) {
      terakan_sampler_terascale_1_create_descriptor(
         pCreateInfo, getenv("TERAKAN_DEBUG_FORCE_BASE_MIP") != NULL, sampler->sampler.sampler);
   } else {
      sampler->sampler = (struct terakan_sampler_descriptor){
         .sampler = {
            S_03C000_CLAMP_X(terakan_sampler_translate_address_mode(pCreateInfo->addressModeU)) |
               S_03C000_CLAMP_Y(terakan_sampler_translate_address_mode(pCreateInfo->addressModeV)) |
               S_03C000_CLAMP_Z(terakan_sampler_translate_address_mode(pCreateInfo->addressModeW)) |
               S_03C000_XY_MAG_FILTER(terakan_sampler_translate_filter(
                  pCreateInfo->magFilter, pCreateInfo->anisotropyEnable)) |
               S_03C000_XY_MIN_FILTER(terakan_sampler_translate_filter(
                  pCreateInfo->minFilter, pCreateInfo->anisotropyEnable)) |
               S_03C000_MIP_FILTER(terakan_sampler_translate_mipmap_mode(pCreateInfo->mipmapMode)) |
               S_03C000_BORDER_COLOR_TYPE(
                  terakan_sampler_translate_border_color(pCreateInfo->borderColor)) |
               S_03C000_DEPTH_COMPARE_FUNCTION(
                  pCreateInfo->compareEnable ? (uint32_t)pCreateInfo->compareOp : 0) |
               S_03C000_FORCE_UNNORMALIZED(
                  pCreateInfo->unnormalizedCoordinates &&
                  terakan_device_physical_device(device)->chip_info.is_r9xx),
            S_03C004_MIN_LOD(terakan_sampler_translate_min_max_lod(pCreateInfo->minLod)) |
               S_03C004_MAX_LOD(terakan_sampler_translate_min_max_lod(pCreateInfo->maxLod)),
            S_03C008_LOD_BIAS(terakan_sampler_translate_mip_lod_bias(pCreateInfo->mipLodBias)) |
               S_03C008_TRUNCATE_COORD(pCreateInfo->magFilter == VK_FILTER_NEAREST &&
                                       pCreateInfo->minFilter == VK_FILTER_NEAREST) |
               S_03C008_DISABLE_CUBE_WRAP(
                  (pCreateInfo->flags & VK_SAMPLER_CREATE_NON_SEAMLESS_CUBE_MAP_BIT_EXT) != 0) |
               S_03C008_TYPE(1),
         }};

      if (pCreateInfo->anisotropyEnable) {
         uint32_t const max_aniso_ratio =
            terakan_sampler_translate_max_anisotropy(pCreateInfo->maxAnisotropy);
         sampler->sampler.sampler[0] |= S_03C000_MAX_ANISO_RATIO(max_aniso_ratio);
         sampler->sampler.sampler[1] |= S_03C004_PERF_MIP(max_aniso_ratio + 6);
         sampler->sampler.sampler[2] |= S_03C008_ANISO_BIAS(max_aniso_ratio);
      }

      /* Diagnostic switch for separating mip layout/addressing failures from base-level texture
       * sampling failures in applications. Clamping both limits to zero makes every implicit-LOD
       * sample use the base level without changing normal driver behavior.
       */
      if (getenv("TERAKAN_DEBUG_FORCE_BASE_MIP") != NULL) {
         sampler->sampler.sampler[1] &= C_03C004_MIN_LOD & C_03C004_MAX_LOD;
      }
   }

   /* The fixed border colour types deliver 1.0 as a float, which is what the specification asks
    * for from `VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK` and `_WHITE` and what those pass with. The
    * integer variants have to deliver the integer 1 instead, and the narrow integer formats read
    * the fixed white back as something else: every 8-bit and 16-bit `uint`/`sint` format, and the
    * stencil aspect of every combined depth/stencil format, fails the CTS `clamp_to_border` cases
    * while the 32-bit integer formats and every normalized one pass.
    *
    * The per-sampler border colour registers are already emitted for the `REGISTER` type by
    * `terakan_hw_config_sqk`, and they carry raw bits, so pointing the integer types at them with
    * the integer values written out gives the specified result at every width.
    */
   if (!terakan_device_physical_device(device)->chip_info.is_terascale_1) {
      uint32_t const * integer_border_color = NULL;
      static uint32_t const integer_opaque_black[4] = {0, 0, 0, 1};
      static uint32_t const integer_opaque_white[4] = {1, 1, 1, 1};
      switch (pCreateInfo->borderColor) {
      case VK_BORDER_COLOR_INT_OPAQUE_BLACK:
         integer_border_color = integer_opaque_black;
         break;
      case VK_BORDER_COLOR_INT_OPAQUE_WHITE:
         integer_border_color = integer_opaque_white;
         break;
      default:
         break;
      }
      if (integer_border_color != NULL) {
         sampler->sampler.sampler[0] =
            (sampler->sampler.sampler[0] & C_03C000_BORDER_COLOR_TYPE) |
            S_03C000_BORDER_COLOR_TYPE(V_03C000_SQ_TEX_BORDER_COLOR_REGISTER);
         memcpy(sampler->sampler.register_border_color, integer_border_color,
                sizeof(sampler->sampler.register_border_color));
      }
   }

   /* TODO(Triang3l): Custom border colours, VK_EXT_custom_border_color. */

   sampler->unnormalized_coordinates = pCreateInfo->unnormalizedCoordinates;

   *pSampler = terakan_sampler_to_handle(sampler);
   return VK_SUCCESS;
}
