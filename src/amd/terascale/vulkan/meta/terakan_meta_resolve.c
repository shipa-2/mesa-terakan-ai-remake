/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_meta_impl.h"

#include "terakan_barrier.h"
#include "terakan_entrypoints.h"
#include "terakan_image.h"

#include "util/macros.h"
#include "util/u_math.h"

static bool
terakan_meta_resolve_region_is_full(struct terakan_image const * const src_image,
                                    struct terakan_image const * const dst_image,
                                    VkImageResolve2 const * const region)
{
   VkExtent3D const src_extent = {
      .width = u_minify(src_image->vk.extent.width, region->srcSubresource.mipLevel),
      .height = u_minify(src_image->vk.extent.height, region->srcSubresource.mipLevel),
      .depth = 1,
   };
   VkExtent3D const dst_extent = {
      .width = u_minify(dst_image->vk.extent.width, region->dstSubresource.mipLevel),
      .height = u_minify(dst_image->vk.extent.height, region->dstSubresource.mipLevel),
      .depth = 1,
   };

   return region->srcOffset.x == 0 && region->srcOffset.y == 0 && region->srcOffset.z == 0 &&
          region->dstOffset.x == 0 && region->dstOffset.y == 0 && region->dstOffset.z == 0 &&
          region->extent.width == src_extent.width &&
          region->extent.height == src_extent.height && region->extent.depth == 1 &&
          region->extent.width == dst_extent.width &&
          region->extent.height == dst_extent.height;
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdResolveImage2(VkCommandBuffer const command_buffer_handle,
                         VkResolveImageInfo2 const * const resolve_info)
{
   struct terakan_image const * const src_image =
      terakan_image_from_handle(resolve_info->srcImage);
   struct terakan_image const * const dst_image =
      terakan_image_from_handle(resolve_info->dstImage);
   if (unlikely(src_image == NULL || dst_image == NULL ||
                src_image->vk.samples <= VK_SAMPLE_COUNT_1_BIT ||
                dst_image->vk.samples != VK_SAMPLE_COUNT_1_BIT)) {
      return;
   }
   unsigned const src_aspect_index =
      terakan_format_aspect_index(src_image->format_info.aspect_map,
                                  VK_IMAGE_ASPECT_COLOR_BIT, 0);
   unsigned const dst_aspect_index =
      terakan_format_aspect_index(dst_image->format_info.aspect_map,
                                  VK_IMAGE_ASPECT_COLOR_BIT, 0);
   struct terascale_format_info const src_format =
      src_image->format_info.aspect_formats[src_aspect_index];
   struct terascale_format_info const dst_format =
      dst_image->format_info.aspect_formats[dst_aspect_index];
   /* CB_RESOLVE averages samples. Vulkan integer resolves select one sample instead. */
   if (unlikely(src_format.number_type == TERASCALE_FORMAT_NUMBER_TYPE_UINT ||
                src_format.number_type == TERASCALE_FORMAT_NUMBER_TYPE_SINT ||
                dst_format.number_type == TERASCALE_FORMAT_NUMBER_TYPE_UINT ||
                dst_format.number_type == TERASCALE_FORMAT_NUMBER_TYPE_SINT)) {
      return;
   }

   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(command_buffer_handle)->command_writer.gfx;

   terakan_barrier_emit_actions_unconditionally(
      command_writer, TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
                         TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS);

   struct terakan_meta_config_draw_begin_options const begin_options = {
      .vgt_primitive_type = V_008958_DI_PT_RECTLIST,
      .cb_and_db_shader_control_mode = TERAKAN_META_CONFIG_DRAW_BEGIN_CB_MODE_DYNAMIC,
      .rasterization =
         {
            .enable = true,
            .msaa_num_samples_log2 = util_logbase2((uint32_t)src_image->vk.samples),
            .msaa_num_anchor_samples_log2 = util_logbase2((uint32_t)src_image->vk.samples),
         },
   };
   terakan_meta_config_draw_begin(command_writer, &begin_options);
   terakan_meta_config_draw_set_sq_pgm_vs(command_writer,
                                          TERAKAN_META_SHADER_POSITION_AND_LAYER_FROM_INDEX_VS);
   terakan_meta_config_draw_set_sq_pgm_ps(command_writer, TERAKAN_META_SHADER_DUMMY_OPAQUE_PS);
   terakan_meta_config_draw_set_cb_color_control_for_mode(command_writer, V_028808_CB_RESOLVE);

   command_writer->post_color_image_copy_write_barrier_actions |=
      TERAKAN_BARRIER_ACTION_FLUSH_INV_CB_RTV_DATA |
      TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS;

   for (uint32_t region_index = 0; region_index < resolve_info->regionCount; ++region_index) {
      VkImageResolve2 const * const region = &resolve_info->pRegions[region_index];

      /* Evergreen's fixed-function CB resolve requires matching full-size source and destination
       * surfaces. Partial and scaled regions need a shader fallback.
       */
      if (unlikely(region->srcSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
                   region->dstSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
                   !terakan_meta_resolve_region_is_full(src_image, dst_image, region))) {
         continue;
      }

      uint32_t const layer_count =
         MIN2(region->srcSubresource.layerCount, region->dstSubresource.layerCount);
      for (uint32_t layer_index = 0; layer_index < layer_count; ++layer_index) {
         struct terakan_image_descriptor_create_info descriptor_info[2] = {
            {
               .image = src_image,
               .view_format = src_format,
               .image_aspect_index = src_aspect_index,
               .subresource_range =
                  {
                     .base_mip_level = region->srcSubresource.mipLevel,
                     .max_level_count = 1,
                     .base_z_or_array_layer =
                        region->srcSubresource.baseArrayLayer + layer_index,
                     .max_depth_or_layer_count = 1,
                  },
            },
            {
               .image = dst_image,
               .view_format = dst_format,
               .image_aspect_index = dst_aspect_index,
               .subresource_range =
                  {
                     .base_mip_level = region->dstSubresource.mipLevel,
                     .max_level_count = 1,
                     .base_z_or_array_layer =
                        region->dstSubresource.baseArrayLayer + layer_index,
                     .max_depth_or_layer_count = 1,
                  },
            },
         };

         if (unlikely(!terakan_image_descriptor_subresource_range_sanitize(
                         src_image, &descriptor_info[0].subresource_range, false) ||
                      !terakan_image_descriptor_subresource_range_sanitize(
                         dst_image, &descriptor_info[1].subresource_range, false))) {
            continue;
         }

         struct terakan_color_descriptor color[2];
         struct terakan_color_meta_descriptor meta[2];
         if (unlikely(terakan_image_create_color_descriptor(
                         &descriptor_info[0], V_028C70_TEXTURE2DARRAY, &color[0], &meta[0]) != 1 ||
                      terakan_image_create_color_descriptor(
                         &descriptor_info[1], V_028C70_TEXTURE2DARRAY, &color[1], &meta[1]) != 1)) {
            continue;
         }

         struct terakan_bo const * const bos[2] = {src_image->bo, dst_image->bo};
         terakan_meta_config_draw_set_cb_rtvs_and_db_shader_control(
            command_writer, 0xFF, bos, color, meta, TERAKAN_SHADER_DB_SHADER_CONTROL_IDENTITY);
         /* CB_RESOLVE consumes RTV 0 as the source and routes the resolved value to RTV 1, while
          * only color export 0 is enabled.
          */
         terakan_hw_config_draw_set_cb_target_mask(&command_writer->hw_config_draw, 0xF);

         struct terakan_screen_rect const screen_bounds = {
            .bounds = {
               [1] = {
                  G_028C78_WIDTH_MAX(color[0].dim) + 1,
                  G_028C78_HEIGHT_MAX(color[0].dim) + 1,
               },
            },
         };
         VkRect2D const rect = {
            .extent = {
               .width = region->extent.width,
               .height = region->extent.height,
            },
         };
         terakan_meta_draw_rect(command_writer,
                                terakan_vk_rect_to_screen_rect(rect, screen_bounds), 1);
      }
   }
}
