/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Depth-comparison ("shadow") sampling, for all eight VkCompareOp values.
 *
 * Nothing else in this suite exercises it, yet it is what every shadow map in a real renderer is
 * built on: a TERAKAN_DEBUG_RENDER trace of Buckshot Roulette shows its frames are dominated by
 * depth-only shadow passes (4096x4096, 512x512, 256x256 and 128x128), and the only thing those
 * passes exist to feed is a comparison sample. A comparison that always answers "shadowed" leaves a
 * scene rendering its geometry and albedo but receiving no light, which is what that game currently
 * looks like on this driver.
 *
 * A depth image is cleared to a known value, then a compute shader samples it through a
 * VK_COMPARE_OP sampler with the reference sweeping across x, so a single dispatch covers
 * reference-below, reference-equal and reference-above for that op. Nearest filtering keeps every
 * result exactly 0.0 or 1.0; with linear filtering the hardware would average four comparisons and
 * a wrong result could hide in an intermediate value.
 *
 * Each op is checked against its own truth table rather than against "shadow maps usually use
 * LESS", so an implementation that hard-codes one comparison, ignores the op, or inverts the
 * comparison is caught, and the failure names which op and which side of the stored depth broke.
 */

#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

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
/* Exactly representable, and exactly one of the swept reference values (x = 4 gives 4/8), so the
 * equality-sensitive ops are actually exercised at equality rather than near it.
 */
constexpr float kStoredDepth = 0.5F;

uint32_t const shadow_compare_spirv[] = {
#include "terakan_shadow_compare.spv.h"
};

struct CompareCase {
   VkCompareOp op;
   char const * name;
};
constexpr CompareCase kCompares[] = {
   {VK_COMPARE_OP_NEVER, "NEVER"},
   {VK_COMPARE_OP_LESS, "LESS"},
   {VK_COMPARE_OP_EQUAL, "EQUAL"},
   {VK_COMPARE_OP_LESS_OR_EQUAL, "LESS_OR_EQUAL"},
   {VK_COMPARE_OP_GREATER, "GREATER"},
   {VK_COMPARE_OP_NOT_EQUAL, "NOT_EQUAL"},
   {VK_COMPARE_OP_GREATER_OR_EQUAL, "GREATER_OR_EQUAL"},
   {VK_COMPARE_OP_ALWAYS, "ALWAYS"},
};
constexpr uint32_t kCompareCount = sizeof(kCompares) / sizeof(kCompares[0]);

/* The Vulkan comparison is reference OP stored. */
bool
expected_pass(VkCompareOp op, float reference, float stored)
{
   switch (op) {
   case VK_COMPARE_OP_NEVER: return false;
   case VK_COMPARE_OP_LESS: return reference < stored;
   case VK_COMPARE_OP_EQUAL: return reference == stored;
   case VK_COMPARE_OP_LESS_OR_EQUAL: return reference <= stored;
   case VK_COMPARE_OP_GREATER: return reference > stored;
   case VK_COMPARE_OP_NOT_EQUAL: return reference != stored;
   case VK_COMPARE_OP_GREATER_OR_EQUAL: return reference >= stored;
   default: return true;
   }
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

struct PushConstants {
   uint32_t width;
   uint32_t height;
};

} // namespace

