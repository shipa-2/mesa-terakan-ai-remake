/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "terakan_test_device.h"


namespace fs = std::filesystem;

struct Binding {
   uint32_t set;
   VkDescriptorSetLayoutBinding vk;
};

static VkShaderStageFlags
godot_stage_flags_to_vulkan(uint32_t const stages)
{
   VkShaderStageFlags result = 0;
   if (stages & (1U << 0))
      result |= VK_SHADER_STAGE_VERTEX_BIT;
   if (stages & (1U << 1))
      result |= VK_SHADER_STAGE_FRAGMENT_BIT;
   if (stages & (1U << 2))
      result |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
   if (stages & (1U << 3))
      result |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
   if (stages & (1U << 4))
      result |= VK_SHADER_STAGE_COMPUTE_BIT;
   return result;
}

static bool
godot_uniform_type_to_vulkan(uint32_t const type, VkDescriptorType & result,
                             bool & descriptor_count_is_length)
{
   descriptor_count_is_length = false;
   switch (type) {
   case 0:
      result = VK_DESCRIPTOR_TYPE_SAMPLER;
      descriptor_count_is_length = true;
      return true;
   case 1:
      result = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      descriptor_count_is_length = true;
      return true;
   case 2:
      result = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      descriptor_count_is_length = true;
      return true;
   case 3:
      result = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      descriptor_count_is_length = true;
      return true;
   case 4:
      result = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
      return true;
   case 6:
      result = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
      return true;
   case 7:
      result = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      return true;
   case 8:
      result = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      return true;
   case 9:
      result = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
      return true;
   case 10:
      result = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      return true;
   case 11:
      result = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
      return true;
   default:
      return false;
   }
}

static std::vector<uint32_t>
read_spirv(fs::path const & path)
{
   std::ifstream stream(path, std::ios::binary | std::ios::ate);
   if (!stream)
      throw std::runtime_error("unable to open SPIR-V");
   std::streamoff const byte_size = stream.tellg();
   if (byte_size <= 0 || (byte_size & 3))
      throw std::runtime_error("invalid SPIR-V size");
   stream.seekg(0);
   std::vector<uint32_t> spirv(static_cast<size_t>(byte_size) / sizeof(uint32_t));
   stream.read(reinterpret_cast<char *>(spirv.data()), byte_size);
   if (!stream || spirv.front() != 0x07230203)
      throw std::runtime_error("invalid SPIR-V contents");
   return spirv;
}

static char const *
unsupported_capability(std::vector<uint32_t> const & spirv,
                       VkPhysicalDeviceFeatures const & features)
{
   for (size_t offset = 5; offset < spirv.size();) {
      uint32_t const word_count = spirv[offset] >> 16;
      uint32_t const opcode = spirv[offset] & 0xFFFF;
      if (word_count == 0 || word_count > spirv.size() - offset)
         return "malformed instruction";
      if (opcode == 17 && word_count >= 2) {
         switch (spirv[offset + 1]) {
         case 9:
            return "Float16 requires an extension feature";
         case 22:
            if (!features.shaderInt16)
               return "shaderInt16 is disabled";
            break;
         case 49:
            if (!features.shaderStorageImageExtendedFormats)
               return "shaderStorageImageExtendedFormats is disabled";
            break;
         case 56:
            if (!features.shaderStorageImageWriteWithoutFormat)
               return "shaderStorageImageWriteWithoutFormat is disabled";
            break;
         case 5345:
            return "VulkanMemoryModel requires an extension feature";
         default:
            break;
         }
      }
      offset += word_count;
   }
   return nullptr;
}

