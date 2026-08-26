/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* #MemoryIntegrity for the colour/UAV side of VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC.
 *
 * terakan_dynamic_offset_bounds covers the resource side, where a buffer's extent is one SIZE field
 * that a dynamic offset only has to reclamp. A buffer UAV states the same extent as three fields
 * that only mean anything together: BASE is the descriptor's alignment granularity floor, VIEW the
 * byte distance from there to the start of the range, and DIM the inclusive index of the last
 * element counted from BASE rather than from VIEW. A dynamic offset therefore has to rebuild all
 * three; moving BASE alone leaves DIM describing the old, further end, and hands the shader the
 * dynamic offset's worth of memory past the range to write into.
 *
 * The buffer is placed at the start of a much larger VkDeviceMemory allocation whose remainder is
 * poisoned and acts as a guard region, so a UAV that reaches past the buffer writes somewhere the
 * host can see. The shader writes every element of the nominal range, stamping each with its own
 * index.
 *
 * Three things are then checked for each dynamic offset, and the second and third are what a
 * bounds bug breaks: that the elements that do fit landed at the right addresses, that nothing
 * below the shifted window was touched, and that the guard region is intact.
 *
 * The offsets deliberately include ones that are not multiples of the UAV's base granularity, so
 * the VIEW half of the rebuild is exercised and not just BASE, and one that is past the end of the
 * buffer entirely.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The buffer is the first 256 bytes; everything after it in the allocation is guard. */
#define BUFFER_SIZE_BYTES 256u
#define BUFFER_WORDS (BUFFER_SIZE_BYTES / 4u)
#define ALLOCATION_BYTES 4096u
#define ALLOCATION_WORDS (ALLOCATION_BYTES / 4u)

#define POISON_PATTERN 0xBAADF00Du
#define MARKER_BASE 0xABCD0000u

#define CHECK_VK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_vk_result = (expression);                                               \
      if (check_vk_result != VK_SUCCESS) {                                                         \
         fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_vk_result);            \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

static const uint32_t dynamic_uav_bounds_spirv[] = {
#include "terakan_dynamic_uav_bounds.spv.h"
};

/* 0 is the unshifted baseline. 4 is the smallest offset the driver advertises, and neither 4, 192
 * nor 252 is a multiple of any plausible UAV base granularity, so VIEW has to carry the remainder.
 * 252 leaves room for exactly one element, and 256 lands on the end of the buffer with no room at
 * all.
 */
