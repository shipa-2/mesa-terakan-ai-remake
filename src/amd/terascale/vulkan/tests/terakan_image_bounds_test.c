/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* #MemoryIntegrity for storage image bounds, the image half of what terakan_dynamic_offset_bounds
 * and terakan_dynamic_uav_bounds cover for buffers.
 *
 * A storage image is bound through CB_COLOR*, and what keeps a shader's stores inside it is the
 * hardware's own bounds state rather than anything in the shader: WIDTH_MAX and HEIGHT_MAX in
 * CB_COLOR*_DIM, and SLICE_START/SLICE_MAX in CB_COLOR*_VIEW. Nothing else stands between an
 * out-of-range imageStore and whatever memory follows the image, so those fields being right for
 * the bound view is the whole of the guarantee -- and the specification makes it absolute: a store
 * with out-of-bounds coordinates has no effect, with no feature bit to opt out of.
 *
 * The same bounds state also decides which part of the image a store reaches, so getting it wrong
 * does not only leak memory. This is exactly where the omni-shadow regression lived: DB_DEPTH_VIEW
 * was not re-emitted when only the array layer changed, and every layer's rendering piled into one.
 * The four views here are chosen with that in mind -- a plain one, a mip level other than zero, a
 * trailing subrange of the array layers, and both at once -- and each is checked not just for
 * staying inside the image but for landing on its own level and its own layers, with every other
 * subresource of the image required to still hold the clear value.
 *
 * Writes that land in the image's own padding are not failures. That padding is inside the memory
 * the image was given, so nothing outside the resource is at risk; the guard buffer bound directly
 * behind the image begins only past the end of its memory requirements.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "terakan_test_device.h"

#define IMAGE_WIDTH 20u
#define IMAGE_HEIGHT 12u
#define IMAGE_LEVELS 4u
#define IMAGE_LAYERS 4u

#define GUARD_BYTES 65536u
#define GUARD_WORDS (GUARD_BYTES / 4u)

#define POISON_PATTERN 0xBAADF00Du
#define CLEAR_VALUE 0u

#define CHECK_VK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_vk_result = (expression);                                               \
      if (check_vk_result != VK_SUCCESS) {                                                         \
         fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_vk_result);            \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

static const uint32_t image_bounds_spirv[] = {
#include "terakan_image_bounds.spv.h"

};

struct bound_view {
   char const * name;
   uint32_t base_mip_level;
   uint32_t base_array_layer;
   uint32_t layer_count;
};

/* A plain view, one that moves the level, one that moves the base layer, and one that moves both.
 * The last two are the shape the omni-shadow regression broke.
 */
static const struct bound_view bound_views[] = {
   {"whole image", 0, 0, IMAGE_LAYERS},
   {"mip level 2", 2, 0, IMAGE_LAYERS},
   {"layers 2..3", 0, 2, 2},
   {"mip level 2, layers 1..2", 2, 1, 2},
};

static uint32_t
level_extent(uint32_t const extent, uint32_t const level)
{
   uint32_t const minified = extent >> level;
   return minified != 0 ? minified : 1u;
}

/* Must match the shader. The coordinate is relative to the bound view. */
static uint32_t
expected_marker(uint32_t const x, uint32_t const y, uint32_t const view_layer)
{
   return 0xAB000000u | ((view_layer & 0xFFu) << 16) | ((y & 0xFFu) << 8) | (x & 0xFFu);
}

static uint64_t
align_up(uint64_t const value, uint64_t const alignment)
{
   return alignment <= 1 ? value : (value + alignment - 1) / alignment * alignment;
}

