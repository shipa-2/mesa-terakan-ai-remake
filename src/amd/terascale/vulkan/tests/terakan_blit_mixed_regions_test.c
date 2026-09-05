/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* A vkCmdBlitImage whose regions are a mix of the two kinds the driver handles differently.
 *
 * A blit region whose source and destination are the same size, between identical formats, is not a
 * blit at all -- terakan_meta_blit_image.c turns those into copies and issues them together as one
 * vkCmdCopyImage, leaving only the genuinely scaled regions to the meta draw. That split is where
 * the bug was: the loop over the scaled regions called the same conversion helper again just to ask
 * whether a region was one of the already-converted ones, and handed it `copies[0]` as the place to
 * write its answer. The first accumulated copy was therefore overwritten by the last convertible
 * region's parameters, so one region of the blit silently became a duplicate of another and its own
 * destination was never written.
 *
 * It only bites when a single blit mixes both kinds and the first convertible region is not the last
 * one, which is why nothing caught it: the driver's own blit tests use uniform region sets, and CTS
 * only trips it in the 3D groups of dEQP-VK.api.copy_and_blit.core.blit_image.all_formats.
 *
 * The three regions here are that shape at its smallest. Region 0 and region 2 are same-size and
 * become copies; region 1 is a halving blit and stays a draw. Every source texel carries its own
 * coordinate, so a destination area that received another region's source is recognizable rather
 * than merely wrong.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "terakan_test_device.h"


#define IMAGE_SIZE 16u
#define IMAGE_TEXELS (IMAGE_SIZE * IMAGE_SIZE)

/* Nothing writes here, so a destination area left at the sentinel says the region was dropped. */
#define SENTINEL 0xEEEEEEEEu

#define CHECK_VK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_vk_result = (expression);                                               \
      if (check_vk_result != VK_SUCCESS) {                                                         \
         fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_vk_result);            \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

/* R8G8B8A8_UINT, so the comparison is exact and each texel names its own source coordinate. */
static uint32_t
source_texel(uint32_t const x, uint32_t const y)
{
   return 0x40u | (x << 8) | (y << 16) | (0xABu << 24);
}

