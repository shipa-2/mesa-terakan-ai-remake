/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define OUTPUT_COUNT 16
#define OUTPUT_COMPONENTS 6
#define PREFIX_DWORDS 4
#define REGION_DWORDS (1 + OUTPUT_COUNT * OUTPUT_COMPONENTS + 1)
#define SUFFIX_DWORDS 4
#define TOTAL_DWORDS (PREFIX_DWORDS + REGION_DWORDS + SUFFIX_DWORDS)

#define ITERATION_COUNT 7
#define DISPATCH_COUNT 12
#define INNER_CANARY 0x7A11C0DEu
#define OUTER_CANARY 0xA5A55A5Au
#define OUTPUT_CANARY 0xDEADBEEFu

#define CHECK_VK(expression)                                                                        \
   do {                                                                                             \
      VkResult const check_vk_result = (expression);                                                \
      if (check_vk_result != VK_SUCCESS) {                                                          \
         fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_vk_result);             \
         return 1;                                                                                  \
      }                                                                                             \
   } while (0)

static const uint32_t compute_loop_spirv[] = {
#include "terakan_compute_loop.spv.h"
};

static uint32_t
expected_loop_value(void)
{
   uint32_t value = 1;
   for (uint32_t iteration = 0; iteration < ITERATION_COUNT; ++iteration)
      value = value * 3 + 1;
   return value;
}

