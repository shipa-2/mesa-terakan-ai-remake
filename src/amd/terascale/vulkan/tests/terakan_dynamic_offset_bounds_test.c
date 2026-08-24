/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Regression test for #MemoryIntegrity: a VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC descriptor is
 * written with a static range covering the whole (small) real buffer. A dynamic offset is then
 * applied at bind time that is deliberately invalid (offset + dynamicOffset + range exceeds the
 * real VkBuffer size), pushing the nominal window past the end of the buffer into a guard region
 * poisoned with a distinctive pattern living in the same VkDeviceMemory allocation, immediately
 * past the buffer's declared size.
 *
 * The shader reads the last dword of the nominal (pre-dynamic-offset) range. If the hardware SIZE
 * field is not reclamped against the buffer's real remaining extent when the dynamic offset is
 * applied, that read lands squarely in the poisoned guard region and returns the poison pattern.
 * If it is correctly clamped, the read is entirely out of the (now much smaller) valid range and
 * must return 0 per this hardware's documented out-of-bounds behavior.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BUFFER_SIZE_BYTES 64u
#define POISON_REGION_BYTES 64u
#define ALLOCATION_BYTES (BUFFER_SIZE_BYTES + POISON_REGION_BYTES)
#define DYNAMIC_OFFSET_BYTES 48u
#define POISON_PATTERN 0xBAADF00Du
#define OUTPUT_SENTINEL 0x5A5A5A5Au

#define CHECK_VK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_vk_result = (expression);                                               \
      if (check_vk_result != VK_SUCCESS) {                                                         \
         fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_vk_result);            \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

