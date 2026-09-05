/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* A characterization tool, not a pass/fail test, which is why it is built but not run by
 * bin/terakan-test.
 *
 * `dEQP-VK.glsl.texture_gather.*.cube.*` splits cleanly by format: every `rgba8` and `depth32f`
 * case passes and every `rgba8i` and `rgba8ui` case fails -- 46 of them, including the ones that
 * exclude the face corners, every wrap-mode combination, both sizes, and the base-level and
 * nearest-filter variants. The same integer formats gathered from a 2D or 2D-array image pass
 * 192 of 192. So it is neither the cube nor the integer format on its own.
 *
 * This gathers the same direction from two cube images holding the same numbers, one `R8G8B8A8_
 * UNORM` and one `R8G8B8A8_UINT`, so what comes back can be compared side by side. Texel (x, y)
 * of face f holds 16*f + 4*y + x in every channel, so a value names its face and its texel, and
 * an ordinary sample of the same direction is taken as well because that path works for both
 * formats.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "terakan_test_device.h"

#define CK(e)                                                                                      \
   do {                                                                                            \
      VkResult const r = (e);                                                                      \
      if (r) {                                                                                     \
         fprintf(stderr, "%s -> %d\n", #e, r);                                                     \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

static uint32_t const cube_gather_spirv[] = {
#include "terakan_cube_gather.spv.h"
};

#define FACE_SIZE    4u
#define RESULT_COUNT 6u

static uint32_t
memory_type(VkPhysicalDevice const physical_device, uint32_t const bits,
            VkMemoryPropertyFlags const flags)
{
   VkPhysicalDeviceMemoryProperties properties;
   vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
   for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
      if ((bits & (1u << index)) &&
          (properties.memoryTypes[index].propertyFlags & flags) == flags) {
         return index;
      }
   }
   return UINT32_MAX;
}

struct probe_direction {
   char const * name;
   float direction[4];
};

/* Directions chosen to land in the middle of a face, where no gather footprint can cross an edge,
 * and then near an edge and a corner, so a face-selection problem and an edge one are told apart.
 */
static struct probe_direction const probe_directions[] = {
   {"+X centre", {1.0f, 0.0f, 0.0f, 0.0f}},
   {"-X centre", {-1.0f, 0.0f, 0.0f, 0.0f}},
   {"+Y centre", {0.0f, 1.0f, 0.0f, 0.0f}},
   {"+Z centre", {0.0f, 0.0f, 1.0f, 0.0f}},
   {"+X quarter", {1.0f, -0.25f, 0.25f, 0.0f}},
   {"+Z quarter", {0.25f, -0.25f, 1.0f, 0.0f}},
   {"+X near edge", {1.0f, 0.0f, 0.9f, 0.0f}},
   {"+X near corner", {1.0f, 0.9f, 0.9f, 0.0f}},
};

int
main(void)
{
   VkApplicationInfo const application_info = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                               .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo const instance_create_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application_info};
   VkInstance instance;
   CK(vkCreateInstance(&instance_create_info, NULL, &instance));

   uint32_t physical_device_count = 8;
   VkPhysicalDevice physical_devices[8];
   CK(vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices));
   VkPhysicalDevice physical_device = VK_NULL_HANDLE;
   VkPhysicalDeviceProperties properties;
   for (uint32_t index = 0; index < physical_device_count; ++index) {
      vkGetPhysicalDeviceProperties(physical_devices[index], &properties);
      if (terakan_test_device_matches(properties.deviceName)) {
         physical_device = physical_devices[index];
         break;
      }
   }
   if (physical_device == VK_NULL_HANDLE) {
      fprintf(stderr, "No Terakan device found\n");
      return 77;
   }
   fprintf(stderr, "device=%s\n", properties.deviceName);

   float const queue_priority = 1.0f;
   VkDeviceQueueCreateInfo const queue_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueCount = 1,
      .pQueuePriorities = &queue_priority};
   VkDeviceCreateInfo const device_create_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                                  .queueCreateInfoCount = 1,
                                                  .pQueueCreateInfos = &queue_create_info};
   VkDevice device;
   CK(vkCreateDevice(physical_device, &device_create_info, NULL, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, 0, 0, &queue);

   VkFormat const formats[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT};
   VkImage images[2];
   VkDeviceMemory image_memories[2];
   VkImageView views[2];
   for (unsigned image_index = 0; image_index < 2; ++image_index) {
      VkImageCreateInfo const image_create_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = formats[image_index],
         .extent = {FACE_SIZE, FACE_SIZE, 1},
         .mipLevels = 1,
         .arrayLayers = 6,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
      CK(vkCreateImage(device, &image_create_info, NULL, &images[image_index]));
      VkMemoryRequirements requirements;
      vkGetImageMemoryRequirements(device, images[image_index], &requirements);
      VkMemoryAllocateInfo const allocate_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = requirements.size,
         .memoryTypeIndex = memory_type(physical_device, requirements.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
      CK(vkAllocateMemory(device, &allocate_info, NULL, &image_memories[image_index]));
      CK(vkBindImageMemory(device, images[image_index], image_memories[image_index], 0));
      VkImageViewCreateInfo const view_create_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = images[image_index],
         .viewType = VK_IMAGE_VIEW_TYPE_CUBE,
         .format = formats[image_index],
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6}};
      CK(vkCreateImageView(device, &view_create_info, NULL, &views[image_index]));
   }

   /* Texel (x, y) of face f holds 16*f + 4*y + x in every channel. */
   VkDeviceSize const upload_size = (VkDeviceSize)FACE_SIZE * FACE_SIZE * 6u * 4u;
   VkBufferCreateInfo const upload_create_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                                  .size = upload_size,
                                                  .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
   VkBuffer upload_buffer;
   CK(vkCreateBuffer(device, &upload_create_info, NULL, &upload_buffer));
   VkMemoryRequirements upload_requirements;
   vkGetBufferMemoryRequirements(device, upload_buffer, &upload_requirements);
   VkMemoryAllocateInfo const upload_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = upload_requirements.size,
      .memoryTypeIndex = memory_type(physical_device, upload_requirements.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
   VkDeviceMemory upload_memory;
   CK(vkAllocateMemory(device, &upload_allocate_info, NULL, &upload_memory));
   CK(vkBindBufferMemory(device, upload_buffer, upload_memory, 0));
   uint8_t * upload_map;
   CK(vkMapMemory(device, upload_memory, 0, VK_WHOLE_SIZE, 0, (void **)&upload_map));
   for (unsigned face = 0; face < 6; ++face) {
      for (unsigned y = 0; y < FACE_SIZE; ++y) {
         for (unsigned x = 0; x < FACE_SIZE; ++x) {
            uint8_t const value = (uint8_t)(16u * face + 4u * y + x);
            uint8_t * const texel =
               upload_map + 4u * ((face * FACE_SIZE + y) * FACE_SIZE + x);
            texel[0] = value;
            texel[1] = value;
            texel[2] = value;
            texel[3] = value;
         }
      }
   }

   VkDeviceSize const results_size = sizeof(uint32_t) * 4u * RESULT_COUNT;
   VkBufferCreateInfo const results_create_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                                   .size = results_size,
                                                   .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
   VkBuffer results_buffer;
   CK(vkCreateBuffer(device, &results_create_info, NULL, &results_buffer));
   VkMemoryRequirements results_requirements;
   vkGetBufferMemoryRequirements(device, results_buffer, &results_requirements);
   VkMemoryAllocateInfo const results_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = results_requirements.size,
      .memoryTypeIndex = memory_type(physical_device, results_requirements.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
   VkDeviceMemory results_memory;
   CK(vkAllocateMemory(device, &results_allocate_info, NULL, &results_memory));
   CK(vkBindBufferMemory(device, results_buffer, results_memory, 0));
   uint32_t * results;
   CK(vkMapMemory(device, results_memory, 0, VK_WHOLE_SIZE, 0, (void **)&results));

   VkSamplerCreateInfo const sampler_create_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
   VkSampler sampler;
   CK(vkCreateSampler(device, &sampler_create_info, NULL, &sampler));

   VkDescriptorSetLayoutBinding const bindings[] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
   VkDescriptorSetLayoutCreateInfo const set_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 3,
      .pBindings = bindings};
   VkDescriptorSetLayout set_layout;
   CK(vkCreateDescriptorSetLayout(device, &set_layout_create_info, NULL, &set_layout));
   VkPushConstantRange const push_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float) * 4};
   VkPipelineLayoutCreateInfo const pipeline_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push_range};
   VkPipelineLayout pipeline_layout;
   CK(vkCreatePipelineLayout(device, &pipeline_layout_create_info, NULL, &pipeline_layout));
   VkShaderModuleCreateInfo const module_create_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(cube_gather_spirv),
      .pCode = cube_gather_spirv};
   VkShaderModule module;
   CK(vkCreateShaderModule(device, &module_create_info, NULL, &module));
   VkComputePipelineCreateInfo const pipeline_create_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = module,
                .pName = "main"},
      .layout = pipeline_layout};
   VkPipeline pipeline;
   CK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, NULL, &pipeline));

   VkDescriptorPoolSize const pool_sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
                                              {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
   VkDescriptorPoolCreateInfo const pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 2,
      .pPoolSizes = pool_sizes};
   VkDescriptorPool descriptor_pool;
   CK(vkCreateDescriptorPool(device, &pool_create_info, NULL, &descriptor_pool));
   VkDescriptorSetAllocateInfo const set_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = descriptor_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &set_layout};
   VkDescriptorSet descriptor_set;
   CK(vkAllocateDescriptorSets(device, &set_allocate_info, &descriptor_set));
   VkDescriptorImageInfo const image_infos[2] = {
      {.sampler = sampler, .imageView = views[0], .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
      {.sampler = sampler, .imageView = views[1], .imageLayout = VK_IMAGE_LAYOUT_GENERAL}};
   VkDescriptorBufferInfo const results_info = {
      .buffer = results_buffer, .offset = 0, .range = VK_WHOLE_SIZE};
   VkWriteDescriptorSet const writes[] = {
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
       .dstSet = descriptor_set,
       .dstBinding = 0,
       .descriptorCount = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       .pImageInfo = &image_infos[0]},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
       .dstSet = descriptor_set,
       .dstBinding = 1,
       .descriptorCount = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       .pImageInfo = &image_infos[1]},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
       .dstSet = descriptor_set,
       .dstBinding = 2,
       .descriptorCount = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       .pBufferInfo = &results_info}};
   vkUpdateDescriptorSets(device, 3, writes, 0, NULL);

   VkCommandPoolCreateInfo const command_pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
   VkCommandPool command_pool;
   CK(vkCreateCommandPool(device, &command_pool_create_info, NULL, &command_pool));
   VkCommandBufferAllocateInfo const command_buffer_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = command_pool,
      .commandBufferCount = 1};
   VkCommandBuffer command_buffer;
   CK(vkAllocateCommandBuffers(device, &command_buffer_allocate_info, &command_buffer));
   VkFenceCreateInfo const fence_create_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   CK(vkCreateFence(device, &fence_create_info, NULL, &fence));

   printf("texel (x, y) of face f holds 16*f + 4*y + x\n");
   printf("%-16s %-9s %s\n", "direction", "fetch", "components");
   for (unsigned direction_index = 0;
        direction_index < sizeof(probe_directions) / sizeof(probe_directions[0]);
        ++direction_index) {
      memset(results, 0, (size_t)results_size);
      CK(vkResetCommandBuffer(command_buffer, 0));
      VkCommandBufferBeginInfo const begin_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      CK(vkBeginCommandBuffer(command_buffer, &begin_info));
      VkImageSubresourceRange const whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
      for (unsigned image_index = 0; image_index < 2; ++image_index) {
         VkImageMemoryBarrier const to_dst = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[image_index],
            .subresourceRange = whole};
         vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_dst);
         VkBufferImageCopy const region = {
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 6},
            .imageExtent = {FACE_SIZE, FACE_SIZE, 1}};
         vkCmdCopyBufferToImage(command_buffer, upload_buffer, images[image_index],
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
         VkImageMemoryBarrier const to_read = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[image_index],
            .subresourceRange = whole};
         vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1,
                              &to_read);
      }
      vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0,
                              1, &descriptor_set, 0, NULL);
      vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                         sizeof(float) * 4, probe_directions[direction_index].direction);
      vkCmdDispatch(command_buffer, 1, 1, 1);
      VkMemoryBarrier const to_host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                       .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                       .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, NULL, 0, NULL);
      CK(vkEndCommandBuffer(command_buffer));
      CK(vkResetFences(device, 1, &fence));
      VkSubmitInfo const submit_info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                        .commandBufferCount = 1,
                                        .pCommandBuffers = &command_buffer};
      CK(vkQueueSubmit(queue, 1, &submit_info, fence));
      if (vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull) != VK_SUCCESS) {
         fprintf(stderr, "%s: fence wait failed\n", probe_directions[direction_index].name);
         return 1;
      }

      static char const * const fetch_names[RESULT_COUNT] = {
         "gath.0 U", "gath.0 I", "gath.1 U", "gath.1 I", "sample U", "sample I"};
      for (unsigned result_index = 0; result_index < RESULT_COUNT; ++result_index) {
         printf("%-16s %-9s %3u %3u %3u %3u%s\n", probe_directions[direction_index].name,
                fetch_names[result_index], results[result_index * 4 + 0],
                results[result_index * 4 + 1], results[result_index * 4 + 2],
                results[result_index * 4 + 3],
                (result_index % 2 == 1 &&
                 memcmp(&results[result_index * 4], &results[(result_index - 1) * 4],
                        sizeof(uint32_t) * 4) != 0)
                   ? "   <-- DIFFERS FROM UNORM"
                   : "");
      }
   }

   vkDeviceWaitIdle(device);
   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, command_pool, NULL);
   vkDestroyDescriptorPool(device, descriptor_pool, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyShaderModule(device, module, NULL);
   vkDestroyPipelineLayout(device, pipeline_layout, NULL);
   vkDestroyDescriptorSetLayout(device, set_layout, NULL);
   vkDestroySampler(device, sampler, NULL);
   vkUnmapMemory(device, results_memory);
   vkDestroyBuffer(device, results_buffer, NULL);
   vkFreeMemory(device, results_memory, NULL);
   vkUnmapMemory(device, upload_memory);
   vkDestroyBuffer(device, upload_buffer, NULL);
   vkFreeMemory(device, upload_memory, NULL);
   for (unsigned image_index = 0; image_index < 2; ++image_index) {
      vkDestroyImageView(device, views[image_index], NULL);
      vkDestroyImage(device, images[image_index], NULL);
      vkFreeMemory(device, image_memories[image_index], NULL);
   }
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   return 0;
}
