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
#include "terakan_entrypoints.h"
#include "terakan_draw.h"
#include "terakan_hw_state.h"

void
terakan_before_hw_draw(struct terakan_command_writer * command_writer)
{
   terakan_hw_state_draw_emit_modified(command_writer);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDraw(
   VkCommandBuffer const commandBuffer, uint32_t const vertexCount, uint32_t const instanceCount,
   uint32_t const firstVertex, uint32_t const firstInstance)
{
   struct terakan_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer;

   terakan_before_hw_draw(command_writer);

   /* TODO(Triang3l): Draw. */
}
