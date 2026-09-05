/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Probe for shaderStorageImageExtendedFormats: does a storage image declared with an explicit
 * non-mandatory format (rg16f, VK_FORMAT_R16G16_SFLOAT) actually work for both imageStore and
 * imageLoad?
 *
 * terakan_format.c already advertises VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT broadly (any format
 * with a CB color view, an SQ texture fetch view and an SQ vertex/buffer fetch view, not just the
 * baseline mandatory formats), but the shaderStorageImageExtendedFormats VkPhysicalDeviceFeature
 * bit itself is never set (see the TODO(Triang3l) comment in terakan_physical_device.c). This
 * tests the underlying claim directly on real hardware, with a typed rg16f storage image and no
 * shaderStorageImage{Read,Write}WithoutFormat involved, rather than assuming format-capability
 * advertisement alone means the feature works end to end.
 *
 * One compute pass writes a distinct (r, g) value per texel via imageStore, a second reads them
 * back via imageLoad into a buffer. If both match what was written, extended-format storage images
 * work for at least this representative format and the feature bit should be safe to expose.
 */

#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "terakan_test_device.h"

#define VK_CHECK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_result = (expression);                                                  \
      if (check_result != VK_SUCCESS) {                                                            \
         std::fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_result);          \
         return 1;                                                                                 \
      }                                                                                            \
   } while (false)

namespace {

constexpr uint32_t kWidth = 17;
constexpr uint32_t kHeight = 13;

uint32_t const store_spirv[] = {
#include "terakan_extended_format_storage_image.spv.h"
};
uint32_t const load_spirv[] = {
#include "terakan_extended_format_storage_image_load.spv.h"

};

/* IEEE 754 binary16 encode, matching what imageStore(rg16f, ...) writes and what the readback
 * compares against -- exact for the simple values this test uses (no rounding edge cases).
 */
uint16_t
encode_half(float value)
{
   uint32_t bits;
   std::memcpy(&bits, &value, sizeof(bits));
   uint32_t const sign = (bits >> 16) & 0x8000u;
   int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
   uint32_t mantissa = bits & 0x7FFFFFu;
   if (exponent <= 0)
      return static_cast<uint16_t>(sign);
   if (exponent >= 31)
      return static_cast<uint16_t>(sign | 0x7C00u);
   return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13));
}

float
decode_half(uint16_t half)
{
   uint32_t const sign = (static_cast<uint32_t>(half) & 0x8000u) << 16;
   uint32_t const exponent = (static_cast<uint32_t>(half) >> 10) & 0x1Fu;
   uint32_t const mantissa = static_cast<uint32_t>(half) & 0x3FFu;
   uint32_t bits;
   if (exponent == 0) {
      bits = sign;
   } else {
      bits = sign | ((exponent - 15u + 127u) << 23) | (mantissa << 13);
   }
   float result;
   std::memcpy(&result, &bits, sizeof(result));
   return result;
}

uint32_t
find_memory_type(VkPhysicalDevice physical_device, uint32_t bits, VkMemoryPropertyFlags flags)
{
   VkPhysicalDeviceMemoryProperties properties;
   vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
   for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags)
         return i;
   }
   return UINT32_MAX;
}

} // namespace

