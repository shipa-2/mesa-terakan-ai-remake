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

#include "terakan_meta.h"

#include "terakan_buffer.h"
#include "terakan_command_buffer.h"
#include "terakan_image.h"
#include "terakan_physical_device.h"

#include "util/macros.h"

#include <assert.h>

/* One layer per draw for simplicity (eliminates lots of per-pixel instructions). */

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
   TERAKAN_SHADER_CF_UAV(false, STORE_TYPED, 0, 1, 0, 0xF, true),

   /* 3: Moving the G and B components to GPR.X. */
   S_SQ_CF_WORD0_ADDR(18) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_COUNT(1) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 4: Store the G component. */
   TERAKAN_SHADER_CF_UAV(false, STORE_TYPED, 0, 2, 0, 0xF, true),

   /* 5: Store the B component. */
   TERAKAN_SHADER_CF_UAV(false, STORE_TYPED, 0, 3, 1, 0xF, false),

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
   TERAKAN_SHADER_CF_UAV(false, STORE_TYPED, 0, 1, 0, 0xF, true),

   /* 3: Moving the G and B components to GPR.X. */
   S_SQ_CF_WORD0_ADDR(24) | S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS) |
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),
   S_SQ_CF_ALU_WORD1_COUNT(1) | S_SQ_CF_ALU_WORD1_BARRIER(true) |
      EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU,

   /* 4: Store the G component. */
   TERAKAN_SHADER_CF_UAV(false, STORE_TYPED, 0, 2, 0, 0xF, true),

   /* 5: Store the B component. */
   TERAKAN_SHADER_CF_UAV(false, STORE_TYPED, 0, 3, 1, 0xF, false),

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
   .kcache_needed = (uint16_t)1 << TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
   .resources_needed =
      {
         [BITSET_BITWORD(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META)] =
            BITSET_BIT(TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META),
      },
   .stage =
      {
         .ps =
            {
               .db_shader_control = TERAKAN_META_DB_SHADER_CONTROL_PS_MEMORY_EXPORT,
            },
      },
};

static void
terakan_meta_copy_expand_3x_begin(struct terakan_gfx_command_writer * const command_writer)
{
   command_writer->post_color_image_copy_write_barrier_actions |=
      TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_UAV | TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;

   terakan_meta_begin_2d_immediate_rects(command_writer, TERAKAN_META_PA_CL_VTE_CNTL_2D,
                                         TERAKAN_META_DB_RENDER_OVERRIDE_DEFAULT, true);

   terakan_meta_set_vs(command_writer, TERAKAN_META_SHADER_POSITION_FROM_INDEX_VS);
   terakan_meta_set_ps(command_writer, TERAKAN_META_SHADER_COPY_EXPAND_3X_PS, true);

   terakan_meta_begin_cb(command_writer, 0xF, 0b0);

   command_writer->push_constants_state.up_to_date_push_constants_bound_to_stages &=
      ~VK_SHADER_STAGE_FRAGMENT_BIT;
}