static void
read_layout(fs::path const & path, std::vector<Binding> & bindings,
            VkPushConstantRange & push_constant)
{
   std::ifstream stream(path.string() + ".layout");
   if (!stream)
      throw std::runtime_error("unable to open layout");
   std::string line;
   while (std::getline(stream, line)) {
      std::istringstream fields(line);
      std::string kind;
      fields >> kind;
      if (kind == "push") {
         uint32_t godot_stages;
         fields >> push_constant.size >> godot_stages;
         push_constant.offset = 0;
         push_constant.stageFlags = godot_stage_flags_to_vulkan(godot_stages);
      } else if (kind == "vk_push") {
         fields >> push_constant.size >> push_constant.stageFlags;
         push_constant.offset = 0;
      } else if (kind == "binding") {
         Binding binding = {};
         uint32_t godot_type;
         uint32_t length;
         uint32_t godot_stages;
         fields >> binding.set >> binding.vk.binding >> godot_type >> length >> godot_stages;
         bool descriptor_count_is_length;
         if (!godot_uniform_type_to_vulkan(godot_type, binding.vk.descriptorType,
                                           descriptor_count_is_length)) {
            throw std::runtime_error("unsupported Godot uniform type " +
                                     std::to_string(godot_type));
         }
         binding.vk.descriptorCount = descriptor_count_is_length ? std::max(1U, length) : 1;
         binding.vk.stageFlags = godot_stage_flags_to_vulkan(godot_stages);
         bindings.push_back(binding);
      } else if (kind == "vk_binding") {
         Binding binding = {};
         uint32_t descriptor_type;
         fields >> binding.set >> binding.vk.binding >> descriptor_type >>
            binding.vk.descriptorCount >> binding.vk.stageFlags;
         binding.vk.descriptorType = static_cast<VkDescriptorType>(descriptor_type);
         bindings.push_back(binding);
      } else if (!kind.empty()) {
         throw std::runtime_error("unknown layout record");
      }
      if (!fields)
         throw std::runtime_error("malformed layout record");
   }
}

struct TestBuffer {
   VkBuffer buffer = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   uint8_t * mapping = nullptr;
   VkDeviceSize payload_size = 0;
};

static constexpr VkDeviceSize test_buffer_guard_size = 16;
static constexpr uint32_t test_buffer_guard = 0xA5C39E71;

static uint32_t
find_host_memory_type(VkPhysicalDevice const physical_device, uint32_t const memory_type_bits)
{
   VkPhysicalDeviceMemoryProperties properties;
   vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
   VkMemoryPropertyFlags const required =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
      if ((memory_type_bits & (1U << index)) &&
          (properties.memoryTypes[index].propertyFlags & required) == required)
         return index;
   }
   return UINT32_MAX;
}

static uint32_t
find_memory_type(VkPhysicalDevice const physical_device, uint32_t const memory_type_bits)
{
   VkPhysicalDeviceMemoryProperties properties;
   vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
   for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
      if (memory_type_bits & (1U << index))
         return index;
   }
   return UINT32_MAX;
}

static VkResult
create_test_buffer(VkDevice const device, VkPhysicalDevice const physical_device,
                   VkDeviceSize const payload_size, VkBufferUsageFlags const usage,
                   TestBuffer & buffer)
{
   buffer.payload_size = payload_size;
   VkBufferCreateInfo const create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = test_buffer_guard_size + payload_size + test_buffer_guard_size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkResult result = vkCreateBuffer(device, &create_info, nullptr, &buffer.buffer);
   if (result != VK_SUCCESS)
      return result;
   VkMemoryRequirements requirements;
   vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);
   uint32_t const memory_type = find_host_memory_type(physical_device, requirements.memoryTypeBits);
   if (memory_type == UINT32_MAX)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   VkMemoryAllocateInfo const allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = memory_type,
   };
   result = vkAllocateMemory(device, &allocate_info, nullptr, &buffer.memory);
   if (result != VK_SUCCESS)
      return result;
   result = vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0);
   if (result != VK_SUCCESS)
      return result;
   result = vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0,
                        reinterpret_cast<void **>(&buffer.mapping));
   if (result != VK_SUCCESS)
      return result;
   std::fill_n(reinterpret_cast<uint32_t *>(buffer.mapping),
               (test_buffer_guard_size + payload_size + test_buffer_guard_size) / sizeof(uint32_t),
               test_buffer_guard);
   return VK_SUCCESS;
}

static void
destroy_test_buffer(VkDevice const device, TestBuffer & buffer)
{
   if (buffer.mapping != nullptr)
      vkUnmapMemory(device, buffer.memory);
   if (buffer.buffer != VK_NULL_HANDLE)
      vkDestroyBuffer(device, buffer.buffer, nullptr);
   if (buffer.memory != VK_NULL_HANDLE)
      vkFreeMemory(device, buffer.memory, nullptr);
}

static void *
test_buffer_payload(TestBuffer & buffer)
{
   return buffer.mapping + test_buffer_guard_size;
}

static bool
test_buffer_guards_intact(TestBuffer const & buffer)
{
   for (VkDeviceSize offset = 0; offset < test_buffer_guard_size; offset += sizeof(uint32_t)) {
      uint32_t prefix, suffix;
      std::memcpy(&prefix, buffer.mapping + offset, sizeof(prefix));
      std::memcpy(&suffix,
                  buffer.mapping + test_buffer_guard_size + buffer.payload_size + offset,
                  sizeof(suffix));
      if (prefix != test_buffer_guard || suffix != test_buffer_guard)
         return false;
   }
   return true;
}

