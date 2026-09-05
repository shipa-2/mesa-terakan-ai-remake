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

#ifndef TERAKAN_IMAGE_H
#define TERAKAN_IMAGE_H

#include "terakan_bo.h"
#include "terakan_descriptor.h"
#include "terakan_format.h"
#include "terakan_screen_rect.h"

#include "gallium/drivers/r600/evergreend.h"
#include "util/bitscan.h"
#include "util/macros.h"
#include "util/u_endian.h"
#include "util/u_math.h"
#include "vk_format.h"
#include "vk_image.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct terakan_screen_rect terakan_vk_rect_to_screen_rect(VkRect2D rect,
                                                          struct terakan_screen_rect clip_rect);

#define TERAKAN_IMAGE_MAX_WIDTH_HEIGHT_LOG2 14
#define TERAKAN_IMAGE_MAX_WIDTH_HEIGHT      (1 << TERAKAN_IMAGE_MAX_WIDTH_HEIGHT_LOG2)
/* Maximum depth or array layer count supported by TC. */
#define TERAKAN_IMAGE_MAX_SLICES_LOG2 13
#define TERAKAN_IMAGE_MAX_SLICES      (1 << TERAKAN_IMAGE_MAX_SLICES_LOG2)
/* Maximum depth or array layer count supported by CB and DB.
 * In Terakan, images can use more than this (up to TERAKAN_IMAGE_MAX_SLICES) if they can't
 * potentially be accessed by the application through image views referencing arbitrary Z or array
 * layer ranges that'd require CB or DB access.
 */
#define TERAKAN_IMAGE_MAX_TARGET_SLICES_LOG2 11
#define TERAKAN_IMAGE_MAX_TARGET_SLICES      (1 << TERAKAN_IMAGE_MAX_TARGET_SLICES_LOG2)

/* Returns -1 for an invalid sample count. */
static inline int
terakan_image_vk_sample_count_to_hw_log2(VkSampleCountFlagBits const sample_count,
                                         bool const allow_16_samples)
{
   int const sample_count_log2 = ffs((uint32_t)sample_count) - 1;
   /* #MemoryIntegrity: Don't allow too large sample counts because they're used in hardware
    * fixed-function state registers and in addressing in various places on both the GPU and the
    * CPU.
    */
   if (unlikely(sample_count_log2 < 0 || sample_count_log2 > (allow_16_samples ? 4 : 3))) {
      return -1;
   }
   return sample_count_log2;
}

struct terakan_image_surface_level {
   uint32_t offset_in_memory_bytes_shr8;
   uint32_t slice_size_bytes_shr8;
   /* [2] is depth for 3D, array layers otherwise. */
   uint16_t aligned_extent_surfels[3];
   /* 2D is degraded to 1D for small mips. */
   uint8_t array_mode;
};

struct terakan_image_surface_tiling {
   /* ATTRIB register field values (log2, some being exponent-biased). */
   uint8_t attrib_tile_split; /* 0 = 2^6 bytes. */
   uint8_t attrib_bank_width;
   uint8_t attrib_bank_height;
   uint8_t attrib_macro_tile_aspect;

   /* Not including NUM_BANKS, assuming it's the same for all images on the device, because it can't
    * be changed in a useful way on DRM Radeon as of 2.50.0.
    *
    * In most cases, the R800 AddrLib uses the logical bank count, which is the number of banks
    * multiplied by the number of ranks from MC_ARB_RAMCFG. One exception is that under some other
    * conditions, for large micro-tiles and large numbers of logical banks, it can divide NUM_BANKS
    * by 2.
    * However, DRM Radeon 2.50.0 doesn't provide the number of ranks (only the number of banks) to
    * applications, and also it doesn't expose a field for setting NUM_BANKS in BO metadata.
    */

   /* CB non-display tiling = TC non-display tiling || array mode is linear. */
   bool tc_non_display;
};

struct terakan_image_surface_aspect {
   uint32_t alignment_bytes_shr8;
   uint32_t offset_in_memory_bytes_shr8;
   uint32_t size_bytes_shr8;

   uint8_t bytes_per_block;

   struct terakan_image_surface_tiling tiling;

   struct terakan_image_surface_level levels[TERAKAN_IMAGE_MAX_WIDTH_HEIGHT + 1];
};

struct terakan_image_surface {
   uint32_t alignment_bytes_shr8;
   uint32_t size_bytes_shr8;

   struct {
      uint32_t alignment_bytes_shr8;
      uint32_t offset_in_memory_bytes_shr8;
      uint32_t size_bytes_shr8;
      uint32_t slice_size_bytes_shr8;
      uint32_t slice_tile_max;
      uint8_t attrib_bank_height;
   } fmask;

   struct {
      uint32_t alignment_bytes_shr8;
      uint32_t offset_in_memory_bytes_shr8;
      uint32_t size_bytes_shr8;
      uint32_t slice_size_bytes_shr8;
      uint32_t slice_tile_max;
   } cmask;

