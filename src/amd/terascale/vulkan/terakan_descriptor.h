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

#ifndef TERAKAN_DESCRIPTOR_H
#define TERAKAN_DESCRIPTOR_H

#include "terakan_bo.h"

#include "amd/terascale/common/terascale_format.h"
#include "gallium/drivers/r600/eg_sq.h"
#include "gallium/drivers/r600/evergreend.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stage indices for hardware boolean, loop and sampler constant addresses. */
enum terakan_descriptor_hw_stage {
   TERAKAN_DESCRIPTOR_HW_STAGE_PS = 0,
   TERAKAN_DESCRIPTOR_HW_STAGE_VSES = 1,
   TERAKAN_DESCRIPTOR_HW_STAGE_GS = 2,
   TERAKAN_DESCRIPTOR_HW_STAGE_HS = 3,
   TERAKAN_DESCRIPTOR_HW_STAGE_LS = 4,
   TERAKAN_DESCRIPTOR_HW_STAGE_CS = 5,
};

/* MRTs [0, TERAKAN_COLOR_HW_RTV_COUNT) support both color attachments and storage buffers/images.
 * MRTs [TERAKAN_COLOR_HW_RTV_COUNT, TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT) support only storage
 * buffers/images.
 *
 * RTV - Render Target View in Direct3D terms.
 * UAV - Unordered Access View in Direct3D, also known as Random Access Target (RAT) on TeraScale.
 */
#define TERAKAN_COLOR_HW_RTV_COUNT         8
#define TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT 12

/* Limit the UAV count in pixel shaders by maxFragmentCombinedOutputResources, which includes
 * "output Location decorated color attachments", and with dual-source blending, both sources
 * correspond to the same color attachment in Vulkan, but in the hardware, dual-source blending uses
 * two separate MRT indices and CB_COLOR1_INFO's SOURCE_FORMAT, so with dual-source blending, two
 * rather than one RTV/UAV bindings are occupied by the first attachment.
 */
#define TERAKAN_COLOR_UAV_COUNT_PIXEL (TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT - 1)

/* Constant cache (kcache) hardware properties. */
#define TERAKAN_KCACHE_HW_LINE_BYTES_LOG2          8
#define TERAKAN_KCACHE_HW_LINE_BYTES               (1 << TERAKAN_KCACHE_HW_LINE_BYTES_LOG2)
#define TERAKAN_KCACHE_HW_MAX_LINES_IN_BUFFER_LOG2 8
#define TERAKAN_KCACHE_HW_MAX_LINES_IN_BUFFER      (1 << TERAKAN_KCACHE_HW_MAX_LINES_IN_BUFFER_LOG2)
#define TERAKAN_KCACHE_HW_MAX_BUFFER_SIZE_BYTES_LOG2                                               \
   (TERAKAN_KCACHE_HW_LINE_BYTES_LOG2 + TERAKAN_KCACHE_HW_MAX_LINES_IN_BUFFER_LOG2)
#define TERAKAN_KCACHE_HW_MAX_BUFFER_SIZE_BYTES (1 << TERAKAN_KCACHE_HW_MAX_BUFFER_SIZE_BYTES_LOG2)
#define TERAKAN_KCACHE_HW_BUFFERS_PER_STAGE     16
/* "Indexed locks of banks 14 and 15 are ignored" according to the KCACHE_BANK_INDEX_MODE#
 * documentation.
 */
#define TERAKAN_KCACHE_HW_RELATIVE_INDEXABLE_BUFFERS 14

/* Kcache allocation. */
/* The buffer with driver and application and push constants is never accessed with a relative
 * index, place it near the end.
 */
#define TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS (TERAKAN_KCACHE_HW_BUFFERS_PER_STAGE - 1)
/* Number of kcache buffers starting from 0 allocated for uniform buffers from application pipeline
 * layouts.
 */
#define TERAKAN_KCACHE_MAX_UNIFORM_BUFFERS TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS

/* For easier writing of meta shaders.
 * offsetof doesn't produce a constant on MSVC 2022, so using dword offsets instead.
 */
#define TERAKAN_KCACHE_BANK_SOURCE_BASE(bank) ((0x80 << ((bank) >> 1)) + (((bank) & 0b1) << 5))
#define TERAKAN_KCACHE_DWORD_LINE(offset_dwords)                                                   \
   ((offset_dwords) / (TERAKAN_KCACHE_HW_LINE_BYTES / sizeof(uint32_t)))
#define TERAKAN_KCACHE_DWORD_VECTOR(offset_dwords)                                                 \
   (((offset_dwords) & (TERAKAN_KCACHE_HW_LINE_BYTES / sizeof(uint32_t) - 1)) / 4)
#define TERAKAN_KCACHE_DWORD_SOURCE(bank, offset_dwords)                                           \
   (TERAKAN_KCACHE_BANK_SOURCE_BASE(bank) + TERAKAN_KCACHE_DWORD_VECTOR(offset_dwords))
#define TERAKAN_KCACHE_DWORD_COMPONENT(offset_dwords) ((offset_dwords) & 3)
#define TERAKAN_KCACHE_DWORD_WORD0_SRC0(bank, offset_dwords)                                       \
   (S_SQ_ALU_WORD0_SRC0_SEL(TERAKAN_KCACHE_DWORD_SOURCE(bank, offset_dwords)) |                    \
    S_SQ_ALU_WORD0_SRC0_CHAN(TERAKAN_KCACHE_DWORD_COMPONENT(offset_dwords)))
#define TERAKAN_KCACHE_DWORD_WORD0_SRC1(bank, offset_dwords)                                       \
   (S_SQ_ALU_WORD0_SRC1_SEL(TERAKAN_KCACHE_DWORD_SOURCE(bank, offset_dwords)) |                    \
    S_SQ_ALU_WORD0_SRC1_CHAN(TERAKAN_KCACHE_DWORD_COMPONENT(offset_dwords)))
/* With a filler for the second OP2 operand for single-operand instructions. */
#define TERAKAN_KCACHE_DWORD_WORD0_SRC01(bank, offset_dwords)                                      \
   (TERAKAN_KCACHE_DWORD_WORD0_SRC0(bank, offset_dwords) |                                         \
    TERAKAN_KCACHE_DWORD_WORD0_SRC1(bank, offset_dwords))
#define TERAKAN_KCACHE_DWORD_WORD1_SRC2(bank, offset_dwords)                                       \
   (S_SQ_ALU_WORD1_OP3_SRC2_SEL(TERAKAN_KCACHE_DWORD_SOURCE(bank, offset_dwords)) |                \
    S_SQ_ALU_WORD1_OP3_SRC2_CHAN(TERAKAN_KCACHE_DWORD_COMPONENT(offset_dwords)))

#define TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE 176
#define TERAKAN_RESOURCE_HW_COUNT_VERTEX        160
#define TERAKAN_RESOURCE_HW_COUNT_FETCH         32

#define TERAKAN_RESOURCE_HW_OFFSET_PS 0
#define TERAKAN_RESOURCE_HW_OFFSET_VSES                                                            \
   (TERAKAN_RESOURCE_HW_OFFSET_PS + TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE)
#define TERAKAN_RESOURCE_HW_OFFSET_GS                                                              \
   (TERAKAN_RESOURCE_HW_OFFSET_VSES + TERAKAN_RESOURCE_HW_COUNT_VERTEX)
#define TERAKAN_RESOURCE_HW_OFFSET_HS                                                              \
   (TERAKAN_RESOURCE_HW_OFFSET_GS + TERAKAN_RESOURCE_HW_COUNT_VERTEX)
#define TERAKAN_RESOURCE_HW_OFFSET_LS                                                              \
   (TERAKAN_RESOURCE_HW_OFFSET_HS + TERAKAN_RESOURCE_HW_COUNT_VERTEX)