static const uint32_t dynamic_offset_bounds_spirv[] = {
#include "terakan_dynamic_offset_bounds.spv.h"
};

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
      .pApplicationName = "terakan-dynamic-offset-bounds-test",
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo const instance_create_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application_info,
   };
   VkInstance instance;
   CHECK_VK(vkCreateInstance(&instance_create_info, NULL, &instance));

   /* More than one physical device can enumerate as "(Terakan)" now that TeraScale 1 (R600/R700)
    * devices are recognized: they enumerate and report properties, but cannot yet create a device
    * (see terakan_physical_device_chip_info::is_terascale_1 and terakan_CreateDevice). Look through
    * every enumerated device for one this driver can actually create, rather than assuming there is
    * exactly one Terakan-capable device or that the first one enumerated is usable.
    */
   uint32_t physical_device_count = 0;
   CHECK_VK(vkEnumeratePhysicalDevices(instance, &physical_device_count, NULL));
   VkPhysicalDevice physical_devices[8];
   if (physical_device_count > 8) {
      physical_device_count = 8;
   }
   CHECK_VK(vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices));
   VkPhysicalDevice physical_device = VK_NULL_HANDLE;
   for (uint32_t device_index = 0; device_index < physical_device_count; ++device_index) {
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(physical_devices[device_index], &properties);
      /* TeraScale 1 (R600/R700) devices are also named "... (Terakan)" now that they enumerate,
       * but cannot create a device yet (see the comment above), so they are excluded by their
       * "TeraScale 1" name prefix rather than picked and failed on below.
       */
      if (strstr(properties.deviceName, "(Terakan)") == NULL ||
          strstr(properties.deviceName, "TeraScale 1") != NULL) {
         continue;
      }
      physical_device = physical_devices[device_index];
      break;
   }
   if (physical_device == VK_NULL_HANDLE) {
      fprintf(stderr, "No usable Terakan physical device found among %u enumerated\n",
              physical_device_count);
      return 1;
   }

   uint32_t queue_family_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, NULL);
   VkQueueFamilyProperties queue_families[16];
   if (queue_family_count > 16)
      queue_family_count = 16;
   vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_families);
   uint32_t compute_queue_family = UINT32_MAX;
   for (uint32_t family_index = 0; family_index < queue_family_count; ++family_index) {
      if (queue_families[family_index].queueFlags & VK_QUEUE_COMPUTE_BIT) {
         compute_queue_family = family_index;
         break;
      }
   }
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

   /* Source buffer: its declared VkBuffer size is BUFFER_SIZE_BYTES, but it is bound at offset 0
    * of a larger VkDeviceMemory allocation so the guard region physically follows it in the same
    * memory, reachable by the GPU through the same BO.
    */
   VkBufferCreateInfo const source_buffer_create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = BUFFER_SIZE_BYTES,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer source_buffer;
   CHECK_VK(vkCreateBuffer(device, &source_buffer_create_info, NULL, &source_buffer));
   VkMemoryRequirements source_requirements;
   vkGetBufferMemoryRequirements(device, source_buffer, &source_requirements);
   uint32_t const source_memory_type =
      find_host_memory_type(physical_device, source_requirements.memoryTypeBits);
   if (source_memory_type == UINT32_MAX) {
      fprintf(stderr, "No host-visible coherent memory type\n");
      return 1;
   }
   VkDeviceSize const source_allocation_size =
      source_requirements.size > ALLOCATION_BYTES ? source_requirements.size : ALLOCATION_BYTES;
   VkMemoryAllocateInfo const source_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = source_allocation_size,
      .memoryTypeIndex = source_memory_type,
   };
   VkDeviceMemory source_memory;
   CHECK_VK(vkAllocateMemory(device, &source_allocate_info, NULL, &source_memory));
   CHECK_VK(vkBindBufferMemory(device, source_buffer, source_memory, 0));

   uint8_t * source_mapped;
   CHECK_VK(vkMapMemory(device, source_memory, 0, VK_WHOLE_SIZE, 0, (void **)&source_mapped));
   for (uint32_t byte_index = 0; byte_index < BUFFER_SIZE_BYTES; ++byte_index)
      source_mapped[byte_index] = 0;
   uint32_t * const poison = (uint32_t *)(source_mapped + BUFFER_SIZE_BYTES);
   for (uint32_t word_index = 0; word_index < POISON_REGION_BYTES / sizeof(uint32_t); ++word_index)
      poison[word_index] = POISON_PATTERN;

   /* Separate output buffer, unrelated to the guard region above. */
   VkBufferCreateInfo const output_buffer_create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = sizeof(uint32_t),
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer output_buffer;
   CHECK_VK(vkCreateBuffer(device, &output_buffer_create_info, NULL, &output_buffer));
   VkMemoryRequirements output_requirements;
   vkGetBufferMemoryRequirements(device, output_buffer, &output_requirements);
   uint32_t const output_memory_type =
      find_host_memory_type(physical_device, output_requirements.memoryTypeBits);
   if (output_memory_type == UINT32_MAX) {
      fprintf(stderr, "No host-visible coherent memory type for output\n");
      return 1;
   }
   VkMemoryAllocateInfo const output_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = output_requirements.size,
      .memoryTypeIndex = output_memory_type,
   };
   VkDeviceMemory output_memory;
   CHECK_VK(vkAllocateMemory(device, &output_allocate_info, NULL, &output_memory));
   CHECK_VK(vkBindBufferMemory(device, output_buffer, output_memory, 0));
   uint32_t * output_mapped;
   CHECK_VK(vkMapMemory(device, output_memory, 0, VK_WHOLE_SIZE, 0, (void **)&output_mapped));
   *output_mapped = OUTPUT_SENTINEL;

   VkDescriptorSetLayoutBinding const layout_bindings[] = {
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
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

   VkDescriptorPoolSize const pool_sizes[] = {
      {
         .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
         .descriptorCount = 1,
      },
      {
         .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
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

   /* The static range covers the whole real buffer, which is valid on its own
    * (VUID-VkWriteDescriptorSet-descriptorType-00340: offset + range <= buffer size, 0 + 64 <= 64).
    * The violation only appears once the dynamic offset is applied at bind time below.
    */
   VkDescriptorBufferInfo const source_buffer_info = {
      .buffer = source_buffer,
      .offset = 0,
      .range = BUFFER_SIZE_BYTES,
   };
   VkDescriptorBufferInfo const output_buffer_info = {
      .buffer = output_buffer,
      .offset = 0,
      .range = sizeof(uint32_t),
   };
   VkWriteDescriptorSet const descriptor_writes[] = {
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_set,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
         .pBufferInfo = &source_buffer_info,
      },
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_set,
         .dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &output_buffer_info,
      },
   };
   vkUpdateDescriptorSets(device, 2, descriptor_writes, 0, NULL);

   VkShaderModuleCreateInfo const shader_module_create_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(dynamic_offset_bounds_spirv),
      .pCode = dynamic_offset_bounds_spirv,
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
   /* Deliberately invalid per the Vulkan spec (offset + dynamicOffset + range > buffer size):
    * this is exactly the application misuse #MemoryIntegrity clamping must survive gracefully.
    */
   uint32_t const dynamic_offset = DYNAMIC_OFFSET_BYTES;
   vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                           &descriptor_set, 1, &dynamic_offset);
   vkCmdDispatch(command_buffer, 1, 1, 1);
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

   uint32_t const result = *output_mapped;
   bool const pass = result != POISON_PATTERN;
   printf("result = 0x%08X (poison = 0x%08X)\n", result, POISON_PATTERN);
   if (!pass) {
      fprintf(stderr,
              "Out-of-bounds read past a dynamically-offset descriptor's real buffer extent "
              "returned the guard region's poison pattern instead of being clamped\n");
   }

   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, command_pool, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyShaderModule(device, shader_module, NULL);
   vkDestroyDescriptorPool(device, descriptor_pool, NULL);
   vkDestroyPipelineLayout(device, pipeline_layout, NULL);
   vkDestroyDescriptorSetLayout(device, set_layout, NULL);
   vkUnmapMemory(device, output_memory);
   vkFreeMemory(device, output_memory, NULL);
   vkDestroyBuffer(device, output_buffer, NULL);
   vkUnmapMemory(device, source_memory);
   vkFreeMemory(device, source_memory, NULL);
   vkDestroyBuffer(device, source_buffer, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   printf("%s\n", pass ? "PASS" : "FAIL");
   return pass ? 0 : 1;
}
