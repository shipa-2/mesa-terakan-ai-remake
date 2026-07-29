/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_descriptor.h"
#include "terakan_physical_device.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                                           \
   do {                                                                                            \
      if (!(condition)) {                                                                          \
         fprintf(stderr, "CHECK failed at %s:%u: %s\n", __FILE__, __LINE__, #condition);           \
         abort();                                                                                  \
      }                                                                                            \
   } while (0)

static void
test_buffer_dim_is_in_elements(void)
{
   struct terakan_physical_device physical_device = {
      .tiling_info.pipe_interleave_bytes_log2 = 8,
   };
   struct terakan_color_descriptor descriptor = {};
   uint32_t base_offset_bytes = UINT32_MAX;

   terakan_color_descriptor_calculate_buffer_base_pitch_slice_dim_offset(
      &descriptor, 0x100000, 18, sizeof(uint32_t), &physical_device, &base_offset_bytes);

   CHECK(base_offset_bytes == 0);
   CHECK(descriptor.dim == 17);
}

static void
test_buffer_dim_includes_aligned_base_offset(void)
{
   struct terakan_physical_device physical_device = {
      .tiling_info.pipe_interleave_bytes_log2 = 8,
   };
   struct terakan_color_descriptor descriptor = {};
   uint32_t base_offset_bytes = UINT32_MAX;

   terakan_color_descriptor_calculate_buffer_base_pitch_slice_dim_offset(
      &descriptor, 0x100010, 9, sizeof(uint32_t), &physical_device, &base_offset_bytes);

   CHECK(base_offset_bytes == 16);
   CHECK(descriptor.base == 0x100000 >> 8);
   CHECK(descriptor.dim == 12);
}

int
main(void)
{
   test_buffer_dim_is_in_elements();
   test_buffer_dim_includes_aligned_base_offset();
   return 0;
}