#define TERAKAN_RESOURCE_HW_OFFSET_CS                                                              \
   (TERAKAN_RESOURCE_HW_OFFSET_LS + TERAKAN_RESOURCE_HW_COUNT_VERTEX)
#define TERAKAN_RESOURCE_HW_OFFSET_FS                                                              \
   (TERAKAN_RESOURCE_HW_OFFSET_CS + TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE)
#define TERAKAN_RESOURCE_HW_COUNT (TERAKAN_RESOURCE_HW_OFFSET_FS + TERAKAN_RESOURCE_HW_COUNT_FETCH)

struct terakan_resource_descriptor {
   uint32_t resource[8];
};

/* Dynamically indexable immediate constant arrays in application shader code.
 * Also used for a single resource binding for meta draws or dispatches as it can be quickly
 * invalidated alongside the shader itself.
 */
#define TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META 0
/* Dynamically indexable driver and application push constants. */
#define TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS                                                      \
   (TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META + 1)
/* VS: Base vertex and instance.
 * GS and VS/TES ALT_CONST: Ring buffer.
 * CS: Number of workgroups.
 * Not needed in FS, so can be used for an additional input attachment.
 * Can be used in meta non-pixel shaders for an additional resource.
 */
#define TERAKAN_RESOURCE_RANGE_NON_PIXEL_STAGE_SPECIFIC (TERAKAN_RESOURCE_HW_COUNT_VERTEX - 1)
/* Pixel and compute shaders provide additional 16 resource bindings beyond the 160 available in
 * vertex stages. Use them for resources specific to those stages. In fragment shaders, place IMMED
 * buffers of UAVs (11 with the limitations of maxFragmentCombinedOutputResources's interaction with
 * dual-source blending) there, and give the rest of that range to an extension of the mutable
 * resource type descriptor space for use as input attachments (at least 4 are mandatory in Vulkan).
 * To reduce hardware binding changes when switching between fragment shaders that have the same UAV
 * bindings, but different color output counts, not offsetting immediate buffer resource indices by
 * the color output count unlike UAV indices themselves.
 */
#define TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_PIXEL                                            \
   (TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE - TERAKAN_COLOR_UAV_COUNT_PIXEL)
#define TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_COMPUTE                                          \
   (TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE - TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT)
/* Resources from the application's pipeline layout:
 * - Sampled images, uniform texel buffers.
 * - Storage images, storage texel buffers, storage buffers - read-only when coherence with writable
 *   ones is not needed (vertex stages, or `restrict readonly` without `coherent`), as well as for
 *   UAV info queries.
 * - Uniform buffers - for dynamic indexing.
 * - Input attachments.
 */
#define TERAKAN_RESOURCE_RANGE_MUTABLE_BASE (TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS + 1)
#define TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL                                         \
   (TERAKAN_RESOURCE_RANGE_NON_PIXEL_STAGE_SPECIFIC - TERAKAN_RESOURCE_RANGE_MUTABLE_BASE)
#define TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL                                             \
   (TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_PIXEL - TERAKAN_RESOURCE_RANGE_MUTABLE_BASE)
static_assert(
   TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL >=
      TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL,
   "The maximum number of mutable resource bindings per stage is expected to be bound by vertex "
   "stages, which have fewer hardware resource bindings.");
static_assert(
   TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL >= 15 + 4 + 128 + 8,
   "There should be enough non-pixel shader mutable resource bindings for the minimum Direct3D 11 "
   "binding counts plus Vulkan storage buffers.");
static_assert(
   TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL >= 15 + 4 + 128 + 8 + 4,
   "There should be enough pixel shader mutable resource bindings for the minimum Direct3D 11 "
   "binding counts plus Vulkan storage buffers and input attachments.");
#define TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_IN_PIPELINE                                       \
   (TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL * 5 +                                       \
    TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL)

/* SQ_VTX_CONSTANT doesn't have words 5 and 6, so using word 5 for the BO priority. */
#define TERAKAN_RESOURCE_BUFFER_PRIORITY_WORD 5

