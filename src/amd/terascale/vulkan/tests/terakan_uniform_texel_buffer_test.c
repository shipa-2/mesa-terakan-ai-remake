/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* texelFetch on a uniform texel buffer.
 *
 * This had no coverage at all, and did not work: a sampled buffer -- GLSL's textureBuffer, SPIR-V's
 * Dim=Buffer with Sampled=1 -- reaches the binding lowering as an ordinary texture instruction, and
 * that pass asked for a VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE binding. A uniform texel buffer is not one,
 * so the type check failed, the fetch was lowered to a null descriptor, and every read returned zero
 * for every format. Silently: the shader compiled, the draw ran, the answer was zeros.
 *
 * DXVK maps D3D11 typed buffer SRVs onto uniform texel buffers, so this was not an exotic corner.
 *
 * Two formats are read here, both integer so the comparison is exact: R32_UINT for the
 * single-channel case, where the fetch must also expand to (x, 0, 0, 1), and R8G8B8A8_UINT for the
 * four-channel case, where all four components carry data and the byte order matters.
 *
 * Three-component formats are deliberately not tested. The hardware does not fetch 8_8_8 or
 * 16_16_16 from a buffer correctly -- see the comment in terakan_nir_buffer.c recording that they
 * return completely invalid values -- and they are what remains failing in
 * dEQP-VK.api.buffer_view.access.uniform_texel_buffer.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "terakan_test_device.h"

#define TEXEL_COUNT 8u
#define SOURCE_WORDS 64u

#define CHECK_VK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_vk_result = (expression);                                               \
      if (check_vk_result != VK_SUCCESS) {                                                         \
         fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_vk_result);            \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

static const uint32_t uniform_texel_buffer_spirv[] = {
#include "terakan_uniform_texel_buffer.spv.h"

};

/* The source word at index i. Every byte differs from every other byte in the same word, so a
 * component fetched from the wrong offset is visible rather than coincidentally right.
 */
static uint32_t
source_word(uint32_t const index)
{
   return (0x11u + index * 4u) | ((0x12u + index * 4u) << 8) |
          ((0x13u + index * 4u) << 16) | ((0x14u + index * 4u) << 24);
}

struct texel_case {
   char const * name;
   VkFormat format;
   /* Expected components of texel `index`. */
   void (*expected)(uint32_t index, uint32_t out[4]);
};

static void
expected_r32_uint(uint32_t const index, uint32_t out[4])
{
   out[0] = source_word(index);
   out[1] = 0;
   out[2] = 0;
   out[3] = 1;
}

static void
expected_r8g8b8a8_uint(uint32_t const index, uint32_t out[4])
{
   uint32_t const word = source_word(index);
   for (int component = 0; component < 4; ++component)
      out[component] = (word >> (8 * component)) & 0xFFu;
}

