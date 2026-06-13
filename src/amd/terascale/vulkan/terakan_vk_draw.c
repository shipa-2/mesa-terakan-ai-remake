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

#include "terakan_buffer.h"
#include "terakan_command_buffer.h"
#include "terakan_entrypoints.h"
#include "terakan_vk_state.h"

#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/macros.h"
#include "util/u_endian.h"
#include "util/u_math.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

VKAPI_ATTR void VKAPI_CALL
terakan_CmdBindIndexBuffer2(VkCommandBuffer const commandBuffer, VkBuffer const bufferHandle,
                            VkDeviceSize const offset, VkDeviceSize const size,
                            VkIndexType const indexType)
{
   bool const index_type_32_bit = indexType == VK_INDEX_TYPE_UINT32;
   uint32_t const index_type = index_type_32_bit
                                  ? TERAKAN_HW_CONFIG_DRAW_VGT_DMA_INDEX_TYPE_32_HOST_ENDIAN
                                  : TERAKAN_HW_CONFIG_DRAW_VGT_DMA_INDEX_TYPE_16_HOST_ENDIAN;

   struct terakan_hw_config_draw_vgt_dma_index_buffer index_buffer = {};
   struct terakan_buffer const * const buffer = terakan_buffer_from_handle(bufferHandle);
   /* #MemoryIntegrity is mostly handled deeper, just set up the setter's arguments safely. */
   if (buffer != NULL && offset <= buffer->vk.size) {
      index_buffer.bo = buffer->bo;
      index_buffer.va = buffer->va + offset;
      uint64_t const size_indices_u64 =
         vk_buffer_range(&buffer->vk, offset, size) >> (index_type_32_bit ? 2 : 1);
      index_buffer.size_indices = (uint32_t)MIN2(size_indices_u64, UINT32_MAX);
   }

   struct terakan_app_config_draw * const config =
      &terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx->app_config_draw;

   terakan_app_config_draw_set_vgt_dma_index_buffer(config, index_buffer, index_type);

   /* The `vkCmdSetPrimitiveRestartIndexEXT` reference in the Vulkan 1.4.352 specification says:
    *
    *     "Binding an index buffer invalidates the custom index value."
    */
   terakan_app_config_draw_set_vgt_dma_index_buffer_multi_prim_reset_index(
      config, index_type_32_bit ? 0xFFFFFFFFu : 0xFFFFu);
}

/* TODO(Triang3l): `vkCmdSetPrimitiveRestartIndexEXT`. */

static void
terakan_vk_draw_set_vertex_instance_offsets(struct terakan_gfx_command_writer * const command_writer,
                                            uint32_t const vertex_offset,
                                            uint32_t const instance_offset)
{
   terakan_app_config_draw_set_vgt_index_offset(&command_writer->app_config_draw, vertex_offset);

   /* The instance offset is not needed by internal draws, modify `hw_config` directly. */
   terakan_hw_config_shared_sq_vtx_start_inst_loc(&command_writer->hw_config_shared,
                                                  instance_offset);
}

static void
terakan_vk_before_draw(struct terakan_gfx_command_writer * const command_writer)
{
   terakan_vk_state_dynamic_apply(command_writer);

   terakan_gfx_command_writer_before_app_draw(command_writer);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDraw(VkCommandBuffer const commandBuffer, uint32_t const vertexCount,
                uint32_t const instanceCount, uint32_t const firstVertex,
                uint32_t const firstInstance)
{
   if (unlikely(instanceCount == 0)) {
      /* `VGT_NUM_INSTANCES` 0 is interpreted as 1. */
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   terakan_hw_config_shared_draw_set_vgt_num_instances(&command_writer->hw_config_shared,
                                                       instanceCount);
   terakan_app_config_draw_set_vgt_dma_index_buffer_draw_indexed(&command_writer->app_config_draw,
                                                                 false);
   terakan_vk_draw_set_vertex_instance_offsets(command_writer, firstVertex, firstInstance);

   terakan_vk_before_draw(command_writer);

   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW, 3);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_DRAW_INDEX_AUTO, 3 - 2, 0);
   *packet++ = vertexCount;
   *packet++ = S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_AUTO_INDEX);
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDrawIndexed(VkCommandBuffer const commandBuffer, uint32_t const indexCount,
                       uint32_t const instanceCount, uint32_t const firstIndex,
                       int32_t const vertexOffset, uint32_t const firstInstance)
{
   if (unlikely(instanceCount == 0)) {
      /* `VGT_NUM_INSTANCES` 0 is interpreted as 1. */
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   terakan_hw_config_shared_draw_set_vgt_num_instances(&command_writer->hw_config_shared,
                                                       instanceCount);
   terakan_app_config_draw_set_vgt_dma_index_buffer_draw_indexed(&command_writer->app_config_draw,
                                                                 true);
   terakan_vk_draw_set_vertex_instance_offsets(command_writer, (uint32_t)vertexOffset,
                                               firstInstance);

   terakan_vk_before_draw(command_writer);

   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW, 4);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(EG_PKT3_DRAW_INDEX_OFFSET, 4 - 2, 0);
   *packet++ = firstIndex;
   *packet++ = indexCount;
   *packet++ = S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_DMA);
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}
