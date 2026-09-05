/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Extended image gather: component selection, constant offsets, a non-constant offset, and the
 * four-independent-offset form, plus ordinary offset sampling at the extremes of the advertised
 * texel offset range.
 *
 * shaderImageGatherExtended is what makes the Offset and ConstOffsets operands legal on gather at
 * all, so nothing here could be written before the feature was advertised. The backend turned out
 * to have most of the machinery already: SFN selects GATHER4_O for a non-constant offset and folds
 * a constant one into the TEX instruction's own offset fields, and component selection was already
 * implemented. What was missing was the feature bit, the gather offset limits, and any coverage.
 *
 * The source image holds texel (x, y) channel c as 1000*c + 32*y + x, which is exact in float and
 * says at a glance which texel and which channel a result came from. The sample point is texel
 * coordinate 16.1 on both axes, so the gather footprint is texels 15 and 16 with nothing on a
 * boundary, and offsets of -8..7 stay well inside the image.
 *
 * Both the footprint and its order are checked. Vulkan defines the gather result as
 * (t(i0,j1), t(i1,j1), t(i1,j0), t(i0,j0)), and a driver that got the footprint right but the
 * order wrong would still return the same four numbers, so comparing them as a set would miss it.
 */

#include <vulkan/vulkan.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "terakan_test_device.h"

#define IMAGE_SIZE 32u

/* Texel coordinate 16.1 puts the footprint on texels 15 and 16. */
#define FOOTPRINT_I0 15
#define FOOTPRINT_J0 15
/* Nearest filtering of the same coordinate lands on texel 16. */
#define NEAREST_TEXEL 16

#define DYNAMIC_OFFSET_X (-6)
#define DYNAMIC_OFFSET_Y 2

#define CHECK_VK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_vk_result = (expression);                                               \
      if (check_vk_result != VK_SUCCESS) {                                                         \
         fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_vk_result);            \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

static const uint32_t image_gather_spirv[] = {
#include "terakan_image_gather.spv.h"

};

/* Must match the shader's fill. */
static float
texel(int const x, int const y, int const channel)
{
   return (float)(1000 * channel + 32 * y + x);
}

enum case_kind {
   /* A gather: the four texels of the footprint, in Vulkan's order. */
   CASE_GATHER,
   /* textureGatherOffsets: one texel per offset, the lower-left of each footprint. */
   CASE_GATHER_OFFSETS,
   /* A nearest sample: all four channels of one texel. */
   CASE_SAMPLE,
};

struct gather_case {
   char const * name;
   enum case_kind kind;
   int component;
   int offset_x, offset_y;
   /* CASE_GATHER_OFFSETS only. */
   int offsets[4][2];
};

static const struct gather_case cases[] = {
   {"textureGather component 0", CASE_GATHER, 0, 0, 0, {{0, 0}}},
   {"textureGather component 1", CASE_GATHER, 1, 0, 0, {{0, 0}}},
   {"textureGather component 2", CASE_GATHER, 2, 0, 0, {{0, 0}}},
   {"textureGather component 3", CASE_GATHER, 3, 0, 0, {{0, 0}}},
   {"textureGatherOffset (0,0)", CASE_GATHER, 1, 0, 0, {{0, 0}}},
   {"textureGatherOffset (3,0)", CASE_GATHER, 1, 3, 0, {{0, 0}}},
   {"textureGatherOffset (0,-5)", CASE_GATHER, 1, 0, -5, {{0, 0}}},
   {"textureGatherOffset (-8,7)", CASE_GATHER, 1, -8, 7, {{0, 0}}},
   {"textureGatherOffset (7,-8)", CASE_GATHER, 1, 7, -8, {{0, 0}}},
   {"textureGatherOffset, non-constant offset", CASE_GATHER, 1, DYNAMIC_OFFSET_X,
    DYNAMIC_OFFSET_Y, {{0, 0}}},
   {"textureGatherOffsets", CASE_GATHER_OFFSETS, 1, 0, 0,
    {{0, 0}, {2, 1}, {-3, 4}, {5, -6}}},
   {"textureLodOffset (0,0)", CASE_SAMPLE, 0, 0, 0, {{0, 0}}},
   {"textureLodOffset (7,7)", CASE_SAMPLE, 0, 7, 7, {{0, 0}}},
   {"textureLodOffset (-8,-8)", CASE_SAMPLE, 0, -8, -8, {{0, 0}}},
};
#define CASE_COUNT (sizeof(cases) / sizeof(cases[0]))

