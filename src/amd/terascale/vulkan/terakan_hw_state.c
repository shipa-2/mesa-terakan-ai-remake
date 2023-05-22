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

#include "terakan_command_buffer.h"
#include "terakan_hw_state.h"

#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/macros.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef void (* terakan_hw_state_draw_emit_function)(
   struct terakan_command_writer * command_writer, enum terakan_hw_state_draw_index state_index);

static void
terakan_hw_state_draw_emit_cb_blend_rgba(
   struct terakan_command_writer * const command_writer,
   UNUSED enum terakan_hw_state_draw_index const state_index)
{
   uint32_t * packet = terakan_command_writer_emit(command_writer, 2 + 4, 0, 0);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 4, 0);
   *packet++ = TERAKAN_CONTEXT_REG_OFFSET(R_028414_CB_BLEND_RED);
   memcpy(packet, command_writer->hw_state_draw.cb_blend_rgba, sizeof(float) * 4);
}

static terakan_hw_state_draw_emit_function const terakan_hw_state_draw_emit_functions[
   TERAKAN_HW_STATE_DRAW_COUNT] = {
   [TERAKAN_HW_STATE_DRAW_CB_BLEND_RGBA] = terakan_hw_state_draw_emit_cb_blend_rgba,
};

void
terakan_hw_state_draw_written(
   struct terakan_hw_state_draw * const state, enum terakan_hw_state_draw_index const state_index,
   bool modified)
{
   if (!BITSET_TEST(state->state_ever_written, state_index)) {
      BITSET_SET(state->state_ever_written, state_index);
      modified = true;
   }
   if (modified) {
      BITSET_SET(state->state_modified, state_index);
   }
}

void
terakan_hw_state_draw_emit_all(struct terakan_command_writer * const command_writer)
{
   struct terakan_hw_state_draw * const state = &command_writer->hw_state_draw;
   BITSET_ZERO(state->state_modified);
   unsigned state_index;
   BITSET_FOREACH_SET(state_index, state->state_ever_written, TERAKAN_HW_STATE_DRAW_COUNT) {
      terakan_hw_state_draw_emit_functions[state_index](
         command_writer, (enum terakan_hw_state_draw_index)state_index);
   }
}

void
terakan_hw_state_draw_emit_modified(struct terakan_command_writer * const command_writer)
{
   struct terakan_hw_state_draw * const state = &command_writer->hw_state_draw;
   unsigned state_index;
   BITSET_FOREACH_SET(state_index, state->state_modified, TERAKAN_HW_STATE_DRAW_COUNT) {
      terakan_hw_state_draw_emit_functions[state_index](
         command_writer, (enum terakan_hw_state_draw_index)state_index);
      if (unlikely(!BITSET_TEST(state->state_modified, state_index))) {
         /* If state_modified was zeroed during an emit call, switched to another indirect buffer,
          * and all state has been applied.
          */
         return;
      }
      BITSET_CLEAR(state->state_modified, state_index);
   }
}

void
terakan_hw_state_draw_reset(struct terakan_hw_state_draw * const state)
{
   BITSET_ZERO(state->state_ever_written);
   BITSET_ZERO(state->state_modified);
}
