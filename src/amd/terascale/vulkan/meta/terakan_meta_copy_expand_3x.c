/*
 * Copyright © 2026 Vitaliy Triang3l Kuzmin
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

#include "terakan_meta_impl.h"

#include "terakan_buffer.h"
#include "terakan_image.h"
#include "terakan_physical_device.h"

#include "util/macros.h"

#include <assert.h>

/* One layer per draw for simplicity (eliminates lots of per-pixel instructions).
 * 3x-expanded images have only the color aspect, so always using the aspect 0 is sufficient.
 */

enum {
   /* All values are in surface elements (components) and unsigned. */
   TERAKAN_META_COPY_EXPAND_3X_CONST_SRC_PITCH,
   TERAKAN_META_COPY_EXPAND_3X_CONST_DST_PITCH,
   /* Sub-pipe-interleave offset plus an arbitrary offset of the top-left screen pixel in the
    * destination UAV.
    */
   TERAKAN_META_COPY_EXPAND_3X_CONST_DST_OFFSET,

   TERAKAN_META_COPY_EXPAND_3X_CONSTS_COUNT,
};

/* TODO(Triang3l): Cacheless UAV writes? */

/* UAV instructions don't support swizzling, so the destination address and the surfel must be in X.
 */

static uint32_t const terakan_meta_copy_expand_3x_ps_r8xx[] = {
   /* 0: Address calculation. */
   S_SQ_CF_WORD0_ADDR(7) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(
      TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_COPY_EXPAND_3X_CONST_SRC_PITCH)) |
      S_SQ_CF_ALU_WORD1_COUNT(8) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch from the source. */
   S_SQ_CF_WORD0_ADDR(16),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Store the R component. */
   TERAKAN_SHADER_CF_UAV(false, STORE_TYPED, 0, 1, 0, 0xF, true, true),

   /* 3: Moving the G and B components to GPR.X. */
   S_SQ_CF_WORD0_ADDR(18) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_COUNT(1) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 4: Store the G component. */
   TERAKAN_SHADER_CF_UAV(false, STORE_TYPED, 0, 2, 0, 0xF, true, true),

   /* 5: Store the B component. */
   TERAKAN_SHADER_CF_UAV(false, STORE_TYPED, 0, 3, 1, 0xF, true, false),

   /* 6: End. */
   TERAKAN_SHADER_CF_PS_DUMMY_EXPORT_DONE_AND_END_R8XX,

   /* 7: ALU clause before the fetch and R storing. */

   /* +0-2:
    * X in surfels plus the destination offset to PV.X.
    * Destination row address to PS.
    *
    *     PV.X = MULADD_UINT24 R0.X, 3, CB[push].dst_offset
    * (T) PS (Y) = MULLO_UINT R0.Y, CB[push].dst_pitch
    * Cycle 0: X = R0, T constant.
    * Cycle 2: Y = R0.
    */
   TERAKAN_SHADER_OP3_NW(false, 'X', MULADD_UINT24, EG, 0, 'X', V_SQ_ALU_SRC_LITERAL, 'X', 0, 0,
                         VEC_012) |
      TERAKAN_KCACHE_DWORD_WORD1_SRC2(0, TERAKAN_META_COPY_EXPAND_3X_CONST_DST_OFFSET),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_COPY_EXPAND_3X_CONST_DST_PITCH) |
      TERAKAN_SHADER_OP2_NW(true, 'Y', MULLO_UINT, EG, 0, 'Y', 0, 0, SCL_210),
   3,
   0,

   /* +3-4:
    * Destination R component address to R1.X.
    * Source row address to PS.
    *
    *     R1.X = ADD_INT PS, PV.X
    * (T) PS (Y) = MULLO_UINT R0.Y, CB[push].src_pitch
    * Cycle 0: T constant.
    * Cycle 2: Y = R0.
    */
   TERAKAN_SHADER_OP2(false, 1, 'X', ADD_INT, EG, V_SQ_ALU_SRC_PS, 0, V_SQ_ALU_SRC_PV, 'X',
                      VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_COPY_EXPAND_3X_CONST_SRC_PITCH) |
      TERAKAN_SHADER_OP2_NW(true, 'Y', MULLO_UINT, EG, 0, 'Y', 0, 0, SCL_210),

   /* +5-7:
    * Destination G component address to R2.X.
    * Source address to R0.W.
    *
    *     R2.X = ADD_INT PV.X, 1
    * (V) R0.W = MULADD_UINT24 R0.X, 3, PS
    * Cycle 0: X = R0.
    */
   TERAKAN_SHADER_OP2(false, 2, 'X', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'X', V_SQ_ALU_SRC_1_INT, 0,
                      VEC_012),
   TERAKAN_SHADER_OP3(true, 0, 'W', MULADD_UINT24, EG, 0, 'X', V_SQ_ALU_SRC_LITERAL, 'X',
                      V_SQ_ALU_SRC_PS, 0, VEC_012),
   3,
   0,

   /* +8: Destination B component address to R3.X.
    *
    * (v) R3.X = ADD_INT PV.X, 1
    */
   TERAKAN_SHADER_OP2(true, 3, 'X', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'X', V_SQ_ALU_SRC_1_INT, 0,
                      VEC_012),

   /* 16-17: Vertex-fetch from the source to R0.XYZ. */
   S_SQ_VTX_WORD0_FETCH_TYPE(SQ_VTX_FETCH_NO_INDEX_OFFSET) |
      S_SQ_VTX_WORD0_BUFFER_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_VTX_WORD0_SRC_GPR(0) | S_SQ_VTX_WORD0_SRC_SEL_X(TERASCALE_SWIZZLE_W) |
      S_SQ_VTX_WORD0_MEGA_FETCH_COUNT(sizeof(uint32_t) * 3 - 1),
   S_SQ_VTX_WORD1_GPR_DST_GPR(0) | S_SQ_VTX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_VTX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_VTX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_VTX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_MASK) | S_SQ_VTX_WORD1_USE_CONST_FIELDS(true),
   S_SQ_VTX_WORD2_MEGA_FETCH(true),
   0,

   /* 18: ALU clause after storing R and before storing G and B. */

   /* +0: Move the G component to R0.X.
    *
    * (v) R0.X = MOV R0.Y
    * Cycle 0: Y = R0.
    */
   TERAKAN_SHADER_OP1(true, 0, 'X', MOV, EG, 0, 'Y', VEC_012),

   /* +1: Move the B component to R1.X.
    *
    * (v) R1.X = MOV R0.Z
    * Cycle 0: Z = R0.
    */
   TERAKAN_SHADER_OP1(true, 1, 'X', MOV, EG, 0, 'Z', VEC_012),
};