#define TERAKAN_SAMPLER_HW_COUNT_PER_STAGE 18

struct terakan_sampler_descriptor {
   uint32_t sampler[3];
   /* May be uninitialized if not used by the sampler. */
   uint32_t register_border_color[4];
};

/* `CB_COLOR[0-11]` register intermediate representation used for both RTVs and UAVs.
 * Throughout the driver, descriptors containing as detailed information as possible are passed, and
 * only in `terakan_hw_config`, the descriptors are normalized specifically for RTV or UAV usage.
 * Specifically, in the intermediate representation:
 * - For buffer UAVs (which, according to Radeon Evergreen / Northern Islands Acceleration, must use
 *   the `LINEAR_ALIGNED` array mode, not `LINEAR_GENERAL`), `VIEW` (as a 32-bit integer) contains
 *   the offset (in elements for Vulkan storage texel buffers, in bytes for Vulkan storage buffers)
 *   that needs to be added to the address when accessing the UAV in shaders, so that the base
 *   address in the client API can have a granularity smaller than the `LINEAR_ALIGNED` alignment
 *   requirement (pipe interleave - 256 or 512 bytes), which may be greater than the alignment
 *   expected by the API (16 bytes - `D3D11_RAW_UAV_SRV_BYTE_ALIGNMENT` - for byte address buffers
 *   in Direct3D 11, for example), or be technically allowed (like 256 bytes for storage buffers in
 *   Vulkan), yet severely limit application compatibility.
 * - `INFO.BLEND_BYPASS` and `INFO.SOURCE_FORMAT` are always configured according to the format,
 *   even for UAVs.
 *   - `INFO.SOURCE_FORMAT` in the intermediate representation must be selected specifically for the
 *     actual device architecture generation, since it's more important for it to reflect the
 *     hardware behavior (`DUAL_EXPORT_ENABLE` configuration also depends on it) rather than to
 *     provide more detailed abstract info about the format.
 * - `INFO.RAT` in most cases must be set depending on whether the color descriptor will be used as
 *   an RTV or as a UAV (most importantly when passing it to `terakan_hw_config_draw`), unless
 *   stated otherwise (like in a common descriptor stored in an image view).
 * - `INFO.RESOURCE_TYPE` and `DIM` are always configured, even for RTVs - there are cases where
 *   subresource pixel sizes (with mip level and size-compatible views of block-compressed images
 *   handled), rather than merely micro tile granularity pitches, may be useful when working with
 *   RTVs, such as for clamping and bounds checking purposes, and for determining whether the last
 *   micro tile column or row can be cleared in tile metadata fully if the size of the subresource
 *   is not a multiple of the micro tile size.
 * - `ATTRIB.NUM_SAMPLES` and `ATTRIB.NUM_FRAGMENTS` are always configured on all architecture
 *   generations, primarily for ensuring that the desired multisampling configuration is allowed.
 */
/* TODO(Triang3l): Revisit `buffer_uav_validated_as_image`. If it's not needed (UAVs are not tracked
 * by DRM Radeon because they're not in `CB_TARGET_MASK`), remove it completely from the driver.
 * Otherwise expand the comment about `VIEW` for buffers.
 */
struct terakan_color_descriptor {
   uint32_t base;
   uint32_t pitch;
   uint32_t slice;
   uint32_t view;
   uint32_t info;
   uint32_t attrib;
   uint32_t dim;
};

#define TERAKAN_COLOR_DESCRIPTOR_BUFFER_UAV_INFO_CONST_FIELDS                                      \
   (S_028C70_ARRAY_MODE(V_028C70_ARRAY_LINEAR_ALIGNED) | S_028C70_BLEND_BYPASS(true) |             \
    S_028C70_SOURCE_FORMAT(V_028C70_EXPORT_4C_32BPC) | S_028C70_RAT(true) |                        \
    S_028C70_RESOURCE_TYPE(V_028C70_BUFFER))
