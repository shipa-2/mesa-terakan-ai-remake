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
#include "util/format/u_format.h"
#include "vk_format.h"
#include "vk_log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

   /* Characterization hook for terakan_border_color_swizzle_probe. `TERAKAN_BORDER_PROBE_VALUES`
    * takes four comma-separated floats and writes them into the per-sampler border colour
    * registers exactly as given, so what the hardware does with `DST_SEL` and the border colour
    * can be read off a result rather than inferred from tests whose border components happen to
    * be equal to each other.
    */
   {
      char const * const probe_values = getenv("TERAKAN_BORDER_PROBE_VALUES");
      if (probe_values != NULL) {
         float components[4] = {0.0f, 0.0f, 0.0f, 0.0f};
         char const * cursor = probe_values;
         for (unsigned component_index = 0; component_index < 4 && *cursor != '\0';
              ++component_index) {
            char * end = NULL;
            components[component_index] = strtof(cursor, &end);
            if (end == cursor) {
               break;
            }
            cursor = (*end == ',') ? end + 1 : end;
         }
         sampler->sampler.sampler[0] =
            (sampler->sampler.sampler[0] & C_03C000_BORDER_COLOR_TYPE) |
            S_03C000_BORDER_COLOR_TYPE(V_03C000_SQ_TEX_BORDER_COLOR_REGISTER);
         memcpy(sampler->sampler.register_border_color, components,
                sizeof(sampler->sampler.register_border_color));
      }
   }

   /* TODO(Triang3l): Custom border colours, VK_EXT_custom_border_color. */

   sampler->unnormalized_coordinates = pCreateInfo->unnormalizedCoordinates;

   *pSampler = terakan_sampler_to_handle(sampler);
   return VK_SUCCESS;
}

/* The border colour registers hold normalized floats, not the raw values the border colour was
 * specified with. Evergreen denormalizes what they hold by the format of the view being sampled,
 * so an integer border colour has to be divided by its channel's maximum on the way in --
 * `evergreen_convert_border_color` in Gallium r600 does the same thing for the same reason.
 * Writing the integer 1 straight into the register makes it a denormal that reads back as zero,
 * which is what `sampler.border_swizzle` saw: `INT_OPAQUE_WHITE` came back as zero for every
 * integer format while the fixed border colour types, which never touch these registers, were
 * right.
 *
 * The format is the view's, not the sampler's, so this cannot be done when the sampler is created.
 */
void
terakan_sampler_descriptor_normalize_integer_border_color(
   struct terakan_sampler_descriptor * const sampler_descriptor,
   struct terakan_resource_descriptor const * const view_resource, VkFormat const view_format,
   bool const stencil_aspect)
{
   if (G_03C000_BORDER_COLOR_TYPE(sampler_descriptor->sampler[0]) !=
       V_03C000_SQ_TEX_BORDER_COLOR_REGISTER) {
      return;
   }

   /* The stencil aspect of a combined format is an 8-bit unsigned integer, and its own format
    * description would describe the depth part instead.
    */
   unsigned channel_bits[4] = {8, 8, 8, 8};
   bool channel_signed[4] = {false, false, false, false};
   if (!stencil_aspect) {
      struct util_format_description const * const description =
         util_format_description(vk_format_to_pipe_format(view_format));
      if (description == NULL || !util_format_is_pure_integer(vk_format_to_pipe_format(view_format))) {
         return;
      }
      for (unsigned component_index = 0; component_index < 4; ++component_index) {
         unsigned const channel_index =
            MIN2(component_index, (unsigned)description->nr_channels - 1u);
         channel_bits[component_index] = description->channel[channel_index].size;
         channel_signed[component_index] =
            description->channel[channel_index].type == UTIL_FORMAT_TYPE_SIGNED;
      }
   }

   /* The hardware applies `DST_SEL` to the border colour twice, which
    * terakan_border_color_swizzle_probe reads off directly: with the registers holding four
    * distinct values, a view swizzled `argb` -- `(W, X, Y, Z)` -- samples its border as
    * `(Z, W, X, Y)`, which is that permutation composed with itself, and the self-inverse `bgra`
    * comes back as the identity. Constants survive either way, and an ordinary sample gets them
    * right.
    *
    * So the registers have to hold the border colour with the swizzle applied backwards once:
    * writing `register[DST_SEL[j]] = border[j]` leaves the double application landing on
    * `border[DST_SEL[i]]`, which is what the component was asked for.
    */
   uint32_t const dst_sel[4] = {
      G_030010_DST_SEL_X(view_resource->resource[4]),
      G_030010_DST_SEL_Y(view_resource->resource[4]),
      G_030010_DST_SEL_Z(view_resource->resource[4]),
      G_030010_DST_SEL_W(view_resource->resource[4]),
   };
   uint32_t const border_color[4] = {
      sampler_descriptor->register_border_color[0], sampler_descriptor->register_border_color[1],
      sampler_descriptor->register_border_color[2], sampler_descriptor->register_border_color[3]};

   for (unsigned component_index = 0; component_index < 4; ++component_index) {
      /* A component the swizzle turns into a constant leaves no register to write, and the
       * hardware produces the constant on its own.
       */
      if (dst_sel[component_index] > TERASCALE_SWIZZLE_W) {
         continue;
      }
      unsigned const register_index = dst_sel[component_index];
      unsigned const bits = channel_bits[register_index];
      if (bits == 0 || bits > 32) {
         continue;
      }
      /* 32-bit integer formats get nothing out of these registers at all: writing a plain 0.5f
       * and sampling the border reads back zero, while the same probe on a 16-bit format reads
       * back 32768, which is 0.5 * 2^16. So the denormalization is by 2^bits, and 32 bits is past
       * what the border colour path carries. Those cases are left failing rather than papered
       * over.
       */
      double const maximum = channel_signed[register_index]
                                ? (double)((1ull << (bits - 1u)) - 1ull)
                                : (double)((1ull << bits) - 1ull);
      double const raw = channel_signed[register_index]
                            ? (double)(int32_t)border_color[component_index]
                            : (double)border_color[component_index];
      float const normalized = (float)(raw / maximum);
      memcpy(&sampler_descriptor->register_border_color[register_index], &normalized,
             sizeof(float));
   }
}
