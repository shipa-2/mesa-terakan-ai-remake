/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_sampler_terascale_1.h"

#include "gallium/drivers/r600/r600d.h"
#include "util/macros.h"
#include "util/u_math.h"

#include <assert.h>
#include <math.h>

static uint32_t
translate_filter(VkFilter const filter, bool const anisotropic)
{
   if (filter == VK_FILTER_LINEAR) {
      return anisotropic ? V_03C000_SQ_TEX_XY_FILTER_ANISO_BILINEAR
                         : V_03C000_SQ_TEX_XY_FILTER_BILINEAR;
   }
   assert(filter == VK_FILTER_NEAREST);
   return anisotropic ? V_03C000_SQ_TEX_XY_FILTER_ANISO_POINT : V_03C000_SQ_TEX_XY_FILTER_POINT;
}

static uint32_t
translate_address_mode(VkSamplerAddressMode const mode)
{
   switch (mode) {
   case VK_SAMPLER_ADDRESS_MODE_REPEAT:
      return V_03C000_SQ_TEX_WRAP;
   case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
      return V_03C000_SQ_TEX_MIRROR;
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:
      return V_03C000_SQ_TEX_CLAMP_LAST_TEXEL;
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
      return V_03C000_SQ_TEX_CLAMP_BORDER;
   case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE:
      return V_03C000_SQ_TEX_MIRROR_ONCE_LAST_TEXEL;
   default:
      assert(!"Unsupported sampler address mode");
      return V_03C000_SQ_TEX_WRAP;
   }
}

static uint32_t
translate_mip_filter(VkSamplerMipmapMode const mode)
{
   return mode == VK_SAMPLER_MIPMAP_MODE_LINEAR ? V_03C000_SQ_TEX_Z_FILTER_LINEAR
                                                : V_03C000_SQ_TEX_Z_FILTER_POINT;
}

static uint32_t
translate_border_color(VkBorderColor const color)
{
   switch (color) {
   case VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK:
   case VK_BORDER_COLOR_INT_TRANSPARENT_BLACK:
      return V_03C000_SQ_TEX_BORDER_COLOR_TRANS_BLACK;
   case VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK:
   case VK_BORDER_COLOR_INT_OPAQUE_BLACK:
      return V_03C000_SQ_TEX_BORDER_COLOR_OPAQUE_BLACK;
   case VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE:
   case VK_BORDER_COLOR_INT_OPAQUE_WHITE:
      return V_03C000_SQ_TEX_BORDER_COLOR_OPAQUE_WHITE;
   case VK_BORDER_COLOR_FLOAT_CUSTOM_EXT:
   case VK_BORDER_COLOR_INT_CUSTOM_EXT:
      return V_03C000_SQ_TEX_BORDER_COLOR_REGISTER;
   default:
      assert(!"Unsupported sampler border color");
      return V_03C000_SQ_TEX_BORDER_COLOR_TRANS_BLACK;
   }
}

static uint32_t
lod_6_4(float lod)
{
   if (unlikely(isnan(lod)))
      return 0;
   return (uint32_t)(CLAMP(lod, 0.0f, 15.0f) * 64.0f);
}

static int32_t
lod_bias_6_6(float bias)
{
   if (unlikely(isnan(bias)))
      return 0;
   return (int32_t)(CLAMP(bias, -16.0f, 16.0f) * 64.0f);
}

void
terakan_sampler_terascale_1_create_descriptor(VkSamplerCreateInfo const * const create_info,
                                              bool const force_base_mip, uint32_t descriptor_out[3])
{
   uint32_t max_aniso_ratio = 0;
   if (create_info->anisotropyEnable) {
      float const max_anisotropy = CLAMP(create_info->maxAnisotropy, 1.0f, 16.0f);
      max_aniso_ratio = util_logbase2((unsigned)max_anisotropy);
   }

   descriptor_out[0] =
      S_03C000_CLAMP_X(translate_address_mode(create_info->addressModeU)) |
      S_03C000_CLAMP_Y(translate_address_mode(create_info->addressModeV)) |
      S_03C000_CLAMP_Z(translate_address_mode(create_info->addressModeW)) |
      S_03C000_XY_MAG_FILTER(
         translate_filter(create_info->magFilter, create_info->anisotropyEnable)) |
      S_03C000_XY_MIN_FILTER(
         translate_filter(create_info->minFilter, create_info->anisotropyEnable)) |
      /* Z_FILTER is independent of both XY fields. r600_create_sampler_state() leaves it at NONE;
       * do not guess how its single value is meant to represent Vulkan's distinct minification and
       * magnification filters. 3D filtering therefore remains unsupported pending RV710 evidence.
       */
      S_03C000_Z_FILTER(V_03C000_SQ_TEX_Z_FILTER_NONE) |
      S_03C000_MIP_FILTER(translate_mip_filter(create_info->mipmapMode)) |
      S_03C000_MAX_ANISO_RATIO(max_aniso_ratio) |
      S_03C000_BORDER_COLOR_TYPE(translate_border_color(create_info->borderColor)) |
      S_03C000_DEPTH_COMPARE_FUNCTION(create_info->compareEnable ? create_info->compareOp : 0);

   descriptor_out[1] = S_03C004_MIN_LOD(force_base_mip ? 0 : lod_6_4(create_info->minLod)) |
                       S_03C004_MAX_LOD(force_base_mip ? 0 : lod_6_4(create_info->maxLod)) |
                       S_03C004_LOD_BIAS(lod_bias_6_6(create_info->mipLodBias));

   /* r600_create_sampler_state() sets only TYPE in word 2. Evergreen's LOD_BIAS,
    * TRUNCATE_COORD, DISABLE_CUBE_WRAP, PERF_MIP and ANISO_BIAS fields do not have the same
    * meanings or locations on R600/R700 and are intentionally not copied here.
    */
   descriptor_out[2] = S_03C008_TYPE(1);
}
