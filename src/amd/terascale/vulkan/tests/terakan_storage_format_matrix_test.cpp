/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Coverage for the "Complete storage-image/UAV format and atomic coverage" P1 item's load/store
 * half: does every format that advertises VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT actually survive an
 * imageStore followed by an imageLoad?
 *
 * terakan_extended_format_storage_image already showed that two representative non-mandatory
 * formats work, which was enough to expose shaderStorageImageExtendedFormats, and
 * terakan_storage_image_atomic covers the atomic half for R32_UINT. Neither says anything about the
 * breadth of the advertised set, and terakan_format.c advertises STORAGE_IMAGE for every format
 * with a CB colour view plus SQ texture and buffer fetch views -- a much larger set than the Vulkan
 * baseline. This walks that set.
 *
 * Each format is checked by one dispatch that stores a known value through a formatless storage
 * image and immediately loads the same texel back (separated by memoryBarrierImage() on a coherent
 * image, which is what makes a same-invocation store-then-load of one address well defined), so a
 * single shader per numeric class covers every format the view can supply. Formats the
 * implementation does not advertise as storage images are skipped and reported, not failed.
 *
 * Only the channels a format actually has are compared: a one-channel format loads as (r, 0, 0, 1)
 * and a three-channel one as (r, g, b, 1), so comparing all four would fail formats that are
 * behaving correctly. Tolerances are per format, since a value exactly representable in
 * R32_SFLOAT is not exactly representable in R8_UNORM.
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

constexpr uint32_t kWidth = 8;
constexpr uint32_t kHeight = 8;
constexpr uint32_t kTexels = kWidth * kHeight;

enum NumericClass { kFloat = 0, kUint = 1, kSint = 2, kClassCount = 3 };

uint32_t const storage_format_float_spirv[] = {
#include "terakan_storage_format_float.spv.h"
};
uint32_t const storage_format_uint_spirv[] = {
#include "terakan_storage_format_uint.spv.h"
};
uint32_t const storage_format_sint_spirv[] = {
#include "terakan_storage_format_sint.spv.h"

};

struct FormatCase {
   VkFormat format;
   char const * name;
   NumericClass numeric_class;
   uint32_t channels;
   float tolerance; /* Float class only. */
};

/* The values stored are chosen to fit every format in their class: the float ones are simple
 * fractions every normalized and float format can represent closely, and the integer ones are
 * small enough for an 8-bit channel (and, for the signed case, its negative range).
 */
constexpr float kFloatBase[4] = {0.25F, 0.5F, 0.75F, 1.0F};
constexpr uint32_t kUintBase[4] = {1u, 2u, 3u, 1u};
constexpr int32_t kSintBase[4] = {1, -2, 3, -1};

