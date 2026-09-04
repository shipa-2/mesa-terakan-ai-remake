/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* An array of storage images indexed at run time, read once and stored to twice under a condition.
 *
 * Reduced from the poorest-in-resources case of dEQP-VK.binding_model.descriptorset_random that
 * failed once the index register was being loaded per block: the compute shader here is that case's
 * shader verbatim. The defect it guards is in the scheduler rather than in this driver --
 * RatInstr::do_ready() reported a MEM_RAT ready without waiting for the ALU that loads the index
 * register its RAT id is taken from, so the read could be issued ahead of its own SET_CF_IDX and
 * come back with whatever array element the register still held. That is invisible to a shader
 * whose array index is a literal, which is why so much of binding_model passed while this did not.
 *
 * The output image records one texel per invocation: 1 where the read returned the value its
 * element holds, 0 where it did not.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define OUTPUT_WIDTH 8u
#define OUTPUT_HEIGHT 8u
#define OUTPUT_TEXELS (OUTPUT_WIDTH * OUTPUT_HEIGHT)

/* Must match the shader's `indexed_images`. Element i is filled with i + 1. */
#define INDEXED_IMAGE_COUNT 3u

#define CHECK_VK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_vk_result = (expression);                                               \
      if (check_vk_result != VK_SUCCESS) {                                                         \
         fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_vk_result);            \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

static const uint32_t indexed_image_array_spirv[] = {
#include "terakan_indexed_image_array.spv.h"
};

static uint32_t
find_memory_type(VkPhysicalDevice const physical_device, uint32_t const memory_type_bits,
                 VkMemoryPropertyFlags const required)
{
   VkPhysicalDeviceMemoryProperties properties;
   vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
   for (uint32_t type_index = 0; type_index < properties.memoryTypeCount; ++type_index) {
      if ((memory_type_bits & (1u << type_index)) &&
          (properties.memoryTypes[type_index].propertyFlags & required) == required)
         return type_index;
   }
   return UINT32_MAX;
}