#define TERAKAN_COLOR_DESCRIPTOR_BUFFER_UAV_ATTRIB (S_028C74_NON_DISP_TILING_ORDER(true))

struct terakan_physical_device;

/* May increase as `bytes_per_element` grows. */
unsigned terakan_color_descriptor_buffer_uav_base_granularity_log2(
   unsigned bytes_per_element, struct terakan_physical_device const * physical_device);

void terakan_color_descriptor_calculate_buffer_pitch_slice(
   struct terakan_color_descriptor * descriptor, unsigned bytes_per_element,
   struct terakan_physical_device const * physical_device);

void terakan_color_descriptor_calculate_buffer_base_pitch_slice_dim_offset(
   struct terakan_color_descriptor * descriptor, uint64_t va, VkDeviceSize elements,
   unsigned bytes_per_element, struct terakan_physical_device const * physical_device,
   uint32_t * base_granularity_offset_bytes_out);

static inline void
terakan_color_descriptor_calculate_buffer_base_pitch_slice_view_dim(
   struct terakan_color_descriptor * const descriptor, uint64_t const va,
   VkDeviceSize const elements, unsigned const bytes_per_element,
   struct terakan_physical_device const * const physical_device,
   bool const base_granularity_offset_in_elements)
{
   uint32_t base_granularity_offset;
   terakan_color_descriptor_calculate_buffer_base_pitch_slice_dim_offset(
      descriptor, va, elements, bytes_per_element, physical_device, &base_granularity_offset);
   if (base_granularity_offset_in_elements) {
      base_granularity_offset /= bytes_per_element;
   }
   /* Used by the driver, must be zeroed before being passed to the hardware. */
   descriptor->view = base_granularity_offset;
}

/* If `bo` is NULL, `descriptor` is ignored. */
static inline bool
terakan_color_descriptor_is_bound(struct terakan_bo const * const bo,
                                  struct terakan_color_descriptor const * const descriptor)
{
   /* Many callers expect these checks, including the NULL pointer checks, to be done by this
    * function, and make assumptions based on their presence, so they must not be removed without
    * updating the callers.
    * `FORMAT == INVALID` exactly is used by DRM Radeon as of 2.50.0 and 2.51.0 to determine if a
    * color target enabled in `CB_TARGET_MASK` is unbound.
    */
   return bo != NULL && descriptor != NULL &&
          G_028C70_FORMAT(descriptor->info) != TERASCALE_FORMAT_INDEX_INVALID;
}

static inline void
terakan_color_descriptor_to_hw_rtv(struct terakan_color_descriptor * const descriptor)
{
   descriptor->info &= C_028C70_RAT & C_028C70_RESOURCE_TYPE;
   /* The meaning of `DIM` depends on `RESOURCE_TYPE`, but it's used only for UAVs by the hardware.
    * For RTVs, scissor must be used to prevent out-of-bounds access.
    */
   descriptor->dim = 0;
}

static inline void
terakan_color_descriptor_to_hw_uav(struct terakan_color_descriptor * const descriptor)
{
   if (G_028C70_RESOURCE_TYPE(descriptor->info) == V_028C70_BUFFER) {
      /* Remove the buffer UAV base offset stored for internal use in the driver. */
      descriptor->view = 0;
   }
   descriptor->info &= C_028C70_FAST_CLEAR & C_028C70_COMPRESSION & C_028C70_BLEND_CLAMP &
                       C_028C70_SIMPLE_FLOAT & C_028C70_SOURCE_FORMAT;
   descriptor->info |= S_028C70_BLEND_BYPASS(true) |
                       S_028C70_SOURCE_FORMAT(V_028C70_EXPORT_4C_32BPC) | S_028C70_RAT(true);
   descriptor->attrib &= C_028C74_FORCE_DST_ALPHA_1;
}

struct terakan_device;

/* The BO is `device->uav_immediate_bo`. */
struct terakan_resource_descriptor
terakan_color_descriptor_info_to_uav_immediate_resource(struct terakan_device const * device,
                                                        uint32_t color_info);