static const struct texel_case texel_cases[] = {
   {"r32_uint", VK_FORMAT_R32_UINT, expected_r32_uint},
   {"r8g8b8a8_uint", VK_FORMAT_R8G8B8A8_UINT, expected_r8g8b8a8_uint},
};
#define TEXEL_CASE_COUNT (sizeof(texel_cases) / sizeof(texel_cases[0]))

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
      .pApplicationName = "terakan-uniform-texel-buffer-test",
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
      return TERAKAN_TEST_DEVICE_NOT_FOUND_STATUS;
   }
   fprintf(stderr, "device=%s queue_family=%u\n", properties.deviceName, compute_queue_family);

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

   VkBuffer source_buffer, results_buffer;
   VkDeviceMemory source_memory, results_memory;
   uint32_t * source_mapping;
   uint32_t * results_mapping;
   {
      VkDeviceSize const sizes[2] = {SOURCE_WORDS * 4u, TEXEL_COUNT * 4u * 4u};
      VkBufferUsageFlags const usages[2] = {VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
      VkBuffer * const buffers[2] = {&source_buffer, &results_buffer};
      VkDeviceMemory * const memories[2] = {&source_memory, &results_memory};
      uint32_t ** const mappings[2] = {&source_mapping, &results_mapping};
      for (int i = 0; i < 2; ++i) {
         VkBufferCreateInfo const buffer_create_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = sizes[i],
            .usage = usages[i],
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         };
         CHECK_VK(vkCreateBuffer(device, &buffer_create_info, NULL, buffers[i]));
         VkMemoryRequirements requirements;
         vkGetBufferMemoryRequirements(device, *buffers[i], &requirements);
         VkMemoryAllocateInfo const allocate_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = find_memory_type(physical_device, requirements.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
         };
         CHECK_VK(vkAllocateMemory(device, &allocate_info, NULL, memories[i]));
         CHECK_VK(vkBindBufferMemory(device, *buffers[i], *memories[i], 0));
         CHECK_VK(vkMapMemory(device, *memories[i], 0, VK_WHOLE_SIZE, 0, (void **)mappings[i]));
      }
   }
   for (uint32_t i = 0; i < SOURCE_WORDS; ++i)
      source_mapping[i] = source_word(i);

   VkDescriptorSetLayoutBinding const bindings[2] = {
      {.binding = 0,
       .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
       .descriptorCount = 1,
       .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
      {.binding = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       .descriptorCount = 1,
       .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
   };
   VkDescriptorSetLayoutCreateInfo const set_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2,
      .pBindings = bindings,
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
      {.type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .descriptorCount = TEXEL_CASE_COUNT},
      {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = TEXEL_CASE_COUNT},
   };
   VkDescriptorPoolCreateInfo const pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = TEXEL_CASE_COUNT,
      .poolSizeCount = 2,
      .pPoolSizes = pool_sizes,
   };
   VkDescriptorPool descriptor_pool;
   CHECK_VK(vkCreateDescriptorPool(device, &pool_create_info, NULL, &descriptor_pool));

   VkShaderModuleCreateInfo const shader_module_create_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(uniform_texel_buffer_spirv),
      .pCode = uniform_texel_buffer_spirv,
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
   for (unsigned case_index = 0; case_index < TEXEL_CASE_COUNT; ++case_index) {
      struct texel_case const * const texel_case = &texel_cases[case_index];

      VkFormatProperties format_properties;
      vkGetPhysicalDeviceFormatProperties(physical_device, texel_case->format, &format_properties);
      if (!(format_properties.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT)) {
         fprintf(stderr, "%s is not advertised as a uniform texel buffer\n", texel_case->name);
         ++failures;
         continue;
      }

      VkBufferViewCreateInfo const view_create_info = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
         .buffer = source_buffer,
         .format = texel_case->format,
         .offset = 0,
         .range = VK_WHOLE_SIZE,
      };
      VkBufferView buffer_view;
      CHECK_VK(vkCreateBufferView(device, &view_create_info, NULL, &buffer_view));

      VkDescriptorSetAllocateInfo const set_allocate_info = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
         .descriptorPool = descriptor_pool,
         .descriptorSetCount = 1,
         .pSetLayouts = &set_layout,
      };
      VkDescriptorSet descriptor_set;
      CHECK_VK(vkAllocateDescriptorSets(device, &set_allocate_info, &descriptor_set));
      VkDescriptorBufferInfo const results_info = {
         .buffer = results_buffer, .offset = 0, .range = TEXEL_COUNT * 4u * 4u,
      };
      VkWriteDescriptorSet const writes[2] = {
         {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = descriptor_set,
          .dstBinding = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
          .pTexelBufferView = &buffer_view},
         {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = descriptor_set,
          .dstBinding = 1,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &results_info},
      };
      vkUpdateDescriptorSets(device, 2, writes, 0, NULL);

      memset(results_mapping, 0xEE, TEXEL_COUNT * 4u * 4u);

      CHECK_VK(vkResetCommandBuffer(command_buffer, 0));
      VkCommandBufferBeginInfo const begin_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      };
      CHECK_VK(vkBeginCommandBuffer(command_buffer, &begin_info));
      vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                              &descriptor_set, 0, NULL);
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

      unsigned case_failures = 0;
      unsigned zero_texels = 0;
      for (uint32_t texel = 0; texel < TEXEL_COUNT; ++texel) {
         uint32_t expected[4];
         texel_case->expected(texel, expected);
         uint32_t const * const actual = results_mapping + texel * 4u;
         bool matches = true;
         for (int component = 0; component < 4; ++component) {
            if (actual[component] != expected[component])
               matches = false;
         }
         if (actual[0] == 0 && actual[1] == 0 && actual[2] == 0 && actual[3] == 0)
            ++zero_texels;
         if (matches)
            continue;
         if (case_failures == 0) {
            fprintf(stderr, "%s texel %u = %08X %08X %08X %08X, expected %08X %08X %08X %08X\n",
                    texel_case->name, texel, actual[0], actual[1], actual[2], actual[3],
                    expected[0], expected[1], expected[2], expected[3]);
         }
         ++case_failures;
      }
      if (case_failures != 0) {
         if (zero_texels == TEXEL_COUNT) {
            fprintf(stderr,
                    "  every texel is zero: the fetch was lowered to a null descriptor rather than "
                    "reaching the buffer view\n");
         }
         ++failures;
      } else {
         fprintf(stderr, "%s: %u texels read correctly\n", texel_case->name, TEXEL_COUNT);
      }

      vkDestroyBufferView(device, buffer_view, NULL);
   }

   printf("uniform_texel_buffer formats=%zu failures=%u %s\n", TEXEL_CASE_COUNT, failures,
          failures == 0 ? "PASS" : "FAIL");

   vkDeviceWaitIdle(device);
   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, command_pool, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyShaderModule(device, shader_module, NULL);
   vkDestroyDescriptorPool(device, descriptor_pool, NULL);
   vkDestroyPipelineLayout(device, pipeline_layout, NULL);
   vkDestroyDescriptorSetLayout(device, set_layout, NULL);
   vkUnmapMemory(device, source_memory);
   vkDestroyBuffer(device, source_buffer, NULL);
   vkFreeMemory(device, source_memory, NULL);
   vkUnmapMemory(device, results_memory);
   vkDestroyBuffer(device, results_buffer, NULL);
   vkFreeMemory(device, results_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   return failures == 0 ? 0 : 1;
}