int
main()
{
   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-extended-format-storage-image-test",
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo const instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application_info,
   };
   VkInstance instance;
   VK_CHECK(vkCreateInstance(&instance_info, nullptr, &instance));

   uint32_t physical_device_count = 0;
   VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_device_count, nullptr));
   std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
   VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices.data()));
   VkPhysicalDevice physical_device = VK_NULL_HANDLE;
   uint32_t queue_family = UINT32_MAX;
   VkPhysicalDeviceProperties properties = {};
   for (VkPhysicalDevice candidate : physical_devices) {
      vkGetPhysicalDeviceProperties(candidate, &properties);
      if (!terakan_test_device_matches(properties.deviceName))
         continue;
      uint32_t family_count = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
      std::vector<VkQueueFamilyProperties> families(family_count);
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
      for (uint32_t i = 0; i < family_count; ++i) {
         if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            physical_device = candidate;
            queue_family = i;
            break;
         }
      }
      if (physical_device != VK_NULL_HANDLE)
         break;
   }
   if (physical_device == VK_NULL_HANDLE) {
      std::fprintf(stderr, "Terakan graphics device not found\n");
      return 1;
   }

   VkFormatProperties format_properties;
   vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16G16_SFLOAT,
                                       &format_properties);
   if (!(format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
      std::fprintf(stderr, "R16G16_SFLOAT does not advertise STORAGE_IMAGE, nothing to probe\n");
      return 77;
   }
   std::fprintf(stderr, "device=%s queue_family=%u\n", properties.deviceName, queue_family);

   float const priority = 1.0F;
   VkDeviceQueueCreateInfo const queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = queue_family,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   VkDeviceCreateInfo const device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
   };
   VkDevice device;
   VK_CHECK(vkCreateDevice(physical_device, &device_info, nullptr, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, queue_family, 0, &queue);

   VkImageCreateInfo const image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R16G16_SFLOAT,
      .extent = {kWidth, kHeight, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_STORAGE_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage image;
   VK_CHECK(vkCreateImage(device, &image_info, nullptr, &image));
   VkMemoryRequirements image_requirements;
   vkGetImageMemoryRequirements(device, image, &image_requirements);
   uint32_t const image_memory_type = find_memory_type(
      physical_device, image_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (image_memory_type == UINT32_MAX) {
      std::fprintf(stderr, "No device-local memory for the image\n");
      return 1;
   }
   VkMemoryAllocateInfo const image_allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = image_requirements.size,
      .memoryTypeIndex = image_memory_type,
   };
   VkDeviceMemory image_memory;
   VK_CHECK(vkAllocateMemory(device, &image_allocation, nullptr, &image_memory));
   VK_CHECK(vkBindImageMemory(device, image, image_memory, 0));
   VkImageViewCreateInfo const view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = image_info.format,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   VkImageView view;
   VK_CHECK(vkCreateImageView(device, &view_info, nullptr, &view));

   uint32_t const texel_count = kWidth * kHeight;
   VkBufferCreateInfo const output_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = texel_count * sizeof(float) * 2,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer output_buffer;
   VK_CHECK(vkCreateBuffer(device, &output_info, nullptr, &output_buffer));
   VkMemoryRequirements output_requirements;
   vkGetBufferMemoryRequirements(device, output_buffer, &output_requirements);
   uint32_t const output_memory_type =
      find_memory_type(physical_device, output_requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   VkMemoryAllocateInfo const output_allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = output_requirements.size,
      .memoryTypeIndex = output_memory_type,
   };
   VkDeviceMemory output_memory;
   VK_CHECK(vkAllocateMemory(device, &output_allocation, nullptr, &output_memory));
   VK_CHECK(vkBindBufferMemory(device, output_buffer, output_memory, 0));
   float * output_mapping;
   VK_CHECK(vkMapMemory(device, output_memory, 0, VK_WHOLE_SIZE, 0,
                        reinterpret_cast<void **>(&output_mapping)));
   for (uint32_t i = 0; i < texel_count * 2; ++i)
      output_mapping[i] = -1.0F;

   VkDescriptorSetLayoutBinding const store_binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
   };
   VkDescriptorSetLayoutCreateInfo const store_set_layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &store_binding,
   };
   VkDescriptorSetLayout store_set_layout;
   VK_CHECK(
      vkCreateDescriptorSetLayout(device, &store_set_layout_info, nullptr, &store_set_layout));

   VkDescriptorSetLayoutBinding const load_bindings[] = {
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
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
   VkDescriptorSetLayoutCreateInfo const load_set_layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2,
      .pBindings = load_bindings,
   };
   VkDescriptorSetLayout load_set_layout;
   VK_CHECK(vkCreateDescriptorSetLayout(device, &load_set_layout_info, nullptr, &load_set_layout));

   VkPipelineLayoutCreateInfo const store_pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &store_set_layout,
   };
   VkPipelineLayout store_pipeline_layout;
   VK_CHECK(vkCreatePipelineLayout(device, &store_pipeline_layout_info, nullptr,
                                   &store_pipeline_layout));
   VkPipelineLayoutCreateInfo const load_pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &load_set_layout,
   };
   VkPipelineLayout load_pipeline_layout;
   VK_CHECK(
      vkCreatePipelineLayout(device, &load_pipeline_layout_info, nullptr, &load_pipeline_layout));

   VkDescriptorPoolSize const pool_sizes[] = {
      {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 2},
      {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1},
   };
   VkDescriptorPoolCreateInfo const pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 2,
      .poolSizeCount = 2,
      .pPoolSizes = pool_sizes,
   };
   VkDescriptorPool descriptor_pool;
   VK_CHECK(vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool));

   VkDescriptorSetAllocateInfo const store_set_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = descriptor_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &store_set_layout,
   };
   VkDescriptorSet store_descriptor_set;
   VK_CHECK(vkAllocateDescriptorSets(device, &store_set_allocate_info, &store_descriptor_set));
   VkDescriptorSetAllocateInfo const load_set_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = descriptor_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &load_set_layout,
   };
   VkDescriptorSet load_descriptor_set;
   VK_CHECK(vkAllocateDescriptorSets(device, &load_set_allocate_info, &load_descriptor_set));

   VkDescriptorImageInfo const image_descriptor = {
      .imageView = view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
   };
   VkDescriptorBufferInfo const buffer_descriptor = {
      .buffer = output_buffer,
      .offset = 0,
      .range = VK_WHOLE_SIZE,
   };
   VkWriteDescriptorSet const writes[] = {
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = store_descriptor_set,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo = &image_descriptor,
      },
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = load_descriptor_set,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo = &image_descriptor,
      },
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = load_descriptor_set,
         .dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &buffer_descriptor,
      },
   };
   vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

   VkShaderModuleCreateInfo const store_module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(store_spirv),
      .pCode = store_spirv,
   };
   VkShaderModule store_module;
   VK_CHECK(vkCreateShaderModule(device, &store_module_info, nullptr, &store_module));
   VkShaderModuleCreateInfo const load_module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(load_spirv),
      .pCode = load_spirv,
   };
   VkShaderModule load_module;
   VK_CHECK(vkCreateShaderModule(device, &load_module_info, nullptr, &load_module));

   VkComputePipelineCreateInfo const store_pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
               .stage = VK_SHADER_STAGE_COMPUTE_BIT,
               .module = store_module,
               .pName = "main"},
      .layout = store_pipeline_layout,
   };
   VkPipeline store_pipeline;
   VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &store_pipeline_info, nullptr,
                                     &store_pipeline));
   VkComputePipelineCreateInfo const load_pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
               .stage = VK_SHADER_STAGE_COMPUTE_BIT,
               .module = load_module,
               .pName = "main"},
      .layout = load_pipeline_layout,
   };
   VkPipeline load_pipeline;
   VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &load_pipeline_info, nullptr,
                                     &load_pipeline));

   VkCommandPoolCreateInfo const command_pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = queue_family,
   };
   VkCommandPool command_pool;
   VK_CHECK(vkCreateCommandPool(device, &command_pool_info, nullptr, &command_pool));
   VkCommandBufferAllocateInfo const command_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer command_buffer;
   VK_CHECK(vkAllocateCommandBuffers(device, &command_allocate_info, &command_buffer));
   VkCommandBufferBeginInfo const begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info));

   VkImageMemoryBarrier const to_general = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                        &to_general);

   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, store_pipeline);
   vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, store_pipeline_layout, 0,
                           1, &store_descriptor_set, 0, nullptr);
   vkCmdDispatch(command_buffer, (kWidth + 7) / 8, (kHeight + 7) / 8, 1);

   VkMemoryBarrier const store_to_load = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &store_to_load, 0, nullptr, 0,
                        nullptr);

   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, load_pipeline);
   vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, load_pipeline_layout, 0,
                           1, &load_descriptor_set, 0, nullptr);
   vkCmdDispatch(command_buffer, (kWidth + 7) / 8, (kHeight + 7) / 8, 1);

   VkMemoryBarrier const host_ready = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_ready, 0, nullptr, 0, nullptr);
   VK_CHECK(vkEndCommandBuffer(command_buffer));

   VkFenceCreateInfo const fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   VK_CHECK(vkCreateFence(device, &fence_info, nullptr, &fence));
   VkSubmitInfo const submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
   };
   VK_CHECK(vkQueueSubmit(queue, 1, &submit_info, fence));
   VkResult const wait_result = vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull);
   if (wait_result != VK_SUCCESS) {
      std::fprintf(stderr, "Fence wait failed with VkResult %d\n", wait_result);
      return 1;
   }

   uint32_t mismatches = 0;
   for (uint32_t y = 0; y < kHeight; ++y) {
      for (uint32_t x = 0; x < kWidth; ++x) {
         uint32_t const linear_index = y * kWidth + x;
         float const expected_r = decode_half(encode_half(float(linear_index) * 0.125F - 32.0F));
         float const expected_g = decode_half(encode_half(float(linear_index) * -0.25F + 16.0F));
         float const actual_r = output_mapping[linear_index * 2 + 0];
         float const actual_g = output_mapping[linear_index * 2 + 1];
         if (actual_r != expected_r || actual_g != expected_g) {
            if (mismatches < 8) {
               std::fprintf(stderr, "(%u,%u) = (%.4f,%.4f), expected (%.4f,%.4f) FAIL\n", x, y,
                            actual_r, actual_g, expected_r, expected_g);
            }
            ++mismatches;
         }
      }
   }
   std::printf("extended_format_storage_image texels=%u mismatches=%u %s\n", texel_count,
               mismatches, mismatches == 0 ? "PASS" : "FAIL");

   vkDeviceWaitIdle(device);
   vkDestroyFence(device, fence, nullptr);
   vkDestroyCommandPool(device, command_pool, nullptr);
   vkDestroyPipeline(device, store_pipeline, nullptr);
   vkDestroyPipeline(device, load_pipeline, nullptr);
   vkDestroyShaderModule(device, store_module, nullptr);
   vkDestroyShaderModule(device, load_module, nullptr);
   vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
   vkDestroyPipelineLayout(device, store_pipeline_layout, nullptr);
   vkDestroyPipelineLayout(device, load_pipeline_layout, nullptr);
   vkDestroyDescriptorSetLayout(device, store_set_layout, nullptr);
   vkDestroyDescriptorSetLayout(device, load_set_layout, nullptr);
   vkDestroyImageView(device, view, nullptr);
   vkDestroyImage(device, image, nullptr);
   vkFreeMemory(device, image_memory, nullptr);
   vkUnmapMemory(device, output_memory);
   vkDestroyBuffer(device, output_buffer, nullptr);
   vkFreeMemory(device, output_memory, nullptr);
   vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr);
   return mismatches == 0 ? 0 : 1;
}
