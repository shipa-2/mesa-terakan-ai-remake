/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* vkCmdClearDepthStencilImage with one range per aspect.
 *
 * Clearing a combined depth/stencil image with two ranges over the same subresource -- one naming
 * only stencil, one only depth -- left the depth right and the stencil zero. The two draws
 * interfere: reversing the order in which the ranges were processed made both come out right, so
 * the second draw was destroying what the first wrote.
 *
 * The mechanism was not established. A DB data flush between the draws changes nothing, so it is
 * not a stale depth cache; setting STENCILWRITEMASK to zero for the depth-only range changes
 * nothing; binding only the aspect a range clears changes nothing; and the depth draw's stencil ops
 * are KEEP with STENCIL_ENABLE clear, so on paper it cannot touch stencil at all. What does work is
 * not issuing the second draw: the clear now visits each subresource once with the union of the
 * aspects every range asks of it, which is what the two draws were meant to add up to.
 *
 * dEQP-VK.api.image_clearing.*.clear_depth_stencil_image went from 297 passing / 153 failing to
 * 350 / 100 on that change.
 *
 * All three orderings are checked here: stencil range first, depth range first, and one range
 * naming both aspects.
 *
 * This test does not fail against the unfixed driver, and that is worth stating rather than hiding.
 * The interference could not be reproduced outside CTS: not at 32x32 or 256x256, not with
 * D32_SFLOAT_S8_UINT or D16_UNORM_S8_UINT, not with the image filled by an earlier clear first, and
 * not with sampled usage added. So this is coverage that locks the three orderings in place, and
 * the CTS group is what carries the evidence for the change.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "terakan_test_device.h"


#define IMAGE_WIDTH 256u
#define IMAGE_HEIGHT 256u
#define TEXELS (IMAGE_WIDTH * IMAGE_HEIGHT)

#define CLEAR_DEPTH 0.25f
#define CLEAR_STENCIL 0x5Au

#define CHECK_VK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_vk_result = (expression);                                               \
      if (check_vk_result != VK_SUCCESS) {                                                         \
         fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_vk_result);            \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

static uint32_t
find_memory_type(VkPhysicalDevice const physical_device, uint32_t const memory_type_bits,
                 VkMemoryPropertyFlags const properties)
{
   VkPhysicalDeviceMemoryProperties memory_properties;
   vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
   for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
      if ((memory_type_bits & (1u << i)) != 0 &&
          (memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
         return i;
      }
   }
   return UINT32_MAX;
}

