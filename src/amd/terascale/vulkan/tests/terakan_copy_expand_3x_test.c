/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Copying a three-component image to and from a buffer.
 *
 * These formats are stored as three separate surfels per texel, and the copy shader fetched all
 * three at once, as 8_8_8, 16_16_16 or 32_32_32. Only the last of those works: this hardware
 * returns completely invalid values for three-component 8- and 16-bit buffer fetches, a limitation
 * the driver already records for api.buffer_view and vertex_input, and which
 * terakan_nir_load_raw_resource_buffer asserts against.
 *
 * So every copy of a one- or two-byte-surfel three-component image read nonsense. It also made
 * clears of those formats look broken when they were not: dEQP-VK.api.image_clearing verifies by
 * copying the image out, so a correct clear read back wrong. A probe showed the split directly --
 * the image's memory held the right bytes while vkCmdCopyImageToBuffer returned others.
 *
 * The components are now fetched one at a time, which avoids the broken format entirely, with one
 * shader per surfel size because the fetch format is part of the instruction rather than of the
 * descriptor.
 *
 * Two things are checked for each format. A clear followed by a read-back isolates the image to
 * buffer direction against a value the clear test already covers independently. A pattern uploaded
 * and read back exercises both directions, with every surfel of the image distinct so that a copy
 * landing on the wrong surfel is visible rather than coincidentally right.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "terakan_test_device.h"


#define IMAGE_WIDTH 8u
#define IMAGE_HEIGHT 4u
#define TEXELS (IMAGE_WIDTH * IMAGE_HEIGHT)
#define SURFELS (TEXELS * 3u)

#define CHECK_VK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_vk_result = (expression);                                               \
      if (check_vk_result != VK_SUCCESS) {                                                         \
         fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_vk_result);            \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

struct format_case {
   VkFormat format;
   char const * name;
   unsigned bytes_per_surfel;
   uint32_t clear_components[3];
};

static const struct format_case format_cases[] = {
   {VK_FORMAT_R8G8B8_USCALED, "r8g8b8_uscaled", 1, {0x11, 0x22, 0x33}},
   {VK_FORMAT_R16G16B16_USCALED, "r16g16b16_uscaled", 2, {0x1122, 0x3344, 0x5566}},
   {VK_FORMAT_R32G32B32_UINT, "r32g32b32_uint", 4, {0x11223344, 0x55667788, 0x99AABBDD}},
};

/* Every surfel of the image gets its own value, so a copy that reads or writes the wrong surfel
 * shows up as a displaced value rather than as a plausible one.
 */