static uint32_t const terakan_meta_copy_expand_3x_ps_r9xx[] = {
   /* 0: Address calculation. */
   S_SQ_CF_WORD0_ADDR(8) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_KCACHE_ADDR0(
      TERAKAN_KCACHE_DWORD_LINE(TERAKAN_META_COPY_EXPAND_3X_CONST_SRC_PITCH)) |
      S_SQ_CF_ALU_WORD1_COUNT(13) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 1: Fetch from the source. */
   S_SQ_CF_WORD0_ADDR(22),
   S_SQ_CF_WORD1_COUNT(0) | S_SQ_CF_WORD1_BARRIER(true) | EG_V_SQ_CF_WORD1_SQ_CF_INST_TEX,

   /* 2: Store the R component. */
   TERAKAN_SHADER_CF_UAV(false, STORE_TYPED, 0, 1, 0, 0xF, true, true),

   /* 3: Moving the G and B components to GPR.X. */
   S_SQ_CF_WORD0_ADDR(24) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_COUNT(1) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 4: Store the G component. */
   TERAKAN_SHADER_CF_UAV(false, STORE_TYPED, 0, 2, 0, 0xF, true, true),

   /* 5: Store the B component. */
   TERAKAN_SHADER_CF_UAV(false, STORE_TYPED, 0, 3, 1, 0xF, true, false),

   /* 6-7: End. */
   TERAKAN_SHADER_CF_PS_DUMMY_EXPORT_DONE_R9XX,
   TERAKAN_SHADER_CF_END_R9XX,

   /* 8: ALU clause before the fetch and R storing. */

   /* +0-3:
    * Destination row address to R1.X.
    * Done early due to the GPR dependency latency.
    *
    * MULLO_UINT uses 4 slots.
    * R1.X = MULLO_UINT R0.Y, CB[push].dst_pitch
    * PV.Y = MULLO_UINT R0.Y, CB[push].dst_pitch
    * PV.Z = MULLO_UINT R0.Y, CB[push].dst_pitch
    * PV.W = MULLO_UINT R0.Y, CB[push].dst_pitch
    * Cycle 0: Y = R0.
    */
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_COPY_EXPAND_3X_CONST_DST_PITCH) |
      TERAKAN_SHADER_OP2(false, 1, 'X', MULLO_UINT, EG, 0, 'Y', 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_COPY_EXPAND_3X_CONST_DST_PITCH) |
      TERAKAN_SHADER_OP2_NW(false, 'Y', MULLO_UINT, EG, 0, 'Y', 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_COPY_EXPAND_3X_CONST_DST_PITCH) |
      TERAKAN_SHADER_OP2_NW(false, 'Z', MULLO_UINT, EG, 0, 'Y', 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_COPY_EXPAND_3X_CONST_DST_PITCH) |
      TERAKAN_SHADER_OP2_NW(true, 'W', MULLO_UINT, EG, 0, 'Y', 0, 0, VEC_012),

   /* +4-7: Source row address to PV.
    *
    * PV.X = MULLO_UINT R0.Y, CB[push].src_pitch
    * PV.Y = MULLO_UINT R0.Y, CB[push].src_pitch
    * PV.Z = MULLO_UINT R0.Y, CB[push].src_pitch
    * PV.W = MULLO_UINT R0.Y, CB[push].src_pitch
    * Cycle 0: Y = R0.
    */
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_COPY_EXPAND_3X_CONST_SRC_PITCH) |
      TERAKAN_SHADER_OP2_NW(false, 'X', MULLO_UINT, EG, 0, 'Y', 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_COPY_EXPAND_3X_CONST_SRC_PITCH) |
      TERAKAN_SHADER_OP2_NW(false, 'Y', MULLO_UINT, EG, 0, 'Y', 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_COPY_EXPAND_3X_CONST_SRC_PITCH) |
      TERAKAN_SHADER_OP2_NW(false, 'Z', MULLO_UINT, EG, 0, 'Y', 0, 0, VEC_012),
   TERAKAN_KCACHE_DWORD_WORD0_SRC1(0, TERAKAN_META_COPY_EXPAND_3X_CONST_SRC_PITCH) |
      TERAKAN_SHADER_OP2_NW(true, 'W', MULLO_UINT, EG, 0, 'Y', 0, 0, VEC_012),

   /* +8-10:
    * X in surfels plus the destination offset to PV.X.
    * Source address to R0.W.
    *
    * PV.X = MULADD_UINT24 R0.X, 3, CB[push].dst_offset
    * R0.W = MULADD_UINT24 R0.X, 3, PV
    * Cycle 0: X = R0.
    */
   TERAKAN_SHADER_OP3_NW(false, 'X', MULADD_UINT24, EG, 0, 'X', V_SQ_ALU_SRC_LITERAL, 'X', 0, 0,
                         VEC_012) |
      TERAKAN_KCACHE_DWORD_WORD1_SRC2(0, TERAKAN_META_COPY_EXPAND_3X_CONST_DST_OFFSET),
   TERAKAN_SHADER_OP3(true, 0, 'W', MULADD_UINT24, EG, 0, 'X', V_SQ_ALU_SRC_LITERAL, 'X',
                      V_SQ_ALU_SRC_PV, 'W', VEC_012),
   3,
   0,

   /* +11: Destination R component address to R1.X.
    *
    * R1.X = ADD_INT R1.X, PV.X
    * Cycle 0: X = R1.
    */
   TERAKAN_SHADER_OP2(true, 1, 'X', ADD_INT, EG, 1, 'X', V_SQ_ALU_SRC_PV, 'X', VEC_012),

   /* +12: Destination G component address to R2.X.
    *
    * R2.X = ADD_INT PV.X, 1
    */
   TERAKAN_SHADER_OP2(true, 2, 'X', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'X', V_SQ_ALU_SRC_1_INT, 0,
                      VEC_012),

   /* +13: Destination B component address to R3.X.
    *
    * R3.X = ADD_INT PV.X, 1
    */
   TERAKAN_SHADER_OP2(true, 3, 'X', ADD_INT, EG, V_SQ_ALU_SRC_PV, 'X', V_SQ_ALU_SRC_1_INT, 0,
                      VEC_012),

   /* 22-23: Vertex-fetch from the source to R0.XYZ. */
   S_SQ_VTX_WORD0_FETCH_TYPE(SQ_VTX_FETCH_NO_INDEX_OFFSET) |
      S_SQ_VTX_WORD0_BUFFER_ID(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META) |
      S_SQ_VTX_WORD0_SRC_GPR(0) | S_SQ_VTX_WORD0_SRC_SEL_X(TERASCALE_SWIZZLE_W),
   S_SQ_VTX_WORD1_GPR_DST_GPR(0) | S_SQ_VTX_WORD1_DST_SEL_X(TERASCALE_SWIZZLE_X) |
      S_SQ_VTX_WORD1_DST_SEL_Y(TERASCALE_SWIZZLE_Y) |
      S_SQ_VTX_WORD1_DST_SEL_Z(TERASCALE_SWIZZLE_Z) |
      S_SQ_VTX_WORD1_DST_SEL_W(TERASCALE_SWIZZLE_MASK) | S_SQ_VTX_WORD1_USE_CONST_FIELDS(true),
   0,
   0,

   /* 24: ALU clause after storing R and before storing G and B. */

   /* +0: Move the G component to R0.X.
    *
    * R0.X = MOV R0.Y
    * Cycle 0: Y = R0.
    */
   TERAKAN_SHADER_OP1(true, 0, 'X', MOV, EG, 0, 'Y', VEC_012),

   /* +1: Move the B component to R1.X.
    *
    * R1.X = MOV R0.Z, unused 0
    * Cycle 0: Z = R0.
    */
   TERAKAN_SHADER_OP1(true, 1, 'X', MOV, EG, 0, 'Z', VEC_012),
};

