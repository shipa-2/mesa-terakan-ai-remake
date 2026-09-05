/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Storage image views of a single slice of an array image.
 *
 * A write through a non-array view with a non-zero baseArrayLayer always landed on slice zero. The
 * colour descriptor encodes the slice in CB_COLOR*_VIEW's SLICE_START correctly, but the view type
 * decided RESOURCE_TYPE, and VK_IMAGE_VIEW_TYPE_2D asked for TEXTURE2D. The slice fields mean
 * nothing to the hardware under a non-array resource type, so every write collapsed onto the first
 * slice.
 *
 * Array views were unaffected -- TEXTURE2DARRAY, with the layer supplied in the coordinate's Z --
 * which is why 1d_array_base_slice, 2d_array_base_slice and cube_array_base_slice all passed while
 * 1d_base_slice, 2d_base_slice and every cube case failed:
 * dEQP-VK.binding_model.shader_access.*.storage_image.* failed 216 of its 1470 supported cases and
 * the failures were exactly those leaves.
 *
 * The fix is to describe single-slice views with the array resource type as well, bounded to one
 * slice by SLICE_MAX, and to supply the Z coordinate the array type expects -- zero for a non-array
 * view, the cube face for a cube one, which the UAV coordinate builder had been dropping.
 *
 * The array view written alongside is the negative control: it selected its slice correctly before
 * the fix and must still do so, so a fix that moved the slice by the wrong amount, or applied the
 * offset twice, shows up here rather than passing silently.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "terakan_test_device.h"

#define IMAGE_SIZE 4u
#define IMAGE_LAYERS 4u
#define TEXELS (IMAGE_SIZE * IMAGE_SIZE)
/* The layer the array view's slice 1 resolves to: baseArrayLayer 1 plus the shader's Z of 1. */
#define ARRAY_VIEW_BASE_LAYER 1u
#define ARRAY_VIEW_WRITTEN_LAYER 2u

#define CHECK_VK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_vk_result = (expression);                                               \
      if (check_vk_result != VK_SUCCESS) {                                                         \
         fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_vk_result);            \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

static const uint32_t base_slice_spirv[] = {
#include "terakan_storage_image_base_slice.spv.h"

};

/* Each layer is cleared to a value naming itself, so a write that lands on the wrong layer is
 * visible as a displaced value rather than as an absence.
 */