constexpr FormatCase kFormats[] = {
   {VK_FORMAT_R8_UNORM, "R8_UNORM", kFloat, 1, 1.0F / 255.0F},
   {VK_FORMAT_R8G8_UNORM, "R8G8_UNORM", kFloat, 2, 1.0F / 255.0F},
   {VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", kFloat, 4, 1.0F / 255.0F},
   {VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM", kFloat, 4, 1.0F / 255.0F},
   {VK_FORMAT_R8G8B8A8_SNORM, "R8G8B8A8_SNORM", kFloat, 4, 1.0F / 127.0F},
   {VK_FORMAT_R16_UNORM, "R16_UNORM", kFloat, 1, 1.0F / 65535.0F},
   {VK_FORMAT_R16G16B16A16_UNORM, "R16G16B16A16_UNORM", kFloat, 4, 1.0F / 65535.0F},
   {VK_FORMAT_R16_SFLOAT, "R16_SFLOAT", kFloat, 1, 1.0F / 1024.0F},
   {VK_FORMAT_R16G16_SFLOAT, "R16G16_SFLOAT", kFloat, 2, 1.0F / 1024.0F},
   {VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT", kFloat, 4, 1.0F / 1024.0F},
   {VK_FORMAT_R32_SFLOAT, "R32_SFLOAT", kFloat, 1, 1.0F / 100000.0F},
   {VK_FORMAT_R32G32_SFLOAT, "R32G32_SFLOAT", kFloat, 2, 1.0F / 100000.0F},
   {VK_FORMAT_R32G32B32A32_SFLOAT, "R32G32B32A32_SFLOAT", kFloat, 4, 1.0F / 100000.0F},
   {VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10_UNORM_PACK32", kFloat, 4, 1.0F / 1023.0F},
   {VK_FORMAT_B10G11R11_UFLOAT_PACK32, "B10G11R11_UFLOAT_PACK32", kFloat, 3, 1.0F / 100.0F},

   {VK_FORMAT_R8_UINT, "R8_UINT", kUint, 1, 0.0F},
   {VK_FORMAT_R8G8B8A8_UINT, "R8G8B8A8_UINT", kUint, 4, 0.0F},
   {VK_FORMAT_R16_UINT, "R16_UINT", kUint, 1, 0.0F},
   {VK_FORMAT_R16G16B16A16_UINT, "R16G16B16A16_UINT", kUint, 4, 0.0F},
   {VK_FORMAT_R32_UINT, "R32_UINT", kUint, 1, 0.0F},
   {VK_FORMAT_R32G32_UINT, "R32G32_UINT", kUint, 2, 0.0F},
   {VK_FORMAT_R32G32B32A32_UINT, "R32G32B32A32_UINT", kUint, 4, 0.0F},
   {VK_FORMAT_A2B10G10R10_UINT_PACK32, "A2B10G10R10_UINT_PACK32", kUint, 4, 0.0F},

   {VK_FORMAT_R8_SINT, "R8_SINT", kSint, 1, 0.0F},
   {VK_FORMAT_R8G8B8A8_SINT, "R8G8B8A8_SINT", kSint, 4, 0.0F},
   {VK_FORMAT_R16_SINT, "R16_SINT", kSint, 1, 0.0F},
   {VK_FORMAT_R32_SINT, "R32_SINT", kSint, 1, 0.0F},
   {VK_FORMAT_R32G32B32A32_SINT, "R32G32B32A32_SINT", kSint, 4, 0.0F},
};
constexpr uint32_t kFormatCount = sizeof(kFormats) / sizeof(kFormats[0]);

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
      .pApplicationName = "terakan-storage-format-matrix-test",
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
      return TERAKAN_TEST_DEVICE_NOT_FOUND_STATUS;
   }
   std::fprintf(stderr, "device=%s queue_family=%u formats=%u\n", properties.deviceName,
                queue_family, kFormatCount);

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

   VkDescriptorSetLayoutBinding const bindings[] = {
      {.binding = 0,
       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
       .descriptorCount = 1,
       .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
      {.binding = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       .descriptorCount = 1,
       .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
   };
   VkDescriptorSetLayoutCreateInfo const set_layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2,
      .pBindings = bindings,
   };
   VkDescriptorSetLayout set_layout;
   VK_CHECK(vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &set_layout));
   VkPushConstantRange const push_range = {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 16,
   };
   VkPipelineLayoutCreateInfo const pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push_range,
   };
   VkPipelineLayout pipeline_layout;
   VK_CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout));

   struct ShaderBlob {
      uint32_t const * code;
      size_t size;
   };
   ShaderBlob const blobs[kClassCount] = {
      {storage_format_float_spirv, sizeof(storage_format_float_spirv)},
      {storage_format_uint_spirv, sizeof(storage_format_uint_spirv)},
      {storage_format_sint_spirv, sizeof(storage_format_sint_spirv)},
   };
   VkPipeline pipelines[kClassCount];
   VkShaderModule modules[kClassCount];
   for (uint32_t i = 0; i < kClassCount; ++i) {
      VkShaderModuleCreateInfo const module_info = {
         .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
         .codeSize = blobs[i].size,
         .pCode = blobs[i].code,
      };
      VK_CHECK(vkCreateShaderModule(device, &module_info, nullptr, &modules[i]));
      VkComputePipelineCreateInfo const pipeline_info = {
         .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
         .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = modules[i],
                  .pName = "main"},
         .layout = pipeline_layout,
      };
      VK_CHECK(
         vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipelines[i]));
   }

   VkDescriptorPoolSize const pool_sizes[] = {
      {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1},
      {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1},
   };
   VkDescriptorPoolCreateInfo const pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 2,
      .pPoolSizes = pool_sizes,
   };
   VkDescriptorPool descriptor_pool;
   VK_CHECK(vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool));
   VkDescriptorSetAllocateInfo const set_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = descriptor_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &set_layout,
   };
   VkDescriptorSet descriptor_set;
   VK_CHECK(vkAllocateDescriptorSets(device, &set_allocate_info, &descriptor_set));

   VkDeviceSize const output_bytes = kTexels * 4 * sizeof(uint32_t);
   VkBufferCreateInfo const output_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = output_bytes,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer output_buffer;
   VK_CHECK(vkCreateBuffer(device, &output_info, nullptr, &output_buffer));
   VkMemoryRequirements output_requirements;
   vkGetBufferMemoryRequirements(device, output_buffer, &output_requirements);
   VkMemoryAllocateInfo const output_allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = output_requirements.size,
      .memoryTypeIndex =
         find_memory_type(physical_device, output_requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   VkDeviceMemory output_memory;
   VK_CHECK(vkAllocateMemory(device, &output_allocation, nullptr, &output_memory));
   VK_CHECK(vkBindBufferMemory(device, output_buffer, output_memory, 0));
   void * output_mapping;
   VK_CHECK(vkMapMemory(device, output_memory, 0, VK_WHOLE_SIZE, 0, &output_mapping));

   VkCommandPoolCreateInfo const command_pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
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

   uint32_t tested = 0, skipped = 0, failed = 0;
   for (uint32_t format_index = 0; format_index < kFormatCount; ++format_index) {
      FormatCase const & format_case = kFormats[format_index];

      VkFormatProperties format_properties;
      vkGetPhysicalDeviceFormatProperties(physical_device, format_case.format, &format_properties);
      if (!(format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
         std::printf("  %-26s not advertised as a storage image, skipped\n", format_case.name);
         ++skipped;
         continue;
      }

      VkImageCreateInfo const image_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = format_case.format,
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
      VkMemoryAllocateInfo const image_allocation = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = image_requirements.size,
         .memoryTypeIndex = find_memory_type(physical_device, image_requirements.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
      };
      VkDeviceMemory image_memory;
      VK_CHECK(vkAllocateMemory(device, &image_allocation, nullptr, &image_memory));
      VK_CHECK(vkBindImageMemory(device, image, image_memory, 0));
      VkImageViewCreateInfo const view_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = image,
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = format_case.format,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      VkImageView view;
      VK_CHECK(vkCreateImageView(device, &view_info, nullptr, &view));

      VkDescriptorImageInfo const image_descriptor = {
         .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      };
      VkDescriptorBufferInfo const buffer_descriptor = {
         .buffer = output_buffer, .offset = 0, .range = VK_WHOLE_SIZE,
      };
      VkWriteDescriptorSet const writes[] = {
         {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = descriptor_set,
          .dstBinding = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .pImageInfo = &image_descriptor},
         {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = descriptor_set,
          .dstBinding = 1,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &buffer_descriptor},
      };
      vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
      std::memset(output_mapping, 0xA5, output_bytes);

      VK_CHECK(vkResetCommandBuffer(command_buffer, 0));
      VkCommandBufferBeginInfo const begin_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      };
      VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info));
      VkImageMemoryBarrier const to_general = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
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
      vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                        pipelines[format_case.numeric_class]);
      vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                              &descriptor_set, 0, nullptr);
      uint32_t push[4];
      if (format_case.numeric_class == kFloat)
         std::memcpy(push, kFloatBase, sizeof(push));
      else if (format_case.numeric_class == kUint)
         std::memcpy(push, kUintBase, sizeof(push));
      else
         std::memcpy(push, kSintBase, sizeof(push));
      vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                         sizeof(push), push);
      vkCmdDispatch(command_buffer, (kWidth + 7) / 8, (kHeight + 7) / 8, 1);
      VkMemoryBarrier const host_ready = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_ready, 0, nullptr, 0, nullptr);
      VK_CHECK(vkEndCommandBuffer(command_buffer));

      VkSubmitInfo const submit_info = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &command_buffer,
      };
      VK_CHECK(vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE));
      VkResult const wait_result = vkQueueWaitIdle(queue);
      if (wait_result != VK_SUCCESS) {
         std::fprintf(stderr, "  %-26s queue wait failed with VkResult %d FAIL\n",
                      format_case.name, wait_result);
         ++failed;
      } else {
         uint32_t mismatches = 0;
         for (uint32_t texel = 0; texel < kTexels && mismatches == 0; ++texel) {
            for (uint32_t channel = 0; channel < format_case.channels; ++channel) {
               bool channel_ok;
               if (format_case.numeric_class == kFloat) {
                  float const actual =
                     static_cast<float const *>(output_mapping)[texel * 4 + channel];
                  channel_ok = std::fabs(actual - kFloatBase[channel]) <= format_case.tolerance;
                  if (!channel_ok && mismatches == 0) {
                     std::fprintf(stderr, "  %-26s texel %u channel %u = %f, expected %f FAIL\n",
                                  format_case.name, texel, channel, actual, kFloatBase[channel]);
                  }
               } else if (format_case.numeric_class == kUint) {
                  uint32_t const actual =
                     static_cast<uint32_t const *>(output_mapping)[texel * 4 + channel];
                  channel_ok = actual == kUintBase[channel];
                  if (!channel_ok && mismatches == 0) {
                     std::fprintf(stderr, "  %-26s texel %u channel %u = %u, expected %u FAIL\n",
                                  format_case.name, texel, channel, actual, kUintBase[channel]);
                  }
               } else {
                  int32_t const actual =
                     static_cast<int32_t const *>(output_mapping)[texel * 4 + channel];
                  channel_ok = actual == kSintBase[channel];
                  if (!channel_ok && mismatches == 0) {
                     std::fprintf(stderr, "  %-26s texel %u channel %u = %d, expected %d FAIL\n",
                                  format_case.name, texel, channel, actual, kSintBase[channel]);
                  }
               }
               if (!channel_ok)
                  ++mismatches;
            }
         }
         if (mismatches == 0) {
            std::printf("  %-26s PASS\n", format_case.name);
         } else {
            ++failed;
         }
         ++tested;
      }

      vkDestroyImageView(device, view, nullptr);
      vkDestroyImage(device, image, nullptr);
      vkFreeMemory(device, image_memory, nullptr);
   }

   std::printf("storage_format_matrix tested=%u skipped=%u failed=%u %s\n", tested, skipped, failed,
               failed == 0 ? "PASS" : "FAIL");

   vkDeviceWaitIdle(device);
   vkDestroyCommandPool(device, command_pool, nullptr);
   for (uint32_t i = 0; i < kClassCount; ++i) {
      vkDestroyPipeline(device, pipelines[i], nullptr);
      vkDestroyShaderModule(device, modules[i], nullptr);
   }
   vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
   vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
   vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
   vkUnmapMemory(device, output_memory);
   vkDestroyBuffer(device, output_buffer, nullptr);
   vkFreeMemory(device, output_memory, nullptr);
   vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr);
   return failed == 0 ? 0 : 1;
}