static const uint32_t dynamic_offsets[] = {0u, 4u, 192u, 252u, 256u};

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
      .pApplicationName = "terakan-dynamic-uav-bounds-test",
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
   if (physical_device_count == 0) {
      fprintf(stderr, "No physical devices\n");
      return 1;
   }
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
   fprintf(stderr, "device=%s queue_family=%u buffer=%u bytes allocation=%u bytes\n",
           properties.deviceName, compute_queue_family, BUFFER_SIZE_BYTES, ALLOCATION_BYTES);

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

   /* The buffer declares only its own 256 bytes, but lives at the start of a far larger allocation
    * so that the guard region physically follows it in the same VkDeviceMemory.
    */
   VkBufferCreateInfo const buffer_create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = BUFFER_SIZE_BYTES,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer target_buffer;
   CHECK_VK(vkCreateBuffer(device, &buffer_create_info, NULL, &target_buffer));
   VkMemoryRequirements target_requirements;
   vkGetBufferMemoryRequirements(device, target_buffer, &target_requirements);
   if (target_requirements.size > ALLOCATION_BYTES) {
      fprintf(stderr, "The buffer needs %llu bytes, more than the whole allocation\n",
              (unsigned long long)target_requirements.size);
      return 1;
   }
   uint32_t const memory_type = find_host_memory_type(physical_device,
                                                      target_requirements.memoryTypeBits);
   if (memory_type == UINT32_MAX) {
      fprintf(stderr, "No host-visible memory type for the buffer\n");
      return 1;
   }
   VkMemoryAllocateInfo const allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = ALLOCATION_BYTES,
      .memoryTypeIndex = memory_type,
   };
   VkDeviceMemory memory;
   CHECK_VK(vkAllocateMemory(device, &allocate_info, NULL, &memory));
   CHECK_VK(vkBindBufferMemory(device, target_buffer, memory, 0));
   uint32_t * mapping;
   CHECK_VK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&mapping));

   VkDescriptorSetLayoutBinding const binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
   };
   VkDescriptorSetLayoutCreateInfo const set_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &binding,
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
      .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
      .descriptorCount = 1,
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
   /* The static range covers the whole real buffer; the dynamic offset is what moves it. */
   VkDescriptorBufferInfo const buffer_info = {
      .buffer = target_buffer,
      .offset = 0,
      .range = BUFFER_SIZE_BYTES,
   };
   VkWriteDescriptorSet const write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = descriptor_set,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
      .pBufferInfo = &buffer_info,
   };
   vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

   VkShaderModuleCreateInfo const shader_module_create_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(dynamic_uav_bounds_spirv),
      .pCode = dynamic_uav_bounds_spirv,
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
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
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

   unsigned failures = 0;
   for (unsigned case_index = 0; case_index < sizeof(dynamic_offsets) / sizeof(dynamic_offsets[0]);
        ++case_index) {
      uint32_t const dynamic_offset = dynamic_offsets[case_index];

      for (uint32_t word_index = 0; word_index < ALLOCATION_WORDS; ++word_index)
         mapping[word_index] = POISON_PATTERN;

      CHECK_VK(vkResetCommandBuffer(command_buffer, 0));
      VkCommandBufferBeginInfo const begin_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      };
      CHECK_VK(vkBeginCommandBuffer(command_buffer, &begin_info));
      vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                              &descriptor_set, 1, &dynamic_offset);
      vkCmdDispatch(command_buffer, 1, 1, 1);
      VkMemoryBarrier const host_ready = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_ready, 0, NULL, 0, NULL);
      CHECK_VK(vkEndCommandBuffer(command_buffer));

      CHECK_VK(vkResetFences(device, 1, &fence));
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

      /* The guard region must be intact for every offset, including the one with no room at all. */
      unsigned guard_touched = 0;
      for (uint32_t word_index = BUFFER_WORDS; word_index < ALLOCATION_WORDS; ++word_index) {
         if (mapping[word_index] != POISON_PATTERN) {
            if (guard_touched == 0) {
               fprintf(stderr,
                       "dynamic offset %u: guard word %u (byte %u, %u past the end of the buffer) "
                       "= 0x%08X, expected the poison 0x%08X\n",
                       dynamic_offset, word_index, word_index * 4u,
                       word_index * 4u - BUFFER_SIZE_BYTES, mapping[word_index], POISON_PATTERN);
            }
            ++guard_touched;
         }
      }
      if (guard_touched != 0) {
         fprintf(stderr,
                 "dynamic offset %u: %u guard words written -- DIM was not rebuilt for the offset, "
                 "so the UAV still reached to the range's old end\n",
                 dynamic_offset, guard_touched);
         ++failures;
      }

      if (dynamic_offset >= BUFFER_SIZE_BYTES) {
         /* Nothing fits. Where the driver puts the unavoidable minimal window is its own business;
          * the only requirement is the guard check above.
          */
         if (guard_touched == 0) {
            fprintf(stderr, "dynamic offset %u: guard intact (no room in the buffer)\n",
                    dynamic_offset);
         }
         continue;
      }

      /* Everything below the shifted window must be untouched, and every element that fits must be
       * where the offset puts it, carrying its own index.
       */
      unsigned placement_failures = 0;
      uint32_t const first_word = dynamic_offset / 4u;
      for (uint32_t word_index = 0; word_index < BUFFER_WORDS; ++word_index) {
         uint32_t const expected = word_index < first_word
                                      ? POISON_PATTERN
                                      : (MARKER_BASE | (word_index - first_word));
         if (mapping[word_index] != expected) {
            if (placement_failures == 0) {
               fprintf(stderr,
                       "dynamic offset %u: word %u (byte %u) = 0x%08X, expected 0x%08X%s\n",
                       dynamic_offset, word_index, word_index * 4u, mapping[word_index], expected,
                       word_index < first_word ? " (below the shifted window)" : "");
            }
            ++placement_failures;
         }
      }
      if (placement_failures != 0) {
         ++failures;
      } else {
         fprintf(stderr, "dynamic offset %u: %u words placed correctly, guard intact\n",
                 dynamic_offset, BUFFER_WORDS - first_word);
      }
   }

   printf("dynamic_uav_bounds cases=%zu failures=%u %s\n",
          sizeof(dynamic_offsets) / sizeof(dynamic_offsets[0]), failures,
          failures == 0 ? "PASS" : "FAIL");

   vkDeviceWaitIdle(device);
   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, command_pool, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyShaderModule(device, shader_module, NULL);
   vkDestroyDescriptorPool(device, descriptor_pool, NULL);
   vkDestroyPipelineLayout(device, pipeline_layout, NULL);
   vkDestroyDescriptorSetLayout(device, set_layout, NULL);
   vkUnmapMemory(device, memory);
   vkDestroyBuffer(device, target_buffer, NULL);
   vkFreeMemory(device, memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   return failures == 0 ? 0 : 1;
}