/* Additional hardware CB_COLOR[0-7] registers. */
struct terakan_color_meta_descriptor {
   uint32_t cmask;
   uint32_t cmask_slice;
   /* For single-sampled images, FMASK must be equal to BASE. */
   uint32_t fmask;
   uint32_t fmask_slice;
};

static inline struct terakan_color_meta_descriptor
terakan_color_meta_descriptor_create_disabled(struct terakan_color_descriptor const * const color)
{
   return (struct terakan_color_meta_descriptor){
      .cmask = color->base,
      .cmask_slice = S_028C80_TILE_MAX(0),
      .fmask = color->base,
      .fmask_slice = S_028C88_TILE_MAX(G_028C68_SLICE_TILE_MAX(color->slice)),
   };
}

struct terakan_depth_stencil_descriptor {
   uint32_t view;
   /* `DB_Z_INFO` contains some configuration shared between depth and stencil, even if the image
    * contains only stencil.
    * `NUM_SAMPLES` is used throughout the driver regardless of the architecture generation for
    * purposes like ensuring that no framebuffer attachments with incompatible sample counts are
    * bound for #MemoryIntegrity.
    */
   uint32_t z_info;
   uint32_t stencil_info;
   uint32_t z_base;
   uint32_t stencil_base;
   uint32_t size;
   uint32_t slice;
};

/* If `bo` is NULL, `descriptor` is ignored. */
static inline void
terakan_depth_stencil_descriptor_is_bound(
   struct terakan_bo const * const bo,
   struct terakan_depth_stencil_descriptor const * const descriptor, bool * const depth_bound_out,
   bool * const stencil_bound_out)
{
   /* Many callers expect these checks, including the NULL pointer checks, to be done by this
    * function, and make assumptions based on their presence, so they must not be removed without
    * updating the callers.
    * `FORMAT == INVALID` exactly is used by DRM Radeon as of 2.50.0 and 2.51.0 to determine if an
    * aspect of the image enabled in `DB_DEPTH_CONTROL` is unbound.
    */
   if (bo == NULL || descriptor == NULL) {
      if (depth_bound_out != NULL) {
         *depth_bound_out = false;
      }
      if (stencil_bound_out != NULL) {
         *stencil_bound_out = false;
      }
      return;
   }
   if (depth_bound_out != NULL) {
      *depth_bound_out = G_028040_FORMAT(descriptor->z_info) != V_028040_Z_INVALID;
   }
   if (stencil_bound_out != NULL) {
      *stencil_bound_out = G_028044_FORMAT(descriptor->stencil_info) != V_028044_STENCIL_INVALID;
   }
}

static inline bool
terakan_descriptor_type_has_resource(VkDescriptorType const descriptor_type)
{
   return descriptor_type != VK_DESCRIPTOR_TYPE_SAMPLER;
}

static inline bool
terakan_descriptor_type_has_sampler(VkDescriptorType const descriptor_type)
{
   return descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLER ||
          descriptor_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

static inline bool
terakan_descriptor_type_has_uav(VkDescriptorType const descriptor_type)
{
   if (descriptor_type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
       descriptor_type != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER &&
       descriptor_type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
       descriptor_type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
      return false;
   }
   /* For read-only bindings of the given types, and for size queries, UAVs always have a
    * corresponding resource.
    */
   assert(terakan_descriptor_type_has_resource(descriptor_type));
   return true;
}

bool
terakan_descriptor_create_for_uniform_buffer(struct terakan_bo const * bo, uint64_t va,
                                             VkDeviceSize range,
                                             struct terakan_resource_descriptor * resource_out);

bool terakan_descriptor_create_for_storage_buffer(
   struct terakan_bo const * bo, uint64_t va, VkDeviceSize range,
   struct terakan_physical_device const * physical_device,
   struct terakan_resource_descriptor * resource_out, struct terakan_color_descriptor * color_out);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_DESCRIPTOR_H */
