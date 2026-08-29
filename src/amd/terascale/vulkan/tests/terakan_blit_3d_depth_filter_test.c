/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* VK_FILTER_LINEAR blits of a 3D image must interpolate along depth.
 *
 * The blit path sampled every source as a 2D array and picked the nearest slice, which is right for
 * an array -- there is nothing between two layers -- and wrong for a 3D image, where depth is a
 * continuous axis. Every 3D linear blit came out as a stack of nearest slices:
 * dEQP-VK.api.copy_and_blit.core.blit_image.all_formats.color.3d failed 273 of its cases, all of
 * them the `linear_stripes_z` variants, which are the ones whose content varies along depth.
 *
 * Three things were missing and all three were needed. The source is now described as a 3D
 * resource rather than a 2D array; the sampler's Z_FILTER, a field of its own that a linear XY
 * filter does not touch, is set to linear; and the pixel shader takes a depth coordinate, which the
 * hand-written 2D one has no way to accept. That shader is built as NIR.
 *
 * A two-slice source is blitted into four destination slices, so the destination slices land at
 * source depths 0.25, 0.75, 1.25 and 1.75. With texel centres at 0.5 and 1.5, the first and last
 * clamp to the two source values and the middle two are quarter and three-quarter mixes. Nearest
 * sampling gives the two source values twice each, so the two filters cannot be confused for one
 * another, and the same blit is run with VK_FILTER_NEAREST as the negative control -- it must keep
 * producing the unmixed values.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SRC_DEPTH 2u
#define DST_DEPTH 4u
/* The two source slices, chosen at the ends of the range so a mix is unmistakable. */
#define SRC_LOW 0u
#define SRC_HIGH 255u
/* Filtering is done at reduced precision on this hardware, so the mixes are compared with a
 * tolerance; it is far tighter than the distance between a mix and either unmixed value.
 */
