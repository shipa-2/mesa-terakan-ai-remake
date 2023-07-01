/*
 * Copyright © 2023 Vitaliy Triang3l Kuzmin
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

#ifndef TERAKAN_HW_STATE_H
#define TERAKAN_HW_STATE_H

#include "winsys/terakan_winsys.h"
#include "terakan_descriptor.h"
#include "terakan_limits.h"

#include "util/bitset.h"

#include <stdbool.h>
#include <stdint.h>

enum terakan_hw_state_draw_index {
   /* Generally ordered roughly by the location of the hardware unit in the pipeline, and within
    * each unit, by the register addresses.
    */

   TERAKAN_HW_STATE_DRAW_VGT_INDEX_TYPE,

   TERAKAN_HW_STATE_DRAW_VGT_INDEX_BUFFER,

   TERAKAN_HW_STATE_DRAW_VGT_PRIMITIVE_TYPE,

   TERAKAN_HW_STATE_DRAW_VGT_INDEX_OFFSET,

   TERAKAN_HW_STATE_DRAW_SQ_VTX_START_INST_LOC,

   TERAKAN_HW_STATE_DRAW_PA_CL_CLIP_CNTL,

   TERAKAN_HW_STATE_DRAW_PA_SU_SC_MODE_CNTL,

   TERAKAN_HW_STATE_DRAW_CB_BLEND_RGBA,

   TERAKAN_HW_STATE_DRAW_CB_COLOR_FIRST,
   TERAKAN_HW_STATE_DRAW_CB_COLOR_LAST =
      TERAKAN_HW_STATE_DRAW_CB_COLOR_FIRST + TERAKAN_LIMITS_HW_COLOR_RAT_COUNT - 1,

   TERAKAN_HW_STATE_DRAW_COUNT,
};

/* State applied before performing application's or internal draws, and reapplied when switching to
 * a new indirect buffer in the Vulkan command buffer.
 */
struct terakan_hw_state_draw {
   /* Whether each state item has ever been written, and thus has a value that's not complete junk,
    * and is potentially relevant to the current command buffer.
    */
   BITSET_DECLARE(state_ever_written, TERAKAN_HW_STATE_DRAW_COUNT);
   /* Whether each state item has been modified and needs to be emitted before the next draw. */
   BITSET_DECLARE(state_modified, TERAKAN_HW_STATE_DRAW_COUNT);

   /* TERAKAN_HW_STATE_DRAW_VGT_INDEX_TYPE */
   uint32_t vgt_index_type;

   /* TERAKAN_HW_STATE_DRAW_VGT_INDEX_BUFFER */
   struct terakan_winsys_bo const * vgt_index_buffer_bo;
   uint64_t vgt_index_buffer_base;
   /* In units of indices. */
   uint32_t vgt_index_buffer_size;

   /* TERAKAN_HW_STATE_DRAW_VGT_PRIMITIVE_TYPE */
   uint32_t vgt_primitive_type;

   /* TERAKAN_HW_STATE_DRAW_VGT_INDEX_OFFSET */
   uint32_t vgt_index_offset;

   /* TERAKAN_HW_STATE_DRAW_SQ_VTX_START_INST_LOC */
   uint32_t sq_vtx_start_inst_loc;

   /* TERAKAN_HW_STATE_DRAW_PA_CL_CLIP_CNTL */
   uint32_t pa_cl_clip_cntl;

   /* TERAKAN_HW_STATE_DRAW_PA_SU_SC_MODE_CNTL */
   uint32_t pa_su_sc_mode_cntl;

   /* TERAKAN_HW_STATE_DRAW_CB_BLEND_RGBA */
   float cb_blend_rgba[4];

   /* TERAKAN_HW_STATE_DRAW_CB_COLOR_FIRST...LAST */
   struct terakan_winsys_bo const * cb_color_bo[TERAKAN_LIMITS_HW_COLOR_RAT_COUNT];
   /* The values are undefined if the respective cb_color_bo is NULL. */
   struct terakan_color_descriptor cb_color[TERAKAN_LIMITS_HW_COLOR_RAT_COUNT];
   struct terakan_color_meta_descriptor cb_color_meta[TERAKAN_LIMITS_HW_COLOR_MRT_COUNT];
};

struct terakan_gfx_command_writer;

/* Pass the result of the external comparison to reduce the amount of state setting packets if the
 * state was not modified if needed (especially recommended when using static state in pipeline
 * objects). Floating-point state must be compared using memcmp to distinguish between the signs of
 * zero.
 * The function must still be called even if the state wasn't changed, however, to mark the state
 * item as needed in the current command buffer and also to make sure it's emitted for the first
 * time before the next draw.
 */
static inline void
terakan_hw_state_draw_written(struct terakan_hw_state_draw * const state,
                              enum terakan_hw_state_draw_index const state_index, bool modified)
{
   if (!BITSET_TEST(state->state_ever_written, state_index)) {
      BITSET_SET(state->state_ever_written, state_index);
      modified = true;
   }
   if (modified) {
      BITSET_SET(state->state_modified, state_index);
   }
}

void terakan_hw_state_draw_emit_modified(struct terakan_gfx_command_writer * command_writer);

void terakan_hw_state_draw_emit_all(struct terakan_gfx_command_writer * command_writer);

void terakan_hw_state_draw_reset(struct terakan_hw_state_draw * state);

#endif /* TERAKAN_HW_STATE_H */
