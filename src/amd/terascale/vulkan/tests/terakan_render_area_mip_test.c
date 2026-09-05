/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* The render area of a render pass with a depth attachment at a mip level past the first.
 *
 * vk_image_view::extent is already the extent of the view's own mip level -- the common runtime
 * sets it from vk_image_mip_level_extent(image, base_mip_level) -- and the render pass minified it
 * again by that same level when clamping the render area to the depth attachment. So a render pass
 * targeting mip N was scissored to a 2^N-th of the level and everything outside that corner was
 * never drawn. Only the depth path did this; the colour one takes its bound from the descriptor's
 * DIM, which is built from the level, and was right.
 *
 * dEQP-VK.pipeline.monolithic.render_to_image failed 422 of its 1245 supported cases on this, and
 * every one of them was a `mipmap` case with a depth or stencil attachment: the colour-only mipmap
 * cases passed, and so did every single-level `small` and `huge` case.
 *
 * No shaders are needed to see it. The load-op clear is itself scissored by the render area, so a
 * render pass that only clears its colour attachment at a mip level already exposes the whole
 * bound: with the bug only a corner of the level is cleared and the rest keeps the value it was
 * filled with beforehand.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "terakan_test_device.h"


#define IMAGE_SIZE 64u
#define MIP_LEVELS 4u
/* Level 2 is 16x16, and the bug would clamp the render area to 16 >> 2 = 4. */
#define TEST_LEVEL 2u
#define LEVEL_SIZE (IMAGE_SIZE >> TEST_LEVEL)
#define LEVEL_TEXELS (LEVEL_SIZE * LEVEL_SIZE)

