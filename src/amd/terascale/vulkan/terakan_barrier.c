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

#include "terakan_barrier.h"
#include "terakan_command_buffer.h"
#include "terakan_cp_dma.h"
#include "terakan_device.h"
#include "terakan_entrypoints.h"
#include "terakan_physical_device.h"

#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/macros.h"
#include "vk_synchronization.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static enum terakan_barrier_action_flags
terakan_barrier_get_src_stage_actions(VkPipelineStageFlags2 const expanded_buffer_src_stages,
                                      VkPipelineStageFlags2 const expanded_image_src_stages)
{
   enum terakan_barrier_action_flags actions = 0;
   VkPipelineStageFlags2 const expanded_src_stages =
      expanded_buffer_src_stages | expanded_image_src_stages;
   if (expanded_src_stages &
       (VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)) {
      actions |= TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CS;
   }
   if (expanded_src_stages &
       (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT |
        VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT)) {
      actions |= TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;
   } else if (expanded_src_stages &
              (VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
               VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
               VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
               VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT |
               VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT)) {
      actions |= TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_VS;
   }
   if (expanded_src_stages & (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT)) {
      actions |= TERAKAN_BARRIER_ACTION_SYNC_ME_TO_CP_DMA;
      if (expanded_buffer_src_stages &
          (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT)) {
         /* Indirect and index buffers can be read by PFP. */
         actions |= TERAKAN_BARRIER_ACTION_SYNC_PFP_TO_ME;
      }
   }
   return actions;
}

enum terakan_barrier_access_flags {
   TERAKAN_BARRIER_ACCESS_INDEX_TC_READ,
   TERAKAN_BARRIER_ACCESS_INDEX_VC_READ,
   TERAKAN_BARRIER_ACCESS_INDEX_SH_READ,
   TERAKAN_BARRIER_ACCESS_INDEX_DB_READ,
   TERAKAN_BARRIER_ACCESS_INDEX_DB_WRITE,
   TERAKAN_BARRIER_ACCESS_INDEX_CB_RAT_READ,
   TERAKAN_BARRIER_ACCESS_INDEX_CB_RAT_WRITE,
   TERAKAN_BARRIER_ACCESS_INDEX_CB_MRT_READ,
   TERAKAN_BARRIER_ACCESS_INDEX_CB_MRT_WRITE,

   TERAKAN_BARRIER_ACCESS_TC_READ = BITFIELD_BIT(TERAKAN_BARRIER_ACCESS_INDEX_TC_READ),
   TERAKAN_BARRIER_ACCESS_VC_READ = BITFIELD_BIT(TERAKAN_BARRIER_ACCESS_INDEX_VC_READ),
   /* Shader (such as ALU constants) - corresponds to SH_ACTION_ENA in CP_COHER_CNTL. */
   TERAKAN_BARRIER_ACCESS_SH_READ = BITFIELD_BIT(TERAKAN_BARRIER_ACCESS_INDEX_SH_READ),
   TERAKAN_BARRIER_ACCESS_DB_READ = BITFIELD_BIT(TERAKAN_BARRIER_ACCESS_INDEX_DB_READ),
   TERAKAN_BARRIER_ACCESS_DB_WRITE = BITFIELD_BIT(TERAKAN_BARRIER_ACCESS_INDEX_DB_WRITE),
   /* Not using cacheless DB access for RATs. */
   TERAKAN_BARRIER_ACCESS_CB_RAT_READ = BITFIELD_BIT(TERAKAN_BARRIER_ACCESS_INDEX_CB_RAT_READ),
   TERAKAN_BARRIER_ACCESS_CB_RAT_WRITE = BITFIELD_BIT(TERAKAN_BARRIER_ACCESS_INDEX_CB_RAT_WRITE),
   TERAKAN_BARRIER_ACCESS_CB_MRT_READ = BITFIELD_BIT(TERAKAN_BARRIER_ACCESS_INDEX_CB_MRT_READ),
   TERAKAN_BARRIER_ACCESS_CB_MRT_WRITE = BITFIELD_BIT(TERAKAN_BARRIER_ACCESS_INDEX_CB_MRT_WRITE),
};

#define TERAKAN_BARRIER_ACCESS_ALL_WRITE                                                           \
   (TERAKAN_BARRIER_ACCESS_DB_WRITE | TERAKAN_BARRIER_ACCESS_CB_RAT_WRITE |                        \
    TERAKAN_BARRIER_ACCESS_CB_MRT_WRITE)