static uint32_t
find_host_memory_type(VkPhysicalDevice const physical_device, uint32_t const memory_type_bits)
{
   VkPhysicalDeviceMemoryProperties properties;
   vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
   for (uint32_t type_index = 0; type_index < properties.memoryTypeCount; ++type_index) {
      VkMemoryPropertyFlags const required =
         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
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
      .pApplicationName = "terakan-compute-loop-test",
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo const instance_create_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application_info,
   };
   VkInstance instance;
   CHECK_VK(vkCreateInstance(&instance_create_info, NULL, &instance));

   uint32_t physical_device_count = 1;
   VkPhysicalDevice physical_device;
   CHECK_VK(vkEnumeratePhysicalDevices(instance, &physical_device_count, &physical_device));
   if (physical_device_count != 1) {
      fprintf(stderr, "Expected exactly one physical device, got %u\n", physical_device_count);
      return 1;
   }

   uint32_t queue_family_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, NULL);
   VkQueueFamilyProperties * const queue_families =
      calloc(queue_family_count, sizeof(*queue_families));
   if (queue_families == NULL)
      return 1;
   vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_families);
   uint32_t compute_queue_family = UINT32_MAX;
   for (uint32_t family_index = 0; family_index < queue_family_count; ++family_index) {
      if (queue_families[family_index].queueFlags & VK_QUEUE_COMPUTE_BIT) {
         compute_queue_family = family_index;
         break;
      }
   }
   free(queue_families);
   if (compute_queue_family == UINT32_MAX) {
      fprintf(stderr, "No compute queue family\n");
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

   VkBufferCreateInfo const buffer_create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = TOTAL_DWORDS * sizeof(uint32_t),
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer buffer;
   CHECK_VK(vkCreateBuffer(device, &buffer_create_info, NULL, &buffer));
   VkMemoryRequirements memory_requirements;
   vkGetBufferMemoryRequirements(device, buffer, &memory_requirements);
   uint32_t const memory_type =
      find_host_memory_type(physical_device, memory_requirements.memoryTypeBits);
   if (memory_type == UINT32_MAX) {
      fprintf(stderr, "No host-visible coherent memory type\n");
      return 1;
   }
   VkMemoryAllocateInfo const memory_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = memory_requirements.size,
      .memoryTypeIndex = memory_type,
   };
   VkDeviceMemory memory;
   CHECK_VK(vkAllocateMemory(device, &memory_allocate_info, NULL, &memory));
   CHECK_VK(vkBindBufferMemory(device, buffer, memory, 0));

   uint32_t * mapped;
   CHECK_VK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
   uint32_t const allocation_dwords = memory_requirements.size / sizeof(uint32_t);
   for (uint32_t index = 0; index < allocation_dwords; ++index)
      mapped[index] = OUTER_CANARY;
   uint32_t * const region = mapped + PREFIX_DWORDS;
   region[0] = INNER_CANARY;
   for (uint32_t output_index = 0; output_index < OUTPUT_COUNT * OUTPUT_COMPONENTS; ++output_index)
      region[1 + output_index] = 1000 + output_index;
   region[REGION_DWORDS - 1] = INNER_CANARY;

   VkDescriptorSetLayoutBinding const layout_bindings[] = {
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      {
         .binding = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      {
         .binding = 2,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      {
         .binding = 3,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      {
         .binding = 4,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
   };
   VkDescriptorSetLayoutCreateInfo const set_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 5,
      .pBindings = layout_bindings,
   };
   VkDescriptorSetLayout set_layout;
   CHECK_VK(vkCreateDescriptorSetLayout(device, &set_layout_create_info, NULL, &set_layout));

   VkPipelineLayoutCreateInfo const pipeline_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges =
         &(VkPushConstantRange){
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = sizeof(uint32_t),
         },
   };
   VkPipelineLayout pipeline_layout;
   CHECK_VK(vkCreatePipelineLayout(device, &pipeline_layout_create_info, NULL, &pipeline_layout));

   VkDescriptorPoolSize const pool_sizes[] = {
      {
         .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .descriptorCount = 1,
      },
      {
         .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 4,
      },
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
   VkDescriptorBufferInfo const descriptor_buffer_infos[] = {
      {
         .buffer = buffer,
         .offset = PREFIX_DWORDS * sizeof(uint32_t),
         .range = sizeof(uint32_t),
      },
      {
         .buffer = buffer,
         .offset = PREFIX_DWORDS * sizeof(uint32_t),
         .range = REGION_DWORDS * sizeof(uint32_t),
      },
      {
         .buffer = buffer,
         .offset = PREFIX_DWORDS * sizeof(uint32_t),
         .range = REGION_DWORDS * sizeof(uint32_t),
      },
      {
         .buffer = buffer,
         .offset = PREFIX_DWORDS * sizeof(uint32_t),
         .range = REGION_DWORDS * sizeof(uint32_t),
      },
      {
      .buffer = buffer,
      .offset = PREFIX_DWORDS * sizeof(uint32_t),
      .range = REGION_DWORDS * sizeof(uint32_t),
      },
   };
   VkWriteDescriptorSet descriptor_writes[5];
   for (uint32_t binding = 0; binding < 5; ++binding) {
      descriptor_writes[binding] = (VkWriteDescriptorSet){
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_set,
         .dstBinding = binding,
         .descriptorCount = 1,
         .descriptorType =
            binding == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &descriptor_buffer_infos[binding],
      };
   }
   vkUpdateDescriptorSets(device, 5, descriptor_writes, 0, NULL);

   VkShaderModuleCreateInfo const shader_module_create_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(compute_loop_spirv),
      .pCode = compute_loop_spirv,
   };
   VkShaderModule shader_module;
   CHECK_VK(vkCreateShaderModule(device, &shader_module_create_info, NULL, &shader_module));
   VkComputePipelineCreateInfo const pipeline_create_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_COMPUTE_BIT,
         .module = shader_module,
         .pName = "main",
      },
      .layout = pipeline_layout,
   };
   VkPipeline pipeline;
   CHECK_VK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, NULL,
                                     &pipeline));

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
   VkCommandBufferBeginInfo const command_buffer_begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   CHECK_VK(vkBeginCommandBuffer(command_buffer, &command_buffer_begin_info));
   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                           &descriptor_set, 0, NULL);
   region[0] = OUTPUT_COUNT;
   uint32_t const iteration_count = ITERATION_COUNT;
   vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                      sizeof(iteration_count), &iteration_count);
   VkMemoryBarrier const inter_dispatch_barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
   };
   for (uint32_t dispatch_index = 0; dispatch_index < DISPATCH_COUNT; ++dispatch_index) {
      vkCmdDispatch(command_buffer, (dispatch_index & 1) ? 1 : 5, 1, 1);
      if (dispatch_index + 1 < DISPATCH_COUNT) {
         vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                              &inter_dispatch_barrier, 0, NULL, 0, NULL);
      }
   }
   VkMemoryBarrier const memory_barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);
   CHECK_VK(vkEndCommandBuffer(command_buffer));

   VkFenceCreateInfo const fence_create_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   };
   VkFence fence;
   CHECK_VK(vkCreateFence(device, &fence_create_info, NULL, &fence));
   VkSubmitInfo const submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
   };
   CHECK_VK(vkQueueSubmit(queue, 1, &submit_info, fence));
   VkResult const wait_result = vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ull);
   if (wait_result != VK_SUCCESS) {
      fprintf(stderr, "Compute fence wait failed with VkResult %d\n", wait_result);
      return 1;
   }

   bool pass = true;
   for (uint32_t index = 0; index < PREFIX_DWORDS; ++index)
      pass &= mapped[index] == OUTER_CANARY;
   pass &= region[0] == OUTPUT_COUNT;
   uint32_t const expected_base = expected_loop_value();
   for (uint32_t invocation = 0; invocation < OUTPUT_COUNT; ++invocation) {
      for (uint32_t component = 0; component < OUTPUT_COMPONENTS; ++component) {
         uint32_t const output_index = invocation * OUTPUT_COMPONENTS + component;
         uint32_t const actual = region[1 + output_index];
         uint32_t expected = 1000 + output_index;
         for (uint32_t dispatch_index = 0; dispatch_index < DISPATCH_COUNT; ++dispatch_index)
            expected = expected * 3 + expected_base;
         printf("output[%u][%u] = %u (expected %u)\n", invocation, component, actual, expected);
         pass &= actual == expected;
      }
   }
   pass &= region[REGION_DWORDS - 1] == INNER_CANARY;
   for (uint32_t index = PREFIX_DWORDS + REGION_DWORDS; index < TOTAL_DWORDS; ++index)
      pass &= mapped[index] == OUTER_CANARY;

   if (!pass) {
      for (uint32_t index = 0; index < allocation_dwords; ++index) {
         if (index < TOTAL_DWORDS || mapped[index] != OUTER_CANARY)
            fprintf(stderr, "mapped[%u] = 0x%08X\n", index, mapped[index]);
      }
   }

   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, command_pool, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyShaderModule(device, shader_module, NULL);
   vkDestroyDescriptorPool(device, descriptor_pool, NULL);
   vkDestroyPipelineLayout(device, pipeline_layout, NULL);
   vkDestroyDescriptorSetLayout(device, set_layout, NULL);
   vkUnmapMemory(device, memory);
   vkFreeMemory(device, memory, NULL);
   vkDestroyBuffer(device, buffer, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   printf("%s\n", pass ? "PASS" : "FAIL");
   return pass ? 0 : 1;
}