int
main()
{
   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-shadow-compare-test",
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
      if (!std::strstr(properties.deviceName, "(Terakan)") ||
          std::strstr(properties.deviceName, "TeraScale 1"))
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

   VkFormatProperties depth_properties;
   vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_D32_SFLOAT, &depth_properties);
   /* Vulkan 1.0 has no separate depth-comparison format feature -- that bit only exists in the
    * VkFormatFeatureFlags2 form -- so being sampleable at all is the check available here.
    */
   if (!(depth_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
      std::fprintf(stderr, "D32_SFLOAT is not sampleable, nothing to probe\n");
      return 77;
   }
   std::fprintf(stderr, "device=%s queue_family=%u stored_depth=%.3f\n", properties.deviceName,
                queue_family, kStoredDepth);

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

   VkImageCreateInfo const depth_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_D32_SFLOAT,
      .extent = {kWidth, kHeight, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage depth_image;
   VK_CHECK(vkCreateImage(device, &depth_info, nullptr, &depth_image));
   VkMemoryRequirements depth_requirements;
   vkGetImageMemoryRequirements(device, depth_image, &depth_requirements);
   VkMemoryAllocateInfo const depth_allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = depth_requirements.size,
      .memoryTypeIndex = find_memory_type(physical_device, depth_requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
   };
   VkDeviceMemory depth_memory;
   VK_CHECK(vkAllocateMemory(device, &depth_allocation, nullptr, &depth_memory));
   VK_CHECK(vkBindImageMemory(device, depth_image, depth_memory, 0));
   VkImageViewCreateInfo const depth_view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = depth_image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_D32_SFLOAT,
      .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
   };
   VkImageView depth_view;
   VK_CHECK(vkCreateImageView(device, &depth_view_info, nullptr, &depth_view));

   uint32_t const texel_count = kWidth * kHeight;
   VkBufferCreateInfo const output_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = texel_count * sizeof(float),
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
   float * output_mapping;
   VK_CHECK(vkMapMemory(device, output_memory, 0, VK_WHOLE_SIZE, 0,
                        reinterpret_cast<void **>(&output_mapping)));

   VkDescriptorSetLayoutBinding const bindings[] = {
      {.binding = 0,
       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
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
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(PushConstants),
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
   VkShaderModuleCreateInfo const module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(shadow_compare_spirv),
      .pCode = shadow_compare_spirv,
   };
   VkShaderModule shader_module;
   VK_CHECK(vkCreateShaderModule(device, &module_info, nullptr, &shader_module));
   VkComputePipelineCreateInfo const pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
               .stage = VK_SHADER_STAGE_COMPUTE_BIT,
               .module = shader_module,
               .pName = "main"},
      .layout = pipeline_layout,
   };
   VkPipeline pipeline;
   VK_CHECK(
      vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline));

   VkDescriptorPoolSize const pool_sizes[] = {
      {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1},
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

   /* The render pass exists only to clear the depth image to a known value. */
   VkAttachmentDescription const attachment = {
      .format = VK_FORMAT_D32_SFLOAT,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
   };
   VkAttachmentReference const depth_reference = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
   };
   VkSubpassDescription const subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .pDepthStencilAttachment = &depth_reference,
   };
   VkRenderPassCreateInfo const render_pass_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
   };
   VkRenderPass render_pass;
   VK_CHECK(vkCreateRenderPass(device, &render_pass_info, nullptr, &render_pass));
   VkFramebufferCreateInfo const framebuffer_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = render_pass,
      .attachmentCount = 1,
      .pAttachments = &depth_view,
      .width = kWidth,
      .height = kHeight,
      .layers = 1,
   };
   VkFramebuffer framebuffer;
   VK_CHECK(vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer));

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

   uint32_t failed_ops = 0;
   /* Both filters, because they are different hardware paths and a real renderer uses the linear
    * one: Godot's shadow samplers filter linearly to get PCF. With a uniform stored depth every
    * PCF tap compares identically, so a correct linear result is still exactly 0.0 or 1.0 and any
    * intermediate or garbage value means the filtered comparison path is broken.
    */
   struct FilterCase {
      VkFilter filter;
      char const * name;
   };
   FilterCase const filters[2] = {{VK_FILTER_NEAREST, "nearest"}, {VK_FILTER_LINEAR, "linear"}};
   for (uint32_t filter_index = 0; filter_index < 2; ++filter_index)
   for (uint32_t compare_index = 0; compare_index < kCompareCount; ++compare_index) {
      CompareCase const & compare_case = kCompares[compare_index];
      FilterCase const & filter_case = filters[filter_index];

      VkSamplerCreateInfo const sampler_info = {
         .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
         /* Nearest: a linear filter would average four comparisons and let a wrong result hide in
          * an intermediate value instead of showing up as a clean 0/1 flip.
          */
         .magFilter = filter_case.filter,
         .minFilter = filter_case.filter,
         .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
         .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
         .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
         .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
         .compareEnable = VK_TRUE,
         .compareOp = compare_case.op,
      };
      VkSampler sampler;
      VK_CHECK(vkCreateSampler(device, &sampler_info, nullptr, &sampler));

      VkDescriptorImageInfo const image_descriptor = {
         .sampler = sampler,
         .imageView = depth_view,
         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      };
      VkDescriptorBufferInfo const buffer_descriptor = {
         .buffer = output_buffer, .offset = 0, .range = VK_WHOLE_SIZE,
      };
      VkWriteDescriptorSet const writes[] = {
         {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = descriptor_set,
          .dstBinding = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &image_descriptor},
         {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = descriptor_set,
          .dstBinding = 1,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &buffer_descriptor},
      };
      vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
      for (uint32_t i = 0; i < texel_count; ++i)
         output_mapping[i] = -1.0F;

      VK_CHECK(vkResetCommandBuffer(command_buffer, 0));
      VkCommandBufferBeginInfo const begin_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      };
      VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info));
      VkClearValue const clear = {.depthStencil = {kStoredDepth, 0}};
      VkRenderPassBeginInfo const render_begin = {
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
         .renderPass = render_pass,
         .framebuffer = framebuffer,
         .renderArea = {{0, 0}, {kWidth, kHeight}},
         .clearValueCount = 1,
         .pClearValues = &clear,
      };
      vkCmdBeginRenderPass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdEndRenderPass(command_buffer);

      VkImageMemoryBarrier const depth_ready = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = depth_image,
         .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &depth_ready);

      vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                              &descriptor_set, 0, nullptr);
      PushConstants const push_constants = {kWidth, kHeight};
      vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                         sizeof(push_constants), &push_constants);
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
      VK_CHECK(vkQueueWaitIdle(queue));

      uint32_t mismatches = 0;
      for (uint32_t y = 0; y < kHeight; ++y) {
         for (uint32_t x = 0; x < kWidth; ++x) {
            float const reference = float(x) / float(kWidth);
            float const want = expected_pass(compare_case.op, reference, kStoredDepth) ? 1.0F : 0.0F;
            float const actual = output_mapping[y * kWidth + x];
            if (std::fabs(actual - want) > 1.0F / 256.0F) {
               if (mismatches < 3) {
                  std::fprintf(stderr,
                               "  %-8s %-17s reference %.3f vs stored %.3f = %.3f, expected %.1f "
                               "FAIL\n",
                               filter_case.name, compare_case.name, reference, kStoredDepth, actual,
                               want);
               }
               ++mismatches;
            }
         }
      }
      if (mismatches == 0) {
         std::printf("  %-8s %-17s PASS\n", filter_case.name, compare_case.name);
      } else {
         std::printf("  %-8s %-17s %u/%u wrong FAIL\n", filter_case.name, compare_case.name,
                     mismatches, texel_count);
         ++failed_ops;
      }
      vkDestroySampler(device, sampler, nullptr);
   }

   std::printf("shadow_compare cases=%u failed=%u %s\n", kCompareCount * 2, failed_ops,
               failed_ops == 0 ? "PASS" : "FAIL");

   vkDeviceWaitIdle(device);
   vkDestroyCommandPool(device, command_pool, nullptr);
   vkDestroyFramebuffer(device, framebuffer, nullptr);
   vkDestroyRenderPass(device, render_pass, nullptr);
   vkDestroyPipeline(device, pipeline, nullptr);
   vkDestroyShaderModule(device, shader_module, nullptr);
   vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
   vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
   vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
   vkDestroyImageView(device, depth_view, nullptr);
   vkDestroyImage(device, depth_image, nullptr);
   vkFreeMemory(device, depth_memory, nullptr);
   vkUnmapMemory(device, output_memory);
   vkDestroyBuffer(device, output_buffer, nullptr);
   vkFreeMemory(device, output_memory, nullptr);
   vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr);
   return failed_ops == 0 ? 0 : 1;
}