int
main(void)
{
   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-indexed-image-array-test",
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
   uint32_t compute_queue_family = UINT32_MAX;
   VkPhysicalDeviceProperties properties;
   memset(&properties, 0, sizeof(properties));
   for (uint32_t device_index = 0; device_index < physical_device_count; ++device_index) {
      vkGetPhysicalDeviceProperties(physical_devices[device_index], &properties);
      if (!strstr(properties.deviceName, "(Terakan)") ||
          strstr(properties.deviceName, "TeraScale 1"))
         continue;
      uint32_t family_count = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(physical_devices[device_index], &family_count, NULL);
      VkQueueFamilyProperties families[8];
      if (family_count > 8)
         family_count = 8;
      vkGetPhysicalDeviceQueueFamilyProperties(physical_devices[device_index], &family_count,
                                               families);
      for (uint32_t family_index = 0; family_index < family_count; ++family_index) {
         if (families[family_index].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            physical_device = physical_devices[device_index];
            compute_queue_family = family_index;
            break;
         }
      }
      if (physical_device != VK_NULL_HANDLE)
         break;
   }
   if (physical_device == VK_NULL_HANDLE) {
      fprintf(stderr, "Terakan compute device not found\n");
      return 1;
   }

   float const queue_priority = 1.0f;
   VkDeviceQueueCreateInfo const queue_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = compute_queue_family,
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
   vkGetDeviceQueue(device, compute_queue_family, 0, &queue);

   /* One image per descriptor: the output, then the array elements. The array elements are one
    * texel each -- only which image the read reaches is under test, not where in it it lands.
    */
   VkImage images[1 + INDEXED_IMAGE_COUNT];
   VkDeviceMemory image_memory[1 + INDEXED_IMAGE_COUNT];
   VkImageView image_views[1 + INDEXED_IMAGE_COUNT];
   for (uint32_t image_index = 0; image_index < 1 + INDEXED_IMAGE_COUNT; ++image_index) {
      bool const is_output = image_index == 0;
      VkImageCreateInfo const image_create_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_R32_SINT,
         .extent = {is_output ? OUTPUT_WIDTH : 1u, is_output ? OUTPUT_HEIGHT : 1u, 1},
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      CHECK_VK(vkCreateImage(device, &image_create_info, NULL, &images[image_index]));
      VkMemoryRequirements requirements;
      vkGetImageMemoryRequirements(device, images[image_index], &requirements);
      uint32_t const memory_type = find_memory_type(physical_device, requirements.memoryTypeBits, 0);
      if (memory_type == UINT32_MAX) {
         fprintf(stderr, "No memory type serves the images\n");
         return 1;
      }
      VkMemoryAllocateInfo const allocate_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = requirements.size,
         .memoryTypeIndex = memory_type,
      };
      CHECK_VK(vkAllocateMemory(device, &allocate_info, NULL, &image_memory[image_index]));
      CHECK_VK(vkBindImageMemory(device, images[image_index], image_memory[image_index], 0));
      VkImageViewCreateInfo const view_create_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = images[image_index],
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = VK_FORMAT_R32_SINT,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      CHECK_VK(vkCreateImageView(device, &view_create_info, NULL, &image_views[image_index]));
   }

   VkBufferCreateInfo const staging_create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = OUTPUT_TEXELS * 4u,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer staging_buffer;
   CHECK_VK(vkCreateBuffer(device, &staging_create_info, NULL, &staging_buffer));
   VkMemoryRequirements staging_requirements;
   vkGetBufferMemoryRequirements(device, staging_buffer, &staging_requirements);
   uint32_t const staging_memory_type =
      find_memory_type(physical_device, staging_requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (staging_memory_type == UINT32_MAX) {
      fprintf(stderr, "No host-visible memory type for the staging buffer\n");
      return 1;
   }
   VkMemoryAllocateInfo const staging_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = staging_requirements.size,
      .memoryTypeIndex = staging_memory_type,
   };
   VkDeviceMemory staging_memory;
   CHECK_VK(vkAllocateMemory(device, &staging_allocate_info, NULL, &staging_memory));
   CHECK_VK(vkBindBufferMemory(device, staging_buffer, staging_memory, 0));
   int32_t * staging_mapping;
   CHECK_VK(vkMapMemory(device, staging_memory, 0, VK_WHOLE_SIZE, 0, (void **)&staging_mapping));

   VkDescriptorSetLayoutBinding const bindings[] = {
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      {
         .binding = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = INDEXED_IMAGE_COUNT,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
   };
   VkDescriptorSetLayoutCreateInfo const set_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2,
      .pBindings = bindings,
   };
   VkDescriptorSetLayout set_layout;
   CHECK_VK(vkCreateDescriptorSetLayout(device, &set_layout_create_info, NULL, &set_layout));
   VkPushConstantRange const push_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 * sizeof(int32_t)};
   VkPipelineLayoutCreateInfo const pipeline_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push_range,
   };
   VkPipelineLayout pipeline_layout;
   CHECK_VK(vkCreatePipelineLayout(device, &pipeline_layout_create_info, NULL, &pipeline_layout));
   VkDescriptorPoolSize const pool_size = {
      .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      .descriptorCount = 1 + INDEXED_IMAGE_COUNT,
   };
   VkDescriptorPoolCreateInfo const pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
   };
   VkDescriptorPool descriptor_pool;
   CHECK_VK(vkCreateDescriptorPool(device, &pool_create_info, NULL, &descriptor_pool));
   VkDescriptorSetAllocateInfo const set_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = descriptor_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &set_layout,
   };
   VkDescriptorSet descriptor_set;
   CHECK_VK(vkAllocateDescriptorSets(device, &set_allocate_info, &descriptor_set));

   VkDescriptorImageInfo output_info = {
      .imageView = image_views[0],
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
   };
   VkDescriptorImageInfo indexed_info[INDEXED_IMAGE_COUNT];
   for (uint32_t element = 0; element < INDEXED_IMAGE_COUNT; ++element) {
      indexed_info[element] = (VkDescriptorImageInfo){
         .imageView = image_views[1 + element],
         .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      };
   }
   VkWriteDescriptorSet const writes[] = {
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_set,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo = &output_info,
      },
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_set,
         .dstBinding = 1,
         .descriptorCount = INDEXED_IMAGE_COUNT,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo = indexed_info,
      },
   };
   vkUpdateDescriptorSets(device, 2, writes, 0, NULL);

   VkShaderModuleCreateInfo const shader_module_create_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(indexed_image_array_spirv),
      .pCode = indexed_image_array_spirv,
   };
   VkShaderModule shader_module;
   CHECK_VK(vkCreateShaderModule(device, &shader_module_create_info, NULL, &shader_module));
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
      .queueFamilyIndex = compute_queue_family,
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
   VkFenceCreateInfo const fence_create_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   CHECK_VK(vkCreateFence(device, &fence_create_info, NULL, &fence));

   VkCommandBufferBeginInfo const begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   CHECK_VK(vkBeginCommandBuffer(command_buffer, &begin_info));

   VkImageSubresourceRange const whole_image = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   VkImageMemoryBarrier to_general[1 + INDEXED_IMAGE_COUNT];
   for (uint32_t image_index = 0; image_index < 1 + INDEXED_IMAGE_COUNT; ++image_index) {
      to_general[image_index] = (VkImageMemoryBarrier){
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = images[image_index],
         .subresourceRange = whole_image,
      };
   }
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                        1 + INDEXED_IMAGE_COUNT, to_general);

   /* Zero the output, and give array element i the value i + 1. Distinct values are the whole
    * point: a read that goes to the wrong element has to be able to say so.
    */
   VkClearColorValue const zero = {.int32 = {0, 0, 0, 0}};
   vkCmdClearColorImage(command_buffer, images[0], VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &whole_image);
   for (uint32_t element = 0; element < INDEXED_IMAGE_COUNT; ++element) {
      VkClearColorValue const value = {.int32 = {(int32_t)element + 1, 0, 0, 0}};
      vkCmdClearColorImage(command_buffer, images[1 + element], VK_IMAGE_LAYOUT_GENERAL, &value, 1,
                           &whole_image);
   }
   VkMemoryBarrier const prepared = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &prepared, 0, NULL, 0, NULL);

   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                           &descriptor_set, 0, NULL);
   int32_t const identity[4] = {0, 1, 2, 3};
   vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                      sizeof(identity), identity);
   vkCmdDispatch(command_buffer, OUTPUT_WIDTH, OUTPUT_HEIGHT, 1);

   VkMemoryBarrier const stored = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &stored, 0, NULL, 0, NULL);
   VkBufferImageCopy const readback = {
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {OUTPUT_WIDTH, OUTPUT_HEIGHT, 1},
   };
   vkCmdCopyImageToBuffer(command_buffer, images[0], VK_IMAGE_LAYOUT_GENERAL, staging_buffer, 1,
                          &readback);
   VkMemoryBarrier const host_ready = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_ready, 0, NULL, 0, NULL);
   CHECK_VK(vkEndCommandBuffer(command_buffer));

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
   for (uint32_t texel = 0; texel < OUTPUT_TEXELS; ++texel) {
      if (staging_mapping[texel] == 1)
         continue;
      if (failures == 0) {
         fprintf(stderr,
                 "invocation %u (%u,%u) = %d: the read of indexed_images[0] did not return the 1 "
                 "that element holds\n",
                 texel, texel % OUTPUT_WIDTH, texel / OUTPUT_WIDTH, staging_mapping[texel]);
      }
      ++failures;
   }

   printf("indexed_image_array wrong=%u/%u %s\n", failures, OUTPUT_TEXELS,
          failures == 0 ? "PASS" : "FAIL");

   vkDeviceWaitIdle(device);
   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, command_pool, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyShaderModule(device, shader_module, NULL);
   vkDestroyDescriptorPool(device, descriptor_pool, NULL);
   vkDestroyPipelineLayout(device, pipeline_layout, NULL);
   vkDestroyDescriptorSetLayout(device, set_layout, NULL);
   for (uint32_t image_index = 0; image_index < 1 + INDEXED_IMAGE_COUNT; ++image_index) {
      vkDestroyImageView(device, image_views[image_index], NULL);
      vkDestroyImage(device, images[image_index], NULL);
      vkFreeMemory(device, image_memory[image_index], NULL);
   }
   vkUnmapMemory(device, staging_memory);
   vkDestroyBuffer(device, staging_buffer, NULL);
   vkFreeMemory(device, staging_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   return failures == 0 ? 0 : 1;
}