/* Where a level's texels start in the readback staging buffer, in texels. */
static uint32_t
level_readback_offset_texels(uint32_t const level)
{
   uint32_t offset = 0;
   for (uint32_t previous = 0; previous < level; ++previous) {
      offset += level_extent(IMAGE_WIDTH, previous) * level_extent(IMAGE_HEIGHT, previous) *
                IMAGE_LAYERS;
   }
   return offset;
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
      .pApplicationName = "terakan-image-bounds-test",
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

   VkImageCreateInfo const image_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R32_UINT,
      .extent = {IMAGE_WIDTH, IMAGE_HEIGHT, 1},
      .mipLevels = IMAGE_LEVELS,
      .arrayLayers = IMAGE_LAYERS,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage image;
   CHECK_VK(vkCreateImage(device, &image_create_info, NULL, &image));
   VkMemoryRequirements image_requirements;
   vkGetImageMemoryRequirements(device, image, &image_requirements);

   /* The guard is a plain buffer bound to the same allocation directly after the image, so it can
    * be poisoned and read back on any memory type rather than needing a host-visible one.
    */
   VkBufferCreateInfo const guard_create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = GUARD_BYTES,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer guard_buffer;
   CHECK_VK(vkCreateBuffer(device, &guard_create_info, NULL, &guard_buffer));
   VkMemoryRequirements guard_requirements;
   vkGetBufferMemoryRequirements(device, guard_buffer, &guard_requirements);

   uint64_t const guard_offset = align_up(image_requirements.size, guard_requirements.alignment);
   uint32_t const memory_type =
      find_memory_type(physical_device,
                       image_requirements.memoryTypeBits & guard_requirements.memoryTypeBits, 0);
   if (memory_type == UINT32_MAX) {
      fprintf(stderr, "No memory type serves both the image and the guard buffer\n");
      return 1;
   }
   fprintf(stderr,
           "device=%s queue_family=%u image=%ux%u, %u levels, %u layers, %llu bytes, guard at "
           "%llu\n",
           properties.deviceName, compute_queue_family, IMAGE_WIDTH, IMAGE_HEIGHT, IMAGE_LEVELS,
           IMAGE_LAYERS, (unsigned long long)image_requirements.size,
           (unsigned long long)guard_offset);

   VkMemoryAllocateInfo const allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = guard_offset + guard_requirements.size,
      .memoryTypeIndex = memory_type,
   };
   VkDeviceMemory memory;
   CHECK_VK(vkAllocateMemory(device, &allocate_info, NULL, &memory));
   CHECK_VK(vkBindImageMemory(device, image, memory, 0));
   CHECK_VK(vkBindBufferMemory(device, guard_buffer, memory, guard_offset));

   uint32_t const image_readback_texels = level_readback_offset_texels(IMAGE_LEVELS);
   VkBufferCreateInfo const staging_create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = (VkDeviceSize)image_readback_texels * 4u + GUARD_BYTES,
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
   uint32_t * staging_mapping;
   CHECK_VK(vkMapMemory(device, staging_memory, 0, VK_WHOLE_SIZE, 0, (void **)&staging_mapping));

   VkDescriptorSetLayoutBinding const binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
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
   VkPushConstantRange const push_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(int32_t)};
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
      .descriptorCount = sizeof(bound_views) / sizeof(bound_views[0]),
   };
   VkDescriptorPoolCreateInfo const pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = sizeof(bound_views) / sizeof(bound_views[0]),
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
   };
   VkDescriptorPool descriptor_pool;
   CHECK_VK(vkCreateDescriptorPool(device, &pool_create_info, NULL, &descriptor_pool));

   VkShaderModuleCreateInfo const shader_module_create_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(image_bounds_spirv),
      .pCode = image_bounds_spirv,
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
   bool image_layout_is_general = false;

   for (unsigned view_index = 0; view_index < sizeof(bound_views) / sizeof(bound_views[0]);
        ++view_index) {
      struct bound_view const * const view_case = &bound_views[view_index];
      uint32_t const view_width = level_extent(IMAGE_WIDTH, view_case->base_mip_level);
      uint32_t const view_height = level_extent(IMAGE_HEIGHT, view_case->base_mip_level);

      VkImageViewCreateInfo const view_create_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = image,
         .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
         .format = VK_FORMAT_R32_UINT,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, view_case->base_mip_level, 1,
                              view_case->base_array_layer, view_case->layer_count},
      };
      VkImageView image_view;
      CHECK_VK(vkCreateImageView(device, &view_create_info, NULL, &image_view));

      VkDescriptorSetAllocateInfo const set_allocate_info = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
         .descriptorPool = descriptor_pool,
         .descriptorSetCount = 1,
         .pSetLayouts = &set_layout,
      };
      VkDescriptorSet descriptor_set;
      CHECK_VK(vkAllocateDescriptorSets(device, &set_allocate_info, &descriptor_set));
      VkDescriptorImageInfo const image_info = {
         .imageView = image_view,
         .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      };
      VkWriteDescriptorSet const write = {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_set,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo = &image_info,
      };
      vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

      CHECK_VK(vkResetCommandBuffer(command_buffer, 0));
      VkCommandBufferBeginInfo const begin_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      };
      CHECK_VK(vkBeginCommandBuffer(command_buffer, &begin_info));

      VkImageSubresourceRange const whole_image = {VK_IMAGE_ASPECT_COLOR_BIT, 0, IMAGE_LEVELS, 0,
                                                   IMAGE_LAYERS};
      VkImageMemoryBarrier const to_general = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = image_layout_is_general ? VK_ACCESS_TRANSFER_READ_BIT : 0,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = image_layout_is_general ? VK_IMAGE_LAYOUT_GENERAL
                                              : VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = image,
         .subresourceRange = whole_image,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_general);
      image_layout_is_general = true;
      /* Clear every level and layer, so a texel outside the bound view that ends up written is
       * distinguishable, as is an in-range texel that never gets written at all. Poison the guard
       * so anything reaching past the image stands out.
       */
      VkClearColorValue const clear_value = {.uint32 = {CLEAR_VALUE, 0, 0, 0}};
      vkCmdClearColorImage(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL, &clear_value, 1,
                           &whole_image);
      vkCmdFillBuffer(command_buffer, guard_buffer, 0, VK_WHOLE_SIZE, POISON_PATTERN);
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
      int32_t const layer_count = (int32_t)view_case->layer_count;
      vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                         sizeof(layer_count), &layer_count);
      /* 4x4 workgroups of 8x8 cover a 32x32 grid of xy starting eight texels before the origin,
       * which overshoots even the largest view in both axes.
       */
      vkCmdDispatch(command_buffer, 4, 4, 1);

      VkMemoryBarrier const stored = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &stored, 0, NULL, 0, NULL);
      VkBufferImageCopy image_readback[IMAGE_LEVELS];
      for (uint32_t level = 0; level < IMAGE_LEVELS; ++level) {
         image_readback[level] = (VkBufferImageCopy){
            .bufferOffset = (VkDeviceSize)level_readback_offset_texels(level) * 4u,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, IMAGE_LAYERS},
            .imageExtent = {level_extent(IMAGE_WIDTH, level), level_extent(IMAGE_HEIGHT, level), 1},
         };
      }
      vkCmdCopyImageToBuffer(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL, staging_buffer,
                             IMAGE_LEVELS, image_readback);
      VkBufferCopy const guard_readback = {
         .srcOffset = 0,
         .dstOffset = (VkDeviceSize)image_readback_texels * 4u,
         .size = GUARD_BYTES,
      };
      vkCmdCopyBuffer(command_buffer, guard_buffer, staging_buffer, 1, &guard_readback);
      VkMemoryBarrier const host_ready = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
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

      /* Every texel of every level and layer: inside the view it must carry its own marker,
       * outside it must still be the clear value.
       */
      unsigned inside_failures = 0, outside_failures = 0;
      for (uint32_t level = 0; level < IMAGE_LEVELS; ++level) {
         uint32_t const width = level_extent(IMAGE_WIDTH, level);
         uint32_t const height = level_extent(IMAGE_HEIGHT, level);
         uint32_t const * const level_texels =
            staging_mapping + level_readback_offset_texels(level);
         for (uint32_t layer = 0; layer < IMAGE_LAYERS; ++layer) {
            bool const layer_in_view =
               layer >= view_case->base_array_layer &&
               layer < view_case->base_array_layer + view_case->layer_count;
            bool const level_in_view = level == view_case->base_mip_level;
            for (uint32_t y = 0; y < height; ++y) {
               for (uint32_t x = 0; x < width; ++x) {
                  uint32_t const actual = level_texels[(layer * height + y) * width + x];
                  bool const in_view = level_in_view && layer_in_view;
                  uint32_t const expected =
                     in_view ? expected_marker(x, y, layer - view_case->base_array_layer)
                             : CLEAR_VALUE;
                  if (actual == expected)
                     continue;
                  if (in_view) {
                     if (inside_failures == 0) {
                        fprintf(stderr,
                                "%s: texel (%u,%u) layer %u of level %u = 0x%08X, expected "
                                "0x%08X%s\n",
                                view_case->name, x, y, layer, level, actual, expected,
                                actual == CLEAR_VALUE
                                   ? " (never written -- the bound clamps too tightly)"
                                   : "");
                     }
                     ++inside_failures;
                  } else {
                     if (outside_failures == 0) {
                        fprintf(stderr,
                                "%s: texel (%u,%u) layer %u of level %u = 0x%08X but is outside "
                                "the bound view, which should have left it at 0x%08X\n",
                                view_case->name, x, y, layer, level, actual, CLEAR_VALUE);
                     }
                     ++outside_failures;
                  }
               }
            }
         }
      }

      unsigned guard_failures = 0;
      uint32_t const * const guard = staging_mapping + image_readback_texels;
      for (uint32_t word_index = 0; word_index < GUARD_WORDS; ++word_index) {
         if (guard[word_index] == POISON_PATTERN)
            continue;
         if (guard_failures == 0) {
            uint32_t const escaped = guard[word_index];
            fprintf(stderr,
                    "%s: guard word %u (%llu bytes past the end of the image) = 0x%08X, expected "
                    "the poison 0x%08X\n",
                    view_case->name, word_index,
                    (unsigned long long)(guard_offset - image_requirements.size +
                                         (uint64_t)word_index * 4u),
                    escaped, POISON_PATTERN);
            if ((escaped & 0xFF000000u) == 0xAB000000u) {
               fprintf(stderr,
                       "  that is an imageStore of view coordinate (%d,%d) layer %d escaping the "
                       "image\n",
                       (int)(int8_t)(escaped & 0xFFu), (int)(int8_t)((escaped >> 8) & 0xFFu),
                       (int)(int8_t)((escaped >> 16) & 0xFFu));
            }
         }
         ++guard_failures;
      }

      if (inside_failures != 0 || outside_failures != 0 || guard_failures != 0) {
         fprintf(stderr, "%s: %u inside, %u outside, %u guard words\n", view_case->name,
                 inside_failures, outside_failures, guard_failures);
         ++failures;
      } else {
         fprintf(stderr, "%s (level %u, layers %u..%u, %ux%u): placed correctly, guard intact\n",
                 view_case->name, view_case->base_mip_level, view_case->base_array_layer,
                 view_case->base_array_layer + view_case->layer_count - 1, view_width, view_height);
      }

      vkDestroyImageView(device, image_view, NULL);
   }

   printf("image_bounds views=%zu failures=%u %s\n",
          sizeof(bound_views) / sizeof(bound_views[0]), failures, failures == 0 ? "PASS" : "FAIL");

   vkDeviceWaitIdle(device);
   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, command_pool, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyShaderModule(device, shader_module, NULL);
   vkDestroyDescriptorPool(device, descriptor_pool, NULL);
   vkDestroyPipelineLayout(device, pipeline_layout, NULL);
   vkDestroyDescriptorSetLayout(device, set_layout, NULL);
   vkDestroyImage(device, image, NULL);
   vkDestroyBuffer(device, guard_buffer, NULL);
   vkFreeMemory(device, memory, NULL);
   vkUnmapMemory(device, staging_memory);
   vkDestroyBuffer(device, staging_buffer, NULL);
   vkFreeMemory(device, staging_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   return failures == 0 ? 0 : 1;
}
