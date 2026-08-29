/*
 * Copyright © 2026 Vitaliy Triang3l Kuzmin
 *
 * SPDX-License-Identifier: MIT
 */

#include "terakan_meta_impl.h"

#include "terakan_barrier.h"
#include "terakan_entrypoints.h"
#include "terakan_image.h"
#include "terakan_sampler.h"

#include "util/bitscan.h"
#include "util/macros.h"
#include "util/u_debug.h"
#include "util/u_math.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>

enum {
   TERAKAN_META_BLIT_CONST_SCALE_X,
   TERAKAN_META_BLIT_CONST_OFF_X,
   TERAKAN_META_BLIT_CONST_SCALE_Y,
   TERAKAN_META_BLIT_CONST_OFF_Y,

   TERAKAN_META_BLIT_CONSTS_COUNT,

   /* The 3D shader reads its constants as two vec4s, so the depth coordinate starts the second one
    * rather than following the fourth value. The first four keep their positions, which is what
    * lets the hand-written 2D shader go on reading them unchanged.
    */
   TERAKAN_META_BLIT_CONST_COORD_Z = 4,

   TERAKAN_META_BLIT_CONSTS_3D_COUNT = 8,
};

static uint32_t const terakan_meta_blit_image_ps_r8xx[] = {
   S_SQ_CF_WORD0_ADDR(8),
   S_SQ_CF_ALU_WORD1_COUNT(0) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   S_SQ_CF_WORD0_ADDR(9),
   S_SQ_CF_ALU_WORD1_COUNT(0) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   S_SQ_CF_WORD0_ADDR(10) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(
      TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_BLIT_CONST_SCALE_X)) |
      S_SQ_CF_ALU_WORD1_COUNT(0) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   S_SQ_CF_WORD0_ADDR(11) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(
      TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_BLIT_CONST_SCALE_X)) |
      S_SQ_CF_ALU_WORD1_COUNT(0) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   S_SQ_CF_WORD0_ADDR(12) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(
      TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_BLIT_CONST_SCALE_X)) |
      S_SQ_CF_ALU_WORD1_COUNT(0) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   S_SQ_CF_WORD0_ADDR(13) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(
      TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_BLIT_CONST_SCALE_X)) |
      S_SQ_CF_ALU_WORD1_COUNT(0) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   S_SQ_CF_WORD0_ADDR(14),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(2),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) | S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   /* Copy floating-point position components separately before scaling. On Palm, applying a
    * MULADD_IEEE directly to both components aliases one source channel to the other.
    */
   TERAKAN_SHADER_OP1(true, 0, 'X', MOV, EG, 1, 'X', VEC_012),
   TERAKAN_SHADER_OP1(true, 0, 'Y', MOV, EG, 1, 'Y', VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_BLIT_CONST_SCALE_X) |
      TERAKAN_SHADER_OP2(true, 0, 'X', MUL_IEEE, EG, 0, 'X', 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_BLIT_CONST_SCALE_Y) |
      TERAKAN_SHADER_OP2(true, 0, 'Y', MUL_IEEE, EG, 0, 'Y', 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_BLIT_CONST_OFF_X) |
      TERAKAN_SHADER_OP2(true, 0, 'X', ADD, EG, 0, 'X', 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_BLIT_CONST_OFF_Y) |
      TERAKAN_SHADER_OP2(true, 0, 'Y', ADD, EG, 0, 'Y', 0, 0, VEC_012),

   S_SQ_TEX_WORD0_TEX_INST(SQ_TEX_INST_SAMPLE) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(2) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_TEX_WORD1_COORD_TYPE_X(V_SQ_TEX_WORD1_COORD_NORMALIZED) |
      S_SQ_TEX_WORD1_COORD_TYPE_Y(V_SQ_TEX_WORD1_COORD_NORMALIZED),
   S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_0) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_0) |
      S_SQ_TEX_WORD2_SAMPLER_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META),
   0,
};

