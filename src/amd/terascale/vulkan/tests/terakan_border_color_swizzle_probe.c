/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* A characterization tool, not a pass/fail test, which is why it is built but not run by
 * bin/terakan-test.
 *
 * What `DST_SEL` does to the border colour is the last unexplained part of
 * `dEQP-VK.pipeline.*.sampler.border_swizzle`. That test cannot answer it: its border colours are
 * `(1,1,1,1)`, `(0,0,0,1)` and `(0,0,0,0)`, so a permutation of the components is invisible and
 * only a constant in the swizzle shows up at all.
 *
 * So this writes four *different* values into the per-sampler border colour registers -- through
 * `TERAKAN_BORDER_PROBE_VALUES`, which exists for this -- and samples far outside the image
 * through views with a range of component mappings. The value that comes back names the register
 * it came from, so the mapping the hardware applies is read off rather than inferred.
 *
 * Both an ordinary sample and a gather of each component are taken, because the gather path is
 * already known to read `DST_SEL` differently from an ordinary sample on this hardware.
 *
 * The same four gathers are also taken well inside the image, where the border colour cannot
 * reach. That is the only way to ask which *channel* a gather read: the border colour path applies
 * the swizzle twice, so it cannot answer it. With every texel holding R=0.125, G=0.25, B=0.375 and
 * A=0.5, a gathered value names its channel. What comes back, against what the swizzle asks for:
 *
 *     rgba -> RGBA    rgb0 -> RGB0    rg0a -> RG0A    a01r -> A01R
 *     bgra -> BGRA    rgb1 -> RGB1    rg1a -> RG1A    gba0 -> GBA0
 *     0gba -> 0GBA    r0ba -> R0BA
 *
 *     argb -> ABGR    1gba -> 1BAR    1rgb -> 1BAR    ba01 -> BG01    01rg -> 01BA
 *
 * Ten of fifteen are right, including every swizzle whose constants are trailing. The five that
 * are wrong do not follow a rule these measurements settle: `1gba` and `1rgb` return the *same*
 * four channels despite asking for different ones, so the hardware is not reading `DST_SEL` for
 * those components at all, and no shift, inverse or double application accounts for `argb` and
 * `ba01` at once.
 *
 * This is why gathering all four components and selecting at run time does not close it. Selecting
 * requires knowing which component of the instruction reaches which channel, and that is exactly
 * what does not follow a rule. A gather through a second descriptor with an identity `DST_SEL`
 * would sidestep it -- the identity row above is correct -- and the shader could then apply the
 * whole swizzle itself, constants included, from the push-constant masks that already exist.
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

static uint32_t const border_color_swizzle_spirv[] = {
#include "terakan_border_color_swizzle.spv.h"
};

#define RESULT_COUNT 10u
#define IMAGE_SIZE   8u

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

struct probe_swizzle {
   char const * name;
   VkComponentMapping mapping;
};