static enum terakan_barrier_access_flags
terakan_barrier_get_hw_access(struct terakan_device const * const device, VkAccessFlags2 access,
                              VkPipelineStageFlags2 const expanded_stages, bool const for_buffer,
                              VkImageAspectFlags const image_aspects)
{
   /* Apply aliases and filter out access not performed by the involved stages. */
   VkPipelineStageFlags2 const all_write_access =
      vk_write_access2_for_pipeline_stage_flags2(expanded_stages);
   VkPipelineStageFlags2 const all_read_access =
      vk_read_access2_for_pipeline_stage_flags2(expanded_stages);
   if (access & VK_ACCESS_2_MEMORY_WRITE_BIT) {
      access |= all_write_access;
   }
   if (access & VK_ACCESS_2_MEMORY_READ_BIT) {
      access |= all_read_access;
   }
   if (access & VK_ACCESS_2_SHADER_WRITE_BIT) {
      access |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
   }
   if (access & VK_ACCESS_2_SHADER_READ_BIT) {
      access |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
   }
   access &= all_write_access | all_read_access;

   enum terakan_barrier_access_flags hw_access = 0;

   /* TODO(Triang3l): CP PFP/ME, VGT access types. */

   /* INDIRECT_COMMAND_READ: Draw parameters or NumWorkgroups.
    * SHADER_STORAGE_READ: Read-only bindings (in non-RAT stages or with `restrict`). RAT immediate
    * buffers are also fetched from via TC, though using uncached reads.
    */
   if (access & (VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT |
                 VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT)) {
      /* Texture cache for draw parameters or NumWorkgroups. */
      hw_access |= TERAKAN_BARRIER_ACCESS_TC_READ;
   }

   if (access & VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT) {
      hw_access |= container_of(device->vk.physical, struct terakan_physical_device const, vk)
                         ->chip_family_info.has_vertex_cache
                      ? TERAKAN_BARRIER_ACCESS_VC_READ
                      : TERAKAN_BARRIER_ACCESS_TC_READ;
   }

   if (access & VK_ACCESS_2_UNIFORM_READ_BIT) {
      /* Texture cache for cases not supported by the ALU like dynamic data addressing, dynamic
       * indexing of banks 14 and 15, exceeding kcache limits.
       */
      hw_access |= TERAKAN_BARRIER_ACCESS_TC_READ | TERAKAN_BARRIER_ACCESS_SH_READ;
   }

   if (access & VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT) {
      hw_access |= TERAKAN_BARRIER_ACCESS_CB_MRT_READ;
   }
   if (access & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT) {
      hw_access |= TERAKAN_BARRIER_ACCESS_CB_MRT_WRITE;
   }

   if (access & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT) {
      hw_access |= TERAKAN_BARRIER_ACCESS_DB_READ;
   }
   if (access & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) {
      hw_access |= TERAKAN_BARRIER_ACCESS_DB_WRITE;
   }

   if (access & VK_ACCESS_2_TRANSFER_WRITE_BIT) {
      /* Copying from images to buffers, and clears not supported by CP DMA, are done via a RAT.
       * Image transfer writes for which image dimensionality matters (not full clears) are done via
       * an MRT (including for depth), but full clears are implemented the same way as buffer
       * clears.
       * Transfers to some single-plane color image formats (such as ones with non-power-of-two
       * bytes per block), however, are done via a RAT.
       */
      if ((for_buffer &&
           (expanded_stages & (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT))) ||
          (image_aspects && (expanded_stages & VK_PIPELINE_STAGE_2_CLEAR_BIT))) {
         hw_access |= TERAKAN_BARRIER_ACCESS_CB_RAT_WRITE;
      }
      if (image_aspects &&
          (expanded_stages & (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT))) {
         hw_access |= TERAKAN_BARRIER_ACCESS_CB_MRT_WRITE;
         /* Images of some single-plane color formats (like with non-power-of-two bytes per
          * block) are transferred to via RATs instead of MRTs.
          */
         if (image_aspects & VK_IMAGE_ASPECT_COLOR_BIT) {
            hw_access |= TERAKAN_BARRIER_ACCESS_CB_RAT_WRITE;
         }
      }
   }

   if ((access & (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)) &&
       ((expanded_stages & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) ||
        ((expanded_stages & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) &&
         device->vk.enabled_features.fragmentStoresAndAtomics))) {
      if (access & VK_ACCESS_2_SHADER_STORAGE_READ_BIT) {
         hw_access |= TERAKAN_BARRIER_ACCESS_CB_RAT_READ;
      }
      if (access & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT) {
         hw_access |= TERAKAN_BARRIER_ACCESS_CB_RAT_WRITE;
      }
   }

   return hw_access;
}