static VkResult
execute_skinning_shader(VkDevice const device, VkPhysicalDevice const physical_device,
                        uint32_t const queue_family, VkPipeline const pipeline,
                        VkPipelineLayout const pipeline_layout,
                        VkDescriptorSetLayout const descriptor_set_layout,
                        std::vector<Binding> const & bindings)
{
   static constexpr uint32_t vertex_count = 320;
   static constexpr uint32_t vertex_components = 6;
   TestBuffer buffers[5];
   VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
   VkCommandPool command_pool = VK_NULL_HANDLE;
   VkFence fence = VK_NULL_HANDLE;
   VkImage transition_image = VK_NULL_HANDLE;
   VkDeviceMemory transition_image_memory = VK_NULL_HANDLE;
   auto cleanup = [&]() {
      if (fence != VK_NULL_HANDLE)
         vkDestroyFence(device, fence, nullptr);
      if (command_pool != VK_NULL_HANDLE)
         vkDestroyCommandPool(device, command_pool, nullptr);
      if (descriptor_pool != VK_NULL_HANDLE)
         vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
      if (transition_image != VK_NULL_HANDLE)
         vkDestroyImage(device, transition_image, nullptr);
      if (transition_image_memory != VK_NULL_HANDLE)
         vkFreeMemory(device, transition_image_memory, nullptr);
      for (TestBuffer & buffer : buffers)
         destroy_test_buffer(device, buffer);
   };
   VkDeviceSize const payload_sizes[5] = {
      16,
      vertex_count * vertex_components * sizeof(float),
      vertex_count * sizeof(uint32_t),
      64,
      vertex_count * vertex_components * sizeof(float),
   };
   VkBufferUsageFlags const usages[5] = {
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
   };
   VkResult result = VK_SUCCESS;
   for (uint32_t index = 0; index < 5; ++index) {
      result = create_test_buffer(device, physical_device, payload_sizes[index], usages[index],
                                  buffers[index]);
      if (result != VK_SUCCESS) {
         cleanup();
         return result;
      }
   }

   char const * const transition_clear_value =
      std::getenv("TERAKAN_CORPUS_TRANSITION_CLEAR");
   bool const transition_clear_before =
      transition_clear_value != nullptr && std::strcmp(transition_clear_value, "after") != 0;
   bool const transition_clear_after =
      transition_clear_value != nullptr && std::strcmp(transition_clear_value, "before") != 0;
   bool const transition_clear = transition_clear_before || transition_clear_after;
   if (transition_clear) {
      VkImageCreateInfo const image_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .extent = {64, 64, 1},
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      result = vkCreateImage(device, &image_info, nullptr, &transition_image);
      if (result != VK_SUCCESS) {
         cleanup();
         return result;
      }
      VkMemoryRequirements requirements;
      vkGetImageMemoryRequirements(device, transition_image, &requirements);
      uint32_t const memory_type =
         find_memory_type(physical_device, requirements.memoryTypeBits);
      if (memory_type == UINT32_MAX) {
         cleanup();
         return VK_ERROR_FEATURE_NOT_PRESENT;
      }
      VkMemoryAllocateInfo const allocate_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = requirements.size,
         .memoryTypeIndex = memory_type,
      };
      result = vkAllocateMemory(device, &allocate_info, nullptr, &transition_image_memory);
      if (result == VK_SUCCESS)
         result = vkBindImageMemory(device, transition_image, transition_image_memory, 0);
      if (result != VK_SUCCESS) {
         cleanup();
         return result;
      }
   }

   reinterpret_cast<uint32_t *>(test_buffer_payload(buffers[0]))[0] = vertex_count;
   float * const vertices = reinterpret_cast<float *>(test_buffer_payload(buffers[1]));
   uint32_t * const bone_indices =
      reinterpret_cast<uint32_t *>(test_buffer_payload(buffers[2]));
   for (uint32_t vertex_index = 0; vertex_index < vertex_count; ++vertex_index) {
      bone_indices[vertex_index] = 0;
      for (uint32_t component = 0; component < vertex_components; ++component) {
         float const sign = (component & 1) ? -1.0F : 1.0F;
         vertices[vertex_index * vertex_components + component] =
            sign * (0.25F * static_cast<float>(vertex_index + component + 1));
      }
   }
   float const identity[16] = {
      1, 0, 0, 0,
      0, 1, 0, 0,
      0, 0, 1, 0,
      0, 0, 0, 1,
   };
   std::memcpy(test_buffer_payload(buffers[3]), identity, sizeof(identity));
   std::memset(test_buffer_payload(buffers[4]), 0xCD, payload_sizes[4]);

   VkDescriptorPoolSize const pool_sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
   };
   VkDescriptorPoolCreateInfo const pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 2,
      .pPoolSizes = pool_sizes,
   };
   result = vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool);
   if (result != VK_SUCCESS) {
      cleanup();
      return result;
   }
   VkDescriptorSetAllocateInfo const allocate_set_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = descriptor_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &descriptor_set_layout,
   };
   VkDescriptorSet descriptor_set;
   result = vkAllocateDescriptorSets(device, &allocate_set_info, &descriptor_set);
   if (result != VK_SUCCESS) {
      cleanup();
      return result;
   }

   if (bindings.size() != 5) {
      cleanup();
      return VK_ERROR_UNKNOWN;
   }
   uint32_t binding_numbers[5];
   for (uint32_t index = 0; index < 5; ++index)
      binding_numbers[index] = bindings[index].vk.binding;
   VkDescriptorBufferInfo buffer_infos[5];
   VkWriteDescriptorSet writes[5];
   for (uint32_t index = 0; index < 5; ++index) {
      buffer_infos[index] = {
         .buffer = buffers[index].buffer,
         .offset = test_buffer_guard_size,
         .range = buffers[index].payload_size,
      };
      writes[index] = {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_set,
         .dstBinding = binding_numbers[index],
         .descriptorCount = 1,
         .descriptorType =
            index == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &buffer_infos[index],
      };
   }
   vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);

   VkCommandPoolCreateInfo const command_pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = queue_family,
   };
   result = vkCreateCommandPool(device, &command_pool_info, nullptr, &command_pool);
   if (result != VK_SUCCESS) {
      cleanup();
      return result;
   }
   VkCommandBufferAllocateInfo const command_buffer_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer command_buffer;
   result = vkAllocateCommandBuffers(device, &command_buffer_info, &command_buffer);
   if (result != VK_SUCCESS) {
      cleanup();
      return result;
   }
   VkCommandBufferBeginInfo const begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   result = vkBeginCommandBuffer(command_buffer, &begin_info);
   if (result != VK_SUCCESS) {
      cleanup();
      return result;
   }
   VkImageSubresourceRange const transition_range = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .baseMipLevel = 0,
      .levelCount = 1,
      .baseArrayLayer = 0,
      .layerCount = 1,
   };
   if (transition_clear) {
      VkImageMemoryBarrier const image_barrier = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = 0,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = transition_image,
         .subresourceRange = transition_range,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &image_barrier);
      if (transition_clear_before) {
         VkClearColorValue const clear_before = {.float32 = {0.25F, 0.0F, 0.0F, 1.0F}};
         vkCmdClearColorImage(command_buffer, transition_image, VK_IMAGE_LAYOUT_GENERAL,
                              &clear_before, 1, &transition_range);
      }
   }
   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                           &descriptor_set, 0, nullptr);
   vkCmdDispatch(command_buffer, 5, 1, 1);
   if (transition_clear_after) {
      VkMemoryBarrier const transition_barrier = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &transition_barrier, 0, nullptr, 0,
                           nullptr);
      VkClearColorValue const clear_after = {.float32 = {0.0F, 0.25F, 0.0F, 1.0F}};
      vkCmdClearColorImage(command_buffer, transition_image, VK_IMAGE_LAYOUT_GENERAL,
                           &clear_after, 1, &transition_range);
   }
   VkMemoryBarrier const memory_barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &memory_barrier, 0, nullptr, 0, nullptr);
   result = vkEndCommandBuffer(command_buffer);
   if (result != VK_SUCCESS) {
      cleanup();
      return result;
   }

   VkFenceCreateInfo const fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   result = vkCreateFence(device, &fence_info, nullptr, &fence);
   if (result != VK_SUCCESS) {
      cleanup();
      return result;
   }
   VkQueue queue;
   vkGetDeviceQueue(device, queue_family, 0, &queue);
   VkSubmitInfo const submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
   };
   result = vkQueueSubmit(queue, 1, &submit_info, fence);
   if (result == VK_SUCCESS)
      result = vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ULL);
   vkDestroyFence(device, fence, nullptr);
   fence = VK_NULL_HANDLE;
   if (result == VK_SUCCESS) {
      float const * const output =
         reinterpret_cast<float const *>(test_buffer_payload(buffers[4]));
      bool valid = true;
      for (uint32_t vertex_index = 0; vertex_index < vertex_count; ++vertex_index) {
         for (uint32_t component = 0; component < vertex_components; ++component) {
            uint32_t const index = vertex_index * vertex_components + component;
            if (vertex_index < 2 || vertex_index + 2 >= vertex_count) {
               std::cout << "SKIN output[" << vertex_index << "][" << component
                         << "]=" << output[index] << " expected=" << vertices[index] << '\n';
            }
            valid &= std::fabs(output[index] - vertices[index]) <= 0.00001F;
         }
      }
      for (TestBuffer const & buffer : buffers)
         valid &= test_buffer_guards_intact(buffer);
      if (!valid)
         result = VK_ERROR_UNKNOWN;
   }

   cleanup();
   return result;
}