#define SWZ(r, g, b, a)                                                                            \
   {VK_COMPONENT_SWIZZLE_##r, VK_COMPONENT_SWIZZLE_##g, VK_COMPONENT_SWIZZLE_##b,                  \
    VK_COMPONENT_SWIZZLE_##a}

static struct probe_swizzle const probe_swizzles[] = {
   {"rgba", SWZ(R, G, B, A)},
   {"argb", SWZ(A, R, G, B)},
   {"bgra", SWZ(B, G, R, A)},
   {"rgb0", SWZ(R, G, B, ZERO)},
   {"rgb1", SWZ(R, G, B, ONE)},
   {"0gba", SWZ(ZERO, G, B, A)},
   {"1gba", SWZ(ONE, G, B, A)},
   {"rg0a", SWZ(R, G, ZERO, A)},
   {"rg1a", SWZ(R, G, ONE, A)},
   {"r0ba", SWZ(R, ZERO, B, A)},
   /* The four dEQP names its failing cases after. */
   {"1rgb", SWZ(ONE, R, G, B)},
   {"ba01", SWZ(B, A, ZERO, ONE)},
   {"01rg", SWZ(ZERO, ONE, R, G)},
   {"a01r", SWZ(A, ZERO, ONE, R)},
   {"gba0", SWZ(G, B, A, ZERO)},
};

int
main(void)
{
   /* Four values far enough apart that the eighth of a unorm8 step between them cannot be
    * confused, and each one names its register: 0.1 is red, 0.2 green, 0.3 blue, 0.4 alpha.
    */
   if (getenv("TERAKAN_BORDER_PROBE_VALUES") == NULL) {
      setenv("TERAKAN_BORDER_PROBE_VALUES", "0.1,0.2,0.3,0.4", 1);
   }
   fprintf(stderr, "border registers = %s\n", getenv("TERAKAN_BORDER_PROBE_VALUES"));

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

   /* The image is cleared to zero and never sampled inside, so anything nonzero in a result came
    * from the border.
    */
   VkImageCreateInfo const image_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {IMAGE_SIZE, IMAGE_SIZE, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
   VkImage image;
   CK(vkCreateImage(device, &image_create_info, NULL, &image));
   VkMemoryRequirements image_requirements;
   vkGetImageMemoryRequirements(device, image, &image_requirements);
   VkMemoryAllocateInfo const image_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = image_requirements.size,
      .memoryTypeIndex = memory_type(physical_device, image_requirements.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
   VkDeviceMemory image_memory;
   CK(vkAllocateMemory(device, &image_allocate_info, NULL, &image_memory));
   CK(vkBindImageMemory(device, image, image_memory, 0));

   VkDeviceSize const results_size = sizeof(float) * 4u * RESULT_COUNT;
   VkBufferCreateInfo const buffer_create_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                                  .size = results_size,
                                                  .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
   VkBuffer results_buffer;
   CK(vkCreateBuffer(device, &buffer_create_info, NULL, &results_buffer));
   VkMemoryRequirements buffer_requirements;
   vkGetBufferMemoryRequirements(device, results_buffer, &buffer_requirements);
   VkMemoryAllocateInfo const buffer_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = buffer_requirements.size,
      .memoryTypeIndex = memory_type(physical_device, buffer_requirements.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
   VkDeviceMemory buffer_memory;
   CK(vkAllocateMemory(device, &buffer_allocate_info, NULL, &buffer_memory));
   CK(vkBindBufferMemory(device, results_buffer, buffer_memory, 0));
   float * results;
   CK(vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, (void **)&results));

   VkSamplerCreateInfo const sampler_create_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE};
   VkSampler sampler;
   CK(vkCreateSampler(device, &sampler_create_info, NULL, &sampler));

   VkDescriptorSetLayoutBinding const bindings[] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
   VkDescriptorSetLayoutCreateInfo const set_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2,
      .pBindings = bindings};
   VkDescriptorSetLayout set_layout;
   CK(vkCreateDescriptorSetLayout(device, &set_layout_create_info, NULL, &set_layout));
   VkPipelineLayoutCreateInfo const pipeline_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout};
   VkPipelineLayout pipeline_layout;
   CK(vkCreatePipelineLayout(device, &pipeline_layout_create_info, NULL, &pipeline_layout));
   VkShaderModuleCreateInfo const module_create_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(border_color_swizzle_spirv),
      .pCode = border_color_swizzle_spirv};
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

   VkDescriptorPoolSize const pool_sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16},
                                              {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16}};
   VkDescriptorPoolCreateInfo const pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 16,
      .poolSizeCount = 2,
      .pPoolSizes = pool_sizes};
   VkDescriptorPool descriptor_pool;
   CK(vkCreateDescriptorPool(device, &pool_create_info, NULL, &descriptor_pool));

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

   printf("%-6s %-10s %-28s %-28s\n", "swz", "fetch", "components", "");
   for (unsigned swizzle_index = 0;
        swizzle_index < sizeof(probe_swizzles) / sizeof(probe_swizzles[0]); ++swizzle_index) {
      VkImageViewCreateInfo const view_create_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = image,
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .components = probe_swizzles[swizzle_index].mapping,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      VkImageView view;
      CK(vkCreateImageView(device, &view_create_info, NULL, &view));

      VkDescriptorSetAllocateInfo const set_allocate_info = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
         .descriptorPool = descriptor_pool,
         .descriptorSetCount = 1,
         .pSetLayouts = &set_layout};
      VkDescriptorSet descriptor_set;
      CK(vkAllocateDescriptorSets(device, &set_allocate_info, &descriptor_set));
      VkDescriptorImageInfo const image_info = {
         .sampler = sampler, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
      VkDescriptorBufferInfo const buffer_info = {
         .buffer = results_buffer, .offset = 0, .range = VK_WHOLE_SIZE};
      VkWriteDescriptorSet const writes[] = {
         {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = descriptor_set,
          .dstBinding = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &image_info},
         {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = descriptor_set,
          .dstBinding = 1,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &buffer_info}};
      vkUpdateDescriptorSets(device, 2, writes, 0, NULL);

      memset(results, 0, (size_t)results_size);

      CK(vkResetCommandBuffer(command_buffer, 0));
      VkCommandBufferBeginInfo const begin_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      CK(vkBeginCommandBuffer(command_buffer, &begin_info));
      VkImageSubresourceRange const whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkImageMemoryBarrier const to_general = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = image,
         .subresourceRange = whole};
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_general);
      /* Channel k of every texel holds (k + 1) / 8, so a gathered value names the channel it was
       * read from and the four texels of a footprint are indistinguishable on purpose -- the
       * question here is which channel, not which texel.
       */
      VkClearColorValue const channels = {.float32 = {0.125f, 0.25f, 0.375f, 0.5f}};
      vkCmdClearColorImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &channels,
                           1, &whole);
      VkImageMemoryBarrier const to_read = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                                            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                            .image = image,
                                            .subresourceRange = whole};
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &to_read);
      vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                              &descriptor_set, 0, NULL);
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
         fprintf(stderr, "%s: fence wait failed\n", probe_swizzles[swizzle_index].name);
         return 1;
      }

      static char const * const fetch_names[RESULT_COUNT] = {
         "brd sample", "brd gath.0", "brd gath.1", "brd gath.2", "brd gath.3",
         "in gath.0",  "in gath.1",  "in gath.2",  "in gath.3",  "in sample"};
      for (unsigned result_index = 0; result_index < RESULT_COUNT; ++result_index) {
         printf("%-6s %-10s %.3f %.3f %.3f %.3f\n", probe_swizzles[swizzle_index].name,
                fetch_names[result_index], results[result_index * 4 + 0],
                results[result_index * 4 + 1], results[result_index * 4 + 2],
                results[result_index * 4 + 3]);
      }

      vkDestroyImageView(device, view, NULL);
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
   vkUnmapMemory(device, buffer_memory);
   vkDestroyBuffer(device, results_buffer, NULL);
   vkFreeMemory(device, buffer_memory, NULL);
   vkDestroyImage(device, image, NULL);
   vkFreeMemory(device, image_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   return 0;
}