struct terakan_meta_shader const terakan_meta_copy_expand_3x_ps = {
   .r8xx =
      {
         .program = terakan_meta_copy_expand_3x_ps_r8xx,
         .program_size_bytes = sizeof(terakan_meta_copy_expand_3x_ps_r8xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     S_028844_NUM_GPRS(4) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
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
   .r9xx =
      {
         .program = terakan_meta_copy_expand_3x_ps_r9xx,
         .program_size_bytes = sizeof(terakan_meta_copy_expand_3x_ps_r9xx),
         .static_registers =
            {
               .sq_pgm_resources =
                  {
                     S_028844_NUM_GPRS(4) | TERAKAN_META_SQ_PGM_RESOURCES_COMMON,
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
   .primary_meta_resource_used = true,
};

static void
terakan_meta_copy_expand_3x_begin(struct terakan_gfx_command_writer * const command_writer)
{
   command_writer->post_color_image_copy_write_barrier_actions |=
      TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_UAV | TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;

   struct terakan_meta_config_draw_begin_options const meta_begin_options = {
      .vgt_primitive_type = V_008958_DI_PT_RECTLIST,
      .cb_and_db_shader_control_mode = TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_NORMAL_UAV_ONLY,
      .rasterization = {.enable = true},
   };
   terakan_meta_config_draw_begin(command_writer, &meta_begin_options);
   terakan_meta_config_draw_set_sq_pgm_vs(command_writer,
                                          TERAKAN_META_SHADER_POSITION_FROM_INDEX_VS);
   terakan_meta_config_draw_set_sq_pgm_ps(command_writer, TERAKAN_META_SHADER_COPY_EXPAND_3X_PS);
}

void
terakan_meta_copy_expand_3x_buffer_to_image(
   struct terakan_gfx_command_writer * const command_writer,
   VkCopyBufferToImageInfo2 const * const copy_buffer_to_image_info)
{
   terakan_meta_copy_expand_3x_begin(command_writer);

   struct terakan_image const * const image =
      terakan_image_from_handle(copy_buffer_to_image_info->dstImage);
   struct terakan_buffer const * const buffer =
      terakan_buffer_from_handle(copy_buffer_to_image_info->srcBuffer);

   unsigned const bytes_per_block = image->surface.aspects[0].bytes_per_block;
   unsigned const bytes_per_surfel = bytes_per_block / 3u;
   /* The image descriptor base is the slice origin. */
   struct terakan_color_descriptor image_descriptor =
      terakan_meta_transfer_expand_3x_uav(bytes_per_surfel);
   /* The buffer descriptor base is the rectangle origin. */
   struct terakan_resource_descriptor buffer_descriptor =
      terakan_meta_transfer_expand_3x_resource(bytes_per_surfel);

   uint32_t constants[TERAKAN_META_COPY_EXPAND_3X_CONSTS_COUNT] = {};
   bool constants_set = false;

   for (uint32_t region_index = 0; region_index < copy_buffer_to_image_info->regionCount;
        ++region_index) {
      VkBufferImageCopy2 const * const region = &copy_buffer_to_image_info->pRegions[region_index];

      struct terakan_meta_copy_buffer_image_region_image region_image;
      if (unlikely(!terakan_meta_copy_buffer_image_translate_region_image(image, region,
                                                                          &region_image))) {
         continue;
      }

      struct terakan_image_surface_level const * const surface_level =
         &image->surface.aspects[region_image.image_descriptor_create_info.image_aspect_index]
             .levels[region_image.image_descriptor_create_info.subresource_range.base_mip_level];
      image_descriptor.base =
         (image->va >> 8) + surface_level->offset_in_memory_bytes_shr8 +
         surface_level->slice_size_bytes_shr8 *
            region_image.image_descriptor_create_info.subresource_range.base_z_or_array_layer;
      image_descriptor.dim = ((uint32_t)surface_level->aligned_extent_surfels[0] *
                              surface_level->aligned_extent_surfels[1]) -
                             1u;
      uint32_t const image_texel_offset_surfels =
         3u * region_image.rect_blocks.bounds[0][0] +
         surface_level->aligned_extent_surfels[0] * (uint32_t)region_image.rect_blocks.bounds[0][1];

      uint64_t const buffer_slice_extent_bytes_minus_1 =
         bytes_per_block *
            terakan_meta_copy_buffer_image_region_image_buffer_slice_extent_blocks(&region_image) -
         1u;
      buffer_descriptor.resource[1] = (uint32_t)MIN2(buffer_slice_extent_bytes_minus_1, UINT32_MAX);
      uint64_t const buffer_y_pitch_surfels = (uint64_t)3u * region_image.buffer_y_pitch_blocks;
      uint64_t const buffer_z_pitch_bytes = bytes_per_block * region_image.buffer_z_pitch_blocks;
      uint64_t buffer_offset = region->bufferOffset;

      if (constants[TERAKAN_META_COPY_EXPAND_3X_CONST_SRC_PITCH] !=
             (uint32_t)buffer_y_pitch_surfels ||
          constants[TERAKAN_META_COPY_EXPAND_3X_CONST_DST_PITCH] !=
             surface_level->aligned_extent_surfels[0] ||
          constants[TERAKAN_META_COPY_EXPAND_3X_CONST_DST_OFFSET] != image_texel_offset_surfels) {
         constants_set = false;
         constants[TERAKAN_META_COPY_EXPAND_3X_CONST_SRC_PITCH] = (uint32_t)buffer_y_pitch_surfels;
         constants[TERAKAN_META_COPY_EXPAND_3X_CONST_DST_PITCH] =
            surface_level->aligned_extent_surfels[0];
         constants[TERAKAN_META_COPY_EXPAND_3X_CONST_DST_OFFSET] = image_texel_offset_surfels;
      }
      if (!constants_set) {
         terakan_meta_config_draw_set_kcache_push_constants(command_writer, sizeof(constants),
                                                            constants, false, true);
         constants_set = true;
      }

      struct terakan_screen_rect const region_extent_rect = {
         .bounds = {
            [1] = {region_image.rect_blocks.bounds[1][0] - region_image.rect_blocks.bounds[0][0],
                   region_image.rect_blocks.bounds[1][1] - region_image.rect_blocks.bounds[0][1]}}};

      for (uint32_t subresource_range_slice = 0;
           subresource_range_slice <
           region_image.image_descriptor_create_info.subresource_range.max_depth_or_layer_count;
           ++subresource_range_slice) {
         if (unlikely(buffer_offset >= buffer->vk.size)) {
            /* #MemoryIntegrity. */
            break;
         }

         terakan_meta_config_draw_set_cb_uav(command_writer, 0, image->bo, &image_descriptor);

         uint64_t const buffer_descriptor_base = buffer->va + buffer_offset;
         buffer_descriptor.resource[0] = (uint32_t)buffer_descriptor_base;
         buffer_descriptor.resource[2] =
            (buffer_descriptor.resource[2] & C_030008_BASE_ADDRESS_HI) |
            S_030008_BASE_ADDRESS_HI(buffer_descriptor_base >> 32);
         buffer_descriptor.resource[1] =
            (uint32_t)MIN2(buffer_descriptor.resource[1], buffer->vk.size - buffer->va - 1u);
         terakan_hw_config_sqk_set_resource_fs(
            &command_writer->hw_config_sqk, TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META,
            buffer->bo, &buffer_descriptor);

         terakan_meta_draw_rect(command_writer, region_extent_rect, 1);

         image_descriptor.base += surface_level->slice_size_bytes_shr8;
         buffer_offset += buffer_z_pitch_bytes;
      }
   }
}

void
terakan_meta_copy_expand_3x_image_to_buffer(
   struct terakan_gfx_command_writer * const command_writer,
   VkCopyImageToBufferInfo2 const * const copy_image_to_buffer_info)
{
   terakan_meta_copy_expand_3x_begin(command_writer);

   uint8_t const pipe_interleave_bytes_log2 =
      terakan_gfx_command_writer_physical_device(command_writer)
         ->tiling_info.pipe_interleave_bytes_log2;

   struct terakan_buffer const * const buffer =
      terakan_buffer_from_handle(copy_image_to_buffer_info->dstBuffer);
   struct terakan_image const * const image =
      terakan_image_from_handle(copy_image_to_buffer_info->srcImage);

   unsigned const bytes_per_block = image->surface.aspects[0].bytes_per_block;
   unsigned const bytes_per_surfel = bytes_per_block / 3u;
   /* The buffer descriptor base is the rectangle origin aligned to the pipe interleave. */
   struct terakan_color_descriptor buffer_descriptor =
      terakan_meta_transfer_expand_3x_uav(bytes_per_surfel);
   /* The image descriptor base is the rectangle origin. */
   struct terakan_resource_descriptor image_descriptor =
      terakan_meta_transfer_expand_3x_resource(bytes_per_surfel);

   uint32_t constants[TERAKAN_META_COPY_EXPAND_3X_CONSTS_COUNT] = {};
   bool constants_set = false;

   for (uint32_t region_index = 0; region_index < copy_image_to_buffer_info->regionCount;
        ++region_index) {
      VkBufferImageCopy2 const * const region = &copy_image_to_buffer_info->pRegions[region_index];

      struct terakan_meta_copy_buffer_image_region_image region_image;
      if (unlikely(!terakan_meta_copy_buffer_image_translate_region_image(image, region,
                                                                          &region_image))) {
         continue;
      }
      uint64_t const buffer_y_pitch_surfels = (uint64_t)3u * region_image.buffer_y_pitch_blocks;
      uint64_t const buffer_z_pitch_bytes = bytes_per_block * region_image.buffer_z_pitch_blocks;
      uint64_t buffer_offset = region->bufferOffset;

      uint64_t const buffer_slice_extent_surfels_minus_1 =
         3u *
            terakan_meta_copy_buffer_image_region_image_buffer_slice_extent_blocks(&region_image) -
         1u;

      struct terakan_screen_rect const region_extent_rect = {
         .bounds = {
            [1] = {region_image.rect_blocks.bounds[1][0] - region_image.rect_blocks.bounds[0][0],
                   region_image.rect_blocks.bounds[1][1] - region_image.rect_blocks.bounds[0][1]}}};

      struct terakan_image_surface_level const * const surface_level =
         &image->surface.aspects[region_image.image_descriptor_create_info.image_aspect_index]
             .levels[region_image.image_descriptor_create_info.subresource_range.base_mip_level];

      uint32_t const image_y_pitch_bytes =
         bytes_per_surfel * (uint32_t)surface_level->aligned_extent_surfels[0];
      uint64_t image_descriptor_base =
         image->va +
         ((uint64_t)(surface_level->offset_in_memory_bytes_shr8 +
                     surface_level->slice_size_bytes_shr8 *
                        region_image.image_descriptor_create_info.subresource_range
                           .base_z_or_array_layer)
          << 8) +
         (image_y_pitch_bytes * region_image.rect_blocks.bounds[0][1] +
          bytes_per_block * (uint32_t)region_image.rect_blocks.bounds[0][0]);
      image_descriptor.resource[1] = bytes_per_block * (uint32_t)region_extent_rect.bounds[1][0] -
                                     1u +
                                     image_y_pitch_bytes * (region_extent_rect.bounds[1][1] - 1u);

      if (constants[TERAKAN_META_COPY_EXPAND_3X_CONST_SRC_PITCH] !=
             surface_level->aligned_extent_surfels[0] ||
          constants[TERAKAN_META_COPY_EXPAND_3X_CONST_DST_PITCH] !=
             (uint32_t)buffer_y_pitch_surfels) {
         constants_set = false;
         constants[TERAKAN_META_COPY_EXPAND_3X_CONST_SRC_PITCH] =
            surface_level->aligned_extent_surfels[0];
         constants[TERAKAN_META_COPY_EXPAND_3X_CONST_DST_PITCH] = (uint32_t)buffer_y_pitch_surfels;
      }

      for (uint32_t subresource_range_slice = 0;
           subresource_range_slice <
           region_image.image_descriptor_create_info.subresource_range.max_depth_or_layer_count;
           ++subresource_range_slice) {
         if (unlikely(buffer_offset >= buffer->vk.size)) {
            /* #MemoryIntegrity: Prevent a large offset from causing an integer wraparound. */
            break;
         }
         uint64_t const buffer_va_surfel_aligned_non_uav_aligned =
            (buffer->va + buffer_offset) & ~(uint64_t)(bytes_per_surfel - 1);
         /* #MemoryIntegrity. */
         uint64_t const buffer_object_end_va_surfel_aligned =
            (buffer->va + buffer->vk.size) & ~(uint64_t)(bytes_per_surfel - 1);
         if (unlikely(buffer_va_surfel_aligned_non_uav_aligned >=
                      buffer_object_end_va_surfel_aligned)) {
            /* Don't subtract 1 from 0. */
            continue;
         }
         uint64_t const buffer_va_aligned =
            buffer_va_surfel_aligned_non_uav_aligned >> pipe_interleave_bytes_log2
                                                           << pipe_interleave_bytes_log2;
         uint32_t const buffer_uav_alignment_offset_surfels =
            (uint32_t)(buffer_va_surfel_aligned_non_uav_aligned - buffer_va_aligned) /
            bytes_per_surfel;
         uint32_t const buffer_max_surfels_minus_1 = (uint32_t)MIN2(
            (buffer_object_end_va_surfel_aligned - buffer_va_aligned) / bytes_per_surfel - 1u,
            UINT32_MAX);
         buffer_descriptor.base = (uint32_t)(buffer_va_aligned >> 8);
         buffer_descriptor.dim = (uint32_t)MIN2(
            buffer_uav_alignment_offset_surfels + buffer_slice_extent_surfels_minus_1,
            buffer_max_surfels_minus_1);
         terakan_meta_config_draw_set_cb_uav(command_writer, 0, buffer->bo, &buffer_descriptor);

         image_descriptor.resource[0] = (uint32_t)image_descriptor_base;
         image_descriptor.resource[2] = (image_descriptor.resource[2] & C_030008_BASE_ADDRESS_HI) |
                                        S_030008_BASE_ADDRESS_HI(image_descriptor_base >> 32);
         terakan_hw_config_sqk_set_resource_fs(
            &command_writer->hw_config_sqk, TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META,
            image->bo, &image_descriptor);

         if (constants[TERAKAN_META_COPY_EXPAND_3X_CONST_DST_OFFSET] !=
             buffer_uav_alignment_offset_surfels) {
            constants_set = false;
            constants[TERAKAN_META_COPY_EXPAND_3X_CONST_DST_OFFSET] =
               buffer_uav_alignment_offset_surfels;
         }
         if (!constants_set) {
            terakan_meta_config_draw_set_kcache_push_constants(command_writer, sizeof(constants),
                                                               constants, false, true);
            constants_set = true;
         }

         terakan_meta_draw_rect(command_writer, region_extent_rect, 1);

         image_descriptor_base += surface_level->slice_size_bytes_shr8 << 8;
         buffer_offset += buffer_z_pitch_bytes;
      }
   }
}

void
terakan_meta_copy_expand_3x_image(struct terakan_gfx_command_writer * const command_writer,
                                  VkCopyImageInfo2 const * const copy_image_info)
{
   terakan_meta_copy_expand_3x_begin(command_writer);

   struct terakan_image const * const src_image =
      terakan_image_from_handle(copy_image_info->srcImage);
   struct terakan_image const * const dst_image =
      terakan_image_from_handle(copy_image_info->dstImage);

   struct terakan_image_surface_aspect const * const src_surface_aspect =
      &src_image->surface.aspects[0];
   struct terakan_image_surface_aspect const * const dst_surface_aspect =
      &dst_image->surface.aspects[0];
   if (unlikely(!terakan_format_is_expand_3x(src_surface_aspect->bytes_per_block) ||
                !terakan_format_is_expand_3x(dst_surface_aspect->bytes_per_block))) {
      /* #MemoryIntegrity. */
      return;
   }
   uint8_t const src_bytes_per_surfel = src_surface_aspect->bytes_per_block / 3u;

   struct terakan_image_descriptor_subresource_range src_subresource_range = {.max_level_count = 1};
   struct terakan_image_descriptor_subresource_range dst_subresource_range = {.max_level_count = 1};

   /* The source descriptor base is the rectangle origin. */
   struct terakan_resource_descriptor src_descriptor =
      terakan_meta_transfer_expand_3x_resource(src_bytes_per_surfel);
   /* The destination descriptor base is the slice origin. */
   struct terakan_color_descriptor dst_descriptor =
      terakan_meta_transfer_expand_3x_uav(dst_surface_aspect->bytes_per_block / 3u);

   uint32_t constants[TERAKAN_META_COPY_EXPAND_3X_CONSTS_COUNT] = {};
   bool constants_set = false;

   for (uint32_t region_index = 0; region_index < copy_image_info->regionCount; ++region_index) {
      VkImageCopy2 const * const region = &copy_image_info->pRegions[region_index];

      /* Sanitize and handle `VK_REMAINING_*`. */

      src_subresource_range.base_mip_level = region->srcSubresource.mipLevel;
      if (src_image->vk.image_type == VK_IMAGE_TYPE_3D) {
         src_subresource_range.base_z_or_array_layer = (uint32_t)region->srcOffset.z;
         src_subresource_range.max_depth_or_layer_count = region->extent.depth;
      } else {
         src_subresource_range.base_z_or_array_layer = region->srcSubresource.baseArrayLayer;
         src_subresource_range.max_depth_or_layer_count = region->srcSubresource.layerCount;
      }
      if (unlikely(!terakan_image_descriptor_subresource_range_sanitize(
             src_image, &src_subresource_range, false))) {
         continue;
      }

      dst_subresource_range.base_mip_level = region->dstSubresource.mipLevel;
      if (dst_image->vk.image_type == VK_IMAGE_TYPE_3D) {
         dst_subresource_range.base_z_or_array_layer = (uint32_t)region->dstOffset.z;
         dst_subresource_range.max_depth_or_layer_count = region->extent.depth;
      } else {
         dst_subresource_range.base_z_or_array_layer = region->dstSubresource.baseArrayLayer;
         dst_subresource_range.max_depth_or_layer_count = region->dstSubresource.layerCount;
      }
      if (unlikely(!terakan_image_descriptor_subresource_range_sanitize(
             dst_image, &dst_subresource_range, false))) {
         continue;
      }

      /* Sanitize the rectangles for #MemoryIntegrity, in particular to avoid incorrect addressing
       * calculations.
       * According to the valid usage rules, the source and destination rectangles must be contained
       * within the respective image subresources. It's trivial to clip an invalid rectangle on the
       * right and bottom. However, clipping to 0 on the left and top would require addressing
       * adjustment, so reject the region if its offset is negative (the conversion to `uint32_t`
       * will make it always exceed the width or height).
       */
      uint32_t const src_subresource_width =
         u_minify(src_image->vk.extent.width, src_subresource_range.base_mip_level);
      uint32_t const src_subresource_height =
         u_minify(src_image->vk.extent.height, src_subresource_range.base_mip_level);
      uint32_t const dst_subresource_width =
         u_minify(dst_image->vk.extent.width, dst_subresource_range.base_mip_level);
      uint32_t const dst_subresource_height =
         u_minify(dst_image->vk.extent.height, dst_subresource_range.base_mip_level);
      if (unlikely((uint32_t)region->srcOffset.x >= src_subresource_width ||
                   (uint32_t)region->srcOffset.y >= src_subresource_height ||
                   (uint32_t)region->dstOffset.x >= dst_subresource_width ||
                   (uint32_t)region->dstOffset.y >= dst_subresource_height)) {
         continue;
      }
      struct terakan_screen_rect region_extent_rect = {
         .bounds = {[1] = {
                       (uint16_t)MIN3(region->extent.width,
                                      src_subresource_width - (uint32_t)region->srcOffset.x,
                                      dst_subresource_width - (uint32_t)region->dstOffset.x),
                       (uint16_t)MIN3(region->extent.height,
                                      src_subresource_height - (uint32_t)region->srcOffset.y,
                                      dst_subresource_height - (uint32_t)region->dstOffset.y),
                    }}};
      if (unlikely(region_extent_rect.bounds[1][0] == 0 || region_extent_rect.bounds[1][1] == 0)) {
         /* #MemoryIntegrity: Avoid incorrect addressing calculations. */
         continue;
      }

      assert(region->srcSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
      struct terakan_image_surface_level const * const src_surface_level =
         &src_surface_aspect->levels[src_subresource_range.base_mip_level];
      assert(region->dstSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
      struct terakan_image_surface_level const * const dst_surface_level =
         &dst_surface_aspect->levels[dst_subresource_range.base_mip_level];

      uint32_t const src_y_pitch_bytes =
         src_bytes_per_surfel * (uint32_t)src_surface_level->aligned_extent_surfels[0];
      uint64_t src_descriptor_base =
         src_image->va +
         ((uint64_t)(src_surface_level->offset_in_memory_bytes_shr8 +
                     src_surface_level->slice_size_bytes_shr8 *
                        src_subresource_range.base_z_or_array_layer)
          << 8) +
         (src_y_pitch_bytes * (uint32_t)region->srcOffset.y +
          src_surface_aspect->bytes_per_block * (uint32_t)region->srcOffset.x);
      src_descriptor.resource[1] =
         src_surface_aspect->bytes_per_block * (uint32_t)region_extent_rect.bounds[1][0] - 1u +
         src_y_pitch_bytes * (region_extent_rect.bounds[1][1] - 1u);

      dst_descriptor.base =
         (dst_image->va >> 8) + dst_surface_level->offset_in_memory_bytes_shr8 +
         dst_surface_level->slice_size_bytes_shr8 * dst_subresource_range.base_z_or_array_layer;
      dst_descriptor.dim = ((uint32_t)dst_surface_level->aligned_extent_surfels[0] *
                            dst_surface_level->aligned_extent_surfels[1]) -
                           1u;

      uint32_t const dst_texel_offset_surfels =
         3u * (uint32_t)region->dstOffset.x +
         dst_surface_level->aligned_extent_surfels[0] * (uint32_t)region->dstOffset.y;
      if (constants[TERAKAN_META_COPY_EXPAND_3X_CONST_SRC_PITCH] !=
             src_surface_level->aligned_extent_surfels[0] ||
          constants[TERAKAN_META_COPY_EXPAND_3X_CONST_DST_PITCH] !=
             dst_surface_level->aligned_extent_surfels[0] ||
          constants[TERAKAN_META_COPY_EXPAND_3X_CONST_DST_OFFSET] != dst_texel_offset_surfels) {
         constants_set = false;
         constants[TERAKAN_META_COPY_EXPAND_3X_CONST_SRC_PITCH] =
            src_surface_level->aligned_extent_surfels[0];
         constants[TERAKAN_META_COPY_EXPAND_3X_CONST_DST_PITCH] =
            dst_surface_level->aligned_extent_surfels[0];
         constants[TERAKAN_META_COPY_EXPAND_3X_CONST_DST_OFFSET] = dst_texel_offset_surfels;
      }
      if (!constants_set) {
         terakan_meta_config_draw_set_kcache_push_constants(command_writer, sizeof(constants),
                                                            constants, false, true);
         constants_set = true;
      }

      uint32_t const region_slice_count = MIN2(src_subresource_range.max_depth_or_layer_count,
                                               dst_subresource_range.max_depth_or_layer_count);
      for (uint32_t region_slice = 0; region_slice < region_slice_count; ++region_slice) {
         src_descriptor.resource[0] = (uint32_t)src_descriptor_base;
         src_descriptor.resource[2] = (src_descriptor.resource[2] & C_030008_BASE_ADDRESS_HI) |
                                      S_030008_BASE_ADDRESS_HI(src_descriptor_base >> 32);
         terakan_hw_config_sqk_set_resource_fs(
            &command_writer->hw_config_sqk, TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META,
            src_image->bo, &src_descriptor);

         terakan_meta_config_draw_set_cb_uav(command_writer, 0, dst_image->bo, &dst_descriptor);

         terakan_meta_draw_rect(command_writer, region_extent_rect, 1);

         src_descriptor_base += src_surface_level->slice_size_bytes_shr8 << 8;
         dst_descriptor.base += dst_surface_level->slice_size_bytes_shr8;
      }
   }
}