static uint32_t
pattern_surfel(unsigned const surfel, unsigned const bytes_per_surfel)
{
   uint32_t const value = 0x9E3779B9u * (surfel + 1u);
   return bytes_per_surfel == 4 ? value : (value & ((1u << (bytes_per_surfel * 8)) - 1u));
}

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
   VkDeviceCreateInfo const device_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_create_info,
   };
   VkDevice device;
   CHECK_VK(vkCreateDevice(physical_device, &device_create_info, NULL, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, 0, 0, &queue);

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

   bool failed = false;
   unsigned checked = 0;
   for (unsigned case_index = 0;
        case_index < sizeof(format_cases) / sizeof(format_cases[0]); ++case_index) {
      struct format_case const * const format_case = &format_cases[case_index];

      VkFormatProperties format_properties;
      vkGetPhysicalDeviceFormatProperties(physical_device, format_case->format,
                                          &format_properties);
      VkFormatFeatureFlags const needed =
         VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
      if ((format_properties.optimalTilingFeatures & needed) != needed) {
         continue;
      }

      VkDeviceSize const buffer_size = (VkDeviceSize)SURFELS * format_case->bytes_per_surfel;

      VkImageCreateInfo const image_create_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = format_case->format,
         .extent = {IMAGE_WIDTH, IMAGE_HEIGHT, 1},
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
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

      VkBuffer buffers[2];
      VkDeviceMemory buffer_memories[2];
      uint8_t * buffer_maps[2];
      VkBufferUsageFlags const buffer_usages[2] = {VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT};
      for (int i = 0; i < 2; ++i) {
         VkBufferCreateInfo const buffer_create_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = buffer_size,
            .usage = buffer_usages[i],
         };
         CHECK_VK(vkCreateBuffer(device, &buffer_create_info, NULL, &buffers[i]));
         VkMemoryRequirements buffer_requirements;
         vkGetBufferMemoryRequirements(device, buffers[i], &buffer_requirements);
         VkMemoryAllocateInfo const buffer_allocate_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = buffer_requirements.size,
            .memoryTypeIndex =
               find_memory_type(physical_device, buffer_requirements.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
         };
         CHECK_VK(vkAllocateMemory(device, &buffer_allocate_info, NULL, &buffer_memories[i]));
         CHECK_VK(vkBindBufferMemory(device, buffers[i], buffer_memories[i], 0));
         CHECK_VK(vkMapMemory(device, buffer_memories[i], 0, VK_WHOLE_SIZE, 0,
                              (void **)&buffer_maps[i]));
      }

      for (unsigned surfel = 0; surfel < SURFELS; ++surfel) {
         uint32_t const value = pattern_surfel(surfel, format_case->bytes_per_surfel);
         memcpy(buffer_maps[0] + surfel * format_case->bytes_per_surfel, &value,
                format_case->bytes_per_surfel);
      }

      /* Pass 0 clears the image and reads it back; pass 1 uploads the pattern and reads that back.
       * The first isolates the image to buffer direction, the second exercises both.
       */
      for (int pass = 0; pass < 2; ++pass) {
         memset(buffer_maps[1], 0xEE, buffer_size);

         VkCommandBufferBeginInfo const begin_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
         };
         CHECK_VK(vkResetCommandBuffer(command_buffer, 0));
         CHECK_VK(vkBeginCommandBuffer(command_buffer, &begin_info));
         VkImageMemoryBarrier const to_general = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .image = image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
         };
         vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_general);

         if (pass == 0) {
            VkClearColorValue clear_value = {.uint32 = {format_case->clear_components[0],
                                                        format_case->clear_components[1],
                                                        format_case->clear_components[2], 0}};
            VkImageSubresourceRange const range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL, &clear_value, 1,
                                 &range);
         } else {
            VkBufferImageCopy const upload = {
               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
               .imageExtent = {IMAGE_WIDTH, IMAGE_HEIGHT, 1},
            };
            vkCmdCopyBufferToImage(command_buffer, buffers[0], image, VK_IMAGE_LAYOUT_GENERAL, 1,
                                   &upload);
         }

         VkMemoryBarrier const between = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask =
               VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
         };
         vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &between, 0, NULL, 0,
                              NULL);
         VkBufferImageCopy const download = {
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {IMAGE_WIDTH, IMAGE_HEIGHT, 1},
         };
         vkCmdCopyImageToBuffer(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL, buffers[1], 1,
                                &download);
         VkMemoryBarrier const to_host = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask =
               VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
         };
         vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
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

         unsigned reported = 0;
         for (unsigned surfel = 0; surfel < SURFELS; ++surfel) {
            uint32_t const expected =
               pass == 0 ? format_case->clear_components[surfel % 3]
                         : pattern_surfel(surfel, format_case->bytes_per_surfel);
            uint32_t got = 0;
            memcpy(&got, buffer_maps[1] + surfel * format_case->bytes_per_surfel,
                   format_case->bytes_per_surfel);
            if (got != expected) {
               if (reported < 4) {
                  fprintf(stderr, "%s %s: surfel %u expected 0x%X, got 0x%X\n", format_case->name,
                          pass == 0 ? "cleared" : "uploaded", surfel, expected, got);
                  ++reported;
               }
               failed = true;
            }
         }
      }

      ++checked;
      vkDeviceWaitIdle(device);
      for (int i = 0; i < 2; ++i) {
         vkUnmapMemory(device, buffer_memories[i]);
         vkDestroyBuffer(device, buffers[i], NULL);
         vkFreeMemory(device, buffer_memories[i], NULL);
      }
      vkDestroyImage(device, image, NULL);
      vkFreeMemory(device, image_memory, NULL);
   }

   if (checked == 0) {
      fprintf(stderr, "No three-component format could be tested\n");
      return 77;
   }
   if (failed) {
      return 1;
   }
   printf("Three-component images copy to and from buffers surfel for surfel\n");
   return 0;
}
