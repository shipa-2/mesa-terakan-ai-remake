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

#include "terakan_cp_dma.h"

#include "terakan_barrier.h"
#include "terakan_buffer.h"
#include "terakan_device.h"
#include "terakan_entrypoints.h"

#include "amd/terascale/common/terascale_wddm.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/macros.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Maximum value that can be specified in the BYTE_COUNT field of a CP DMA command. */
#define TERAKAN_CP_DMA_MAX_BYTE_COUNT (((VkDeviceSize)1 << 21) - 1)

#define TERAKAN_CP_DMA_MAX_ALIGNED_COPY_BYTE_COUNT                                                 \
   (TERAKAN_CP_DMA_MAX_BYTE_COUNT & ~(TERAKAN_CP_DMA_COPY_OPTIMAL_ALIGNMENT - 1))

void
terakan_cp_dma_sync_cp_me(struct terakan_gfx_command_writer * const command_writer)
{
   /* This function must not emit any commands, because synchronization must be done in the same
    * indirect buffer as the last CP DMA command, so memory accessed by CP DMA in the indirect
    * buffer submission can be released to the kernel after the submission is executed.
    */
   if (command_writer->last_unsynced_cp_dma_sync_dword == NULL) {
      return;
   }
   *command_writer->last_unsynced_cp_dma_sync_dword |= PKT3_CP_DMA_CP_SYNC;
   command_writer->last_unsynced_cp_dma_sync_dword = NULL;
}

static void
terakan_cp_dma_prepare(struct terakan_gfx_command_writer * const command_writer)
{
   terakan_barrier_emit_pending_actions(command_writer);
}

/* Important reminder: If the entire operation has to be split into multiple commands, BO references
 * must be reacquired after every terakan_gfx_command_writer_emit_with_bo call, as the new part may
 * end up in a different indirect buffer submission with a fresh list of BO references.
 */

void
terakan_cp_dma_copy(struct terakan_gfx_command_writer * const command_writer,
                    struct terakan_bo const * const src_bo, uint64_t const src_va,
                    enum terakan_bo_priority const src_bo_priority,
                    struct terakan_bo const * const dst_bo, uint64_t const dst_va,
                    enum terakan_bo_priority const dst_bo_priority, VkDeviceSize const size)
{
   uint32_t * packet;

   terakan_cp_dma_prepare(command_writer);