   /* If the image has a combined depth and stencil format and has DB usage enabled, DB register
    * fields shared between depth and stencil are the same for the two aspects.
    */
   struct terakan_image_surface_aspect aspects[TERAKAN_FORMAT_MAX_ASPECTS];
};

/* FMASK and CMASK form one hardware-visible color metadata allocation. Never enable only one of
 * them: CB compression consumes both addresses, and an incomplete pair can make a submission hang
 * rather than merely return incorrect samples.
 */
static inline bool
terakan_image_surface_has_color_metadata(struct terakan_image_surface const * const surface)
{
   bool const has_fmask = surface->fmask.size_bytes_shr8 != 0;
   bool const has_cmask = surface->cmask.size_bytes_shr8 != 0;
   assert(has_fmask == has_cmask);
   if (has_fmask && has_cmask) {
      assert(surface->fmask.slice_size_bytes_shr8 != 0);
      assert(surface->cmask.slice_size_bytes_shr8 != 0);
   }
   return has_fmask && has_cmask;
}

/* TODO(Triang3l): Replace in favor of terakan_format_aspect_index, but need to handle errors when
 * using it, and never to use it with combined depth and stencil.
 */
static inline unsigned
terakan_image_surface_aspect_index(VkFormat const image_format, VkImageAspectFlagBits const aspect)
{
   if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT) {
      return vk_format_has_depth(image_format) ? 1 : 0;
   }
   return 0;
}

struct terakan_image {
   struct vk_image vk;

   /* Derived from the format, but needed in many places, so stored. */
   struct terakan_format_info format_info;

   struct terakan_image_surface surface;