int
main(void)
{
   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo const instance_create_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application_info,
   };
   VkInstance instance;
   CHECK_VK(vkCreateInstance(&instance_create_info, NULL, &instance));

   uint32_t physical_device_count = 8;
   VkPhysicalDevice physical_devices[8];
   CHECK_VK(vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices));
   VkPhysicalDevice physical_device = VK_NULL_HANDLE;
   for (uint32_t i = 0; i < physical_device_count; ++i) {
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(physical_devices[i], &properties);
      if (terakan_test_device_matches(properties.deviceName)) {
         physical_device = physical_devices[i];
         break;
      }
   }
   if (physical_device == VK_NULL_HANDLE) {
      fprintf(stderr, "No Terakan device found\n");
      return 77;
   }

   VkFormatProperties format_properties;
   vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_D16_UNORM_S8_UINT,
                                       &format_properties);
   VkFormatFeatureFlags const needed = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
   if ((format_properties.optimalTilingFeatures & needed) != needed) {
      fprintf(stderr, "D16_UNORM_S8_UINT unsupported as a readable attachment\n");
      return 77;
   }

   float const queue_priority = 1.0f;
   VkDeviceQueueCreateInfo const queue_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueCount = 1,
      .pQueuePriorities = &queue_priority,
   };
   VkDeviceCreateInfo const device_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_create_info,
   };
   VkDevice device;
   CHECK_VK(vkCreateDevice(physical_device, &device_create_info, NULL, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, 0, 0, &queue);

   VkImageCreateInfo const image_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_D16_UNORM_S8_UINT,
      .extent = {IMAGE_WIDTH, IMAGE_HEIGHT, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage image;
   CHECK_VK(vkCreateImage(device, &image_create_info, NULL, &image));
   VkMemoryRequirements image_requirements;
   vkGetImageMemoryRequirements(device, image, &image_requirements);
   VkMemoryAllocateInfo const image_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = image_requirements.size,
      .memoryTypeIndex = find_memory_type(physical_device, image_requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
   };
   VkDeviceMemory image_memory;
   CHECK_VK(vkAllocateMemory(device, &image_allocate_info, NULL, &image_memory));
   CHECK_VK(vkBindImageMemory(device, image, image_memory, 0));

   VkDeviceSize const readback_size = (VkDeviceSize)TEXELS * (sizeof(uint16_t) + 1);
   VkBufferCreateInfo const buffer_create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = readback_size,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
   };
   VkBuffer readback;
   CHECK_VK(vkCreateBuffer(device, &buffer_create_info, NULL, &readback));
   VkMemoryRequirements buffer_requirements;
   vkGetBufferMemoryRequirements(device, readback, &buffer_requirements);
   VkMemoryAllocateInfo const buffer_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = buffer_requirements.size,
      .memoryTypeIndex = find_memory_type(physical_device, buffer_requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   VkDeviceMemory readback_memory;
   CHECK_VK(vkAllocateMemory(device, &buffer_allocate_info, NULL, &readback_memory));
   CHECK_VK(vkBindBufferMemory(device, readback, readback_memory, 0));
   uint8_t * readback_map;
   CHECK_VK(vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0, (void **)&readback_map));

   VkCommandPoolCreateInfo const command_pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
   };
   VkCommandPool command_pool;
   CHECK_VK(vkCreateCommandPool(device, &command_pool_create_info, NULL, &command_pool));
   VkCommandBufferAllocateInfo const command_buffer_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = command_pool,
      .commandBufferCount = 1,
   };
   VkCommandBuffer command_buffer;
   CHECK_VK(vkAllocateCommandBuffers(device, &command_buffer_allocate_info, &command_buffer));
   VkFenceCreateInfo const fence_create_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   CHECK_VK(vkCreateFence(device, &fence_create_info, NULL, &fence));

   static char const * const case_names[3] = {"stencil range first", "depth range first",
                                              "one range, both aspects"};
   bool failed = false;
   for (int case_index = 0; case_index < 3; ++case_index) {
      VkImageSubresourceRange ranges[2];
      uint32_t range_count;
      if (case_index == 2) {
         ranges[0] = (VkImageSubresourceRange){
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
         range_count = 1;
      } else {
         VkImageAspectFlags const first = case_index == 0 ? VK_IMAGE_ASPECT_STENCIL_BIT
                                                          : VK_IMAGE_ASPECT_DEPTH_BIT;
         VkImageAspectFlags const second = case_index == 0 ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                           : VK_IMAGE_ASPECT_STENCIL_BIT;
         ranges[0] = (VkImageSubresourceRange){first, 0, 1, 0, 1};
         ranges[1] = (VkImageSubresourceRange){second, 0, 1, 0, 1};
         range_count = 2;
      }

      memset(readback_map, 0xEE, readback_size);

      VkCommandBufferBeginInfo const begin_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      };
      CHECK_VK(vkResetCommandBuffer(command_buffer, 0));
      CHECK_VK(vkBeginCommandBuffer(command_buffer, &begin_info));
      VkImageMemoryBarrier const to_general = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .image = image,
         .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1},
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &to_general);

      /* The image is filled with something else first, as the tests that found this do: a clear
       * that never runs is indistinguishable from one that runs on already-correct memory.
       */
      VkClearDepthStencilValue const initial_value = {.depth = 1.0f, .stencil = 0xFF};
      VkImageSubresourceRange const whole = {
         VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
      vkCmdClearDepthStencilImage(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL, &initial_value, 1,
                                  &whole);
      VkMemoryBarrier const between_clears = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &between_clears, 0, NULL, 0,
                           NULL);

      VkClearDepthStencilValue const clear_value = {.depth = CLEAR_DEPTH,
                                                    .stencil = CLEAR_STENCIL};
      vkCmdClearDepthStencilImage(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL, &clear_value,
                                  range_count, ranges);

      VkMemoryBarrier const to_transfer = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &to_transfer, 0, NULL, 0, NULL);
      VkBufferImageCopy const copies[2] = {
         {
            .bufferOffset = 0,
            .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
            .imageExtent = {IMAGE_WIDTH, IMAGE_HEIGHT, 1},
         },
         {
            .bufferOffset = (VkDeviceSize)TEXELS * sizeof(uint16_t),
            .imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1},
            .imageExtent = {IMAGE_WIDTH, IMAGE_HEIGHT, 1},
         },
      };
      vkCmdCopyImageToBuffer(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL, readback, 2, copies);
      VkMemoryBarrier const to_host = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, NULL, 0, NULL);
      CHECK_VK(vkEndCommandBuffer(command_buffer));

      CHECK_VK(vkResetFences(device, 1, &fence));
      VkSubmitInfo const submit_info = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &command_buffer,
      };
      CHECK_VK(vkQueueSubmit(queue, 1, &submit_info, fence));
      CHECK_VK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));

      unsigned depth_reported = 0;
      unsigned stencil_reported = 0;
      for (unsigned texel = 0; texel < TEXELS; ++texel) {
         uint16_t depth;
         memcpy(&depth, readback_map + texel * sizeof(uint16_t), sizeof(depth));
         /* D16_UNORM quantizes the clear value, so the comparison allows the one unit of rounding
          * that leaves and no more.
          */
         int const depth_expected = (int)(CLEAR_DEPTH * 65535.0f + 0.5f);
         if (depth < depth_expected - 1 || depth > depth_expected + 1) {
            if (depth_reported++ < 2) {
               fprintf(stderr, "%s: texel %u depth expected %d, got %d\n", case_names[case_index],
                       texel, depth_expected, (int)depth);
            }
            failed = true;
         }
         uint8_t const stencil = readback_map[TEXELS * sizeof(uint16_t) + texel];
         if (stencil != CLEAR_STENCIL) {
            if (stencil_reported++ < 2) {
               fprintf(stderr, "%s: texel %u stencil expected 0x%X, got 0x%X\n",
                       case_names[case_index], texel, CLEAR_STENCIL, stencil);
            }
            failed = true;
         }
      }
   }

   vkDeviceWaitIdle(device);
   if (failed) {
      return 1;
   }
   printf("Depth/stencil clears honour every range whatever order the aspects come in\n");
   return 0;
}