static uint32_t
clear_value(uint32_t const layer)
{
   return 0x11110000u | layer;
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

   /* Two images so the two views never contend for the same layer. */
   VkImage images[2];
   VkDeviceMemory image_memories[2];
   for (int i = 0; i < 2; ++i) {
      VkImageCreateInfo const image_create_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_R32_UINT,
         .extent = {IMAGE_SIZE, IMAGE_SIZE, 1},
         .mipLevels = 1,
         .arrayLayers = IMAGE_LAYERS,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
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

   VkDeviceSize const readback_size =
      (VkDeviceSize)TEXELS * IMAGE_LAYERS * 2 * sizeof(uint32_t);
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
   uint32_t * readback_map;
   CHECK_VK(vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0, (void **)&readback_map));

   VkImageView array_view;
   VkImageViewCreateInfo const array_view_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = images[1],
      .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
      .format = VK_FORMAT_R32_UINT,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, ARRAY_VIEW_BASE_LAYER, 2},
   };
   CHECK_VK(vkCreateImageView(device, &array_view_create_info, NULL, &array_view));

   VkDescriptorSetLayoutBinding const layout_bindings[2] = {
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      {
         .binding = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
   };
   VkDescriptorSetLayoutCreateInfo const set_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2,
      .pBindings = layout_bindings,
   };
   VkDescriptorSetLayout set_layout;
   CHECK_VK(vkCreateDescriptorSetLayout(device, &set_layout_create_info, NULL, &set_layout));
   VkPipelineLayoutCreateInfo const pipeline_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout,
   };
   VkPipelineLayout pipeline_layout;
   CHECK_VK(vkCreatePipelineLayout(device, &pipeline_layout_create_info, NULL, &pipeline_layout));

   VkDescriptorPoolSize const pool_size = {
      .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      .descriptorCount = 2 * IMAGE_LAYERS,
   };
   VkDescriptorPoolCreateInfo const pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = IMAGE_LAYERS,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
   };
   VkDescriptorPool descriptor_pool;
   CHECK_VK(vkCreateDescriptorPool(device, &pool_create_info, NULL, &descriptor_pool));

   VkShaderModuleCreateInfo const module_create_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(base_slice_spirv),
      .pCode = base_slice_spirv,
   };
   VkShaderModule shader_module;
   CHECK_VK(vkCreateShaderModule(device, &module_create_info, NULL, &shader_module));
   VkComputePipelineCreateInfo const pipeline_create_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage =
         {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shader_module,
            .pName = "main",
         },
      .layout = pipeline_layout,
   };
   VkPipeline pipeline;
   CHECK_VK(
      vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, NULL, &pipeline));

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
   for (uint32_t base_layer = 0; base_layer < IMAGE_LAYERS; ++base_layer) {
      VkImageViewCreateInfo const single_view_create_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = images[0],
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = VK_FORMAT_R32_UINT,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, base_layer, 1},
      };
      VkImageView single_view;
      CHECK_VK(vkCreateImageView(device, &single_view_create_info, NULL, &single_view));

      VkDescriptorSetAllocateInfo const set_allocate_info = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
         .descriptorPool = descriptor_pool,
         .descriptorSetCount = 1,
         .pSetLayouts = &set_layout,
      };
      VkDescriptorSet descriptor_set;
      CHECK_VK(vkAllocateDescriptorSets(device, &set_allocate_info, &descriptor_set));
      VkDescriptorImageInfo const image_infos[2] = {
         {.imageView = single_view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
         {.imageView = array_view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
      };
      VkWriteDescriptorSet const writes[2] = {
         {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = descriptor_set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &image_infos[0],
         },
         {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = descriptor_set,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &image_infos[1],
         },
      };
      vkUpdateDescriptorSets(device, 2, writes, 0, NULL);

      memset(readback_map, 0, readback_size);
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
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, IMAGE_LAYERS},
         };
         vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_general);
         for (uint32_t layer = 0; layer < IMAGE_LAYERS; ++layer) {
            VkClearColorValue const clear = {.uint32 = {clear_value(layer), 0, 0, 0}};
            VkImageSubresourceRange const range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
            vkCmdClearColorImage(command_buffer, images[i], VK_IMAGE_LAYOUT_GENERAL, &clear, 1,
                                 &range);
         }
      }
      VkMemoryBarrier const clear_to_shader = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &clear_to_shader, 0, NULL, 0,
                           NULL);
      vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                              &descriptor_set, 0, NULL);
      vkCmdDispatch(command_buffer, 1, 1, 1);
      VkMemoryBarrier const shader_to_transfer = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &shader_to_transfer, 0, NULL, 0,
                           NULL);
      for (int i = 0; i < 2; ++i) {
         VkBufferImageCopy const copy = {
            .bufferOffset = (VkDeviceSize)i * TEXELS * IMAGE_LAYERS * sizeof(uint32_t),
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, IMAGE_LAYERS},
            .imageExtent = {IMAGE_SIZE, IMAGE_SIZE, 1},
         };
         vkCmdCopyImageToBuffer(command_buffer, images[i], VK_IMAGE_LAYOUT_GENERAL, readback, 1,
                                &copy);
      }
      VkMemoryBarrier const transfer_to_host = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &transfer_to_host, 0, NULL, 0, NULL);
      CHECK_VK(vkEndCommandBuffer(command_buffer));

      CHECK_VK(vkResetFences(device, 1, &fence));
      VkSubmitInfo const submit_info = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &command_buffer,
      };
      CHECK_VK(vkQueueSubmit(queue, 1, &submit_info, fence));
      CHECK_VK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));

      for (uint32_t layer = 0; layer < IMAGE_LAYERS; ++layer) {
         for (uint32_t texel = 0; texel < TEXELS; ++texel) {
            uint32_t const single_expected =
               layer == base_layer ? (0xAAAA0000u | texel) : clear_value(layer);
            uint32_t const single_got = readback_map[layer * TEXELS + texel];
            if (single_got != single_expected) {
               fprintf(stderr,
                       "baseArrayLayer %u: layer %u texel %u expected 0x%08X, got 0x%08X\n",
                       base_layer, layer, texel, single_expected, single_got);
               failed = true;
            }
            uint32_t const array_expected = layer == ARRAY_VIEW_WRITTEN_LAYER
                                               ? (0xBBBB0000u | texel)
                                               : clear_value(layer);
            uint32_t const array_got =
               readback_map[TEXELS * IMAGE_LAYERS + layer * TEXELS + texel];
            if (array_got != array_expected) {
               fprintf(stderr,
                       "array view control (baseArrayLayer %u pass): layer %u texel %u expected "
                       "0x%08X, got 0x%08X\n",
                       base_layer, layer, texel, array_expected, array_got);
               failed = true;
            }
         }
      }

      vkDeviceWaitIdle(device);
      vkDestroyImageView(device, single_view, NULL);
      CHECK_VK(vkResetDescriptorPool(device, descriptor_pool, 0));
   }

   if (failed) {
      return 1;
   }
   printf("Non-array storage image views select their own array slice\n");
   return 0;
}
