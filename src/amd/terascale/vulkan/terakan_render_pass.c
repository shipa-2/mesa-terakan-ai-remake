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

#include "terakan_command_buffer.h"
#include "terakan_entrypoints.h"
#include "terakan_image.h"
#include "terakan_state.h"

#include "util/macros.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

VKAPI_ATTR void VKAPI_CALL
terakan_CmdBeginRendering(VkCommandBuffer const commandBuffer,
                          VkRenderingInfo const * const pRenderingInfo)
{
   struct terakan_state_draw * const state =
      &terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx->state_draw;

   terakan_state_draw_set_pending(state, TERAKAN_STATE_DRAW_INDEX_COLOR_ATTACHMENT_USAGE);
   state->color_attachment_usage.bound = 0;
   terakan_state_draw_set_pending(state, TERAKAN_STATE_DRAW_INDEX_CB_COLOR_MRT);
   for (uint32_t color_attachment_index = 0;
        color_attachment_index < pRenderingInfo->colorAttachmentCount; ++color_attachment_index) {
      struct terakan_image_view const * const color_view = terakan_image_view_from_handle(
         pRenderingInfo->pColorAttachments[color_attachment_index].imageView);
      if (color_view == NULL || G_028C70_FORMAT(color_view->color.info) == V_028C70_COLOR_INVALID) {
         continue;
      }
      struct terakan_state_draw_cb_color * const cb_color =
         &state->attachment_cb_color[color_attachment_index];
      cb_color->bo = color_view->bo;
      memcpy(&cb_color->color, &color_view->color, sizeof(struct terakan_color_descriptor));
      terakan_color_descriptor_image_view_to_color_attachment(&cb_color->color);
      memcpy(&cb_color->meta, &color_view->color_meta,
             sizeof(struct terakan_color_meta_descriptor));
      state->color_attachment_usage.bound |= (uint8_t)1 << color_attachment_index;
   }
}