void
terakan_meta_copy_expand_3x_buffer_to_image(
   struct terakan_gfx_command_writer * const command_writer,
   VkCopyBufferToImageInfo2 const * const copy_buffer_to_image_info)
{
   terakan_meta_copy_expand_3x_begin(command_writer);

   struct terakan_buffer const * const src_buffer =
      terakan_buffer_from_handle(copy_buffer_to_image_info->srcBuffer);
   struct terakan_image const * const dst_image =
      terakan_image_from_handle(copy_buffer_to_image_info->dstImage);

   unsigned const bytes_per_surfel = dst_image->surface.aspects[0].bytes_per_block / 3;

   uint32_t src_resource[8];
   terakan_meta_transfer_expand_3x_resource(bytes_per_surfel, src_resource);

   struct terakan_color_descriptor dst_uav = terakan_meta_transfer_expand_3x_uav(
      bytes_per_surfel, terakan_gfx_command_writer_physical_device(command_writer)
                           ->tiling_info.pipe_interleave_bytes_log2);
   /* For simplicity and not to explicitly handle UAV alignment as image slices are always aligned,
    * adjusting only the destination offset in the push constants.
    * `buffer_uav_validated_as_image` doesn't need to be handled because surfel rows already have
    * the necessary pitch alignment, and using the same element format for the UAV as for the
    * surfels.
    * Assuming that images are never 2^32 surfels or larger.
    */
   VkDeviceSize const dst_aspect_size_surfels_minus_one =
      ((VkDeviceSize)dst_image->surface.aspects[0].size_bytes_shr8 << 8) / bytes_per_surfel - 1;
   assert(dst_aspect_size_surfels_minus_one <= UINT32_MAX);
   dst_uav.base =
      (uint32_t)(dst_image->va >> 8) + dst_image->surface.aspects[0].offset_in_memory_bytes_shr8;
   dst_uav.dim = (uint32_t)dst_aspect_size_surfels_minus_one;
   terakan_hw_state_draw_set_cb_color(&command_writer->hw_state_draw, 0, dst_image->bo, &dst_uav,
                                      NULL, true);

   for (uint32_t region_index = 0; region_index < copy_buffer_to_image_info->regionCount;
        ++region_index) {
      VkBufferImageCopy2 const * const region = &copy_buffer_to_image_info->pRegions[region_index];

      uint32_t base_layer, layer_count;
      if (dst_image->vk.image_type == VK_IMAGE_TYPE_3D) {
         base_layer = (uint32_t)region->imageOffset.z;
         layer_count = region->imageExtent.depth;
      } else {
         base_layer = region->imageSubresource.baseArrayLayer;
         layer_count = vk_image_subresource_layer_count(&dst_image->vk, &region->imageSubresource);
      }

      uint32_t const src_y_pitch_surfels =
         3 * (region->bufferRowLength != 0 ? region->bufferRowLength : region->imageExtent.width);
      VkDeviceSize const src_z_pitch_bytes =
         bytes_per_surfel * (VkDeviceSize)src_y_pitch_surfels *
         (region->bufferImageHeight != 0 ? region->bufferImageHeight : region->imageExtent.height);
      VkDeviceSize const src_rect_extent_surfels =
         (VkDeviceSize)src_y_pitch_surfels * (region->imageExtent.height - 1) +
         3 * region->imageExtent.width;
      src_resource[1] = (uint32_t)(bytes_per_surfel * src_rect_extent_surfels - 1);
      src_resource[4] = (uint32_t)src_rect_extent_surfels;
      uint64_t src_va = src_buffer->va + region->bufferOffset;

      struct terakan_image_surface_level const * const dst_surface_level =
         &dst_image->surface.aspects[0].levels[region->imageSubresource.mipLevel];
      uint32_t const dst_z_pitch_surfels =
         ((VkDeviceSize)dst_surface_level->slice_size_bytes_shr8 << 8) / bytes_per_surfel;
      uint32_t dst_offset_surfels =
         (uint32_t)(((VkDeviceSize)(dst_surface_level->offset_in_memory_bytes_shr8 -
                                    dst_image->surface.aspects[0].offset_in_memory_bytes_shr8)
                     << 8) /
                    bytes_per_surfel) +
         dst_z_pitch_surfels * base_layer +
         (dst_surface_level->aligned_extent_surfels[0] * (uint32_t)region->imageOffset.y +
          3 * (uint32_t)region->imageOffset.x);

      for (uint32_t layer_index = 0; layer_index < layer_count; ++layer_index) {
         /* Constants are always changed because the destination offset is. */
         struct terakan_bo const * constants_bo;
         uint32_t constants_va_lines;
         uint32_t * const constants = terakan_push_buffer_allocate_kcache(
            command_writer->base.command_buffer,
            sizeof(uint32_t) * TERAKAN_META_COPY_EXPAND_3X_CONSTS_COUNT, &constants_bo,
            &constants_va_lines);
         if (unlikely(constants == NULL)) {
            return;
         }
         constants[TERAKAN_META_COPY_EXPAND_3X_CONST_SRC_PITCH] = src_y_pitch_surfels;
         constants[TERAKAN_META_COPY_EXPAND_3X_CONST_DST_PITCH] =
            dst_surface_level->aligned_extent_surfels[0];
         constants[TERAKAN_META_COPY_EXPAND_3X_CONST_DST_OFFSET] = dst_offset_surfels;
         terakan_hw_state_sqc_set_kcache_fs(
            &command_writer->hw_state_sqc, TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
            DIV_ROUND_UP(sizeof(uint32_t) * TERAKAN_META_COPY_EXPAND_3X_CONSTS_COUNT,
                         TERAKAN_KCACHE_HW_LINE_BYTES),
            constants_bo, constants_va_lines);

         src_resource[0] = (uint32_t)src_va;
         src_resource[2] =
            (src_resource[2] & C_030008_BASE_ADDRESS_HI) | S_030008_BASE_ADDRESS_HI(src_va >> 32);
         terakan_hw_state_sqc_set_resource_fs(&command_writer->hw_state_sqc,
                                              TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META,
                                              src_buffer->bo, src_resource);

         VkRect2D const rect = {
            .extent =
               {
                  .width = region->imageExtent.width,
                  .height = region->imageExtent.height,
               },
         };
         terakan_meta_emit_rect_3_vertices_draw(command_writer, &rect, 1);

         src_va += src_z_pitch_bytes;
         dst_offset_surfels += dst_z_pitch_surfels;
      }
   }
}