static enum terakan_barrier_action_flags
terakan_barrier_get_cache_actions(enum terakan_barrier_access_flags const src_access,
                                  enum terakan_barrier_access_flags const dst_access)
{
   /* Note that barriers must be transitive, and optimizations breaking that must not be performed.
    * For instance, in a chain like:
    * SHADER_STORAGE_WRITE > SHADER_SAMPLED_READ > VERTEX_ATTRIBUTE_READ
    * the second barrier must not be ignored even though it's read > read, because otherwise the
    * writes made available by CB would've only been made visible to TC, but not to VC.
    */

   /* If there were no writes prior, don't invalidate caches that have already been invalidated.
    * However, caches must still be invalidated for all potential new read-only access types for
    * transitivity.
    */
   enum terakan_barrier_access_flags const invalidate_access =
      src_access & TERAKAN_BARRIER_ACCESS_ALL_WRITE ? dst_access : dst_access & ~src_access;

   enum terakan_barrier_access_flags const involved_access = src_access | invalidate_access;

   /* Handle cases when the data stays within the unit it was written by, and thus the cache doesn't
    * need to be flushed or invalidated. This situation is especially common when ending a render
    * pass for reasons not related to the framebuffer, and then starting a new one continuing
    * drawing to the same framebuffer, in which case an attachment access dependency is still needed
    * to preserve rasterization order (which is defined to be only within a subpass) between the
    * passes.
    */
   if (!(involved_access & ~(TERAKAN_BARRIER_ACCESS_DB_READ | TERAKAN_BARRIER_ACCESS_DB_WRITE)) ||
       !(involved_access &
         ~(TERAKAN_BARRIER_ACCESS_CB_RAT_READ | TERAKAN_BARRIER_ACCESS_CB_RAT_WRITE)) ||
       !(involved_access &
         ~(TERAKAN_BARRIER_ACCESS_CB_MRT_READ | TERAKAN_BARRIER_ACCESS_CB_MRT_WRITE))) {
      return 0;
   }

   enum terakan_barrier_action_flags actions = 0;
   if (involved_access & (TERAKAN_BARRIER_ACCESS_DB_READ | TERAKAN_BARRIER_ACCESS_DB_WRITE)) {
      actions |= TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_DATA;
   }
   if (involved_access &
       (TERAKAN_BARRIER_ACCESS_CB_RAT_READ | TERAKAN_BARRIER_ACCESS_CB_RAT_WRITE)) {
      actions |= TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RAT;
   }
   if (involved_access &
       (TERAKAN_BARRIER_ACCESS_CB_MRT_READ | TERAKAN_BARRIER_ACCESS_CB_MRT_WRITE)) {
      actions |= TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_MRT_DATA;
   }
   if (invalidate_access & TERAKAN_BARRIER_ACCESS_TC_READ) {
      actions |= TERAKAN_BARRIER_ACTION_INV_TC;
   }
   if (invalidate_access & TERAKAN_BARRIER_ACCESS_VC_READ) {
      actions |= TERAKAN_BARRIER_ACTION_INV_VC;
   }
   if (invalidate_access & TERAKAN_BARRIER_ACCESS_SH_READ) {
      actions |= TERAKAN_BARRIER_ACTION_INV_SH;
   }
   return actions;
}

static void
terakan_barrier_emit_event_write(struct terakan_gfx_command_writer * const command_writer,
                                 uint32_t event)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(command_writer, 2, 0, 0, false);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_EVENT_WRITE, 0, 0);
   *packet++ = event;
}