static uint32_t const terakan_meta_blit_image_ps_r9xx[] = {
   S_SQ_CF_WORD0_ADDR(4) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(
      TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_BLIT_CONST_SCALE_X)) |
      S_SQ_CF_ALU_WORD1_COUNT(1) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   S_SQ_CF_WORD0_ADDR(6),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |
      S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0) | S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(0),
   S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_W) |
      S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE,

   TERAKAN_SHADER_CF_END_R9XX,

   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_BLIT_CONST_SCALE_X) |
      TERAKAN_KCACHE_DWORD_WORD1_SRC2(0, TERAKAN_META_BLIT_CONST_OFF_X) |
      TERAKAN_SHADER_OP3(false, 0, 'X', MULADD_IEEE, EG, 0, 'X', 0, 0, 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_BLIT_CONST_SCALE_Y) |
      TERAKAN_KCACHE_DWORD_WORD1_SRC2(0, TERAKAN_META_BLIT_CONST_OFF_Y) |
      TERAKAN_SHADER_OP3(true, 0, 'Y', MULADD_IEEE, EG, 0, 'Y', 0, 0, 0, 0, VEC_012),

   S_SQ_TEX_WORD0_TEX_INST(3) |
      S_SQ_TEX_WORD0_RESOURCE_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_TEX_WORD0_SRC_GPR(0),
   S_SQ_TEX_WORD1_DST_GPR(0) | S_SQ_TEX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_TEX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_W),
   S_SQ_TEX_WORD2_SRC_SEL_X(TERASCALE_SWIZZLE_X) | S_SQ_TEX_WORD2_SRC_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_TEX_WORD2_SRC_SEL_Z(TERASCALE_SWIZZLE_Z) | S_SQ_TEX_WORD2_SRC_SEL_W(TERASCALE_SWIZZLE_0),
   0,
};

struct terakan_meta_shader const terakan_meta_blit_image_ps = {
   .r8xx =
      {
         .program = terakan_meta_blit_image_ps_r8xx,
         .program_size_bytes = sizeof(terakan_meta_blit_image_ps_r8xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     S_028844_NUM_GPRS(3) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                     TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                  },
               .stage =
                  {
                     .ps =
                        {
                           .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                           .spi_ps_in_control =
                              {
                                 S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1) |
                                    S_0286CC_POSITION_ENA(1) | S_0286CC_POSITION_ADDR(1),
                                 0,
                              },
                           .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
                           .cb_shader_mask = 0xF,
                        },
                  },
            },
      },
   .r9xx =
      {
         .program = terakan_meta_blit_image_ps_r9xx,
         .program_size_bytes = sizeof(terakan_meta_blit_image_ps_r9xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     S_028844_NUM_GPRS(1) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
                     TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,
                  },
               .stage =
                  {
                     .ps =
                        {
                           .sq_pgm_exports_ps = S_02884C_EXPORT_COLORS(1),
                           .spi_ps_in_control =
                              {
                                 S_0286CC_NUM_INTERP(1) | S_0286CC_LINEAR_GRADIENT_ENA(1),
                                 S_0286D0_FIXED_PT_POSITION_ENA(1) |
                                    S_0286D0_FIXED_PT_POSITION_ADDR(0),
                              },
                           .spi_baryc_cntl = S_0286E0_LINEAR_CENTER_ENA(1),
                           .cb_shader_mask = 0xF,
                        },
                  },
            },
      },
   .kcache_used = BITFIELD_BIT(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS),
   .samplers_used = BITFIELD_BIT(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META),
   .primary_meta_resource_used = true,
};

