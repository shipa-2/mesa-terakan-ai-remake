/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* vkCmdClearColorImage on a three-component format.
 *
 * These formats have no hardware equivalent, so the driver stores each component as its own surfel
 * and the image is three times as wide in surfels as it is in texels. The colour target cannot
 * express that, and the clear had no path for it at all: it returned without doing anything, for
 * every three-component format, silently. It was the largest single cause of failures measured
 * anywhere in the driver -- dEQP-VK.api.image_clearing failed 1800 of the three-component cases and
 * every one of them was this.
 *
 * The clear now takes the route the 3x copy already used: a UAV, one fragment per texel, three
 * stores at 3x + 0, 1 and 2, with the shader built as NIR.
 *
 * The check reads the image's own memory rather than copying it out, because copying a
 * three-component image with one- or two-byte surfels back to a buffer is separately broken -- that
 * path fetches the source as 8_8_8 or 16_16_16 from a buffer, which this hardware does not do
 * correctly, a limitation the driver already records elsewhere. A probe confirmed the split
 * directly: the image memory held the right bytes while vkCmdCopyImageToBuffer returned others. So
 * a test built on the copy would fail for a reason that has nothing to do with what it is testing.
 *
 * Two things are checked. The first row must hold exactly the three components in order, repeated
 * across the width, which catches a wrong component order or a wrong packing of the clear value.
 * And the number of bytes that changed at all must equal three surfels per texel over every texel
 * of every slice, which catches a clear that stopped after one row or one slice -- the shape of the
 * bug that left every 3D case failing after the first attempt -- and equally one that ran past the
 * end.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define IMAGE_WIDTH 4u
#define IMAGE_HEIGHT 4u
#define IMAGE_DEPTH 3u
/* The clear values are chosen so that no byte of any of them equals this, which is what makes the
 * count of changed bytes meaningful -- a component byte that happened to match the fill would read
 * as untouched.
 */
#define FILL_BYTE 0xCC

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
   /* The clear value, and the bytes each component must become. */
   uint32_t components[3];
};

static const struct format_case format_cases[] = {
   {VK_FORMAT_R8G8B8_USCALED, "r8g8b8_uscaled", 1, {0x11, 0x22, 0x33}},
   {VK_FORMAT_R32G32B32_UINT, "r32g32b32_uint", 4, {0x11223344, 0x55667788, 0x99AABBDD}},
};

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
      if (strstr(properties.deviceName, "(Terakan)") != NULL) {
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
   for (unsigned case_index = 0; case_index < (sizeof(format_cases) / sizeof(format_cases[0])); ++case_index) {
      struct format_case const * const format_case = &format_cases[case_index];

      VkFormatProperties format_properties;
      vkGetPhysicalDeviceFormatProperties(physical_device, format_case->format,
                                          &format_properties);
      if ((format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) == 0) {
         continue;
      }

      /* Both an ordinary 2D image and a 3D one, so that the slice loop is covered as well as the
       * row loop.
       */
      for (int is_3d = 0; is_3d < 2; ++is_3d) {
         uint32_t const depth = is_3d ? IMAGE_DEPTH : 1u;
         VkImageCreateInfo const image_create_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = is_3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
            .format = format_case->format,
            .extent = {IMAGE_WIDTH, IMAGE_HEIGHT, depth},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         };
         VkImage image;
         CHECK_VK(vkCreateImage(device, &image_create_info, NULL, &image));
         VkMemoryRequirements memory_requirements;
         vkGetImageMemoryRequirements(device, image, &memory_requirements);
         uint32_t const memory_type =
            find_memory_type(physical_device, memory_requirements.memoryTypeBits,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
         if (memory_type == UINT32_MAX) {
            /* Without a host-visible image the memory cannot be read directly, and reading it any
             * other way would be testing the broken copy instead.
             */
            vkDestroyImage(device, image, NULL);
            continue;
         }
         VkMemoryAllocateInfo const allocate_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = memory_requirements.size,
            .memoryTypeIndex = memory_type,
         };
         VkDeviceMemory memory;
         CHECK_VK(vkAllocateMemory(device, &allocate_info, NULL, &memory));
         CHECK_VK(vkBindImageMemory(device, image, memory, 0));
         uint8_t * map;
         CHECK_VK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&map));
         memset(map, FILL_BYTE, memory_requirements.size);

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
         VkClearColorValue clear_value;
         for (unsigned component = 0; component < 3; ++component) {
            clear_value.uint32[component] = format_case->components[component];
         }
         clear_value.uint32[3] = 0;
         VkImageSubresourceRange const range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
         vkCmdClearColorImage(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL, &clear_value, 1,
                              &range);
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

         /* The first row starts at the beginning of the allocation whatever the row pitch is, so
          * its contents can be checked without knowing the pitch.
          */
         for (unsigned texel = 0; texel < IMAGE_WIDTH; ++texel) {
            for (unsigned component = 0; component < 3; ++component) {
               uint32_t got = 0;
               memcpy(&got,
                      map + format_case->bytes_per_surfel * (texel * 3 + component),
                      format_case->bytes_per_surfel);
               if (got != format_case->components[component]) {
                  fprintf(stderr, "%s %s: texel %u component %u expected 0x%X, got 0x%X\n",
                          format_case->name, is_3d ? "3d" : "2d", texel, component,
                          format_case->components[component], got);
                  failed = true;
               }
            }
         }

         /* Exactly three surfels per texel over every texel of every slice must have changed --
          * no fewer, which a clear that stopped after one row or one slice would give, and no more.
          */
         size_t written_bytes = 0;
         for (VkDeviceSize i = 0; i < memory_requirements.size; ++i) {
            if (map[i] != FILL_BYTE) {
               ++written_bytes;
            }
         }
         size_t const expected_bytes =
            (size_t)format_case->bytes_per_surfel * 3u * IMAGE_WIDTH * IMAGE_HEIGHT * depth;
         if (written_bytes != expected_bytes) {
            fprintf(stderr, "%s %s: %zu bytes written, expected %zu\n", format_case->name,
                    is_3d ? "3d" : "2d", written_bytes, expected_bytes);
            failed = true;
         }

         ++checked;
         vkDeviceWaitIdle(device);
         vkUnmapMemory(device, memory);
         vkDestroyImage(device, image, NULL);
         vkFreeMemory(device, memory, NULL);
      }
   }

   if (checked == 0) {
      fprintf(stderr, "No three-component format could be tested\n");
      return 77;
   }
   if (failed) {
      return 1;
   }
   printf("Three-component clears write every component of every texel\n");
   return 0;
}