void
terakan_meta_copy_expand_3x_image_to_buffer(
   struct terakan_gfx_command_writer * const command_writer,
   VkCopyImageToBufferInfo2 const * const copy_image_to_buffer_info)
{
   terakan_meta_copy_expand_3x_begin(command_writer);

   struct terakan_image const * const src_image =
      terakan_image_from_handle(copy_image_to_buffer_info->srcImage);
   struct terakan_buffer const * const dst_buffer =
      terakan_buffer_from_handle(copy_image_to_buffer_info->dstBuffer);

   unsigned const bytes_per_surfel = src_image->surface.aspects[0].bytes_per_block / 3;

   uint32_t src_resource[8];
   terakan_meta_transfer_expand_3x_resource(bytes_per_surfel, src_resource);

   unsigned const tile_pipe_interleave_bytes_log2 =
      terakan_gfx_command_writer_physical_device(command_writer)
         ->tiling_info.pipe_interleave_bytes_log2;

   struct terakan_color_descriptor dst_uav =
      terakan_meta_transfer_expand_3x_uav(bytes_per_surfel, tile_pipe_interleave_bytes_log2);

   uint32_t constants[TERAKAN_META_COPY_EXPAND_3X_CONSTS_COUNT] = {};
   struct terakan_bo const * constants_bo = NULL;
   uint32_t constants_va_lines;

   for (uint32_t region_index = 0; region_index < copy_image_to_buffer_info->regionCount;
        ++region_index) {
      VkBufferImageCopy2 const * const region = &copy_image_to_buffer_info->pRegions[region_index];

      uint32_t base_layer, layer_count;
      if (src_image->vk.image_type == VK_IMAGE_TYPE_3D) {
         base_layer = (uint32_t)region->imageOffset.z;
         layer_count = region->imageExtent.depth;
      } else {
         base_layer = region->imageSubresource.baseArrayLayer;
         layer_count = vk_image_subresource_layer_count(&src_image->vk, &region->imageSubresource);
      }

      struct terakan_image_surface_level const * const src_surface_level =
         &src_image->surface.aspects[0].levels[region->imageSubresource.mipLevel];
      VkDeviceSize const src_z_pitch_bytes = (VkDeviceSize)src_surface_level->slice_size_bytes_shr8
                                             << 8;
      uint32_t const src_rect_extent_surfels =
         src_surface_level->aligned_extent_surfels[0] * (region->imageExtent.height - 1) +
         3 * region->imageExtent.width;
      src_resource[1] = bytes_per_surfel * src_rect_extent_surfels - 1;
      src_resource[4] = src_rect_extent_surfels;
      uint64_t src_va =
         src_image->va + ((VkDeviceSize)src_surface_level->offset_in_memory_bytes_shr8 << 8) +
         src_z_pitch_bytes * base_layer +
         bytes_per_surfel * (VkDeviceSize)(src_surface_level->aligned_extent_surfels[0] *
                                              (uint32_t)region->imageOffset.y +
                                           3 * (uint32_t)region->imageOffset.x);

      uint32_t const dst_y_pitch_surfels =
         3 * (region->bufferRowLength != 0 ? region->bufferRowLength : region->imageExtent.width);
      VkDeviceSize const dst_z_pitch_bytes =
         bytes_per_surfel * (VkDeviceSize)dst_y_pitch_surfels *
         (region->bufferImageHeight != 0 ? region->bufferImageHeight : region->imageExtent.height);
      VkDeviceSize const dst_rect_extent_surfels =
         (VkDeviceSize)dst_y_pitch_surfels * (region->imageExtent.height - 1) +
         3 * region->imageExtent.width;
      uint64_t dst_va = dst_buffer->va + region->bufferOffset;

      if (constants[TERAKAN_META_COPY_EXPAND_3X_CONST_SRC_PITCH] !=
             src_surface_level->aligned_extent_surfels[0] ||
          constants[TERAKAN_META_COPY_EXPAND_3X_CONST_DST_PITCH] != dst_y_pitch_surfels) {
         constants_bo = NULL;
         constants[TERAKAN_META_COPY_EXPAND_3X_CONST_SRC_PITCH] =
            src_surface_level->aligned_extent_surfels[0];
         constants[TERAKAN_META_COPY_EXPAND_3X_CONST_DST_PITCH] = dst_y_pitch_surfels;
      }

      for (uint32_t layer_index = 0; layer_index < layer_count; ++layer_index) {
         uint64_t const dst_va_aligned = dst_va >> tile_pipe_interleave_bytes_log2
                                                      << tile_pipe_interleave_bytes_log2;
         uint32_t const dst_offset_surfels =
            (uint32_t)((dst_va - dst_va_aligned) / bytes_per_surfel);

         if (constants[TERAKAN_META_COPY_EXPAND_3X_CONST_DST_OFFSET] != dst_offset_surfels) {
            constants_bo = NULL;
            constants[TERAKAN_META_COPY_EXPAND_3X_CONST_DST_OFFSET] = dst_offset_surfels;
         }

         if (constants_bo == NULL) {
            void * const constants_mapping = terakan_push_buffer_allocate_kcache(
               command_writer->base.command_buffer, sizeof(constants), &constants_bo,
               &constants_va_lines);
            if (unlikely(constants_mapping == NULL)) {
               return;
            }
            memcpy(constants_mapping, constants, sizeof(constants));
            terakan_hw_state_sqc_set_kcache_fs(
               &command_writer->hw_state_sqc, TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
               DIV_ROUND_UP(sizeof(constants), TERAKAN_KCACHE_HW_LINE_BYTES), constants_bo,
               constants_va_lines);
         }

         src_resource[0] = (uint32_t)src_va;
         src_resource[2] =
            (src_resource[2] & C_030008_BASE_ADDRESS_HI) | S_030008_BASE_ADDRESS_HI(src_va >> 32);
         terakan_hw_state_sqc_set_resource_fs(&command_writer->hw_state_sqc,
                                              TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META,
                                              src_image->bo, src_resource);

         dst_uav.base = (uint32_t)(dst_va_aligned >> 8);
         dst_uav.dim = (uint32_t)(dst_offset_surfels + dst_rect_extent_surfels - 1);
         terakan_hw_state_draw_set_cb_color(&command_writer->hw_state_draw, 0, dst_buffer->bo,
                                            &dst_uav, NULL, true);

         VkRect2D const rect = {
            .extent =
               {
                  .width = region->imageExtent.width,
                  .height = region->imageExtent.height,
               },
         };
         terakan_meta_emit_rect_3_vertices_draw(command_writer, &rect, 1);

         src_va += src_z_pitch_bytes;
         dst_va += dst_z_pitch_bytes;
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

   unsigned const bytes_per_surfel = dst_image->surface.aspects[0].bytes_per_block / 3;

   uint32_t src_resource[8];
   terakan_meta_transfer_expand_3x_resource(bytes_per_surfel, src_resource);

   struct terakan_color_descriptor dst_uav = terakan_meta_transfer_expand_3x_uav(
      bytes_per_surfel, terakan_gfx_command_writer_physical_device(command_writer)
                           ->tiling_info.pipe_interleave_bytes_log2);
   /* For simplicity and not to explicitly handle UAV alignment as image slices are always aligned,
    * adjusting only the destination offset in the push constants. Assuming that images are never
    * 2^32 surfels or larger.
    */
   VkDeviceSize const dst_aspect_size_surfels_minus_one =
      ((VkDeviceSize)dst_image->surface.aspects[0].size_bytes_shr8 << 8) / bytes_per_surfel - 1;
   assert(dst_aspect_size_surfels_minus_one <= UINT32_MAX);
   dst_uav.base =
      (uint32_t)(dst_image->va >> 8) + dst_image->surface.aspects[0].offset_in_memory_bytes_shr8;
   dst_uav.dim = (uint32_t)dst_aspect_size_surfels_minus_one;
   terakan_hw_state_draw_set_cb_color(&command_writer->hw_state_draw, 0, dst_image->bo, &dst_uav,
                                      NULL, true);

   for (uint32_t region_index = 0; region_index < copy_image_info->regionCount; ++region_index) {
      VkImageCopy2 const * const region = &copy_image_info->pRegions[region_index];

      uint32_t src_base_layer, layer_count;
      if (src_image->vk.image_type == VK_IMAGE_TYPE_3D) {
         src_base_layer = (uint32_t)region->srcOffset.z;
         layer_count = region->extent.depth;
      } else {
         src_base_layer = region->srcSubresource.baseArrayLayer;
         layer_count = vk_image_subresource_layer_count(&src_image->vk, &region->srcSubresource);
      }
      uint32_t const dst_base_layer = dst_image->vk.image_type == VK_IMAGE_TYPE_3D
                                         ? (uint32_t)region->dstOffset.z
                                         : region->dstSubresource.baseArrayLayer;

      struct terakan_image_surface_level const * const src_surface_level =
         &src_image->surface.aspects[0].levels[region->srcSubresource.mipLevel];
      uint32_t const src_rect_extent_surfels =
         src_surface_level->aligned_extent_surfels[0] * (region->extent.height - 1) +
         3 * region->extent.width;
      src_resource[1] = bytes_per_surfel * src_rect_extent_surfels - 1;
      src_resource[4] = src_rect_extent_surfels;
      VkDeviceSize const src_z_pitch_bytes = (VkDeviceSize)src_surface_level->slice_size_bytes_shr8
                                             << 8;
      uint64_t src_va =
         src_image->va + ((VkDeviceSize)src_surface_level->offset_in_memory_bytes_shr8 << 8) +
         src_z_pitch_bytes * src_base_layer +
         bytes_per_surfel * (VkDeviceSize)(src_surface_level->aligned_extent_surfels[0] *
                                              (uint32_t)region->srcOffset.y +
                                           3 * (uint32_t)region->srcOffset.x);

      struct terakan_image_surface_level const * const dst_surface_level =
         &dst_image->surface.aspects[0].levels[region->dstSubresource.mipLevel];
      uint32_t const dst_z_pitch_surfels =
         ((VkDeviceSize)dst_surface_level->slice_size_bytes_shr8 << 8) / bytes_per_surfel;
      uint32_t dst_offset_surfels =
         (uint32_t)(((VkDeviceSize)(dst_surface_level->offset_in_memory_bytes_shr8 -
                                    dst_image->surface.aspects[0].offset_in_memory_bytes_shr8)
                     << 8) /
                    bytes_per_surfel) +
         dst_z_pitch_surfels * dst_base_layer +
         (dst_surface_level->aligned_extent_surfels[0] * (uint32_t)region->dstOffset.y +
          3 * (uint32_t)region->dstOffset.x);

      for (uint32_t layer_index = 0; layer_index < layer_count; ++layer_index) {
         /* Constants are always changed because the destination offset is. */
         struct terakan_bo const * constants_bo;
         uint32_t constants_va_lines;
         uint32_t * const constants = terakan_push_buffer_allocate_kcache(
            command_writer->base.command_buffer,
            sizeof(uint32_t) * TERAKAN_META_COPY_EXPAND_3X_CONSTS_COUNT, &constants_bo,
            &constants_va_lines);
         if (unlikely(constants == NULL)) {
            return;
         }
         constants[TERAKAN_META_COPY_EXPAND_3X_CONST_SRC_PITCH] =
            src_surface_level->aligned_extent_surfels[0];
         constants[TERAKAN_META_COPY_EXPAND_3X_CONST_DST_PITCH] =
            dst_surface_level->aligned_extent_surfels[0];
         constants[TERAKAN_META_COPY_EXPAND_3X_CONST_DST_OFFSET] = dst_offset_surfels;
         terakan_hw_state_sqc_set_kcache_fs(
            &command_writer->hw_state_sqc, TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
            DIV_ROUND_UP(sizeof(uint32_t) * TERAKAN_META_COPY_EXPAND_3X_CONSTS_COUNT,
                         TERAKAN_KCACHE_HW_LINE_BYTES),
            constants_bo, constants_va_lines);

         src_resource[0] = (uint32_t)src_va;
         src_resource[2] =
            (src_resource[2] & C_030008_BASE_ADDRESS_HI) | S_030008_BASE_ADDRESS_HI(src_va >> 32);
         terakan_hw_state_sqc_set_resource_fs(&command_writer->hw_state_sqc,
                                              TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META,
                                              src_image->bo, src_resource);

         VkRect2D const rect = {
            .extent =
               {
                  .width = region->extent.width,
                  .height = region->extent.height,
               },
         };
         terakan_meta_emit_rect_3_vertices_draw(command_writer, &rect, 1);

         src_va += src_z_pitch_bytes;
         dst_offset_surfels += dst_z_pitch_surfels;
      }
   }
}
