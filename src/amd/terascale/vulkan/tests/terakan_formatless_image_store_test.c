/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 17u
#define HEIGHT 13u
#define TEXEL_COUNT (WIDTH * HEIGHT)
#define GUARD_DWORDS 16u
#define GUARD_VALUE 0xA55A3CC3u

#define CHECK_VK(expression)                                                                        \
   do {                                                                                             \
      VkResult const check_vk_result = (expression);                                                \
      if (check_vk_result != VK_SUCCESS) {                                                          \
         fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_vk_result);             \
         return 1;                                                                                  \
      }                                                                                             \
   } while (0)

static const uint32_t formatless_store_spirv[] = {
#include "terakan_formatless_image_store.spv.h"
};

static const uint32_t formatless_load_spirv[] = {
#include "terakan_formatless_image_load.spv.h"
};

static uint32_t
find_memory_type(VkPhysicalDevice physical_device, uint32_t memory_type_bits,
                 VkMemoryPropertyFlags required)
{
   VkPhysicalDeviceMemoryProperties properties;
   vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
   for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
      if ((memory_type_bits & (1u << i)) &&
          (properties.memoryTypes[i].propertyFlags & required) == required)
         return i;
   }
   return UINT32_MAX;
}

int
main(void)
{
   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-formatless-image-store-test",
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo const instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application_info,
   };
   VkInstance instance;
   CHECK_VK(vkCreateInstance(&instance_info, NULL, &instance));

   uint32_t physical_device_count = 1;
   VkPhysicalDevice physical_device;
   CHECK_VK(vkEnumeratePhysicalDevices(instance, &physical_device_count, &physical_device));
   if (physical_device_count != 1) {
      fprintf(stderr, "Expected one physical device, got %u\n", physical_device_count);
      return 1;
   }

   VkPhysicalDeviceFeatures features;
   vkGetPhysicalDeviceFeatures(physical_device, &features);
   if (!features.shaderStorageImageWriteWithoutFormat ||
       !features.shaderStorageImageReadWithoutFormat) {
      fprintf(stderr, "Formatless storage-image read/write features are not exposed\n");
      return 1;
   }

   VkFormatProperties format_properties;
   vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R32_UINT, &format_properties);
   VkFormatFeatureFlags const required_format_features =
      VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
   if ((format_properties.optimalTilingFeatures & required_format_features) !=
       required_format_features) {
      fprintf(stderr, "R32_UINT lacks storage-image or transfer-source support\n");
      return 1;
   }

   uint32_t queue_family_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, NULL);
   VkQueueFamilyProperties *queue_families = calloc(queue_family_count, sizeof(*queue_families));
   if (!queue_families)
      return 1;
   vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_families);
   uint32_t queue_family = UINT32_MAX;
   for (uint32_t i = 0; i < queue_family_count; ++i) {
      if (queue_families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
         queue_family = i;
         break;
      }
   }
   free(queue_families);
   if (queue_family == UINT32_MAX) {
      fprintf(stderr, "No compute queue family\n");
      return 1;
   }

   float const queue_priority = 1.0f;
   VkDeviceQueueCreateInfo const queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = queue_family,
      .queueCount = 1,
      .pQueuePriorities = &queue_priority,
   };
   VkPhysicalDeviceFeatures const enabled_features = {
      .shaderStorageImageReadWithoutFormat = VK_TRUE,
      .shaderStorageImageWriteWithoutFormat = VK_TRUE,
   };
   VkDeviceCreateInfo const device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
      .pEnabledFeatures = &enabled_features,
   };
   VkDevice device;
   CHECK_VK(vkCreateDevice(physical_device, &device_info, NULL, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, queue_family, 0, &queue);

   VkImageCreateInfo const image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R32_UINT,
      .extent = { WIDTH, HEIGHT, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage image;
   CHECK_VK(vkCreateImage(device, &image_info, NULL, &image));
   VkMemoryRequirements image_requirements;
   vkGetImageMemoryRequirements(device, image, &image_requirements);
   uint32_t const image_memory_type =
      find_memory_type(physical_device, image_requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (image_memory_type == UINT32_MAX) {
      fprintf(stderr, "No device-local image memory type\n");
      return 1;
   }
   VkMemoryAllocateInfo const image_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = image_requirements.size,
      .memoryTypeIndex = image_memory_type,
   };
   VkDeviceMemory image_memory;
   CHECK_VK(vkAllocateMemory(device, &image_allocate_info, NULL, &image_memory));
   CHECK_VK(vkBindImageMemory(device, image, image_memory, 0));

   VkImageViewCreateInfo const view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R32_UINT,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   VkImageView image_view;
   CHECK_VK(vkCreateImageView(device, &view_info, NULL, &image_view));

   VkDeviceSize const region_size = TEXEL_COUNT * sizeof(uint32_t);
   VkDeviceSize const buffer_size = 2 * region_size + GUARD_DWORDS * sizeof(uint32_t);
   VkBufferCreateInfo const buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = buffer_size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer buffer;
   CHECK_VK(vkCreateBuffer(device, &buffer_info, NULL, &buffer));
   VkMemoryRequirements buffer_requirements;
   vkGetBufferMemoryRequirements(device, buffer, &buffer_requirements);
   uint32_t const buffer_memory_type =
      find_memory_type(physical_device, buffer_requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (buffer_memory_type == UINT32_MAX) {
      fprintf(stderr, "No host-visible coherent buffer memory type\n");
      return 1;
   }
   VkMemoryAllocateInfo const buffer_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = buffer_requirements.size,
      .memoryTypeIndex = buffer_memory_type,
   };
   VkDeviceMemory buffer_memory;
   CHECK_VK(vkAllocateMemory(device, &buffer_allocate_info, NULL, &buffer_memory));
   CHECK_VK(vkBindBufferMemory(device, buffer, buffer_memory, 0));
   uint32_t *mapped;
   CHECK_VK(vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
   for (VkDeviceSize i = 0; i < buffer_requirements.size / sizeof(uint32_t); ++i)
      mapped[i] = GUARD_VALUE;
   for (uint32_t i = 0; i < TEXEL_COUNT; ++i)
      mapped[i] = 0x13570000u ^ (i * 0x45D9F3Bu);

   VkDescriptorSetLayoutBinding const layout_bindings[] = {
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      {
         .binding = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
   };
   VkDescriptorSetLayoutCreateInfo const set_layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2,
      .pBindings = layout_bindings,
   };
   VkDescriptorSetLayout set_layout;
   CHECK_VK(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout));
   VkPipelineLayoutCreateInfo const pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout,
   };
   VkPipelineLayout pipeline_layout;
   CHECK_VK(vkCreatePipelineLayout(device, &pipeline_layout_info, NULL, &pipeline_layout));

   VkDescriptorPoolSize const pool_sizes[] = {
      {
         .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = 1,
      },
      {
         .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
      },
   };
   VkDescriptorPoolCreateInfo const pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 2,
      .pPoolSizes = pool_sizes,
   };
   VkDescriptorPool descriptor_pool;
   CHECK_VK(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool));
   VkDescriptorSetAllocateInfo const set_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = descriptor_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &set_layout,
   };
   VkDescriptorSet descriptor_set;
   CHECK_VK(vkAllocateDescriptorSets(device, &set_allocate_info, &descriptor_set));
   VkDescriptorImageInfo const descriptor_image_info = {
      .imageView = image_view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
   };
   VkDescriptorBufferInfo const descriptor_buffer_info = {
      .buffer = buffer,
      .offset = region_size,
      .range = region_size,
   };
   VkWriteDescriptorSet const descriptor_writes[] = {
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_set,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo = &descriptor_image_info,
      },
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_set,
         .dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &descriptor_buffer_info,
      },
   };
   vkUpdateDescriptorSets(device, 2, descriptor_writes, 0, NULL);

   VkShaderModuleCreateInfo shader_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(formatless_store_spirv),
      .pCode = formatless_store_spirv,
   };
   VkShaderModule store_shader;
   CHECK_VK(vkCreateShaderModule(device, &shader_info, NULL, &store_shader));
   shader_info.codeSize = sizeof(formatless_load_spirv);
   shader_info.pCode = formatless_load_spirv;
   VkShaderModule load_shader;
   CHECK_VK(vkCreateShaderModule(device, &shader_info, NULL, &load_shader));
   VkComputePipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_COMPUTE_BIT,
         .module = store_shader,
         .pName = "main",
      },
      .layout = pipeline_layout,
   };
   VkPipeline store_pipeline;
   CHECK_VK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                                     &store_pipeline));
   pipeline_info.stage.module = load_shader;
   VkPipeline load_pipeline;
   CHECK_VK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                                     &load_pipeline));

   VkCommandPoolCreateInfo const command_pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = queue_family,
   };
   VkCommandPool command_pool;
   CHECK_VK(vkCreateCommandPool(device, &command_pool_info, NULL, &command_pool));
   VkCommandBufferAllocateInfo const command_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer command_buffer;
   CHECK_VK(vkAllocateCommandBuffers(device, &command_allocate_info, &command_buffer));
   VkCommandBufferBeginInfo const begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   CHECK_VK(vkBeginCommandBuffer(command_buffer, &begin_info));
   VkImageMemoryBarrier image_barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &image_barrier);
   VkBufferMemoryBarrier buffer_barrier = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = buffer,
      .offset = 0,
      .size = region_size,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_HOST_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &buffer_barrier, 0, NULL);
   VkBufferImageCopy copy_region = {
      .imageSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .layerCount = 1,
      },
      .imageExtent = { WIDTH, HEIGHT, 1 },
   };
   vkCmdCopyBufferToImage(command_buffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                          &copy_region);
   image_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   image_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
   image_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   image_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1,
                        &image_barrier);
   vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                           &descriptor_set, 0, NULL);
   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, load_pipeline);
   vkCmdDispatch(command_buffer, WIDTH, HEIGHT, 1);
   image_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
   image_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
   image_barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
   image_barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1,
                        &image_barrier);
   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, store_pipeline);
   vkCmdDispatch(command_buffer, WIDTH, HEIGHT, 1);
   image_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
   image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   image_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
   image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                        &image_barrier);
   vkCmdCopyImageToBuffer(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                          &copy_region);
   VkMemoryBarrier const host_barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer,
                        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_barrier, 0, NULL, 0, NULL);
   CHECK_VK(vkEndCommandBuffer(command_buffer));

   VkFenceCreateInfo const fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkFence fence;
   CHECK_VK(vkCreateFence(device, &fence_info, NULL, &fence));
   VkSubmitInfo const submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
   };
   CHECK_VK(vkQueueSubmit(queue, 1, &submit_info, fence));
   VkResult const wait_result = vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ull);
   if (wait_result != VK_SUCCESS) {
      fprintf(stderr, "Fence wait failed with VkResult %d\n", wait_result);
      return 1;
   }

   bool pass = true;
   for (uint32_t i = 0; i < TEXEL_COUNT; ++i) {
      uint32_t const expected = 0xC0000000u ^ (i * 0x9E3779B9u);
      if (mapped[i] != expected) {
         fprintf(stderr, "store[%u] = 0x%08X, expected 0x%08X\n", i, mapped[i], expected);
         pass = false;
      }
      uint32_t const load_expected = 0x13570000u ^ (i * 0x45D9F3Bu);
      if (mapped[TEXEL_COUNT + i] != load_expected) {
         fprintf(stderr, "load[%u] = 0x%08X, expected 0x%08X\n", i,
                 mapped[TEXEL_COUNT + i], load_expected);
         pass = false;
      }
   }
   for (uint32_t i = 0; i < GUARD_DWORDS; ++i) {
      if (mapped[2 * TEXEL_COUNT + i] != GUARD_VALUE) {
         fprintf(stderr, "guard[%u] = 0x%08X, expected 0x%08X\n", i,
                 mapped[2 * TEXEL_COUNT + i], GUARD_VALUE);
         pass = false;
      }
   }

   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, command_pool, NULL);
   vkDestroyPipeline(device, load_pipeline, NULL);
   vkDestroyPipeline(device, store_pipeline, NULL);
   vkDestroyShaderModule(device, load_shader, NULL);
   vkDestroyShaderModule(device, store_shader, NULL);
   vkDestroyDescriptorPool(device, descriptor_pool, NULL);
   vkDestroyPipelineLayout(device, pipeline_layout, NULL);
   vkDestroyDescriptorSetLayout(device, set_layout, NULL);
   vkUnmapMemory(device, buffer_memory);
   vkDestroyBuffer(device, buffer, NULL);
   vkFreeMemory(device, buffer_memory, NULL);
   vkDestroyImageView(device, image_view, NULL);
   vkDestroyImage(device, image, NULL);
   vkFreeMemory(device, image_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   printf("%s: %ux%u formatless R32_UINT image load/store with intact guards\n",
          pass ? "PASS" : "FAIL", WIDTH, HEIGHT);
   return pass ? 0 : 1;
}
