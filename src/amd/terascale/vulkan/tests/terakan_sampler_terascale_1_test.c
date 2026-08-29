/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_sampler_terascale_1.h"

#include "gallium/drivers/r600/r600d.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                                           \
   do {                                                                                            \
      if (!(condition)) {                                                                          \
         fprintf(stderr, "check failed at line %u: %s\n", __LINE__, #condition);                   \
         exit(EXIT_FAILURE);                                                                       \
      }                                                                                            \
   } while (0)

int
main(void)
{
   VkSamplerCreateInfo const create_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .mipLodBias = -2.5f,
      .anisotropyEnable = VK_TRUE,
      .maxAnisotropy = 4.0f,
      .compareEnable = VK_TRUE,
      .compareOp = VK_COMPARE_OP_GREATER,
      .minLod = 1.5f,
      .maxLod = 7.25f,
      .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
   };
   uint32_t descriptor[3];
   terakan_sampler_terascale_1_create_descriptor(&create_info, false, descriptor);

   /* Literal R700 S# words, independently decoded below. The deliberately different XY filters
    * prove that Z_FILTER is its own field and remains NONE as in r600_create_sampler_state rather
    * than being changed implicitly by either XY field.
    */
   CHECK(descriptor[0] == 0x10944b8a);
   CHECK(descriptor[1] == 0xf6074060);
   CHECK(descriptor[2] == 0x80000000);
   CHECK(G_03C000_XY_MAG_FILTER(descriptor[0]) == V_03C000_SQ_TEX_XY_FILTER_ANISO_BILINEAR);
   CHECK(G_03C000_XY_MIN_FILTER(descriptor[0]) == V_03C000_SQ_TEX_XY_FILTER_ANISO_POINT);
   CHECK(G_03C000_Z_FILTER(descriptor[0]) == V_03C000_SQ_TEX_Z_FILTER_NONE);
   CHECK(G_03C000_MIP_FILTER(descriptor[0]) == V_03C000_SQ_TEX_Z_FILTER_LINEAR);
   CHECK(G_03C004_MIN_LOD(descriptor[1]) == 96);
   CHECK(G_03C004_MAX_LOD(descriptor[1]) == 464);

   terakan_sampler_terascale_1_create_descriptor(&create_info, true, descriptor);
   CHECK(G_03C004_MIN_LOD(descriptor[1]) == 0);
   CHECK(G_03C004_MAX_LOD(descriptor[1]) == 0);
   CHECK(G_03C004_LOD_BIAS(descriptor[1]) == 0xf60);
   return EXIT_SUCCESS;
}