   struct terakan_bo const * bo;
   VkDeviceSize va;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(terakan_image, vk.base, VkImage, VK_OBJECT_TYPE_IMAGE)

static inline uint32_t
terakan_image_depth_or_array_layers(struct terakan_image const * const image,
                                    uint32_t const mip_level)
{
   if (image->vk.image_type == VK_IMAGE_TYPE_3D) {
      return u_minify(image->vk.extent.depth, mip_level);
   }
   return image->vk.array_layers;
}

static inline bool
terakan_image_is_big_endian(UNUSED struct terakan_image const * const image)
{
#if UTIL_ARCH_BIG_ENDIAN
   /* DB doesn't support endian swapping, but neither does it support linear images, so the
    * endianness doesn't matter to the host.
    * For other cases of tiled images, however, set the endian swap because host endianness is
    * likely to be expected when exporting the image (presentation in DRI expects it, the Gallium
    * R600 driver also uses the host endianness).
    */
   return !(image->vk.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
#else
   return false;
#endif
}

struct terakan_image_descriptor_subresource_range {
   uint32_t base_mip_level;
   /* Before sanitization, the upper bound of the mip level range can exceed the mip level count
    * (including being `UINT32_MAX`, which is `VK_REMAINING_MIP_LEVELS`), in which case it will be
    * silently clamped by sanitization.
    */
   uint32_t max_level_count;
   uint32_t base_z_or_array_layer;
   /* Before sanitization, the upper bound of the layer range can exceed the mip level layer count
    * (including being `UINT32_MAX`, which is `VK_REMAINING_ARRAY_LAYERS`), in which case it will be
    * silently clamped by sanitization.
    */
   uint32_t max_depth_or_layer_count;
};

/* Normalizes the subresource range, so that the ranges are clamped (including handling
 * `VK_REMAINING_*`, which is treated as `UINT32_MAX` maximum counts, by clamping), and returns
 * whether the subresource range can be passed safely further to descriptor creation functions
 * (including whether the subresource range isn't empty).
 * This includes #MemoryIntegrity sanitization.
 * If false is returned, the subresource range is unchanged.
 */
bool terakan_image_descriptor_subresource_range_sanitize(
   struct terakan_image const * image,
   struct terakan_image_descriptor_subresource_range * subresource_range, bool is_cube_view);

/* Before creating descriptors, the subresource range in the create info must be sanitized and
 * normalized using `terakan_image_descriptor_subresource_range_sanitize`.
 */
struct terakan_image_descriptor_create_info {
   struct terakan_image const * image;
   struct terascale_format_info view_format;
   unsigned image_aspect_index;
   struct terakan_image_descriptor_subresource_range subresource_range;
};

/* Returns whether the descriptor was created successfully (if false, the destination structure is
 * not filled).
 *
 * Behaves as if `VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT` and
 * `VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT` are always enabled for all images they're applicable to.
 *
 * 1D views may be implicitly promoted to 2D if necessary (such as for images that can be used as
 * depth / stencil attachments, which must be tiled for DB access, but 1D textures must be linear),
 * so 1D textures must be sampled at V = 0.5 (with unnormalized coordinates, it's the center of the
 * first row, and with normalized, it's also the center of the first row if the image is 1 texel
 * tall).
 *
 * Dimensionality reinterpretation for single-sample images is allowed in many cases, see the
 * implementation for more details. Specifically for transfer implementation purposes, a
 * single-level `2D_ARRAY` resource descriptor can be created for any single-sample image (for 3D,
 * this is similar to what `VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT` allows for color attachments).
 * Also, for transfers, cross-aspect reinterpretation is allowed.
 *
 * If `component_mapping_opt` is NULL, an identity component mapping will be used.
 *
 * `terakan_image_descriptor_subresource_range_sanitize` must be done before calling.
 */
/* TODO(Triang3l): Decide how to implement `VK_EXT_image_sliced_view_of_3d`. The slice range must be
 * ignored for sampled images, but respected for storage images. Take read-only no-UAV (like in
 * vertex stages) storage image reads into account, as well as size queries, and if mutable
 * descriptor type makes supporting it more complicated.
 */
bool terakan_image_create_resource_descriptor(
   struct terakan_image_descriptor_create_info const * descriptor_create_info,
   uint32_t desired_dimensionality, VkComponentMapping const * component_mapping_opt,
   struct terakan_resource_descriptor * descriptor_out);

/* Returns the number of Z slices or array layers accessible through the descriptor as color
 * descriptors support fewer layers than texture resource descriptors, or 0 if a valid color
 * descriptor can't be created given the provided info (the destination structures are not filled in
 * this case).
 *
 * Size-compatible uncompressed views of compressed images, and 2D and 2D array views of 3D images,
 * are allowed regardless of whether that was requested in the client API. 1D images also can be
 * viewed as 2D.
 *
 * Meta draws like copying can access all `TERAKAN_IMAGE_MAX_SLICES` slices of the image even if its
 * depth or layer count exceeds `TERAKAN_IMAGE_MAX_TARGET_SLICES` by adding the result of this
 * function to `base_z_or_array_layer` of the subresource range, creating a new color descriptor,
 * and performing the draw again for the next subset of slices.
 *
 * For transfer implementation purposes, cross-aspect reinterpretation is allowed.
 *
 * `terakan_image_descriptor_subresource_range_sanitize` must be done before calling.
 */
uint32_t terakan_image_create_color_descriptor(
   struct terakan_image_descriptor_create_info const * descriptor_create_info,
   uint32_t desired_info_resource_type, struct terakan_color_descriptor * descriptor_out,
   struct terakan_color_meta_descriptor * meta_descriptor_out_opt);

/* Returns whether the descriptor was created successfully (if false, the destination structure is
 * not filled).
 *
 * 1D and 3D images can be viewed as 2D arrays.
 *
 * Subresource ranges containing slices beyond `TERAKAN_IMAGE_MAX_TARGET_SLICES` are not supported,
 * it's expected that images compatible with DB access with more slices than can be indexed via
 * `gl_Layer` are not exposed via the client API.
 *
 * `terakan_image_descriptor_subresource_range_sanitize` must be done before calling.
 */
/* The aspect format tables describe where each aspect sits within the combined depth/stencil
 * format, so the stencil aspect's value lands in the second component. Anything sampling the
 * stencil aspect on its own sees a single-component image whose value must be the R component, and
 * on R8xx that aspect has its own single-channel surface, so the value is in hardware channel X.
 *
 * Only sampling paths need this. Transfers use the aspect formats for the source and the
 * destination alike, where any consistent placement works.
 */
static inline struct terascale_format_info
terakan_image_stencil_aspect_sampled_format(struct terascale_format_info format)
{
   format.swizzle_r = TERASCALE_SWIZZLE_X;
   format.swizzle_g = TERASCALE_SWIZZLE_0;
   format.swizzle_b = TERASCALE_SWIZZLE_0;
   format.swizzle_a = TERASCALE_SWIZZLE_1;
   return format;
}

bool terakan_image_create_depth_stencil_descriptor(
   struct terakan_image const * image, enum terascale_r8xx_depth_format view_depth_format,
   bool view_may_have_stencil,
   struct terakan_image_descriptor_subresource_range const * subresource_range,
   struct terakan_depth_stencil_descriptor * descriptor_out);

struct terakan_image_view {
   struct vk_image_view vk;

   struct terakan_bo const * bo;

   struct terakan_resource_descriptor resource;

   /* Used for both RTVs and UAVs, so `INFO.RAT` is not specifically defined. */
   struct terakan_color_descriptor color;
   struct terakan_color_meta_descriptor color_meta;

   struct terakan_depth_stencil_descriptor depth_stencil;

   /* The resource descriptor says `NUM_FORMAT = SCALED` where an integer format would ordinarily
    * say `INT`, so the hardware returns the integer value as a float and seamless cube map
    * filtering stays on. The shader is told through
    * `terakan_push_constants_driver::texture_scaled_integer`, which is why this has to be
    * remembered per view.
    */
   bool resource_is_scaled_integer_cube;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(terakan_image_view, vk.base, VkImageView, VK_OBJECT_TYPE_IMAGE_VIEW)

#ifdef __cplusplus
}
#endif

#endif