#define PREFILL_VALUE 0x11u
#define CLEAR_VALUE 0xCCu

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

   float const queue_priority = 1.0f;
   VkDeviceQueueCreateInfo const queue_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueCount = 1,
      .pQueuePriorities = &queue_priority,
   };
   char const * const device_extensions[1] = {VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME};
   VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
      .dynamicRendering = VK_TRUE,
   };
   VkDeviceCreateInfo const device_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &dynamic_rendering,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_create_info,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = device_extensions,
   };
   VkDevice device;
   CHECK_VK(vkCreateDevice(physical_device, &device_create_info, NULL, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, 0, 0, &queue);

   PFN_vkCmdBeginRenderingKHR const cmd_begin_rendering =
      (PFN_vkCmdBeginRenderingKHR)vkGetDeviceProcAddr(device, "vkCmdBeginRenderingKHR");
   PFN_vkCmdEndRenderingKHR const cmd_end_rendering =
      (PFN_vkCmdEndRenderingKHR)vkGetDeviceProcAddr(device, "vkCmdEndRenderingKHR");
   if (cmd_begin_rendering == NULL || cmd_end_rendering == NULL) {
      fprintf(stderr, "Dynamic rendering unavailable\n");
      return 77;
   }

   /* A colour image and a depth image with the same mip chain, as the tests that found this use. */
   VkFormat const formats[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D16_UNORM};
   VkImageUsageFlags const usages[2] = {
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
         VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
   };
   VkImage images[2];
   VkDeviceMemory image_memories[2];
   VkImageView views[2];
   for (int i = 0; i < 2; ++i) {
      VkImageCreateInfo const image_create_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = formats[i],
         .extent = {IMAGE_SIZE, IMAGE_SIZE, 1},
         .mipLevels = MIP_LEVELS,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = usages[i],
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      CHECK_VK(vkCreateImage(device, &image_create_info, NULL, &images[i]));
      VkMemoryRequirements memory_requirements;
      vkGetImageMemoryRequirements(device, images[i], &memory_requirements);
      VkMemoryAllocateInfo const allocate_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = memory_requirements.size,
         .memoryTypeIndex = find_memory_type(physical_device, memory_requirements.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
      };
      CHECK_VK(vkAllocateMemory(device, &allocate_info, NULL, &image_memories[i]));
      CHECK_VK(vkBindImageMemory(device, images[i], image_memories[i], 0));
      VkImageViewCreateInfo const view_create_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = images[i],
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = formats[i],
         .subresourceRange = {i == 0 ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT,
                              TEST_LEVEL, 1, 0, 1},
      };
      CHECK_VK(vkCreateImageView(device, &view_create_info, NULL, &views[i]));
   }

   VkDeviceSize const readback_size = (VkDeviceSize)LEVEL_TEXELS * 4;
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
   memset(readback_map, 0xEE, readback_size);

   VkCommandPoolCreateInfo const command_pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
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
   VkCommandBufferBeginInfo const begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   CHECK_VK(vkBeginCommandBuffer(command_buffer, &begin_info));

   for (int i = 0; i < 2; ++i) {
      VkImageMemoryBarrier const to_general = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .image = images[i],
         .subresourceRange = {i == 0 ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, 0,
                              MIP_LEVELS, 0, 1},
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_general);
   }
   /* Filling the level first is what makes an unrendered part of it visible: without this a
    * partially cleared level and a fully cleared one could read the same.
    */
   VkClearColorValue const prefill = {
      .float32 = {PREFILL_VALUE / 255.0f, PREFILL_VALUE / 255.0f, PREFILL_VALUE / 255.0f,
                  PREFILL_VALUE / 255.0f}};
   VkImageSubresourceRange const colour_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, MIP_LEVELS, 0, 1};
   vkCmdClearColorImage(command_buffer, images[0], VK_IMAGE_LAYOUT_GENERAL, &prefill, 1,
                        &colour_range);
   VkMemoryBarrier const to_render = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &to_render, 0, NULL, 0, NULL);

   /* The render pass clears its colour attachment and draws nothing. The clear is scissored by the
    * render area, so the whole level must come out cleared.
    */
   VkRenderingAttachmentInfo const colour_attachment = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = views[0],
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = {.color = {.float32 = {CLEAR_VALUE / 255.0f, CLEAR_VALUE / 255.0f,
                                           CLEAR_VALUE / 255.0f, CLEAR_VALUE / 255.0f}}},
   };
   VkRenderingAttachmentInfo const depth_attachment = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = views[1],
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = {.depthStencil = {.depth = 1.0f, .stencil = 0}},
   };
   VkRenderingInfo const rendering_info = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {.offset = {0, 0}, .extent = {LEVEL_SIZE, LEVEL_SIZE}},
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &colour_attachment,
      .pDepthAttachment = &depth_attachment,
   };
   cmd_begin_rendering(command_buffer, &rendering_info);
   cmd_end_rendering(command_buffer);

   VkMemoryBarrier const to_transfer = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &to_transfer, 0, NULL, 0, NULL);
   VkBufferImageCopy const download = {
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, TEST_LEVEL, 0, 1},
      .imageExtent = {LEVEL_SIZE, LEVEL_SIZE, 1},
   };
   vkCmdCopyImageToBuffer(command_buffer, images[0], VK_IMAGE_LAYOUT_GENERAL, readback, 1,
                          &download);
   VkMemoryBarrier const to_host = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, NULL, 0, NULL);
   CHECK_VK(vkEndCommandBuffer(command_buffer));

   VkFenceCreateInfo const fence_create_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   CHECK_VK(vkCreateFence(device, &fence_create_info, NULL, &fence));
   VkSubmitInfo const submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
   };
   CHECK_VK(vkQueueSubmit(queue, 1, &submit_info, fence));
   CHECK_VK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));

   unsigned reported = 0;
   bool failed = false;
   for (unsigned y = 0; y < LEVEL_SIZE; ++y) {
      for (unsigned x = 0; x < LEVEL_SIZE; ++x) {
         uint8_t const got = readback_map[(y * LEVEL_SIZE + x) * 4];
         if (got != CLEAR_VALUE) {
            if (reported++ < 4) {
               fprintf(stderr, "texel %u,%u of mip %u expected 0x%X, got 0x%X\n", x, y, TEST_LEVEL,
                       CLEAR_VALUE, got);
            }
            failed = true;
         }
      }
   }

   vkDeviceWaitIdle(device);
   if (failed) {
      return 1;
   }
   printf("A render pass with a depth attachment covers the whole mip level\n");
   return 0;
}