static void
terakan_meta_blit_set_fs_sampler(struct terakan_gfx_command_writer * const command_writer,
                                  VkFilter const filter, bool const filter_depth)
{
   struct terakan_hw_config_sqk * const sqk = &command_writer->hw_config_sqk;
   uint32_t * const sampler =
      sqk->stages_[MESA_SHADER_FRAGMENT]
         .samplers[TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META]
         .sampler;
   uint32_t const xy_filter = terakan_sampler_translate_filter(filter, false);
   uint32_t const clamp = terakan_sampler_translate_address_mode(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

   /* Z_FILTER is a field of its own: a linear XY filter does nothing along depth, which is exactly
    * why a 3D linear blit came out as the nearest slice. It is left at point for everything else,
    * where the third coordinate selects an array layer and must not be interpolated.
    */
   sampler[0] = S_03C000_CLAMP_X(clamp) | S_03C000_CLAMP_Y(clamp) | S_03C000_CLAMP_Z(clamp) |
                S_03C000_XY_MAG_FILTER(xy_filter) | S_03C000_XY_MIN_FILTER(xy_filter) |
                S_03C000_Z_FILTER(filter_depth ? V_03C000_SQ_TEX_Z_FILTER_LINEAR
                                               : V_03C000_SQ_TEX_Z_FILTER_POINT) |
                S_03C000_MIP_FILTER(V_03C000_SQ_TEX_XY_FILTER_POINT) |
                S_03C000_BORDER_COLOR_TYPE(V_03C000_SQ_TEX_BORDER_COLOR_TRANS_BLACK);
   sampler[1] = S_03C004_MIN_LOD(0) | S_03C004_MAX_LOD(0x3ff);
   sampler[2] = 0;
   sampler[3] = 0;
   sqk->stages_[MESA_SHADER_FRAGMENT].modified.samplers |=
      BITFIELD_BIT(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META);
   sqk->draw_stages_pending_ |= BITFIELD_BIT(MESA_SHADER_FRAGMENT);
}

static void
terakan_meta_blit_emit_pre_draw_barriers(struct terakan_gfx_command_writer * const command_writer,
                                          bool const same_image)
{
   if (!terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx) {
      /* R8xx: rely on app barriers; unconditional flush here hangs Palm on mip chains. */
      return;
   }

   enum terakan_barrier_action_flags actions =
      TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
      TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS |
      TERAKAN_BARRIER_ACTION_INV_TC;

   if (same_image) {
      actions |= TERAKAN_BARRIER_ACTION_INV_VC;
   }

   actions |= command_writer->post_color_image_copy_write_barrier_actions;

   terakan_barrier_emit_actions_unconditionally(command_writer, actions);
   command_writer->post_color_image_copy_write_barrier_actions &= ~actions;
}

static void
terakan_meta_blit_region_scale_off(int32_t const src0, int32_t const src1, int32_t const dst0,
                                   int32_t const dst1, float * const scale_out,
                                   float * const off_out)
{
   float const src_size = (float)(src1 - src0);
   float const dst_size = (float)(dst1 - dst0);

   *scale_out = src_size / dst_size;
   *off_out = (float)src0 - (float)dst0 * (*scale_out);
}

/* The depth axis of a blit region is scaled and signed like the other two, so a destination slice
 * corresponds to a source slice rather than to the slice with the same index. Returns that source
 * slice relative to the start of the sampled range.
 *
 * Both deltas carry their direction, so a reversed range mirrors without a special case. The source
 * is sampled as a 2D array, which has no depth filter, so this is the nearest slice whichever
 * filter the blit asked for.
 */
static uint32_t
terakan_meta_blit_depth_source_slice(int32_t const src_begin, int32_t const src_end,
                                     int32_t const dst_begin, int32_t const dst_end,
                                     uint32_t const dst_slice, uint32_t const src_slice_count)
{
   assert(dst_begin != dst_end);

   float const dst_center = (float)(MIN2(dst_begin, dst_end) + (int32_t)dst_slice) + 0.5F;
   float const normalized = (dst_center - (float)dst_begin) / (float)(dst_end - dst_begin);
   int32_t const source = src_begin + (int32_t)floorf(normalized * (float)(src_end - src_begin));

   return (uint32_t)CLAMP(source - MIN2(src_begin, src_end), 0, (int32_t)src_slice_count - 1);
}

static bool
terakan_meta_blit_region_to_copy(VkImageBlit2 const * const blit, bool const formats_identical,
                                 VkImageCopy2 * const copy_out)
{
   int32_t const src_x0 = blit->srcOffsets[0].x;
   int32_t const src_x1 = blit->srcOffsets[1].x;
   int32_t const src_y0 = blit->srcOffsets[0].y;
   int32_t const src_y1 = blit->srcOffsets[1].y;
   int32_t const src_z0 = blit->srcOffsets[0].z;
   int32_t const src_z1 = blit->srcOffsets[1].z;

   int32_t const dst_x0 = blit->dstOffsets[0].x;
   int32_t const dst_x1 = blit->dstOffsets[1].x;
   int32_t const dst_y0 = blit->dstOffsets[0].y;
   int32_t const dst_y1 = blit->dstOffsets[1].y;
   int32_t const dst_z0 = blit->dstOffsets[0].z;
   int32_t const dst_z1 = blit->dstOffsets[1].z;

   uint32_t const src_width = (uint32_t)abs(src_x1 - src_x0);
   uint32_t const src_height = (uint32_t)abs(src_y1 - src_y0);
   uint32_t const dst_width = (uint32_t)abs(dst_x1 - dst_x0);
   uint32_t const dst_height = (uint32_t)abs(dst_y1 - dst_y0);
   uint32_t const src_depth = (uint32_t)abs(src_z1 - src_z0);
   uint32_t const dst_depth = (uint32_t)abs(dst_z1 - dst_z0);

   if (src_width == 0 || src_height == 0 || dst_width == 0 || dst_height == 0 ||
       src_depth == 0 || dst_depth == 0) {
      return false;
   }

   if (!formats_identical || src_width != dst_width || src_height != dst_height ||
       src_depth != dst_depth) {
      return false;
   }

   /* CopyImage cannot mirror. Preserve any reversed source or destination axis for the sampled
    * blit path, where the signed coordinate transform performs the reflection.
    */
   if ((src_x1 - src_x0 < 0) != (dst_x1 - dst_x0 < 0) ||
       (src_y1 - src_y0 < 0) != (dst_y1 - dst_y0 < 0) ||
       (src_z1 - src_z0 < 0) != (dst_z1 - dst_z0 < 0)) {
      return false;
   }

   *copy_out = (VkImageCopy2){
      .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
      .srcSubresource = blit->srcSubresource,
      .dstSubresource = blit->dstSubresource,
      .srcOffset =
         {
            .x = MIN2(src_x0, src_x1),
            .y = MIN2(src_y0, src_y1),
            .z = MIN2(src_z0, src_z1),
         },
      .dstOffset =
         {
            .x = MIN2(dst_x0, dst_x1),
            .y = MIN2(dst_y0, dst_y1),
            .z = MIN2(dst_z0, dst_z1),
         },
      .extent =
         {
            .width = src_width,
            .height = src_height,
            .depth = src_depth,
         },
   };

   return true;
}

#define TERAKAN_META_BLIT_TILE_SIZE 128

static void
terakan_meta_blit_image_region_draw(struct terakan_gfx_command_writer * const command_writer,
                                    VkBlitImageInfo2 const * const blit_info,
                                    VkImageBlit2 const * const region,
                                    uint32_t const * const xy_constants)
{
   struct terakan_image const * const dst_image =
      terakan_image_from_handle(blit_info->dstImage);
   struct terakan_image const * const src_image =
      terakan_image_from_handle(blit_info->srcImage);

   int32_t const dst_x0 = MIN2(region->dstOffsets[0].x, region->dstOffsets[1].x);
   int32_t const dst_y0 = MIN2(region->dstOffsets[0].y, region->dstOffsets[1].y);
   uint32_t const dst_width = (uint32_t)abs(region->dstOffsets[1].x - region->dstOffsets[0].x);
   uint32_t const dst_height = (uint32_t)abs(region->dstOffsets[1].y - region->dstOffsets[0].y);

   if (dst_width == 0 || dst_height == 0) {
      return;
   }

   /* A 3D image takes its depth range from the region's offsets, where it is scaled and signed like
    * the other two axes. An array image takes it from the subresource layers, where Vulkan requires
    * the two layer counts to match, so the mapping there is always one to one and ascending.
    */
   bool const src_is_3d = src_image->vk.image_type == VK_IMAGE_TYPE_3D;
   bool const dst_is_3d = dst_image->vk.image_type == VK_IMAGE_TYPE_3D;

   /* Sampling the source as a 2D array gives the nearest slice whatever the filter asks for, which
    * is correct for an array -- there is nothing between two layers -- and wrong for a 3D image,
    * where VK_FILTER_LINEAR must interpolate along depth as well. Only that case needs the 3D
    * resource, the depth-filtering sampler and the shader that takes a depth coordinate.
    */
   bool const filter_depth = src_is_3d && blit_info->filter == VK_FILTER_LINEAR;
   terakan_meta_config_draw_set_sq_pgm_ps(command_writer,
                                          filter_depth ? TERAKAN_META_SHADER_BLIT_IMAGE_3D_PS
                                                       : TERAKAN_META_SHADER_BLIT_IMAGE_PS);
   terakan_meta_blit_set_fs_sampler(command_writer, blit_info->filter, filter_depth);
   int32_t const src_z_begin =
      src_is_3d ? region->srcOffsets[0].z : (int32_t)region->srcSubresource.baseArrayLayer;
   int32_t const src_z_end =
      src_is_3d ? region->srcOffsets[1].z
                : (int32_t)(region->srcSubresource.baseArrayLayer +
                            region->srcSubresource.layerCount);
   int32_t const dst_z_begin =
      dst_is_3d ? region->dstOffsets[0].z : (int32_t)region->dstSubresource.baseArrayLayer;
   int32_t const dst_z_end =
      dst_is_3d ? region->dstOffsets[1].z
                : (int32_t)(region->dstSubresource.baseArrayLayer +
                            region->dstSubresource.layerCount);
   if (src_z_begin == src_z_end || dst_z_begin == dst_z_end) {
      return;
   }

   struct terakan_image_descriptor_create_info dst_descriptor_create_info = {.image = dst_image};
   struct terakan_image_descriptor_create_info src_descriptor_create_info = {.image = src_image};

   unsigned src_vk_aspect_mask_remaining = (unsigned)region->srcSubresource.aspectMask;
   u_foreach_bit (dst_vk_aspect_bit_index, region->dstSubresource.aspectMask) {
      dst_descriptor_create_info.image_aspect_index = terakan_format_aspect_index(
         dst_image->format_info.aspect_map, (VkImageAspectFlags)1 << dst_vk_aspect_bit_index, 0);
      dst_descriptor_create_info.view_format =
         dst_image->format_info.aspect_formats[dst_descriptor_create_info.image_aspect_index];
      src_descriptor_create_info.image_aspect_index = terakan_format_aspect_index(
         src_image->format_info.aspect_map,
         (VkImageAspectFlags)1 << u_bit_scan(&src_vk_aspect_mask_remaining), 0);
      src_descriptor_create_info.view_format =
         src_image->format_info.aspect_formats[src_descriptor_create_info.image_aspect_index];

      dst_descriptor_create_info.subresource_range.base_mip_level = region->dstSubresource.mipLevel;
      dst_descriptor_create_info.subresource_range.max_level_count = 1;
      src_descriptor_create_info.subresource_range.base_mip_level = region->srcSubresource.mipLevel;
      src_descriptor_create_info.subresource_range.max_level_count = 1;

      src_descriptor_create_info.subresource_range.base_z_or_array_layer =
         (uint32_t)MIN2(src_z_begin, src_z_end);
      src_descriptor_create_info.subresource_range.max_depth_or_layer_count =
         (uint32_t)abs(src_z_end - src_z_begin);
      dst_descriptor_create_info.subresource_range.base_z_or_array_layer =
         (uint32_t)MIN2(dst_z_begin, dst_z_end);
      dst_descriptor_create_info.subresource_range.max_depth_or_layer_count =
         (uint32_t)abs(dst_z_end - dst_z_begin);

      if (unlikely(!terakan_image_descriptor_subresource_range_sanitize(
                      dst_image, &dst_descriptor_create_info.subresource_range, false) ||
                   !terakan_image_descriptor_subresource_range_sanitize(
                      src_image, &src_descriptor_create_info.subresource_range, false))) {
         continue;
      }

      if (!terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx) {
         /* Palm: full pre-blit barrier hangs; invalidate TC so PS can sample the source mip. */
         terakan_barrier_emit_actions_unconditionally(command_writer,
                                                      TERAKAN_BARRIER_ACTION_INV_TC);
      }

      struct terakan_resource_descriptor src_descriptor;
      /* A 3D resource descriptor has no equivalent of BASE_ARRAY -- the slices of a tiled 3D image
       * are interleaved, so the depth range cannot be moved by re-basing -- and it therefore always
       * covers the whole mip. The depth coordinate is normalized over that, exactly as X and Y are
       * normalized over the mip's width and height. Normalizing over the region's own depth range
       * instead is what broke the partial-range cases of blit_image.simple_tests on the first
       * attempt.
       */
      uint32_t const src_mip_depth =
         u_minify(src_image->vk.extent.depth, region->srcSubresource.mipLevel);
      struct terakan_image_descriptor_create_info src_sample_create_info =
         src_descriptor_create_info;
      if (filter_depth) {
         src_sample_create_info.subresource_range.base_z_or_array_layer = 0;
         src_sample_create_info.subresource_range.max_depth_or_layer_count = src_mip_depth;
      }
      if (unlikely(!terakan_image_create_resource_descriptor(
                      &src_sample_create_info,
                      filter_depth ? V_030000_SQ_TEX_DIM_3D : V_030000_SQ_TEX_DIM_2D_ARRAY, NULL,
                      &src_descriptor))) {
         continue;
      }

      uint32_t const src_slice_count =
         src_descriptor_create_info.subresource_range.max_depth_or_layer_count;
      uint32_t const dst_slice_count =
         dst_descriptor_create_info.subresource_range.max_depth_or_layer_count;
      if (src_slice_count == 0 || dst_slice_count == 0) {
         continue;
      }
      uint32_t const src_descriptor_base_array = G_030014_BASE_ARRAY(src_descriptor.resource[5]);
      uint32_t const dst_range_base_slice =
         dst_descriptor_create_info.subresource_range.base_z_or_array_layer;

      /* One destination slice per draw. The pixel shader samples the source at a constant array
       * layer, so a draw covering several destination slices would read the same source slice for
       * all of them; the source slice is selected by re-basing its descriptor instead. That also
       * makes an arbitrary depth mapping, scaled or reversed, cost nothing extra.
       */
      for (uint32_t dst_slice = 0; dst_slice < dst_slice_count; ++dst_slice) {
         dst_descriptor_create_info.subresource_range.base_z_or_array_layer =
            dst_range_base_slice + dst_slice;
         dst_descriptor_create_info.subresource_range.max_depth_or_layer_count = 1;

         struct terakan_color_descriptor dst_descriptor;
         uint32_t const dst_descriptor_slices = terakan_image_create_color_descriptor(
            &dst_descriptor_create_info, V_028C70_TEXTURE2DARRAY, &dst_descriptor, NULL);
         if (unlikely(dst_descriptor_slices == 0)) {
            break;
         }

         if (filter_depth) {
            /* The depth coordinate is constant for the draw, since a draw covers one destination
             * slice, so it goes in with the constants rather than being interpolated. It is
             * normalized over the sampled depth range, which is what the descriptor covers, the
             * same way the X and Y constants are normalized over the source mip extent.
             */
            float scale_z, off_z;
            terakan_meta_blit_region_scale_off(src_z_begin, src_z_end, dst_z_begin, dst_z_end,
                                               &scale_z, &off_z);
            float const dst_center = (float)(MIN2(dst_z_begin, dst_z_end) + (int32_t)dst_slice) +
                                     0.5F;
            float const src_z = dst_center * scale_z + off_z;
            uint32_t constants_3d[TERAKAN_META_BLIT_CONSTS_3D_COUNT] = {0};
            memcpy(constants_3d, xy_constants,
                   sizeof(uint32_t) * TERAKAN_META_BLIT_CONSTS_COUNT);
            constants_3d[TERAKAN_META_BLIT_CONST_COORD_Z] =
               fui(src_z / (float)src_mip_depth);
            if (unlikely(!terakan_meta_config_draw_set_kcache_push_constants(
                   command_writer, sizeof(constants_3d), constants_3d, false, true))) {
               break;
            }
         } else {
            src_descriptor.resource[5] =
               (src_descriptor.resource[5] & C_030014_BASE_ARRAY) |
               S_030014_BASE_ARRAY(src_descriptor_base_array +
                                   terakan_meta_blit_depth_source_slice(src_z_begin, src_z_end,
                                                                        dst_z_begin, dst_z_end,
                                                                        dst_slice, src_slice_count));
         }

         terakan_meta_config_draw_set_cb_rtvs_and_db_shader_control(
            command_writer, 0xF, &dst_image->bo, &dst_descriptor, NULL,
            TERAKAN_SHADER_DB_SHADER_CONTROL_IDENTITY);

         terakan_hw_config_sqk_set_resource_fs(
            &command_writer->hw_config_sqk,
            TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META, src_image->bo, &src_descriptor);

         struct terakan_screen_rect const screen_bounds = {
            .bounds = {[1] = {G_028C78_WIDTH_MAX(dst_descriptor.dim) + 1,
                              G_028C78_HEIGHT_MAX(dst_descriptor.dim) + 1}},
         };

         for (uint32_t tile_y = 0; tile_y < dst_height; tile_y += TERAKAN_META_BLIT_TILE_SIZE) {
            uint32_t const tile_h = MIN2(TERAKAN_META_BLIT_TILE_SIZE, dst_height - tile_y);
            for (uint32_t tile_x = 0; tile_x < dst_width; tile_x += TERAKAN_META_BLIT_TILE_SIZE) {
               uint32_t const tile_w = MIN2(TERAKAN_META_BLIT_TILE_SIZE, dst_width - tile_x);

               VkOffset3D const tile_offset_blocks = vk_image_offset_to_elements(
                  &dst_image->vk,
                  (VkOffset3D){.x = dst_x0 + (int32_t)tile_x, .y = dst_y0 + (int32_t)tile_y, .z = 0});
               VkExtent3D const tile_extent_blocks =
                  vk_image_extent_to_elements(&dst_image->vk,
                                              (VkExtent3D){.width = tile_w, .height = tile_h, .depth = 1});
               VkRect2D const tile_region_rect = {
                  .offset = {.x = tile_offset_blocks.x, .y = tile_offset_blocks.y},
                  .extent = {.width = tile_extent_blocks.width, .height = tile_extent_blocks.height},
               };

               terakan_meta_draw_rect(
                  command_writer,
                  terakan_vk_rect_to_screen_rect(tile_region_rect, screen_bounds),
                  dst_descriptor_slices);
            }
         }

      }
   }
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdBlitImage2(VkCommandBuffer const commandBuffer,
                      VkBlitImageInfo2 const * const pBlitImageInfo)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;
   struct terakan_image const * const src_image =
      terakan_image_from_handle(pBlitImageInfo->srcImage);
   struct terakan_image const * const dst_image =
      terakan_image_from_handle(pBlitImageInfo->dstImage);
   bool const formats_identical = src_image->vk.format == dst_image->vk.format;

   if (getenv("TERAKAN_DEBUG_IMAGE_OPS") != NULL) {
      static unsigned blit_call_count;
      unsigned const call = blit_call_count++;
      if (call < 4096) {
         struct terakan_image const * const src_image =
            terakan_image_from_handle(pBlitImageInfo->srcImage);
         struct terakan_image const * const dst_image =
            terakan_image_from_handle(pBlitImageInfo->dstImage);
         fprintf(stderr,
                 "[TERAKAN_IMAGE_OP] blit #%u src=%p fmt=%u %ux%u dst=%p fmt=%u %ux%u "
                 "regions=%u filter=%u\n",
                 call, (void *)src_image, src_image->vk.format, src_image->vk.extent.width,
                 src_image->vk.extent.height, (void *)dst_image, dst_image->vk.format,
                 dst_image->vk.extent.width, dst_image->vk.extent.height,
                 pBlitImageInfo->regionCount, pBlitImageInfo->filter);
      }
   }

   if (debug_get_bool_option("TERAKAN_SKIP_SCALED_BLIT", false)) {
      STACK_ARRAY(VkImageCopy2, copies, pBlitImageInfo->regionCount);
      uint32_t copy_count = 0;

      for (uint32_t region_index = 0; region_index < pBlitImageInfo->regionCount; ++region_index) {
         if (terakan_meta_blit_region_to_copy(&pBlitImageInfo->pRegions[region_index],
                                              formats_identical,
                                              &copies[copy_count])) {
            copy_count++;
         }
      }

      if (copy_count > 0) {
         VkCopyImageInfo2 const copy_info = {
            .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
            .srcImage = pBlitImageInfo->srcImage,
            .srcImageLayout = pBlitImageInfo->srcImageLayout,
            .dstImage = pBlitImageInfo->dstImage,
            .dstImageLayout = pBlitImageInfo->dstImageLayout,
            .regionCount = copy_count,
            .pRegions = copies,
         };
         terakan_CmdCopyImage2(commandBuffer, &copy_info);
      }

      STACK_ARRAY_FINISH(copies);
      return;
   }

   STACK_ARRAY(VkImageCopy2, copies, pBlitImageInfo->regionCount);
   uint32_t copy_count = 0;
   bool has_scaled_regions = false;
   bool same_image_scaled = false;

   for (uint32_t region_index = 0; region_index < pBlitImageInfo->regionCount; ++region_index) {
      VkImageBlit2 const * const region = &pBlitImageInfo->pRegions[region_index];

      if (terakan_meta_blit_region_to_copy(region, formats_identical, &copies[copy_count])) {
         copy_count++;
      } else {
         has_scaled_regions = true;
         if (pBlitImageInfo->srcImage == pBlitImageInfo->dstImage) {
            same_image_scaled = true;
         }
      }
   }

   if (has_scaled_regions) {
      if (!command_writer->meta_blit_draw_session_active) {
         terakan_meta_blit_emit_pre_draw_barriers(command_writer, same_image_scaled);

         struct terakan_meta_config_draw_begin_options const meta_begin_options = {
            .vgt_primitive_type = V_008958_DI_PT_RECTLIST,
            .cb_and_db_shader_control_mode =
               TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_NORMAL_WITH_RTV_AND_DYNAMIC_DB_SHADER_CONTROL,
            .rasterization = {.enable = true},
         };
         terakan_meta_config_draw_begin(command_writer, &meta_begin_options);
         terakan_meta_config_draw_set_sq_pgm_vs(
            command_writer, TERAKAN_META_SHADER_POSITION_AND_LAYER_FROM_INDEX_VS);
         /* The pixel shader and the sampler are chosen per region rather than here: a blit may mix
          * regions that need the 3D depth filter with regions that must not have it.
          */
         command_writer->meta_blit_draw_session_active = true;
      }

      for (uint32_t region_index = 0; region_index < pBlitImageInfo->regionCount; ++region_index) {
         VkImageBlit2 const * const region = &pBlitImageInfo->pRegions[region_index];

         /* This call is only asking whether the region is one of the ones the loop above already
          * turned into a copy, so its output goes to a scratch destination. It used to be handed
          * `copies[0]`, which overwrote the first accumulated copy with the last convertible
          * region's parameters -- one region of the blit was silently replaced by a duplicate of
          * another. It went unnoticed because it only bites when a blit mixes convertible and
          * scaled regions, which is what the 3D groups of
          * dEQP-VK.api.copy_and_blit.core.blit_image.all_formats do.
          */
         VkImageCopy2 convertibility_probe;
         if (terakan_meta_blit_region_to_copy(region, formats_identical, &convertibility_probe)) {
            continue;
         }

         float scale_x, scale_y, off_x, off_y;
         terakan_meta_blit_region_scale_off(region->srcOffsets[0].x, region->srcOffsets[1].x,
                                            region->dstOffsets[0].x, region->dstOffsets[1].x,
                                            &scale_x, &off_x);
         terakan_meta_blit_region_scale_off(region->srcOffsets[0].y, region->srcOffsets[1].y,
                                            region->dstOffsets[0].y, region->dstOffsets[1].y,
                                            &scale_y, &off_y);
         float const src_mip_width =
            (float)u_minify(src_image->vk.extent.width, region->srcSubresource.mipLevel);
         float const src_mip_height =
            (float)u_minify(src_image->vk.extent.height, region->srcSubresource.mipLevel);
         scale_x /= src_mip_width;
         off_x /= src_mip_width;
         scale_y /= src_mip_height;
         off_y /= src_mip_height;

         uint32_t constants[TERAKAN_META_BLIT_CONSTS_COUNT] = {
            fui(scale_x),
            fui(off_x),
            fui(scale_y),
            fui(off_y),
         };
         terakan_meta_config_draw_set_kcache_push_constants(command_writer, sizeof(constants),
                                                            constants, false, true);
         terakan_meta_blit_image_region_draw(command_writer, pBlitImageInfo, region, constants);
      }

      command_writer->post_color_image_copy_write_barrier_actions |=
         TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
         TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;
      if (same_image_scaled) {
         command_writer->post_color_image_copy_write_barrier_actions |=
            TERAKAN_BARRIER_ACTION_INV_TC | TERAKAN_BARRIER_ACTION_INV_VC;
      }
   }

   if (copy_count > 0) {
      VkCopyImageInfo2 const copy_info = {
         .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
         .srcImage = pBlitImageInfo->srcImage,
         .srcImageLayout = pBlitImageInfo->srcImageLayout,
         .dstImage = pBlitImageInfo->dstImage,
         .dstImageLayout = pBlitImageInfo->dstImageLayout,
         .regionCount = copy_count,
         .pRegions = copies,
      };
      terakan_CmdCopyImage2(commandBuffer, &copy_info);
   }

   STACK_ARRAY_FINISH(copies);

   if (has_scaled_regions) {
      command_writer->meta_blit_draw_session_active = false;
   }
}
