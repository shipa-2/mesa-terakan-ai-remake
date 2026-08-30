/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_resource_descriptor_terascale_1.h"

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
   CHECK(terakan_resource_descriptor_terascale_1_clear_packet_dwords() == 0);

   struct terakan_resource_texture_descriptor_terascale_1_info const texture_info = {
      .dim = V_038000_SQ_TEX_DIM_2D_ARRAY,
      .tile_mode = V_038000_ARRAY_2D_TILED_THIN1,
      .tile_type = true,
      .pitch = 31,
      .width = 127,
      .height = 63,
      .depth = 3,
      .data_format = V_038004_COLOR_8_8_8_8,
      .base_address = 0x00123456,
      .mip_address = 0x00124000,
      .format_comp = {V_038010_SQ_FORMAT_COMP_SIGNED, V_038010_SQ_FORMAT_COMP_UNSIGNED,
                      V_038010_SQ_FORMAT_COMP_SIGNED, V_038010_SQ_FORMAT_COMP_UNSIGNED},
      .num_format = V_038010_SQ_NUM_FORMAT_NORM,
      .srf_mode = V_038010_SRF_MODE_ZERO_CLAMP_MINUS_ONE,
      .force_degamma = true,
      .endian_swap = 0,
      .dst_sel = {V_038010_SQ_SEL_X, V_038010_SQ_SEL_Y, V_038010_SQ_SEL_Z, V_038010_SQ_SEL_1},
      .base_level = 2,
      .last_level = 4,
      .base_array = 1,
      .last_array = 3,
      .max_aniso = 4,
   };
   uint32_t descriptor[TERAKAN_RESOURCE_DESCRIPTOR_TERASCALE_1_DWORDS];
   terakan_resource_texture_descriptor_terascale_1_encode(&texture_info, descriptor);

   uint32_t const expected_texture[] = {
      0x03f81fa5, 0x6800603f, 0x00123456, 0x00124000, 0x2a884811, 0x00060014, 0x80000010,
   };
   for (unsigned i = 0; i < TERAKAN_RESOURCE_DESCRIPTOR_TERASCALE_1_DWORDS; ++i)
      CHECK(descriptor[i] == expected_texture[i]);
   CHECK(G_038000_DIM(descriptor[0]) == V_038000_SQ_TEX_DIM_2D_ARRAY);
   CHECK(G_038014_BASE_ARRAY(descriptor[5]) == 1);
   CHECK(G_038014_LAST_ARRAY(descriptor[5]) == 3);
   CHECK(G_038018_TYPE(descriptor[6]) == V_038010_SQ_TEX_VTX_VALID_TEXTURE);

   uint32_t packet[TERAKAN_RESOURCE_DESCRIPTOR_TERASCALE_1_SET_PACKET_DWORDS];
   CHECK(terakan_resource_descriptor_terascale_1_write_set_packet(packet, 13, 0, descriptor) ==
         packet + TERAKAN_RESOURCE_DESCRIPTOR_TERASCALE_1_SET_PACKET_DWORDS);
   CHECK(packet[0] == 0xc0076d00);
   CHECK(packet[1] == 91);
   for (unsigned i = 0; i < TERAKAN_RESOURCE_DESCRIPTOR_TERASCALE_1_DWORDS; ++i)
      CHECK(packet[2 + i] == expected_texture[i]);

   uint32_t const buffer_source[4] = {0x11223344, 0x00000fff, 0x4d12345a, 0x00006880};
   terakan_resource_buffer_descriptor_terascale_1_encode(buffer_source, descriptor);
   CHECK(descriptor[0] == buffer_source[0]);
   CHECK(descriptor[1] == buffer_source[1]);
   CHECK(descriptor[2] == buffer_source[2]);
   CHECK(descriptor[3] == buffer_source[3]);
   CHECK(descriptor[4] == 0);
   CHECK(descriptor[5] == 0);
   CHECK(descriptor[6] == 0xc0000000);
   return EXIT_SUCCESS;
}