static VkResult
compile_compute_shader(VkDevice const device, VkPhysicalDevice const physical_device,
                       uint32_t const queue_family, fs::path const & path,
                       bool const execute_skinning)
{
   std::vector<uint32_t> const spirv = read_spirv(path);
   std::vector<Binding> bindings;
   VkPushConstantRange push_constant = {};
   read_layout(path, bindings, push_constant);

   uint32_t set_count = 0;
   for (Binding const & binding : bindings)
      set_count = std::max(set_count, binding.set + 1);
   std::vector<VkDescriptorSetLayout> set_layouts(set_count, VK_NULL_HANDLE);
   for (uint32_t set = 0; set < set_count; ++set) {
      std::vector<VkDescriptorSetLayoutBinding> set_bindings;
      for (Binding const & binding : bindings) {
         if (binding.set == set)
            set_bindings.push_back(binding.vk);
      }
      VkDescriptorSetLayoutCreateInfo const create_info = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
         .bindingCount = static_cast<uint32_t>(set_bindings.size()),
         .pBindings = set_bindings.data(),
      };
      VkResult const result =
         vkCreateDescriptorSetLayout(device, &create_info, nullptr, &set_layouts[set]);
      if (result != VK_SUCCESS) {
         for (VkDescriptorSetLayout const layout : set_layouts) {
            if (layout != VK_NULL_HANDLE)
               vkDestroyDescriptorSetLayout(device, layout, nullptr);
         }
         return result;
      }
   }

   VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
   VkPipelineLayoutCreateInfo const pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = set_count,
      .pSetLayouts = set_layouts.data(),
      .pushConstantRangeCount = push_constant.size ? 1U : 0U,
      .pPushConstantRanges = push_constant.size ? &push_constant : nullptr,
   };
   VkResult result =
      vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout);

   VkShaderModule module = VK_NULL_HANDLE;
   if (result == VK_SUCCESS) {
      VkShaderModuleCreateInfo const module_info = {
         .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
         .codeSize = spirv.size() * sizeof(uint32_t),
         .pCode = spirv.data(),
      };
      result = vkCreateShaderModule(device, &module_info, nullptr, &module);
   }

   VkPipeline pipeline = VK_NULL_HANDLE;
   if (result == VK_SUCCESS) {
      VkComputePipelineCreateInfo const pipeline_info = {
         .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
         .stage =
            {
               .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
               .stage = VK_SHADER_STAGE_COMPUTE_BIT,
               .module = module,
               .pName = "main",
            },
         .layout = pipeline_layout,
      };
      result =
         vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
   }

   if (result == VK_SUCCESS && execute_skinning) {
      if (set_layouts.size() != 1)
         result = VK_ERROR_UNKNOWN;
      else
         result = execute_skinning_shader(device, physical_device, queue_family, pipeline,
                                          pipeline_layout, set_layouts[0], bindings);
   }

   if (pipeline != VK_NULL_HANDLE)
      vkDestroyPipeline(device, pipeline, nullptr);
   if (module != VK_NULL_HANDLE)
      vkDestroyShaderModule(device, module, nullptr);
   if (pipeline_layout != VK_NULL_HANDLE)
      vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
   for (VkDescriptorSetLayout const layout : set_layouts)
      vkDestroyDescriptorSetLayout(device, layout, nullptr);
   return result;
}

