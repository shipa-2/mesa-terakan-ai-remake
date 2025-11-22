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

#include "terakan_descriptor.h"

#include "terakan_device.h"
#include "terakan_format.h"
#include "terakan_physical_device.h"

#include "gallium/drivers/r600/evergreend.h"
#include "util/macros.h"
#include "util/u_endian.h"
#include "util/u_math.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

unsigned
terakan_color_descriptor_buffer_uav_base_granularity_log2(
   unsigned const bytes_per_element, struct terakan_physical_device const * const physical_device)
{
   if (physical_device->submission_info_gfx.buffer_uav_validated_as_image) {
      /* Device memory BO sizes (whole BO sizes, not buffer memory requirements) are rounded to make
       * sure a LINEAR_ALIGNED color target with any element size and the smallest possible
       * PITCH_TILE_MAX and SLICE_TILE_MAX is in bounds as long as the BO-relative base address is
       * also rounded. This value is never smaller than the pipe interleave.
       */
      return util_logbase2(
         bytes_per_element *
         terakan_format_pitch_alignment_linear_surfels(
            bytes_per_element, physical_device->tiling_info.pipe_interleave_bytes_log2));
   }

   /* Alignment requirement for LINEAR_ALIGNED, which is the array mode required for buffer UAVs. */
   return physical_device->tiling_info.pipe_interleave_bytes_log2;
}

void
terakan_color_descriptor_calculate_buffer_pitch_slice(
   struct terakan_color_descriptor * const descriptor, unsigned const bytes_per_element,
   struct terakan_physical_device const * const physical_device)
{
   /* PITCH_TILE_MAX and SLICE_TILE_MAX are ignored by the hardware for buffers, and PITCH_TILE_MAX
    * can't store large buffer sizes, but DRM Radeon 2.50.0 validates the surface size based on
    * PITCH_TILE_MAX and SLICE_TILE_MAX regardless of whether the color surface is a buffer UAV.
    * Provide the smallest valid values.
    */
   uint32_t const pitch_elements = terakan_format_pitch_alignment_linear_surfels(
      bytes_per_element, physical_device->tiling_info.pipe_interleave_bytes_log2);
   descriptor->pitch = S_028C64_PITCH_TILE_MAX(pitch_elements / 8 - 1);
   descriptor->slice = S_028C68_SLICE_TILE_MAX(pitch_elements / 64 - 1);
}

void
terakan_color_descriptor_calculate_buffer_base_pitch_slice_dim_offset(
   struct terakan_color_descriptor * const descriptor, uint64_t const va,
   VkDeviceSize const elements, unsigned const bytes_per_element,
   struct terakan_physical_device const * const physical_device,
   uint32_t * const base_granularity_offset_bytes_out)
{
   unsigned const base_granularity_log2 =
      terakan_color_descriptor_buffer_uav_base_granularity_log2(bytes_per_element, physical_device);
   uint64_t const va_granularity_aligned = va >> base_granularity_log2 << base_granularity_log2;
   descriptor->base = (uint32_t)(va_granularity_aligned >> 8);

   terakan_color_descriptor_calculate_buffer_pitch_slice(descriptor, bytes_per_element,
                                                         physical_device);

   uint32_t const base_granularity_offset_bytes = (uint32_t)(va - va_granularity_aligned);
   *base_granularity_offset_bytes_out = base_granularity_offset_bytes;

   assert(elements != 0);
   descriptor->dim = (uint32_t)(base_granularity_offset_bytes / bytes_per_element + elements - 1);
}

void
terakan_color_descriptor_info_to_uav_immediate_resource(struct terakan_device const * const device,
                                                        uint32_t const color_info,
                                                        uint32_t resource_out[8])
{
   enum terascale_format_index const format =
      (enum terascale_format_index)G_028C70_FORMAT(color_info);

   unsigned const bytes_per_texel = terascale_format_bytes_per_block[format];

   uint32_t const va_shr8 = device->uav_immediate_va_shr8[util_logbase2(bytes_per_texel)];

   uint32_t const uav_immediate_size_texels =
      terakan_device_physical_device(device)->chip_family_info.uav_immediate_size_texels;

   enum terascale_format_number_type const number_type =
      (enum terascale_format_number_type)G_028C70_NUMBER_TYPE(color_info);

   uint8_t const * const swizzle =
      terascale_format_cb_color_read_swizzle[terascale_format_channel_count[format]]
                                            [G_028C70_COMP_SWAP(color_info)];

   resource_out[0] = (uint32_t)(va_shr8 << 8);
   resource_out[1] = (uav_immediate_size_texels * bytes_per_texel) - 1;
   resource_out[2] = S_030008_BASE_ADDRESS_HI(va_shr8 >> (32 - 8)) |
                     S_030008_STRIDE(bytes_per_texel) | S_030008_DATA_FORMAT(format) |
                     S_030008_NUM_FORMAT_ALL(terascale_format_get_sq_num_format(number_type)) |
                     S_030008_FORMAT_COMP_ALL(number_type == TERASCALE_FORMAT_NUMBER_TYPE_SNORM ||
                                              number_type == TERASCALE_FORMAT_NUMBER_TYPE_SINT) |
                     S_030008_ENDIAN_SWAP(G_028C70_ENDIAN(color_info));
   /* In Vulkan, storage descriptors can be created only for one aspect, and TeraScale's combined
    * depth / stencil SQ / CB formats correspond only to depth / stencil formats that exist in
    * Vulkan - more specifically, on R8xx / R9xx, depth and stencil are stored separately, with
    * stencil always being TERASCALE_FORMAT_INDEX_8, and 8_24 being used only for the depth aspect
    * (of X8_D24_UNORM_PACK32 and D24_UNORM_S8_UINT). Therefore, for reads from the hardware
    * combined depth / stencil formats, which are two-channel from the perspective of SQ and CB, and
    * thus the CB swap is treated as two-channel, the stencil channel needs to be replaced with 0.
    */
   resource_out[3] =
      S_03000C_UNCACHED(1) | S_03000C_DST_SEL_X(swizzle[0]) |
      S_03000C_DST_SEL_Y(TERASCALE_FORMATS_COMBINED_DEPTH_STENCIL & BITFIELD64_BIT(format)
                            ? TERASCALE_SWIZZLE_0
                            : swizzle[1]) |
      S_03000C_DST_SEL_Z(swizzle[2]) | S_03000C_DST_SEL_W(swizzle[3]);
   resource_out[4] = uav_immediate_size_texels;
   resource_out[5] = 0;
   resource_out[6] = 0;
   resource_out[7] = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER);
   resource_out[TERAKAN_RESOURCE_BUFFER_PRIORITY_WORD] = TERAKAN_BO_PRIORITY_SHADER_RW_BUFFER;
}