   /* Align the source address to the optimal alignment by skipping the misaligned bytes of the
    * source. They will be copied after the rest of the region.
    */
   uint64_t current_src_va_aligned =
      ALIGN_POT(src_va, (uint64_t)TERAKAN_CP_DMA_COPY_OPTIMAL_ALIGNMENT);
   VkDeviceSize const src_misaligned_head_size =
      MIN2((VkDeviceSize)(current_src_va_aligned - src_va), size);
   VkDeviceSize src_aligned_size_remaining = size - src_misaligned_head_size;
   uint64_t current_dst_va_src_aligned = dst_va + src_misaligned_head_size;
   while (src_aligned_size_remaining != 0) {
      uint32_t const command_copy_size =
         (uint32_t)MIN2(src_aligned_size_remaining, TERAKAN_CP_DMA_MAX_ALIGNED_COPY_BYTE_COUNT);
      packet = terakan_gfx_command_writer_emit_with_bo(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 1 + 5, 2, 0, 2);
      if (unlikely(packet == NULL)) {
         return;
      }
      *packet++ = PKT3(PKT3_CP_DMA, 5 - 1, 0);
      uint32_t const * const packet_src = packet;
      *packet++ = (uint32_t)current_src_va_aligned;
      command_writer->last_unsynced_cp_dma_sync_dword = packet;
      *packet++ = (current_src_va_aligned >> 32) & 0xFF;
      uint32_t const * const packet_dst = packet;
      *packet++ = (uint32_t)current_dst_va_src_aligned;
      *packet++ = (current_dst_va_src_aligned >> 32) & 0xFF;
      *packet++ = command_copy_size;
      terakan_gfx_command_writer_add_relocation_for_40_bits(
         command_writer, &packet, packet_src, packet_src + 1,
         TERASCALE_WDDM_PATCH_IDS_CP_DMA_SRC_LO, TERASCALE_WDDM_PATCH_IDS_CP_DMA_SRC_HI,
         terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                   src_bo, true, false, src_bo_priority));
      terakan_gfx_command_writer_add_relocation_for_40_bits(
         command_writer, &packet, packet_dst, packet_dst + 1,
         TERASCALE_WDDM_PATCH_IDS_CP_DMA_DST_LO, TERASCALE_WDDM_PATCH_IDS_CP_DMA_DST_HI,
         terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                   dst_bo, false, true, dst_bo_priority));
      terakan_gfx_command_writer_emit_done(command_writer, packet);
      current_src_va_aligned += command_copy_size;
      current_dst_va_src_aligned += command_copy_size;
      src_aligned_size_remaining -= command_copy_size;
   }

   if (src_misaligned_head_size != 0) {
      packet = terakan_gfx_command_writer_emit_with_bo(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 1 + 5, 2, 0, 2);
      if (unlikely(packet == NULL)) {
         return;
      }
      *packet++ = PKT3(PKT3_CP_DMA, 5 - 1, 0);
      uint32_t const * const packet_src = packet;
      *packet++ = (uint32_t)src_va;
      command_writer->last_unsynced_cp_dma_sync_dword = packet;
      *packet++ = (src_va >> 32) & 0xFF;
      uint32_t const * const packet_dst = packet;
      *packet++ = (uint32_t)dst_va;
      *packet++ = (dst_va >> 32) & 0xFF;
      *packet++ = (uint32_t)src_misaligned_head_size;
      terakan_gfx_command_writer_add_relocation_for_40_bits(
         command_writer, &packet, packet_src, packet_src + 1,
         TERASCALE_WDDM_PATCH_IDS_CP_DMA_SRC_LO, TERASCALE_WDDM_PATCH_IDS_CP_DMA_SRC_HI,
         terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                   src_bo, true, false, src_bo_priority));
      terakan_gfx_command_writer_add_relocation_for_40_bits(
         command_writer, &packet, packet_dst, packet_dst + 1,
         TERASCALE_WDDM_PATCH_IDS_CP_DMA_DST_LO, TERASCALE_WDDM_PATCH_IDS_CP_DMA_DST_HI,
         terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                   dst_bo, false, true, dst_bo_priority));
      terakan_gfx_command_writer_emit_done(command_writer, packet);
   }

   /* Align the internal total amount counter. */
   VkDeviceSize const size_misalignment = size & (TERAKAN_CP_DMA_COPY_OPTIMAL_ALIGNMENT - 1);
   if (size_misalignment != 0) {
      packet = terakan_gfx_command_writer_emit_with_bo(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 1 + 5, 1, 0, 2);
      if (unlikely(packet == NULL)) {
         return;
      }
      struct terakan_bo const * const gfx_discard_bo =
         terakan_gfx_command_writer_device(command_writer)->gfx_discard_bo;
      uint64_t const discard_dest_va = gfx_discard_bo->va + TERAKAN_CP_DMA_COPY_OPTIMAL_ALIGNMENT;
      *packet++ = PKT3(PKT3_CP_DMA, 5 - 1, 0);
      uint32_t const * const packet_src = packet;
      *packet++ = (uint32_t)gfx_discard_bo->va;
      command_writer->last_unsynced_cp_dma_sync_dword = packet;
      *packet++ = (gfx_discard_bo->va >> 32) & 0xFF;
      uint32_t const * const packet_dst = packet;
      *packet++ = (uint32_t)discard_dest_va;
      *packet++ = (discard_dest_va >> 32) & 0xFF;
      *packet++ = (uint32_t)(TERAKAN_CP_DMA_COPY_OPTIMAL_ALIGNMENT - size_misalignment);
      uint32_t const discard_bo_reference = terakan_bo_reference_writer_add_reference(
         &command_writer->base.bo_reference_writer, gfx_discard_bo, true, true,
         TERAKAN_BO_PRIORITY_SYNC);
      terakan_gfx_command_writer_add_relocation_for_40_bits(
         command_writer, &packet, packet_src, packet_src + 1,
         TERASCALE_WDDM_PATCH_IDS_CP_DMA_SRC_LO, TERASCALE_WDDM_PATCH_IDS_CP_DMA_SRC_HI,
         discard_bo_reference);
      terakan_gfx_command_writer_add_relocation_for_40_bits(
         command_writer, &packet, packet_dst, packet_dst + 1,
         TERASCALE_WDDM_PATCH_IDS_CP_DMA_DST_LO, TERASCALE_WDDM_PATCH_IDS_CP_DMA_DST_HI,
         discard_bo_reference);
      terakan_gfx_command_writer_emit_done(command_writer, packet);
   }
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdCopyBuffer2(VkCommandBuffer const commandBuffer,
                       VkCopyBufferInfo2 const * const pCopyBufferInfo)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;
   struct terakan_buffer const * const src_buffer =
      terakan_buffer_from_handle(pCopyBufferInfo->srcBuffer);
   struct terakan_buffer const * const dst_buffer =
      terakan_buffer_from_handle(pCopyBufferInfo->dstBuffer);
   for (uint32_t region_index = 0; region_index < pCopyBufferInfo->regionCount; ++region_index) {
      VkBufferCopy2 const * const region = &pCopyBufferInfo->pRegions[region_index];
      if (unlikely(region->size == 0)) {
         continue;
      }
      command_writer->post_buffer_copy_write_barrier_actions |=
         TERAKAN_BARRIER_ACTION_SYNC_ME_TO_CP_DMA;
      terakan_cp_dma_copy(command_writer, src_buffer->bo, src_buffer->va + region->srcOffset,
                          TERAKAN_BO_PRIORITY_CP_DMA, dst_buffer->bo,
                          dst_buffer->va + region->dstOffset, TERAKAN_BO_PRIORITY_CP_DMA,
                          region->size);
   }
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdUpdateBuffer(VkCommandBuffer const commandBuffer, VkBuffer const dstBuffer,
                        VkDeviceSize const dstOffset, VkDeviceSize const dataSize,
                        void const * const pData)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   struct terakan_bo const * src_bo;
   uint64_t src_va;
   void * const src_mapping =
      terakan_push_buffer_allocate(command_writer->base.command_buffer, dataSize,
                                   TERAKAN_CP_DMA_COPY_OPTIMAL_ALIGNMENT, &src_bo, &src_va);
   if (unlikely(src_mapping == NULL)) {
      return;
   }
   memcpy(src_mapping, pData, dataSize);

   struct terakan_buffer const * const dst_buffer = terakan_buffer_from_handle(dstBuffer);

   command_writer->post_buffer_copy_write_barrier_actions |=
      TERAKAN_BARRIER_ACTION_SYNC_ME_TO_CP_DMA;
   terakan_cp_dma_copy(command_writer, src_bo, src_va, TERAKAN_BO_PRIORITY_CP_DMA, dst_buffer->bo,
                       dst_buffer->va + dstOffset, TERAKAN_BO_PRIORITY_CP_DMA, dataSize);
}

