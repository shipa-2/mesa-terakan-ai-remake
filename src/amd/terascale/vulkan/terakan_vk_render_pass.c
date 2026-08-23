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

#include "terakan_command_buffer.h"
#include "terakan_descriptor.h"
#include "terakan_entrypoints.h"
#include "terakan_image.h"
#include "terakan_vk_state.h"

#include "meta/terakan_meta_impl.h"

#include "util/bitscan.h"
#include "util/macros.h"
#include "util/u_math.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static bool
terakan_render_debug_enabled(void)
{
   static int enabled = -1;
   if (enabled < 0)
      enabled = getenv("TERAKAN_DEBUG_RENDER") != NULL ? 1 : 0;
   return enabled != 0;
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdBeginRendering(VkCommandBuffer const commandBuffer,
                          VkRenderingInfo const * const pRenderingInfo)
{
   struct terakan_command_buffer * const command_buffer =
      terakan_command_buffer_from_handle(commandBuffer);
   struct terakan_gfx_command_writer * const command_writer = command_buffer->command_writer.gfx;
   struct terakan_app_config_draw * const config = &command_writer->app_config_draw;

   if (terakan_render_debug_enabled()) {
      fprintf(stderr, "[TERAKAN_RENDER] begin flags=0x%x area=%d,%d %ux%u colors=%u\n",
              pRenderingInfo->flags, pRenderingInfo->renderArea.offset.x,
              pRenderingInfo->renderArea.offset.y, pRenderingInfo->renderArea.extent.width,
              pRenderingInfo->renderArea.extent.height, pRenderingInfo->colorAttachmentCount);
      for (uint32_t i = 0; i < pRenderingInfo->colorAttachmentCount; ++i) {
         struct terakan_image_view const * const view =
            terakan_image_view_from_handle(pRenderingInfo->pColorAttachments[i].imageView);
         if (view != NULL) {
            fprintf(stderr,
                    "[TERAKAN_RENDER] color[%u] extent=%ux%u samples=%u load=%u store=%u resolve=%u\n",
                    i, view->vk.extent.width, view->vk.extent.height, view->vk.image->samples,
                    pRenderingInfo->pColorAttachments[i].loadOp,
                    pRenderingInfo->pColorAttachments[i].storeOp,
                    pRenderingInfo->pColorAttachments[i].resolveMode);
         }
      }
   }

   command_buffer->rendering_flags = pRenderingInfo->flags;
   for (unsigned color_attachment_index = 0;
        color_attachment_index < TERAKAN_VK_STATE_MAX_COLOR_ATTACHMENTS;
        ++color_attachment_index) {
      command_buffer->rendering_color_resolves[color_attachment_index] =
         (struct terakan_rendering_color_resolve){};
   }
   command_buffer->rendering_depth_resolve = (struct terakan_rendering_color_resolve){};

   VkSampleCountFlagBits db_eqaa_ps_iter_least_fragments_r9xx = VK_SAMPLE_COUNT_16_BIT;

   /* #MemoryIntegrity: Validating the attachments and clamping the render area and the layer count
    * to prevent out of bounds access and indeterminate state, including in meta operations, in case
    * of invalid usage. Sample count validation is expected to be done by `terakan_app_config_draw`
    * since it depends on the rasterization sample count.
    */
   uint16_t render_area_upper_bound[2] = {TERAKAN_IMAGE_MAX_WIDTH_HEIGHT,
                                          TERAKAN_IMAGE_MAX_WIDTH_HEIGHT};
   uint32_t layer_count_minus_1 =
      CLAMP(pRenderingInfo->layerCount, (uint32_t)1, (uint32_t)TERAKAN_IMAGE_MAX_TARGET_SLICES) - 1;

   uint8_t color_attachments_bound = 0b0;
   uint32_t const color_attachment_count =
      MIN2(pRenderingInfo->colorAttachmentCount, TERAKAN_VK_STATE_MAX_COLOR_ATTACHMENTS);
   for (uint32_t color_attachment_index = 0; color_attachment_index < color_attachment_count;
        ++color_attachment_index) {
      struct terakan_image_view const * const color_attachment_view =
         terakan_image_view_from_handle(
            pRenderingInfo->pColorAttachments[color_attachment_index].imageView);
      if (color_attachment_view == NULL) {
         continue;
      }

      db_eqaa_ps_iter_least_fragments_r9xx =
         MIN2(db_eqaa_ps_iter_least_fragments_r9xx, color_attachment_view->vk.image->samples);

      if (!terakan_color_descriptor_is_bound(color_attachment_view->bo,
                                             &color_attachment_view->color)) {
         continue;
      }

      render_area_upper_bound[0] = MIN2(render_area_upper_bound[0],
                                        G_028C78_WIDTH_MAX(color_attachment_view->color.dim) + 1u);
      render_area_upper_bound[1] = MIN2(render_area_upper_bound[1],
                                        G_028C78_HEIGHT_MAX(color_attachment_view->color.dim) + 1u);
      layer_count_minus_1 =
         MIN2(layer_count_minus_1, G_028C6C_SLICE_MAX(color_attachment_view->color.view) -
                                      G_028C6C_SLICE_START(color_attachment_view->color.view));

      color_attachments_bound |= BITFIELD_BIT(color_attachment_index);
   }

   if (terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx) {
      terakan_app_config_draw_set_db_eqaa_ps_iter_least_fragments_log2_r9xx(
         config, util_logbase2((uint32_t)db_eqaa_ps_iter_least_fragments_r9xx));
   }

   uint32_t clear_attachment_count = 0;
   VkClearAttachment clear_attachments[TERAKAN_VK_STATE_MAX_COLOR_ATTACHMENTS + 1];

   struct terakan_image_view const * const depth_attachment_view =
      pRenderingInfo->pDepthAttachment != NULL
         ? terakan_image_view_from_handle(pRenderingInfo->pDepthAttachment->imageView)
         : NULL;
   struct terakan_image_view const * const stencil_attachment_view =
      pRenderingInfo->pStencilAttachment != NULL
         ? terakan_image_view_from_handle(pRenderingInfo->pStencilAttachment->imageView)
         : NULL;
   /* VUID-VkRenderingInfo-pDepthAttachment-06085: "If neither pDepthAttachment or
    * pStencilAttachment are NULL and the imageView member of either structure is not
    * VK_NULL_HANDLE, the imageView member of each structure must be the same
    */
   struct terakan_image_view const * const depth_stencil_attachment_view =
      depth_attachment_view != NULL ? depth_attachment_view : stencil_attachment_view;
   if (depth_stencil_attachment_view != NULL) {
      struct terakan_depth_stencil_descriptor depth_stencil_attachment_descriptor =
         depth_stencil_attachment_view->depth_stencil;
      if (depth_attachment_view == NULL) {
         depth_stencil_attachment_descriptor.z_info &= C_028040_FORMAT;
      }
      if (stencil_attachment_view == NULL) {
         depth_stencil_attachment_descriptor.stencil_info &= C_028044_FORMAT;
      }
      bool depth_bound, stencil_bound;
      terakan_depth_stencil_descriptor_is_bound(depth_stencil_attachment_view->bo,
                                                &depth_stencil_attachment_descriptor, &depth_bound,
                                                &stencil_bound);
      if (depth_bound || stencil_bound) {
         uint32_t const depth_stencil_attachment_width =
            u_minify(depth_stencil_attachment_view->vk.extent.width,
                     depth_stencil_attachment_view->vk.base_mip_level);
         uint32_t const depth_stencil_attachment_height =
            u_minify(depth_stencil_attachment_view->vk.extent.height,
                     depth_stencil_attachment_view->vk.base_mip_level);
         render_area_upper_bound[0] =
            MIN2(render_area_upper_bound[0], depth_stencil_attachment_width);
         render_area_upper_bound[1] =
            MIN2(render_area_upper_bound[1], depth_stencil_attachment_height);

         uint32_t const depth_stencil_layer_start =
            G_028008_SLICE_START(depth_stencil_attachment_descriptor.view);
         layer_count_minus_1 =
            MIN2(layer_count_minus_1, G_028008_SLICE_MAX(depth_stencil_attachment_descriptor.view) -
                                         depth_stencil_layer_start);
         depth_stencil_attachment_descriptor.view =
            (depth_stencil_attachment_descriptor.view & C_028008_SLICE_MAX) |
            S_028008_SLICE_MAX(depth_stencil_layer_start + layer_count_minus_1);

         terakan_app_config_draw_set_db_depth_stencil_buffer(
            config, depth_stencil_attachment_view->bo, &depth_stencil_attachment_descriptor);

         VkClearAttachment depth_stencil_attachment_clear = {};
         if (depth_bound &&
             pRenderingInfo->pDepthAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            depth_stencil_attachment_clear.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
            depth_stencil_attachment_clear.clearValue.depthStencil.depth =
               pRenderingInfo->pDepthAttachment->clearValue.depthStencil.depth;
         }
         if (stencil_bound &&
             pRenderingInfo->pStencilAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            depth_stencil_attachment_clear.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            depth_stencil_attachment_clear.clearValue.depthStencil.stencil =
               pRenderingInfo->pStencilAttachment->clearValue.depthStencil.stencil;
         }
         if (depth_stencil_attachment_clear.aspectMask) {
            assert(clear_attachment_count < ARRAY_SIZE(clear_attachments));
            clear_attachments[clear_attachment_count++] = depth_stencil_attachment_clear;
         }
      } else {
         terakan_app_config_draw_set_db_depth_stencil_buffer(config, NULL, NULL);
      }
   } else {
      terakan_app_config_draw_set_db_depth_stencil_buffer(config, NULL, NULL);
   }

   u_foreach_bit (color_attachment_index, color_attachments_bound) {
      VkRenderingAttachmentInfo const * const color_attachment =
         &pRenderingInfo->pColorAttachments[color_attachment_index];
      struct terakan_image_view const * const color_attachment_view =
         terakan_image_view_from_handle(color_attachment->imageView);

      struct terakan_color_descriptor color_attachment_descriptor = color_attachment_view->color;
      color_attachment_descriptor.view =
         (color_attachment_descriptor.view & C_028C6C_SLICE_MAX) |
         S_028C6C_SLICE_MAX(G_028C6C_SLICE_START(color_attachment_descriptor.view) +
                            layer_count_minus_1);
      color_attachment_descriptor.info &= C_028C70_RAT;
      terakan_app_config_draw_set_cb_color_rtv(
         config, color_attachment_index, color_attachment_view->bo, &color_attachment_descriptor,
         &color_attachment_view->color_meta);

      if (color_attachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
         assert(clear_attachment_count < ARRAY_SIZE(clear_attachments));
         VkClearAttachment * const color_attachment_clear =
            &clear_attachments[clear_attachment_count++];
         color_attachment_clear->aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
         color_attachment_clear->colorAttachment = color_attachment_index;
         color_attachment_clear->clearValue = color_attachment->clearValue;
      }

      if (color_attachment->resolveMode != VK_RESOLVE_MODE_NONE &&
          color_attachment->resolveImageView != VK_NULL_HANDLE) {
         struct terakan_image_view const * const resolve_view =
            terakan_image_view_from_handle(color_attachment->resolveImageView);
         if (resolve_view != NULL) {
            struct terakan_rendering_color_resolve * const resolve =
               &command_buffer->rendering_color_resolves[color_attachment_index];
            resolve->src_image = terakan_image_to_handle(
               container_of(color_attachment_view->vk.image, struct terakan_image, vk));
            resolve->dst_image = terakan_image_to_handle(
               container_of(resolve_view->vk.image, struct terakan_image, vk));
            resolve->src_subresource = (VkImageSubresourceLayers){
               .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
               .mipLevel = color_attachment_view->vk.base_mip_level,
               .baseArrayLayer = color_attachment_view->vk.base_array_layer,
               .layerCount = layer_count_minus_1 + 1,
            };
            resolve->dst_subresource = (VkImageSubresourceLayers){
               .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
               .mipLevel = resolve_view->vk.base_mip_level,
               .baseArrayLayer = resolve_view->vk.base_array_layer,
               .layerCount = MIN2(layer_count_minus_1 + 1, resolve_view->vk.layer_count),
            };
         }
      }
   }

   /* Depth and stencil resolve, currently only VK_RESOLVE_MODE_SAMPLE_ZERO_BIT. Recorded here and
    * performed when rendering ends, the same way color resolves are. Both aspects share one
    * attachment and one resolve destination, so the requested aspects are collected into a single
    * entry whose aspect mask drives which draws run.
    */
   struct {
      VkRenderingAttachmentInfo const * attachment;
      struct terakan_image_view const * view;
      VkImageAspectFlagBits aspect;
   } const resolvable_aspects[] = {
      {pRenderingInfo->pDepthAttachment, depth_attachment_view, VK_IMAGE_ASPECT_DEPTH_BIT},
      {pRenderingInfo->pStencilAttachment, stencil_attachment_view, VK_IMAGE_ASPECT_STENCIL_BIT},
   };
   for (unsigned aspect_index = 0; aspect_index < ARRAY_SIZE(resolvable_aspects); ++aspect_index) {
      struct terakan_image_view const * const attachment_view = resolvable_aspects[aspect_index].view;
      VkRenderingAttachmentInfo const * const attachment =
         resolvable_aspects[aspect_index].attachment;
      if (attachment_view == NULL || attachment == NULL ||
          attachment->resolveMode != VK_RESOLVE_MODE_SAMPLE_ZERO_BIT ||
          attachment->resolveImageView == VK_NULL_HANDLE) {
         continue;
      }
      struct terakan_image_view const * const resolve_view =
         terakan_image_view_from_handle(attachment->resolveImageView);
      if (resolve_view == NULL) {
         continue;
      }
      struct terakan_rendering_color_resolve * const resolve =
         &command_buffer->rendering_depth_resolve;
      resolve->src_image = terakan_image_to_handle(
         container_of(attachment_view->vk.image, struct terakan_image, vk));
      resolve->dst_image = terakan_image_to_handle(
         container_of(resolve_view->vk.image, struct terakan_image, vk));
      resolve->src_subresource = (VkImageSubresourceLayers){
         .aspectMask = resolve->src_subresource.aspectMask |
                       resolvable_aspects[aspect_index].aspect,
         .mipLevel = attachment_view->vk.base_mip_level,
         .baseArrayLayer = attachment_view->vk.base_array_layer,
         .layerCount = layer_count_minus_1 + 1,
      };
      resolve->dst_subresource = (VkImageSubresourceLayers){
         .aspectMask = resolve->src_subresource.aspectMask,
         .mipLevel = resolve_view->vk.base_mip_level,
         .baseArrayLayer = resolve_view->vk.base_array_layer,
         .layerCount = MIN2(layer_count_minus_1 + 1, resolve_view->vk.layer_count),
      };
   }

   u_foreach_bit (color_attachment_index,
                  color_attachments_bound ^ BITFIELD_MASK(TERAKAN_VK_STATE_MAX_COLOR_ATTACHMENTS)) {
      terakan_app_config_draw_set_cb_color_rtv(config, color_attachment_index, NULL, NULL, NULL);
   }

   struct terakan_screen_rect const render_area = terakan_vk_rect_to_screen_rect(
      pRenderingInfo->renderArea,
      (struct terakan_screen_rect){
         .bounds = {[1] = {render_area_upper_bound[0], render_area_upper_bound[1]}}});
   command_buffer->rendering_resolve_area = (VkRect2D){
      .offset = {
         .x = render_area.bounds[0][0],
         .y = render_area.bounds[0][1],
      },
      .extent = {
         .width = render_area.bounds[1][0] - render_area.bounds[0][0],
         .height = render_area.bounds[1][1] - render_area.bounds[0][1],
      },
   };
   terakan_app_config_draw_set_pa_vport_render_area(config, render_area);

   if (!(pRenderingInfo->flags & VK_RENDERING_RESUMING_BIT) && clear_attachment_count != 0) {
      VkClearRect const clear_rect = {
         .rect = {.offset = {.x = render_area.bounds[0][0], .y = render_area.bounds[0][1]},
                  .extent = {.width = render_area.bounds[1][0] - render_area.bounds[0][0],
                             .height = render_area.bounds[1][1] - render_area.bounds[0][1]}},
         .baseArrayLayer = 0,
         .layerCount = layer_count_minus_1 + 1,
      };
      terakan_CmdClearAttachments(commandBuffer, clear_attachment_count, clear_attachments, 1,
                                  &clear_rect);
   }
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdEndRendering(VkCommandBuffer const commandBuffer)
{
   struct terakan_command_buffer * const command_buffer =
      terakan_command_buffer_from_handle(commandBuffer);
   if (terakan_render_debug_enabled())
      fprintf(stderr, "[TERAKAN_RENDER] end flags=0x%x resolve_area=%d,%d %ux%u\n",
              command_buffer->rendering_flags, command_buffer->rendering_resolve_area.offset.x,
              command_buffer->rendering_resolve_area.offset.y,
              command_buffer->rendering_resolve_area.extent.width,
              command_buffer->rendering_resolve_area.extent.height);
   if (!(command_buffer->rendering_flags & VK_RENDERING_SUSPENDING_BIT)) {
      for (unsigned color_attachment_index = 0;
           color_attachment_index < TERAKAN_VK_STATE_MAX_COLOR_ATTACHMENTS;
           ++color_attachment_index) {
         struct terakan_rendering_color_resolve const * const resolve =
            &command_buffer->rendering_color_resolves[color_attachment_index];
         if (resolve->src_image == VK_NULL_HANDLE || resolve->dst_image == VK_NULL_HANDLE) {
            continue;
         }
         VkImageResolve2 const region = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2,
            .srcSubresource = resolve->src_subresource,
            .srcOffset = {
               .x = command_buffer->rendering_resolve_area.offset.x,
               .y = command_buffer->rendering_resolve_area.offset.y,
            },
            .dstSubresource = resolve->dst_subresource,
            .dstOffset = {
               .x = command_buffer->rendering_resolve_area.offset.x,
               .y = command_buffer->rendering_resolve_area.offset.y,
            },
            .extent = {
               .width = command_buffer->rendering_resolve_area.extent.width,
               .height = command_buffer->rendering_resolve_area.extent.height,
               .depth = 1,
            },
         };
         VkResolveImageInfo2 const resolve_info = {
            .sType = VK_STRUCTURE_TYPE_RESOLVE_IMAGE_INFO_2,
            .srcImage = resolve->src_image,
            .srcImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .dstImage = resolve->dst_image,
            .dstImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .regionCount = 1,
            .pRegions = &region,
         };
         terakan_CmdResolveImage2(commandBuffer, &resolve_info);
      }

      struct terakan_rendering_color_resolve const * const depth_stencil_resolve =
         &command_buffer->rendering_depth_resolve;
      if (depth_stencil_resolve->src_image != VK_NULL_HANDLE &&
          depth_stencil_resolve->dst_image != VK_NULL_HANDLE) {
         /* Not routed through vkCmdResolveImage2, which the specification defines for color
          * images only.
          */
         terakan_meta_resolve_depth_stencil(
            command_buffer->command_writer.gfx,
            terakan_image_from_handle(depth_stencil_resolve->src_image),
            terakan_image_from_handle(depth_stencil_resolve->dst_image),
            &depth_stencil_resolve->src_subresource, &depth_stencil_resolve->dst_subresource,
            &command_buffer->rendering_resolve_area,
            depth_stencil_resolve->src_subresource.aspectMask);
      }
   }

   struct terakan_app_config_draw * const config =
      &command_buffer->command_writer.gfx->app_config_draw;
   terakan_app_config_draw_set_pa_vport_render_area(config, (struct terakan_screen_rect){});
   terakan_app_config_draw_set_db_depth_stencil_buffer(config, NULL, NULL);
   for (unsigned color_attachment_index = 0;
        color_attachment_index < TERAKAN_VK_STATE_MAX_COLOR_ATTACHMENTS; ++color_attachment_index) {
      terakan_app_config_draw_set_cb_color_rtv(config, color_attachment_index, NULL, NULL, NULL);
   }
}