bool
terakan_descriptor_create_for_uniform_buffer(struct terakan_bo const * const bo, uint64_t const va,
                                             VkDeviceSize const range, uint32_t resource_out[8])
{
   assert((va & (TERAKAN_KCACHE_HW_LINE_BYTES - 1)) == 0);
   assert(range != VK_WHOLE_SIZE);
   if (bo == NULL || range == 0) {
      return false;
   }

   /* Align to the kcache line size for consistent out-of-bounds behavior between the fetch and the
    * kcache.
    */
   VkDeviceSize const range_aligned = ALIGN_POT(range, (VkDeviceSize)TERAKAN_KCACHE_HW_LINE_BYTES);

   resource_out[0] = (uint32_t)va;
   resource_out[1] = (uint32_t)(range_aligned - 1);
   resource_out[2] = S_030008_BASE_ADDRESS_HI(va >> 32) | S_030008_STRIDE(1);
   resource_out[3] =
      S_03000C_DST_SEL_X(TERASCALE_SWIZZLE_X) | S_03000C_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_03000C_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_03000C_DST_SEL_W(TERASCALE_SWIZZLE_W);
   resource_out[4] = (uint32_t)range_aligned;
   resource_out[5] = 0;
   resource_out[6] = 0;
   resource_out[7] = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER);
   resource_out[TERAKAN_RESOURCE_BUFFER_PRIORITY_WORD] = TERAKAN_BO_PRIORITY_UNIFORM_BUFFER;

   return true;
}

bool
terakan_descriptor_create_for_storage_buffer(
   struct terakan_bo const * const bo, uint64_t const va, VkDeviceSize const range,
   struct terakan_physical_device const * const physical_device, uint32_t resource_out[8],
   struct terakan_color_descriptor * const color_out)
{
   assert((va & (sizeof(uint32_t) - 1)) == 0);
   assert(range != VK_WHOLE_SIZE);
   if (bo == NULL || range == 0) {
      return false;
   }

   /* VK_EXT_robustness2 behavior: rounding up. */
   VkDeviceSize const range_aligned = ALIGN_POT(range, (VkDeviceSize)sizeof(uint32_t));

   resource_out[0] = (uint32_t)va;
   resource_out[1] = (uint32_t)(range_aligned - 1);
   resource_out[2] = S_030008_BASE_ADDRESS_HI(va >> 32) | S_030008_STRIDE(1);
   resource_out[3] =
      S_03000C_DST_SEL_X(TERASCALE_SWIZZLE_X) | S_03000C_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_03000C_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_03000C_DST_SEL_W(TERASCALE_SWIZZLE_W);
   resource_out[4] = (uint32_t)range_aligned;
   resource_out[5] = 0;
   resource_out[6] = 0;
   resource_out[7] = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER);
   resource_out[TERAKAN_RESOURCE_BUFFER_PRIORITY_WORD] = TERAKAN_BO_PRIORITY_SHADER_READ_BUFFER;

   terakan_color_descriptor_calculate_buffer_base_pitch_slice_view_dim(
      color_out, va, range_aligned / sizeof(uint32_t), sizeof(uint32_t), physical_device, false);
   color_out->info = S_028C70_ENDIAN(UTIL_ARCH_BIG_ENDIAN ? TERASCALE_ENDIAN_SWAP_8IN32
                                                          : TERASCALE_ENDIAN_SWAP_NONE) |
                     S_028C70_FORMAT(TERASCALE_FORMAT_INDEX_32) |
                     S_028C70_NUMBER_TYPE(TERASCALE_FORMAT_NUMBER_TYPE_UINT) |
                     TERAKAN_COLOR_DESCRIPTOR_BUFFER_UAV_INFO_CONST_FIELDS;
   color_out->attrib = TERAKAN_COLOR_DESCRIPTOR_BUFFER_UAV_ATTRIB;

   return true;
}