void
terakan_cp_dma_fill(struct terakan_gfx_command_writer * const command_writer,
                    uint32_t const data_dword, struct terakan_bo const * const bo, uint64_t va,
                    enum terakan_bo_priority const bo_priority, VkDeviceSize size_bytes)
{
   terakan_cp_dma_prepare(command_writer);

   while (size_bytes != 0) {
      uint32_t command_fill_size = (uint32_t)MIN2(
         size_bytes, TERAKAN_CP_DMA_MAX_BYTE_COUNT & ~(uint32_t)(sizeof(uint32_t) - 1));

      uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 1 + 5, 1, 0, 1);
      if (unlikely(packet == NULL)) {
         return;
      }
      *packet++ = PKT3(PKT3_CP_DMA, 5 - 1, 0);
      /* TODO(Triang3l): On big-endian hosts, should the data be byte-swapped, or do INDIRECT_BUFFER
       * swap and BUF_SWAP_32BIT cancel each other for CP DMA?
       */
      *packet++ = data_dword;
      command_writer->last_unsynced_cp_dma_sync_dword = packet;
      *packet++ = PKT3_CP_DMA_SRC_SEL(2);
      uint32_t const * const packet_dst = packet;
      *packet++ = (uint32_t)va;
      *packet++ = (va >> 32) & 0xFF;
      *packet++ = command_fill_size;
      terakan_gfx_command_writer_add_relocation_for_40_bits(
         command_writer, &packet, packet_dst, packet_dst + 1,
         TERASCALE_WDDM_PATCH_IDS_CP_DMA_DST_LO, TERASCALE_WDDM_PATCH_IDS_CP_DMA_DST_HI,
         terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer, bo,
                                                   false, true, bo_priority));
      terakan_gfx_command_writer_emit_done(command_writer, packet);

      va += command_fill_size;
      size_bytes -= command_fill_size;
   }
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdFillBuffer(VkCommandBuffer const commandBuffer, VkBuffer const dstBuffer,
                      VkDeviceSize const dstOffset, VkDeviceSize const size, uint32_t const data)
{
   struct terakan_buffer const * const dst_buffer = terakan_buffer_from_handle(dstBuffer);
   VkDeviceSize const dst_offset_aligned = dstOffset & ~(VkDeviceSize)(sizeof(uint32_t) - 1);
   terakan_cp_dma_fill(terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx, data,
                       dst_buffer->bo, dst_buffer->va + dst_offset_aligned,
                       TERAKAN_BO_PRIORITY_CP_DMA,
                       ((dstOffset + vk_buffer_range(&dst_buffer->vk, dstOffset, size)) &
                        ~(VkDeviceSize)(sizeof(uint32_t) - 1)) -
                          dst_offset_aligned);
}