int
main(int argc, char ** argv)
{
   if (argc != 2) {
      std::cerr << "usage: terakan_shader_corpus_test CORPUS_DIRECTORY\n";
      return 2;
   }

   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "Terakan shader corpus test",
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo const instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application_info,
   };
   VkInstance instance;
   VkResult result = vkCreateInstance(&instance_info, nullptr, &instance);
   if (result != VK_SUCCESS) {
      std::cerr << "vkCreateInstance failed: " << result << '\n';
      return 1;
   }

   uint32_t physical_device_count = 0;
   vkEnumeratePhysicalDevices(instance, &physical_device_count, nullptr);
   std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
   vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices.data());
   VkPhysicalDevice physical_device = VK_NULL_HANDLE;
   uint32_t queue_family = 0;
   bool const allow_any_device = std::getenv("TERAKAN_CORPUS_ALLOW_ANY_DEVICE") != nullptr;
   for (VkPhysicalDevice const candidate : physical_devices) {
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(candidate, &properties);
      if (!allow_any_device && !terakan_test_device_matches(properties.deviceName))
         continue;
      uint32_t queue_family_count = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_family_count, nullptr);
      std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_family_count,
                                               queue_families.data());
      for (uint32_t i = 0; i < queue_family_count; ++i) {
         if (queue_families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            physical_device = candidate;
            queue_family = i;
            break;
         }
      }
      if (physical_device != VK_NULL_HANDLE) {
         std::cerr << "device=" << properties.deviceName << '\n';
         break;
      }
   }
   if (physical_device == VK_NULL_HANDLE) {
      std::cerr << "Terakan compute device not found\n";
      vkDestroyInstance(instance, nullptr);
      return 1;
   }

   VkPhysicalDeviceFeatures features;
   vkGetPhysicalDeviceFeatures(physical_device, &features);
   std::cerr << "features shaderInt16=" << features.shaderInt16
             << " storageImageExtendedFormats=" << features.shaderStorageImageExtendedFormats
             << " storageImageWriteWithoutFormat=" << features.shaderStorageImageWriteWithoutFormat
             << '\n';
   float const queue_priority = 1.0F;
   VkDeviceQueueCreateInfo const queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = queue_family,
      .queueCount = 1,
      .pQueuePriorities = &queue_priority,
   };
   VkDeviceCreateInfo const device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
      .pEnabledFeatures = &features,
   };
   VkDevice device;
   result = vkCreateDevice(physical_device, &device_info, nullptr, &device);
   if (result != VK_SUCCESS) {
      std::cerr << "vkCreateDevice failed: " << result << '\n';
      vkDestroyInstance(instance, nullptr);
      return 1;
   }

   unsigned passed = 0;
   unsigned failed = 0;
   unsigned skipped = 0;
   bool const execute_skinning = std::getenv("TERAKAN_CORPUS_EXECUTE_SKINNING") != nullptr;
   bool const ignore_unsupported_capabilities =
      std::getenv("TERAKAN_CORPUS_IGNORE_UNSUPPORTED_CAPABILITIES") != nullptr;
   for (fs::recursive_directory_iterator it(argv[1]), end; it != end; ++it) {
      std::string const filename = it->path().filename().string();
      /* Runtime dumps produced by TERAKAN_DEBUG_DUMP_COMPUTE_SPIRV_DIR contain compute shaders
       * only, but use the generic .spv suffix. Accept both those dumps and the named .comp.spv
       * corpus used by the offline extractor.
       */
      static std::string const suffix = ".spv";
      if (!it->is_regular_file() || filename.size() < suffix.size() ||
          filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0) {
         continue;
      }
      try {
         std::vector<uint32_t> const spirv = read_spirv(it->path());
         if (char const * const reason = unsupported_capability(spirv, features);
             reason != nullptr && !ignore_unsupported_capabilities) {
            std::cout << "SKIP " << it->path() << ' ' << reason << '\n';
            ++skipped;
            continue;
         }
         std::cerr << "BEGIN " << it->path() << '\n';
         result = compile_compute_shader(device, physical_device, queue_family, it->path(),
                                         execute_skinning);
         std::cout << (result == VK_SUCCESS ? "PASS " : "FAIL ") << result << ' ' << it->path()
                   << '\n';
         result == VK_SUCCESS ? ++passed : ++failed;
      } catch (std::exception const & error) {
         std::cout << "ERROR " << it->path() << ' ' << error.what() << '\n';
         ++failed;
      }
   }

   vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr);
   std::cerr << "passed=" << passed << " failed=" << failed << " skipped=" << skipped << '\n';
   return failed ? 1 : 0;
}