#define TOLERANCE 6

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
      if (strstr(properties.deviceName, "(Terakan)") != NULL) {
         physical_device = physical_devices[i];
         break;
      }
   }
   if (physical_device == VK_NULL_HANDLE) {
      fprintf(stderr, "No Terakan device found\n");
      return 77;
   }

   VkFormatProperties format_properties;
   vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R8G8B8A8_UNORM,
                                       &format_properties);
   if ((format_properties.optimalTilingFeatures &
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) == 0) {
      fprintf(stderr, "R8G8B8A8_UNORM linear filtering unsupported\n");
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

   uint32_t const depths[2] = {SRC_DEPTH, DST_DEPTH};
   VkImageUsageFlags const usages[2] = {
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
         VK_IMAGE_USAGE_SAMPLED_BIT,
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
   };
   VkImage images[2];
   VkDeviceMemory image_memories[2];
   for (int i = 0; i < 2; ++i) {
      VkImageCreateInfo const image_create_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_3D,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .extent = {1, 1, depths[i]},
         .mipLevels = 1,
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
   }

   VkDeviceSize const readback_size = DST_DEPTH * 4 * sizeof(uint8_t);
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

   VkFilter const filters[2] = {VK_FILTER_LINEAR, VK_FILTER_NEAREST};
   /* Destination slice centres map to source depths 0.25, 0.75, 1.25 and 1.75, and the source texel
    * centres are at 0.5 and 1.5.
    */
   int const expected[2][DST_DEPTH] = {
      {SRC_LOW, SRC_LOW + (SRC_HIGH - SRC_LOW) / 4, SRC_LOW + 3 * (SRC_HIGH - SRC_LOW) / 4,
       SRC_HIGH},
      {SRC_LOW, SRC_LOW, SRC_HIGH, SRC_HIGH},
   };

   bool failed = false;
   for (int filter_index = 0; filter_index < 2; ++filter_index) {
      memset(readback_map, 0xEE, readback_size);
      VkCommandBufferBeginInfo const begin_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      };
      CHECK_VK(vkResetCommandBuffer(command_buffer, 0));
      CHECK_VK(vkBeginCommandBuffer(command_buffer, &begin_info));

      for (int i = 0; i < 2; ++i) {
         VkImageMemoryBarrier const to_general = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .image = images[i],
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
         };
         vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_general);
      }
      /* The two source slices are written one at a time, since a clear covers the whole image. */
      for (uint32_t slice = 0; slice < SRC_DEPTH; ++slice) {
         uint32_t const value = slice == 0 ? SRC_LOW : SRC_HIGH;
         uint8_t const texel[4] = {(uint8_t)value, (uint8_t)value, (uint8_t)value, 255};
         VkBufferCreateInfo const staging_create_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = sizeof(texel),
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
         };
         VkBuffer staging;
         CHECK_VK(vkCreateBuffer(device, &staging_create_info, NULL, &staging));
         VkMemoryRequirements staging_requirements;
         vkGetBufferMemoryRequirements(device, staging, &staging_requirements);
         VkMemoryAllocateInfo const staging_allocate_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = staging_requirements.size,
            .memoryTypeIndex =
               find_memory_type(physical_device, staging_requirements.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
         };
         VkDeviceMemory staging_memory;
         CHECK_VK(vkAllocateMemory(device, &staging_allocate_info, NULL, &staging_memory));
         CHECK_VK(vkBindBufferMemory(device, staging, staging_memory, 0));
         void * staging_map;
         CHECK_VK(vkMapMemory(device, staging_memory, 0, VK_WHOLE_SIZE, 0, &staging_map));
         memcpy(staging_map, texel, sizeof(texel));
         VkBufferImageCopy const upload = {
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = {0, 0, (int32_t)slice},
            .imageExtent = {1, 1, 1},
         };
         vkCmdCopyBufferToImage(command_buffer, staging, images[0], VK_IMAGE_LAYOUT_GENERAL, 1,
                                &upload);
      }

      VkMemoryBarrier const upload_to_blit = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &upload_to_blit, 0, NULL, 0,
                           NULL);

      VkImageBlit const blit = {
         .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .srcOffsets = {{0, 0, 0}, {1, 1, SRC_DEPTH}},
         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .dstOffsets = {{0, 0, 0}, {1, 1, DST_DEPTH}},
      };
      vkCmdBlitImage(command_buffer, images[0], VK_IMAGE_LAYOUT_GENERAL, images[1],
                     VK_IMAGE_LAYOUT_GENERAL, 1, &blit, filters[filter_index]);

      VkMemoryBarrier const blit_to_read = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &blit_to_read, 0, NULL, 0, NULL);
      VkBufferImageCopy const download = {
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageExtent = {1, 1, DST_DEPTH},
      };
      vkCmdCopyImageToBuffer(command_buffer, images[1], VK_IMAGE_LAYOUT_GENERAL, readback, 1,
                             &download);
      VkMemoryBarrier const read_to_host = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &read_to_host, 0, NULL, 0, NULL);
      CHECK_VK(vkEndCommandBuffer(command_buffer));

      CHECK_VK(vkResetFences(device, 1, &fence));
      VkSubmitInfo const submit_info = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &command_buffer,
      };
      CHECK_VK(vkQueueSubmit(queue, 1, &submit_info, fence));
      CHECK_VK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));

      for (uint32_t slice = 0; slice < DST_DEPTH; ++slice) {
         int const got = readback_map[slice * 4];
         int const want = expected[filter_index][slice];
         if (abs(got - want) > TOLERANCE) {
            fprintf(stderr, "%s: destination slice %u expected %d, got %d\n",
                    filter_index == 0 ? "linear" : "nearest (control)", slice, want, got);
            failed = true;
         }
      }
      vkDeviceWaitIdle(device);
   }

   if (failed) {
      return 1;
   }
   printf("3D linear blits interpolate along depth, nearest ones do not\n");
   return 0;
}