int
main(void)
{
   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-blit-mixed-regions-test",
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo const instance_create_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application_info,
   };
   VkInstance instance;
   CHECK_VK(vkCreateInstance(&instance_create_info, NULL, &instance));

   uint32_t physical_device_count = 0;
   CHECK_VK(vkEnumeratePhysicalDevices(instance, &physical_device_count, NULL));
   VkPhysicalDevice physical_devices[8];
   if (physical_device_count > 8)
      physical_device_count = 8;
   CHECK_VK(vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices));
   VkPhysicalDevice physical_device = VK_NULL_HANDLE;
   uint32_t queue_family = UINT32_MAX;
   VkPhysicalDeviceProperties properties;
   memset(&properties, 0, sizeof(properties));
   for (uint32_t device_index = 0; device_index < physical_device_count; ++device_index) {
      vkGetPhysicalDeviceProperties(physical_devices[device_index], &properties);
      if (!terakan_test_device_matches(properties.deviceName))
         continue;
      uint32_t family_count = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(physical_devices[device_index], &family_count, NULL);
      VkQueueFamilyProperties families[8];
      if (family_count > 8)
         family_count = 8;
      vkGetPhysicalDeviceQueueFamilyProperties(physical_devices[device_index], &family_count,
                                               families);
      for (uint32_t family_index = 0; family_index < family_count; ++family_index) {
         if (families[family_index].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            physical_device = physical_devices[device_index];
            queue_family = family_index;
            break;
         }
      }
      if (physical_device != VK_NULL_HANDLE)
         break;
   }
   if (physical_device == VK_NULL_HANDLE) {
      fprintf(stderr, "Terakan graphics device not found\n");
      return 1;
   }
   fprintf(stderr, "device=%s queue_family=%u\n", properties.deviceName, queue_family);

   float const queue_priority = 1.0f;
   VkDeviceQueueCreateInfo const queue_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = queue_family,
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
   vkGetDeviceQueue(device, queue_family, 0, &queue);

   VkPhysicalDeviceMemoryProperties memory_properties;
   vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
   uint32_t device_local_type = UINT32_MAX, host_type = UINT32_MAX;
   for (uint32_t type_index = 0; type_index < memory_properties.memoryTypeCount; ++type_index) {
      VkMemoryPropertyFlags const flags = memory_properties.memoryTypes[type_index].propertyFlags;
      if (device_local_type == UINT32_MAX && (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
         device_local_type = type_index;
      if (host_type == UINT32_MAX && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
          (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
         host_type = type_index;
   }

   VkImage images[2];
   VkDeviceMemory image_memories[2];
   for (int i = 0; i < 2; ++i) {
      VkImageCreateInfo const image_create_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_R8G8B8A8_UINT,
         .extent = {IMAGE_SIZE, IMAGE_SIZE, 1},
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      CHECK_VK(vkCreateImage(device, &image_create_info, NULL, &images[i]));
      VkMemoryRequirements requirements;
      vkGetImageMemoryRequirements(device, images[i], &requirements);
      VkMemoryAllocateInfo const allocate_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = requirements.size,
         .memoryTypeIndex = device_local_type,
      };
      CHECK_VK(vkAllocateMemory(device, &allocate_info, NULL, &image_memories[i]));
      CHECK_VK(vkBindImageMemory(device, images[i], image_memories[i], 0));
   }

   VkBuffer staging;
   VkDeviceMemory staging_memory;
   uint32_t * staging_mapping;
   {
      VkBufferCreateInfo const buffer_create_info = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = IMAGE_TEXELS * 4u,
         .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      CHECK_VK(vkCreateBuffer(device, &buffer_create_info, NULL, &staging));
      VkMemoryRequirements requirements;
      vkGetBufferMemoryRequirements(device, staging, &requirements);
      VkMemoryAllocateInfo const allocate_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = requirements.size,
         .memoryTypeIndex = host_type,
      };
      CHECK_VK(vkAllocateMemory(device, &allocate_info, NULL, &staging_memory));
      CHECK_VK(vkBindBufferMemory(device, staging, staging_memory, 0));
      CHECK_VK(
         vkMapMemory(device, staging_memory, 0, VK_WHOLE_SIZE, 0, (void **)&staging_mapping));
   }

   VkCommandPoolCreateInfo const command_pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = queue_family,
   };
   VkCommandPool command_pool;
   CHECK_VK(vkCreateCommandPool(device, &command_pool_create_info, NULL, &command_pool));
   VkCommandBufferAllocateInfo const command_buffer_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer command_buffer;
   CHECK_VK(vkAllocateCommandBuffers(device, &command_buffer_allocate_info, &command_buffer));
   VkCommandBufferBeginInfo const begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   CHECK_VK(vkBeginCommandBuffer(command_buffer, &begin_info));

   VkImageSubresourceRange const whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   VkImageMemoryBarrier to_transfer_dst[2];
   for (int i = 0; i < 2; ++i) {
      to_transfer_dst[i] = (VkImageMemoryBarrier){
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = images[i],
         .subresourceRange = whole,
      };
   }
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, to_transfer_dst);

   /* Fill the source with per-texel coordinates and the destination with the sentinel. */
   for (uint32_t y = 0; y < IMAGE_SIZE; ++y) {
      for (uint32_t x = 0; x < IMAGE_SIZE; ++x)
         staging_mapping[y * IMAGE_SIZE + x] = source_texel(x, y);
   }
   VkBufferImageCopy const whole_region = {
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {IMAGE_SIZE, IMAGE_SIZE, 1},
   };
   vkCmdCopyBufferToImage(command_buffer, staging, images[0],
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &whole_region);
   VkClearColorValue const sentinel = {
      .uint32 = {SENTINEL & 0xFF, (SENTINEL >> 8) & 0xFF, (SENTINEL >> 16) & 0xFF,
                 (SENTINEL >> 24) & 0xFF},
   };
   vkCmdClearColorImage(command_buffer, images[1], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &sentinel,
                        1, &whole);

   VkImageMemoryBarrier const to_blit[2] = {
      {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
       .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
       .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
       .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
       .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
       .image = images[0],
       .subresourceRange = whole},
      {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
       .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
       .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
       .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
       .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
       .image = images[1],
       .subresourceRange = whole},
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, to_blit);

   VkImageSubresourceLayers const layer = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
   /* Region 0 and region 2 are same-size and become copies; region 1 halves and stays a draw. The
    * order matters: the bug replaced region 0's copy with region 2's.
    */
   VkImageBlit const regions[3] = {
      {layer, {{0, 0, 0}, {4, 4, 1}}, layer, {{0, 8, 0}, {4, 12, 1}}},
      {layer, {{0, 0, 0}, {4, 4, 1}}, layer, {{8, 0, 0}, {10, 2, 1}}},
      {layer, {{8, 8, 0}, {12, 12, 1}}, layer, {{8, 8, 0}, {12, 12, 1}}},
   };
   vkCmdBlitImage(command_buffer, images[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[1],
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 3, regions, VK_FILTER_NEAREST);

   VkImageMemoryBarrier const to_read = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = images[1],
      .subresourceRange = whole,
   };
   vkCmdPipelineBarrier(command_buffer,
                        VK_PIPELINE_STAGE_TRANSFER_BIT |
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_read);
   vkCmdCopyImageToBuffer(command_buffer, images[1], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging,
                          1, &whole_region);
   VkMemoryBarrier const host_ready = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                        0, 1, &host_ready, 0, NULL, 0, NULL);
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
   VkResult const wait_result = vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull);
   if (wait_result != VK_SUCCESS) {
      fprintf(stderr, "Fence wait failed with VkResult %d\n", wait_result);
      return 1;
   }

   unsigned failures = 0;
   /* Region 0: destination (0,8)-(4,12) must hold source (0,0)-(4,4). */
   unsigned region0_bad = 0, region0_sentinel = 0, region0_from_region2 = 0;
   for (uint32_t dy = 8; dy < 12; ++dy) {
      for (uint32_t dx = 0; dx < 4; ++dx) {
         uint32_t const actual = staging_mapping[dy * IMAGE_SIZE + dx];
         uint32_t const expected = source_texel(dx, dy - 8);
         if (actual == expected)
            continue;
         if (actual == SENTINEL)
            ++region0_sentinel;
         if (actual == source_texel(dx + 8, dy))
            ++region0_from_region2;
         ++region0_bad;
      }
   }
   if (region0_bad != 0) {
      fprintf(stderr, "region 0: %u of 16 destination texels wrong%s%s\n", region0_bad,
              region0_sentinel == region0_bad ? " (all still the sentinel: the region was dropped)"
                                              : "",
              region0_from_region2 == region0_bad ? " (they hold region 2's source)" : "");
      ++failures;
   }
   /* Region 2: destination (8,8)-(12,12) must hold source (8,8)-(12,12). */
   unsigned region2_bad = 0;
   for (uint32_t dy = 8; dy < 12; ++dy) {
      for (uint32_t dx = 8; dx < 12; ++dx) {
         if (staging_mapping[dy * IMAGE_SIZE + dx] != source_texel(dx, dy))
            ++region2_bad;
      }
   }
   if (region2_bad != 0) {
      fprintf(stderr, "region 2: %u of 16 destination texels wrong\n", region2_bad);
      ++failures;
   }
   /* Region 1 halves source (0,0)-(4,4) into (8,0)-(10,2); only that it was written is checked,
    * since which source texel a halving nearest filter picks is not the point here.
    */
   unsigned region1_sentinel = 0;
   for (uint32_t dy = 0; dy < 2; ++dy) {
      for (uint32_t dx = 8; dx < 10; ++dx) {
         if (staging_mapping[dy * IMAGE_SIZE + dx] == SENTINEL)
            ++region1_sentinel;
      }
   }
   if (region1_sentinel != 0) {
      fprintf(stderr, "region 1: %u of 4 destination texels never written\n", region1_sentinel);
      ++failures;
   }

   printf("blit_mixed_regions failures=%u %s\n", failures, failures == 0 ? "PASS" : "FAIL");

   vkDeviceWaitIdle(device);
   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, command_pool, NULL);
   vkUnmapMemory(device, staging_memory);
   vkDestroyBuffer(device, staging, NULL);
   vkFreeMemory(device, staging_memory, NULL);
   for (int i = 0; i < 2; ++i) {
      vkDestroyImage(device, images[i], NULL);
      vkFreeMemory(device, image_memories[i], NULL);
   }
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   return failures == 0 ? 0 : 1;
}
