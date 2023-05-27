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

#include "util/bitset.h"

#include <stdbool.h>

enum terakan_hw_state_draw_index {
   TERAKAN_HW_STATE_DRAW_PA_SU_SC_MODE_CNTL,

   TERAKAN_HW_STATE_DRAW_CB_BLEND_RGBA,

   TERAKAN_HW_STATE_DRAW_COUNT,
};

/* Defaults correspond to zeroed (like via memset) values in Vulkan structures they're obtained
 * from.
 */

#define TERAKAN_HW_STATE_DRAW_DEFAULT_PA_SU_SC_MODE_CNTL \
   (/* cullMode = VK_CULL_MODE_NONE */ \
    S_028814_CULL_FRONT(0) | \
    S_028814_CULL_BACK(0) | \
    /* frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE */ \
    S_028814_FACE(0) | \
    /* polygonMode = VK_POLYGON_MODE_FILL */ \
    S_028814_POLY_MODE(V_028814_X_DISABLE_POLY_MODE) | \
    S_028814_POLYMODE_FRONT_PTYPE(V_028814_X_DRAW_TRIANGLES) | \
    S_028814_POLYMODE_BACK_PTYPE(V_028814_X_DRAW_TRIANGLES) | \
    /* depthBiasEnable = VK_FALSE */ \
    S_028814_POLY_OFFSET_FRONT_ENABLE(0) | \
    S_028814_POLY_OFFSET_BACK_ENABLE(0) | \
    S_028814_POLY_OFFSET_PARA_ENABLE(0) | \
    /* provokingVertexMode = VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT */ \
    S_028814_PROVOKING_VTX_LAST(0))

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

   /* TERAKAN_HW_STATE_DRAW_PA_SU_SC_MODE_CNTL */
   uint32_t pa_su_sc_mode_cntl;

   /* TERAKAN_HW_STATE_DRAW_CB_BLEND_RGBA */
   float cb_blend_rgba[4];
};

struct terakan_command_writer;

/* Pass the result of the external comparison to reduce the amount of state setting packets if the
 * state was not modified if needed (especially recommended when using static state in pipeline
 * objects). Floating-point state must be compared using memcmp to distinguish between the signs of
 * zero.
 * The function must still be called even if the state wasn't changed, however, to mark the state
 * item as needed in the current command buffer and also to make sure it's emitted for the first
 * time before the next draw.
 */
void terakan_hw_state_draw_written(
   struct terakan_hw_state_draw * state, enum terakan_hw_state_draw_index state_index,
   bool modified);

void terakan_hw_state_draw_replace_fields(
   struct terakan_hw_state_draw * state, enum terakan_hw_state_draw_index state_index,
   uint32_t * value, uint32_t keep_fields, uint32_t set_fields);

void terakan_hw_state_draw_emit_modified(struct terakan_command_writer * command_writer);

void terakan_hw_state_draw_emit_all(struct terakan_command_writer * command_writer);

void terakan_hw_state_draw_reset(struct terakan_hw_state_draw * state);

#endif /* TERAKAN_HW_STATE_H */
