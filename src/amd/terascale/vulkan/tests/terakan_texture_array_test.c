/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Constant-indexed arrays of uniform texel buffers.
 *
 * Every element of such an array read element zero. The binding lowering resolves a descriptor
 * through nir_chase_binding, and that helper collects the array indices of a descriptor array only
 * when the descriptor is an image or a sampler -- glsl_type_is_image() || glsl_type_is_sampler().
 * A separate texture, which is what GLSL's textureBuffer becomes, is GLSL_TYPE_TEXTURE and matches
 * neither, so every element chased back to the same binding with no index attached and every fetch
 * landed on the same hardware resource slot.
 *
 * A uniform texel buffer is the only descriptor type with no combined image-sampler form, so it is
 * the one that could not avoid the separate-texture path: arrays of combined image samplers passed
 * all 51 of their cases in dEQP-VK.binding_model.shader_access while arrays of uniform texel buffers
 * failed 84 of 108.
 *
 * A dynamically uniform index took a different route to the same failure. The lowering turns it into
 * nir_tex_src_texture_offset, but the backend's buffer fetch read only nir_tex_src_sampler_offset --
 * which is what GL's combined samplers produce -- so the index was dropped there instead and every
 * element again fetched from the array's base slot. A sampled buffer has no sampler to index
 * through, so the texture offset is the only one it can carry.
 *
 * The array elements view the same buffer at four different offsets, so the four fetches must return
 * four different words rather than one word four times. Both index forms are read here. The non-array binding alongside them is the
 * negative control: it shares the shader and the descriptor set but has no array index to lose, so
 * it stays correct under the bug and would only break if the fix disturbed the per-binding resource
 * base.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_ELEMENTS 4u
#define SOURCE_WORDS 64u
/* Word index viewed by the non-array control binding. */
#define SINGLE_WORD 20u

#define CHECK_VK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_vk_result = (expression);                                               \
      if (check_vk_result != VK_SUCCESS) {                                                         \
         fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_vk_result);            \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

static const uint32_t texture_array_spirv[] = {
#include "terakan_texture_array.spv.h"
};

/* Every word is distinct, so a fetch from the wrong offset is visible rather than coincidentally
 * right.
 */
static uint32_t
source_word(uint32_t const index)
{
   return 0xC0DE0000u | index;
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
   vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R32_UINT, &format_properties);
   if ((format_properties.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) == 0) {
      fprintf(stderr, "R32_UINT uniform texel buffers unsupported\n");
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

   VkDeviceSize const buffer_sizes[2] = {SOURCE_WORDS * sizeof(uint32_t), 256};
   VkBufferUsageFlags const buffer_usages[2] = {VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
   VkBuffer buffers[2];
   VkDeviceMemory memories[2];
   uint32_t * maps[2];
   for (int i = 0; i < 2; ++i) {
      VkBufferCreateInfo const buffer_create_info = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = buffer_sizes[i],
         .usage = buffer_usages[i],
      };
      CHECK_VK(vkCreateBuffer(device, &buffer_create_info, NULL, &buffers[i]));
      VkMemoryRequirements memory_requirements;
      vkGetBufferMemoryRequirements(device, buffers[i], &memory_requirements);
      VkMemoryAllocateInfo const allocate_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = memory_requirements.size,
         .memoryTypeIndex = find_memory_type(physical_device, memory_requirements.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
      };
      CHECK_VK(vkAllocateMemory(device, &allocate_info, NULL, &memories[i]));
      CHECK_VK(vkBindBufferMemory(device, buffers[i], memories[i], 0));
      CHECK_VK(vkMapMemory(device, memories[i], 0, VK_WHOLE_SIZE, 0, (void **)&maps[i]));
   }
   for (uint32_t i = 0; i < SOURCE_WORDS; ++i) {
      maps[0][i] = source_word(i);
   }
   memset(maps[1], 0xEE, 256);

   /* Element i views word 4*i, chosen so that a wrong index is off by more than one word and cannot
    * be mistaken for a rounding or granularity artefact.
    */
   VkBufferView views[ARRAY_ELEMENTS + 1];
   for (uint32_t i = 0; i < ARRAY_ELEMENTS + 1; ++i) {
      uint32_t const word = i < ARRAY_ELEMENTS ? i * 4 : SINGLE_WORD;
      VkBufferViewCreateInfo const view_create_info = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
         .buffer = buffers[0],
         .format = VK_FORMAT_R32_UINT,
         .offset = word * sizeof(uint32_t),
         .range = sizeof(uint32_t),
      };
      CHECK_VK(vkCreateBufferView(device, &view_create_info, NULL, &views[i]));
   }

   VkDescriptorSetLayoutBinding const layout_bindings[3] = {
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
         .descriptorCount = ARRAY_ELEMENTS,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      {
         .binding = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      {
         .binding = 2,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
   };
   VkDescriptorSetLayoutCreateInfo const set_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 3,
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

   VkDescriptorPoolSize const pool_sizes[2] = {
      {.type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .descriptorCount = ARRAY_ELEMENTS + 1},
      {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1},
   };
   VkDescriptorPoolCreateInfo const pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 2,
      .pPoolSizes = pool_sizes,
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

   VkDescriptorBufferInfo const dst_buffer_info = {
      .buffer = buffers[1],
      .offset = 0,
      .range = 256,
   };
   VkWriteDescriptorSet const writes[3] = {
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_set,
         .dstBinding = 0,
         .descriptorCount = ARRAY_ELEMENTS,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
         .pTexelBufferView = views,
      },
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_set,
         .dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
         .pTexelBufferView = &views[ARRAY_ELEMENTS],
      },
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_set,
         .dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &dst_buffer_info,
      },
   };
   vkUpdateDescriptorSets(device, 3, writes, 0, NULL);

   VkShaderModuleCreateInfo const module_create_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(texture_array_spirv),
      .pCode = texture_array_spirv,
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
   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                           &descriptor_set, 0, NULL);
   vkCmdDispatch(command_buffer, ARRAY_ELEMENTS, 1, 1);
   VkMemoryBarrier const memory_barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);
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

   bool failed = false;
   for (uint32_t i = 0; i < ARRAY_ELEMENTS; ++i) {
      uint32_t const expected = source_word(i * 4);
      uint32_t const got = maps[1][i * 4];
      if (got != expected) {
         fprintf(stderr, "array_src[%u] constant index: expected 0x%08X, got 0x%08X\n", i, expected,
                 got);
         failed = true;
      }
      uint32_t const dynamic_got = maps[1][(8 + i) * 4];
      if (dynamic_got != expected) {
         fprintf(stderr, "array_src[%u] dynamic index: expected 0x%08X, got 0x%08X\n", i, expected,
                 dynamic_got);
         failed = true;
      }
   }
   uint32_t const single_expected = source_word(SINGLE_WORD);
   uint32_t const single_got = maps[1][4 * 4];
   if (single_got != single_expected) {
      fprintf(stderr, "single_src (control): expected 0x%08X, got 0x%08X\n", single_expected,
              single_got);
      failed = true;
   }

   vkDeviceWaitIdle(device);
   if (failed) {
      return 1;
   }
   printf("Constant-indexed uniform texel buffer arrays read their own elements\n");
   return 0;
}