void
terakan_barrier_emit_pending_actions(struct terakan_gfx_command_writer * const command_writer)
{
   enum terakan_barrier_action_flags const actions = command_writer->pending_barrier_actions;
   /* Nonzero mostly outside render passes, skip lots of checks if there's nothing to do. */
   if (actions == 0) {
      return;
   }
   command_writer->pending_barrier_actions = 0;

   /* Wait packets must be executed first, because SURFACE_SYNC doesn't wait for shaders if it's not
    * flushing CB or DB.
    */

   if (actions & TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CS) {
      terakan_barrier_emit_event_write(command_writer,
                                       EVENT_TYPE(EVENT_TYPE_CS_PARTIAL_FLUSH) | EVENT_INDEX(4));
   }

   if (actions & TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS) {
      terakan_barrier_emit_event_write(command_writer,
                                       EVENT_TYPE(EVENT_TYPE_PS_PARTIAL_FLUSH) | EVENT_INDEX(4));
   } else if (actions & TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_VS) {
      terakan_barrier_emit_event_write(command_writer,
                                       EVENT_TYPE(EVENT_TYPE_VS_PARTIAL_FLUSH) | EVENT_INDEX(4));
   }

   uint32_t cp_coher_cntl = 0;

   if (actions &
       (TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_DATA | TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_META)) {
      cp_coher_cntl |= S_0085F0_DB_DEST_BASE_ENA(1) | S_0085F0_DB_ACTION_ENA(1);
      if (!(~actions & (TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_DATA |
                        TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_META))) {
         terakan_barrier_emit_event_write(
            command_writer, EVENT_TYPE(EVENT_TYPE_DB_CACHE_FLUSH_AND_INV) | EVENT_INDEX(0));
      } else {
         if (actions & TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_DATA) {
            terakan_gfx_command_writer_emit_event_write_eop_discarding_data(
               command_writer, EVENT_TYPE(EVENT_TYPE_FLUSH_AND_INV_DB_DATA_TS) | EVENT_INDEX(5));
         }
         if (actions & TERAKAN_BARRIER_ACTION_FLUSH_INV_DB_META) {
            terakan_barrier_emit_event_write(
               command_writer, EVENT_TYPE(EVENT_TYPE_FLUSH_AND_INV_DB_META) | EVENT_INDEX(0));
         }
      }
   }

   if (actions &
       (TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RAT | TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_MRT_DATA |
        TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_MRT_META)) {
      cp_coher_cntl |= S_0085F0_CB0_DEST_BASE_ENA(1) | S_0085F0_CB1_DEST_BASE_ENA(1) |
                       S_0085F0_CB2_DEST_BASE_ENA(1) | S_0085F0_CB3_DEST_BASE_ENA(1) |
                       S_0085F0_CB4_DEST_BASE_ENA(1) | S_0085F0_CB5_DEST_BASE_ENA(1) |
                       S_0085F0_CB6_DEST_BASE_ENA(1) | S_0085F0_CB7_DEST_BASE_ENA(1) |
                       S_0085F0_CB_ACTION_ENA(1);
      if (actions & TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RAT) {
         cp_coher_cntl |= S_0085F0_CB8_DEST_BASE_ENA(1) | S_0085F0_CB9_DEST_BASE_ENA(1) |
                          S_0085F0_CB10_DEST_BASE_ENA(1) | S_0085F0_CB11_DEST_BASE_ENA(1) |
                          S_0085F0_SMX_ACTION_ENA(1);
         /* Flushes and invalidates MRT and RAT data, but not meta. */
         terakan_gfx_command_writer_emit_event_write_eop_discarding_data(
            command_writer, EVENT_TYPE(EVENT_TYPE_FLUSH_AND_INV_CB_DATA_TS) | EVENT_INDEX(5));
      } else if (actions & TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_MRT_DATA) {
         /* Flushes and invalidates MRT data, but not meta or RAT data. */
         terakan_barrier_emit_event_write(
            command_writer, EVENT_TYPE(EVENT_TYPE_FLUSH_AND_INV_CB_PIXEL_DATA) | EVENT_INDEX(0));
      }
      if (actions & TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_MRT_META) {
         terakan_barrier_emit_event_write(
            command_writer, EVENT_TYPE(EVENT_TYPE_FLUSH_AND_INV_CB_META) | EVENT_INDEX(0));
      }
   }

   if (actions & TERAKAN_BARRIER_ACTION_INV_TC) {
      cp_coher_cntl |= S_0085F0_TC_ACTION_ENA(1);
   }

   if (actions & TERAKAN_BARRIER_ACTION_INV_VC) {
      cp_coher_cntl |= S_0085F0_VC_ACTION_ENA(1);
   }

   if (actions & TERAKAN_BARRIER_ACTION_INV_SH) {
      cp_coher_cntl |= S_0085F0_SH_ACTION_ENA(1);
   }

   if (cp_coher_cntl) {
      uint32_t * surface_sync_packet =
         terakan_gfx_command_writer_emit(command_writer, 5, 0, 0, false);
      if (unlikely(surface_sync_packet == NULL)) {
         return;
      }
      *surface_sync_packet++ = PKT3(PKT3_SURFACE_SYNC, 4 - 1, 0);
      *surface_sync_packet++ = cp_coher_cntl;
      *surface_sync_packet++ = UINT32_MAX; /* CP_COHER_SIZE */
      *surface_sync_packet++ = 0;          /* CP_COHER_BASE */
      *surface_sync_packet++ = 0xA;        /* POLL_INTERVAL */
   }

   if (actions & TERAKAN_BARRIER_ACTION_SYNC_ME_TO_CP_DMA) {
      terakan_cp_dma_sync_cp_me(command_writer);
   }

   /* If CP DMA needs to be awaited at PFP, this must be done after the CP DMA sync. */
   if (actions & TERAKAN_BARRIER_ACTION_SYNC_PFP_TO_ME) {
      uint32_t * pfp_sync_me_packet =
         terakan_gfx_command_writer_emit(command_writer, 2, 0, 0, false);
      if (unlikely(pfp_sync_me_packet == NULL)) {
         return;
      }
      *pfp_sync_me_packet++ = PKT3(PKT3_PFP_SYNC_ME, 0, 0);
      *pfp_sync_me_packet++ = 0;
   }
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdPipelineBarrier2(VkCommandBuffer const commandBuffer,
                            VkDependencyInfo const * const pDependencyInfo)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;
   struct terakan_device const * const device = container_of(
      command_writer->base.command_buffer->vk.base.device, struct terakan_device const, vk);

   VkPipelineStageFlags2 total_buffer_src_stages = 0, total_image_src_stages = 0;
   enum terakan_barrier_access_flags total_src_access = 0, total_dst_access = 0;

   for (uint32_t barrier_index = 0; barrier_index < pDependencyInfo->memoryBarrierCount;
        ++barrier_index) {
      VkMemoryBarrier2 const * const barrier = &pDependencyInfo->pMemoryBarriers[barrier_index];
      VkPipelineStageFlags2 const barrier_src_stages =
         vk_expand_src_stage_flags2(barrier->srcStageMask);
      total_buffer_src_stages |= barrier_src_stages;
      total_image_src_stages |= barrier_src_stages;
      total_src_access |= terakan_barrier_get_hw_access(device, barrier->srcAccessMask,
                                                        barrier_src_stages, true, true);
      total_dst_access |= terakan_barrier_get_hw_access(
         device, barrier->dstAccessMask, vk_expand_src_stage_flags2(barrier->dstStageMask), true,
         VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT |
            VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT |
            VK_IMAGE_ASPECT_PLANE_2_BIT);
   }

   for (uint32_t barrier_index = 0; barrier_index < pDependencyInfo->bufferMemoryBarrierCount;
        ++barrier_index) {
      VkBufferMemoryBarrier2 const * const barrier =
         &pDependencyInfo->pBufferMemoryBarriers[barrier_index];
      VkPipelineStageFlags2 const barrier_src_stages =
         vk_expand_src_stage_flags2(barrier->srcStageMask);
      total_buffer_src_stages |= barrier_src_stages;
      total_src_access |= terakan_barrier_get_hw_access(device, barrier->srcAccessMask,
                                                        barrier_src_stages, true, false);
      total_dst_access |= terakan_barrier_get_hw_access(
         device, barrier->dstAccessMask, vk_expand_src_stage_flags2(barrier->dstStageMask), true,
         VK_IMAGE_ASPECT_NONE);
   }

   for (uint32_t barrier_index = 0; barrier_index < pDependencyInfo->imageMemoryBarrierCount;
        ++barrier_index) {
      VkImageMemoryBarrier2 const * const barrier =
         &pDependencyInfo->pImageMemoryBarriers[barrier_index];
      VkPipelineStageFlags2 const barrier_src_stages =
         vk_expand_src_stage_flags2(barrier->srcStageMask);
      total_image_src_stages |= barrier_src_stages;
      total_src_access |= terakan_barrier_get_hw_access(device, barrier->srcAccessMask,
                                                        barrier_src_stages, false, true);
      total_dst_access |= terakan_barrier_get_hw_access(
         device, barrier->dstAccessMask, vk_expand_src_stage_flags2(barrier->dstStageMask), false,
         barrier->subresourceRange.aspectMask);
   }

   command_writer->pending_barrier_actions |=
      terakan_barrier_get_src_stage_actions(total_buffer_src_stages, total_image_src_stages) |
      terakan_barrier_get_cache_actions(total_src_access, total_dst_access);
}