static void
expected_values(struct gather_case const * const c, float out[4])
{
   switch (c->kind) {
   case CASE_GATHER: {
      int const i0 = FOOTPRINT_I0 + c->offset_x, j0 = FOOTPRINT_J0 + c->offset_y;
      out[0] = texel(i0, j0 + 1, c->component);
      out[1] = texel(i0 + 1, j0 + 1, c->component);
      out[2] = texel(i0 + 1, j0, c->component);
      out[3] = texel(i0, j0, c->component);
   } break;
   case CASE_GATHER_OFFSETS:
      for (int i = 0; i < 4; ++i) {
         out[i] = texel(FOOTPRINT_I0 + c->offsets[i][0], FOOTPRINT_J0 + c->offsets[i][1],
                        c->component);
      }
      break;
   case CASE_SAMPLE:
      for (int i = 0; i < 4; ++i)
         out[i] = texel(NEAREST_TEXEL + c->offset_x, NEAREST_TEXEL + c->offset_y, i);
      break;
   }
}

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
      .pApplicationName = "terakan-image-gather-test",
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

   VkPhysicalDeviceFeatures available_features;
   vkGetPhysicalDeviceFeatures(physical_device, &available_features);
   if (!available_features.shaderImageGatherExtended) {
      fprintf(stderr, "shaderImageGatherExtended is not advertised\n");
      return 1;
   }
   fprintf(stderr,
           "device=%s queue_family=%u texel offsets [%d, %d] gather offsets [%d, %d]\n",
           properties.deviceName, compute_queue_family, properties.limits.minTexelOffset,
           properties.limits.maxTexelOffset, properties.limits.minTexelGatherOffset,
           properties.limits.maxTexelGatherOffset);
   /* Every offset the shader uses has to be inside what the device advertises, or the test would
    * be exercising undefined behaviour rather than the driver.
    */
   for (unsigned i = 0; i < CASE_COUNT; ++i) {
      int const low = cases[i].kind == CASE_SAMPLE ? properties.limits.minTexelOffset
                                                   : properties.limits.minTexelGatherOffset;
      int const high = cases[i].kind == CASE_SAMPLE ? properties.limits.maxTexelOffset
                                                    : properties.limits.maxTexelGatherOffset;
      int used[10], used_count = 0;
      if (cases[i].kind == CASE_GATHER_OFFSETS) {
         for (int k = 0; k < 4; ++k) {
            used[used_count++] = cases[i].offsets[k][0];
            used[used_count++] = cases[i].offsets[k][1];
         }
      } else {
         used[used_count++] = cases[i].offset_x;
         used[used_count++] = cases[i].offset_y;
      }
      for (int k = 0; k < used_count; ++k) {
         if (used[k] < low || used[k] > high) {
            fprintf(stderr, "%s uses offset %d, outside the advertised range [%d, %d]\n",
                    cases[i].name, used[k], low, high);
            return 1;
         }
      }
   }

   float const queue_priority = 1.0f;
   VkDeviceQueueCreateInfo const queue_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = compute_queue_family,
      .queueCount = 1,
      .pQueuePriorities = &queue_priority,
   };
   VkPhysicalDeviceFeatures enabled_features = {.shaderImageGatherExtended = VK_TRUE};
   VkDeviceCreateInfo const device_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_create_info,
      .pEnabledFeatures = &enabled_features,
   };
   VkDevice device;
   CHECK_VK(vkCreateDevice(physical_device, &device_create_info, NULL, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, compute_queue_family, 0, &queue);

   VkImageCreateInfo const image_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R32G32B32A32_SFLOAT,
      .extent = {IMAGE_SIZE, IMAGE_SIZE, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage image;
   CHECK_VK(vkCreateImage(device, &image_create_info, NULL, &image));
   VkMemoryRequirements image_requirements;
   vkGetImageMemoryRequirements(device, image, &image_requirements);
   VkMemoryAllocateInfo const image_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = image_requirements.size,
      .memoryTypeIndex = find_memory_type(physical_device, image_requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
   };
   VkDeviceMemory image_memory;
   CHECK_VK(vkAllocateMemory(device, &image_allocate_info, NULL, &image_memory));
   CHECK_VK(vkBindImageMemory(device, image, image_memory, 0));
   VkImageViewCreateInfo const view_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R32G32B32A32_SFLOAT,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   VkImageView image_view;
   CHECK_VK(vkCreateImageView(device, &view_create_info, NULL, &image_view));
   VkSamplerCreateInfo const sampler_create_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = VK_LOD_CLAMP_NONE,
      .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
   };
   VkSampler sampler;
   CHECK_VK(vkCreateSampler(device, &sampler_create_info, NULL, &sampler));

   VkDeviceSize const upload_bytes = (VkDeviceSize)IMAGE_SIZE * IMAGE_SIZE * 4 * sizeof(float);
   VkBufferCreateInfo const upload_create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = upload_bytes,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer upload_buffer;
   CHECK_VK(vkCreateBuffer(device, &upload_create_info, NULL, &upload_buffer));
   VkMemoryRequirements upload_requirements;
   vkGetBufferMemoryRequirements(device, upload_buffer, &upload_requirements);
   VkMemoryAllocateInfo const upload_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = upload_requirements.size,
      .memoryTypeIndex =
         find_memory_type(physical_device, upload_requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   VkDeviceMemory upload_memory;
   CHECK_VK(vkAllocateMemory(device, &upload_allocate_info, NULL, &upload_memory));
   CHECK_VK(vkBindBufferMemory(device, upload_buffer, upload_memory, 0));
   float * upload_mapping;
   CHECK_VK(vkMapMemory(device, upload_memory, 0, VK_WHOLE_SIZE, 0, (void **)&upload_mapping));
   for (uint32_t y = 0; y < IMAGE_SIZE; ++y) {
      for (uint32_t x = 0; x < IMAGE_SIZE; ++x) {
         for (int channel = 0; channel < 4; ++channel)
            upload_mapping[((y * IMAGE_SIZE) + x) * 4 + channel] = texel((int)x, (int)y, channel);
      }
   }

   VkDeviceSize const results_bytes = (VkDeviceSize)CASE_COUNT * 4 * sizeof(float);
   VkBufferCreateInfo const results_create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = results_bytes,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer results_buffer;
   CHECK_VK(vkCreateBuffer(device, &results_create_info, NULL, &results_buffer));
   VkMemoryRequirements results_requirements;
   vkGetBufferMemoryRequirements(device, results_buffer, &results_requirements);
   VkMemoryAllocateInfo const results_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = results_requirements.size,
      .memoryTypeIndex =
         find_memory_type(physical_device, results_requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   VkDeviceMemory results_memory;
   CHECK_VK(vkAllocateMemory(device, &results_allocate_info, NULL, &results_memory));
   CHECK_VK(vkBindBufferMemory(device, results_buffer, results_memory, 0));
   float * results_mapping;
   CHECK_VK(vkMapMemory(device, results_memory, 0, VK_WHOLE_SIZE, 0, (void **)&results_mapping));
   for (unsigned i = 0; i < CASE_COUNT * 4; ++i)
      results_mapping[i] = -1.0f;

   VkDescriptorSetLayoutBinding const bindings[2] = {
      {.binding = 0,
       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
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
   VkPushConstantRange const push_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 2 * sizeof(int32_t)};
   VkPipelineLayoutCreateInfo const pipeline_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push_range,
   };
   VkPipelineLayout pipeline_layout;
   CHECK_VK(vkCreatePipelineLayout(device, &pipeline_layout_create_info, NULL, &pipeline_layout));
   VkDescriptorPoolSize const pool_sizes[2] = {
      {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1},
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
   VkDescriptorImageInfo const image_info = {
      .sampler = sampler,
      .imageView = image_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
   };
   VkDescriptorBufferInfo const results_info = {
      .buffer = results_buffer, .offset = 0, .range = results_bytes,
   };
   VkWriteDescriptorSet const writes[2] = {
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
       .pBufferInfo = &results_info},
   };
   vkUpdateDescriptorSets(device, 2, writes, 0, NULL);

   VkShaderModuleCreateInfo const shader_module_create_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(image_gather_spirv),
      .pCode = image_gather_spirv,
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
   VkCommandBufferBeginInfo const begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   CHECK_VK(vkBeginCommandBuffer(command_buffer, &begin_info));

   VkImageSubresourceRange const whole_image = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   VkImageMemoryBarrier const to_transfer = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = whole_image,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_transfer);
   VkBufferImageCopy const upload_region = {
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {IMAGE_SIZE, IMAGE_SIZE, 1},
   };
   vkCmdCopyBufferToImage(command_buffer, upload_buffer, image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload_region);
   VkImageMemoryBarrier const to_sampled = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = whole_image,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &to_sampled);

   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                           &descriptor_set, 0, NULL);
   int32_t const dynamic_offset[2] = {DYNAMIC_OFFSET_X, DYNAMIC_OFFSET_Y};
   vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                      sizeof(dynamic_offset), dynamic_offset);
   vkCmdDispatch(command_buffer, 1, 1, 1);

   VkMemoryBarrier const host_ready = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_ready, 0, NULL, 0, NULL);
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
   VkResult const wait_result = vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull);
   if (wait_result != VK_SUCCESS) {
      fprintf(stderr, "Fence wait failed with VkResult %d\n", wait_result);
      return 1;
   }

   unsigned failures = 0;
   for (unsigned i = 0; i < CASE_COUNT; ++i) {
      float expected[4];
      expected_values(&cases[i], expected);
      float const * const actual = results_mapping + 4 * i;
      bool matches = true;
      for (int k = 0; k < 4; ++k) {
         if (actual[k] != expected[k])
            matches = false;
      }
      if (matches)
         continue;
      fprintf(stderr, "%s:\n  got      %g %g %g %g\n  expected %g %g %g %g\n", cases[i].name,
              actual[0], actual[1], actual[2], actual[3], expected[0], expected[1], expected[2],
              expected[3]);
      /* A result that is a permutation of the expected one means the footprint was right and the
       * order was not, which is a different bug from landing on the wrong texels.
       */
      if (cases[i].kind == CASE_GATHER) {
         bool permutation = true;
         for (int k = 0; k < 4; ++k) {
            bool found = false;
            for (int m = 0; m < 4; ++m) {
               if (actual[k] == expected[m])
                  found = true;
            }
            if (!found)
               permutation = false;
         }
         if (permutation) {
            fprintf(stderr,
                    "  the four texels are right but their order is not: the footprint was found "
                    "and the destination swizzle is wrong\n");
         }
      }
      ++failures;
   }

   printf("image_gather cases=%zu failures=%u %s\n", CASE_COUNT, failures,
          failures == 0 ? "PASS" : "FAIL");

   vkDeviceWaitIdle(device);
   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, command_pool, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyShaderModule(device, shader_module, NULL);
   vkDestroyDescriptorPool(device, descriptor_pool, NULL);
   vkDestroyPipelineLayout(device, pipeline_layout, NULL);
   vkDestroyDescriptorSetLayout(device, set_layout, NULL);
   vkUnmapMemory(device, results_memory);
   vkDestroyBuffer(device, results_buffer, NULL);
   vkFreeMemory(device, results_memory, NULL);
   vkUnmapMemory(device, upload_memory);
   vkDestroyBuffer(device, upload_buffer, NULL);
   vkFreeMemory(device, upload_memory, NULL);
   vkDestroySampler(device, sampler, NULL);
   vkDestroyImageView(device, image_view, NULL);
   vkDestroyImage(device, image, NULL);
   vkFreeMemory(device, image_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   return failures == 0 ? 0 : 1;
}
