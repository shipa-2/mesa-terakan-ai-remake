/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* The reducing depth/stencil resolve modes, VK_RESOLVE_MODE_MIN_BIT and VK_RESOLVE_MODE_MAX_BIT.
 *
 * Sample zero only ever reads one sample, so it cannot tell whether the other samples are being
 * addressed at all. Here each sample of a 4x attachment is given a different depth, by restricting
 * every draw to one sample through the pipeline's sample mask, and the three modes are then
 * resolved out of that same attachment into three destinations. The depths are chosen so that the
 * minimum, the maximum and sample zero are three different values, which is what makes a mode that
 * silently behaves like another one visible.
 *
 * The same attachment is resolved three times: the first render draws and resolves sample zero,
 * and the two that follow load it back untouched and resolve the reducing modes out of it.
 */

#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
constexpr VkFormat kFormat = VK_FORMAT_D32_SFLOAT;
constexpr uint32_t kMaxSampleCount = 8;

/* Deliberately not monotonic, so that at four and eight samples the minimum, the maximum and
 * sample zero are three distinct values. Two samples cannot manage that, since sample zero is
 * necessarily one of the two extremes; there the maximum coincides with it and only the minimum
 * distinguishes a mode that silently reads a single sample.
 */
constexpr float kSampleDepths[kMaxSampleCount] = {0.25F,  0.125F, 0.5F,    0.75F,
                                                  0.375F, 0.625F, 0.1875F, 0.9375F};

struct ModeCase {
   char const * name;
   VkResolveModeFlagBits mode;
};
constexpr ModeCase kModes[] = {
   {"sample zero", VK_RESOLVE_MODE_SAMPLE_ZERO_BIT},
   {"minimum", VK_RESOLVE_MODE_MIN_BIT},
   {"maximum", VK_RESOLVE_MODE_MAX_BIT},
};
constexpr uint32_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);

float
expected_depth(VkResolveModeFlagBits mode, uint32_t sample_count)
{
   float result = kSampleDepths[0];
   for (uint32_t sample = 1; sample < sample_count; ++sample) {
      if (mode == VK_RESOLVE_MODE_MIN_BIT && kSampleDepths[sample] < result)
         result = kSampleDepths[sample];
      else if (mode == VK_RESOLVE_MODE_MAX_BIT && kSampleDepths[sample] > result)
         result = kSampleDepths[sample];
   }
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

std::vector<uint32_t>
read_spirv(char const * path)
{
   std::FILE * const file = std::fopen(path, "rb");
   if (file == nullptr)
      return {};
   std::fseek(file, 0, SEEK_END);
   long const byte_count = std::ftell(file);
   std::fseek(file, 0, SEEK_SET);
   std::vector<uint32_t> words(static_cast<size_t>(byte_count) / sizeof(uint32_t));
   size_t const read = std::fread(words.data(), 1, static_cast<size_t>(byte_count), file);
   std::fclose(file);
   return read == static_cast<size_t>(byte_count) ? words : std::vector<uint32_t>{};
}

struct Image {
   VkImage image = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   VkImageView view = VK_NULL_HANDLE;
};

VkResult
create_depth_image(VkDevice device, VkPhysicalDevice physical_device, VkSampleCountFlagBits samples,
                   VkImageUsageFlags usage, Image & out)
{
   VkImageCreateInfo const info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = kFormat,
      .extent = {kWidth, kHeight, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = samples,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkResult result = vkCreateImage(device, &info, nullptr, &out.image);
   if (result != VK_SUCCESS)
      return result;
   VkMemoryRequirements requirements;
   vkGetImageMemoryRequirements(device, out.image, &requirements);
   uint32_t const memory_type = find_memory_type(physical_device, requirements.memoryTypeBits,
                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (memory_type == UINT32_MAX)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   VkMemoryAllocateInfo const allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = memory_type,
   };
   result = vkAllocateMemory(device, &allocation, nullptr, &out.memory);
   if (result != VK_SUCCESS)
      return result;
   result = vkBindImageMemory(device, out.image, out.memory, 0);
   if (result != VK_SUCCESS)
      return result;
   VkImageViewCreateInfo const view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = out.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = kFormat,
      .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
   };
   return vkCreateImageView(device, &view_info, nullptr, &out.view);
}

} // namespace

int
main(int argc, char ** argv)
{
   if (argc != 4) {
      std::fprintf(stderr, "usage: %s VERT_SPV FRAG_SPV SAMPLE_COUNT\n", argv[0]);
      return 2;
   }
   uint32_t const kSampleCount = (uint32_t)std::atoi(argv[3]);
   if (kSampleCount != 2 && kSampleCount != 4 && kSampleCount != 8) {
      std::fprintf(stderr, "SAMPLE_COUNT must be 2, 4 or 8\n");
      return 2;
   }
   VkSampleCountFlagBits const kSamples = (VkSampleCountFlagBits)kSampleCount;
   std::vector<uint32_t> const vertex_spirv = read_spirv(argv[1]);
   std::vector<uint32_t> const fragment_spirv = read_spirv(argv[2]);
   if (vertex_spirv.empty() || fragment_spirv.empty()) {
      std::fprintf(stderr, "Unable to read shader SPIR-V\n");
      return 2;
   }

   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-resolve-modes-test",
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

   /* The modes have to be advertised, not merely implemented. */
   VkPhysicalDeviceDepthStencilResolveProperties resolve_properties = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES,
   };
   VkPhysicalDeviceProperties2 properties_2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &resolve_properties,
   };
   vkGetPhysicalDeviceProperties2(physical_device, &properties_2);
   VkResolveModeFlags const wanted =
      VK_RESOLVE_MODE_SAMPLE_ZERO_BIT | VK_RESOLVE_MODE_MIN_BIT | VK_RESOLVE_MODE_MAX_BIT;
   if ((resolve_properties.supportedDepthResolveModes & wanted) != wanted) {
      std::fprintf(stderr, "supportedDepthResolveModes is 0x%x, expected 0x%x to be present\n",
                   resolve_properties.supportedDepthResolveModes, wanted);
      return 1;
   }
   std::fprintf(stderr, "device=%s queue_family=%u samples=%u\n", properties.deviceName,
                queue_family, kSampleCount);

   float const priority = 1.0F;
   VkDeviceQueueCreateInfo const queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = queue_family,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamic_rendering_feature = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR,
      .dynamicRendering = VK_TRUE,
   };
   char const * const device_extension = VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME;
   VkDeviceCreateInfo const device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &dynamic_rendering_feature,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = &device_extension,
   };
   VkDevice device;
   VK_CHECK(vkCreateDevice(physical_device, &device_info, nullptr, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, queue_family, 0, &queue);

   auto const cmd_begin_rendering = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
      vkGetDeviceProcAddr(device, "vkCmdBeginRenderingKHR"));
   auto const cmd_end_rendering = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
      vkGetDeviceProcAddr(device, "vkCmdEndRenderingKHR"));
   if (cmd_begin_rendering == nullptr || cmd_end_rendering == nullptr) {
      std::fprintf(stderr, "The dynamic rendering entry points are missing\n");
      return 1;
   }

   Image multisample;
   VK_CHECK(create_depth_image(device, physical_device, kSamples,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT,
                               multisample));
   Image destinations[kModeCount];
   for (Image & destination : destinations) {
      VK_CHECK(create_depth_image(device, physical_device, VK_SAMPLE_COUNT_1_BIT,
                                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  destination));
   }

   VkDeviceSize const image_bytes = kWidth * kHeight * sizeof(float);
   VkBufferCreateInfo const readback_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = image_bytes * kModeCount,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer readback_buffer;
   VK_CHECK(vkCreateBuffer(device, &readback_info, nullptr, &readback_buffer));
   VkMemoryRequirements readback_requirements;
   vkGetBufferMemoryRequirements(device, readback_buffer, &readback_requirements);
   uint32_t const readback_memory_type =
      find_memory_type(physical_device, readback_requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (readback_memory_type == UINT32_MAX) {
      std::fprintf(stderr, "No host-visible coherent memory for the readback buffer\n");
      return 1;
   }
   VkMemoryAllocateInfo const readback_allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = readback_requirements.size,
      .memoryTypeIndex = readback_memory_type,
   };
   VkDeviceMemory readback_memory;
   VK_CHECK(vkAllocateMemory(device, &readback_allocation, nullptr, &readback_memory));
   VK_CHECK(vkBindBufferMemory(device, readback_buffer, readback_memory, 0));
   float * readback_mapping;
   VK_CHECK(vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0,
                        reinterpret_cast<void **>(&readback_mapping)));
   for (uint32_t i = 0; i < kWidth * kHeight * kModeCount; ++i)
      readback_mapping[i] = -1.0F;

   VkPushConstantRange const push_range = {
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
      .offset = 0,
      .size = sizeof(float),
   };
   VkPipelineLayoutCreateInfo const pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push_range,
   };
   VkPipelineLayout pipeline_layout;
   VK_CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout));

   VkShaderModule modules[2];
   std::vector<uint32_t> const * const shader_code[2] = {&vertex_spirv, &fragment_spirv};
   for (uint32_t i = 0; i < 2; ++i) {
      VkShaderModuleCreateInfo const module_info = {
         .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
         .codeSize = shader_code[i]->size() * sizeof(uint32_t),
         .pCode = shader_code[i]->data(),
      };
      VK_CHECK(vkCreateShaderModule(device, &module_info, nullptr, &modules[i]));
   }
   VkPipelineShaderStageCreateInfo const stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = modules[0], .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = modules[1], .pName = "main"},
   };
   VkPipelineVertexInputStateCreateInfo const vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
   };
   VkPipelineInputAssemblyStateCreateInfo const input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   VkViewport const viewport = {0.0F, 0.0F, (float)kWidth, (float)kHeight, 0.0F, 1.0F};
   VkRect2D const scissor = {{0, 0}, {kWidth, kHeight}};
   VkPipelineViewportStateCreateInfo const viewport_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor,
   };
   VkPipelineRasterizationStateCreateInfo const rasterization = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0F,
   };
   /* Depth writes must not be filtered by a comparison, so every draw lands whatever the sample
    * already holds.
    */
   VkPipelineDepthStencilStateCreateInfo const depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE,
      .depthWriteEnable = VK_TRUE,
      .depthCompareOp = VK_COMPARE_OP_ALWAYS,
   };
   VkPipelineColorBlendStateCreateInfo const color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
   };
   VkPipelineRenderingCreateInfoKHR const pipeline_rendering = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
      .depthAttachmentFormat = kFormat,
   };

   /* One pipeline per sample, differing only in the sample mask, which is what confines a draw to
    * a single sample.
    */
   VkPipeline pipelines[kMaxSampleCount];
   VkSampleMask sample_masks[kMaxSampleCount];
   for (uint32_t sample = 0; sample < kSampleCount; ++sample) {
      sample_masks[sample] = 1u << sample;
      VkPipelineMultisampleStateCreateInfo const multisample_state = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
         .rasterizationSamples = kSamples,
         .pSampleMask = &sample_masks[sample],
      };
      VkGraphicsPipelineCreateInfo const pipeline_info = {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .pNext = &pipeline_rendering,
         .stageCount = 2, .pStages = stages,
         .pVertexInputState = &vertex_input,
         .pInputAssemblyState = &input_assembly,
         .pViewportState = &viewport_state,
         .pRasterizationState = &rasterization,
         .pMultisampleState = &multisample_state,
         .pDepthStencilState = &depth_stencil,
         .pColorBlendState = &color_blend,
         .layout = pipeline_layout,
         .renderPass = VK_NULL_HANDLE,
      };
      VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                         &pipelines[sample]));
   }

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

   VkImageSubresourceRange const whole_depth = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
   auto const barrier = [&](VkImage image, VkAccessFlags src_access, VkAccessFlags dst_access) {
      VkImageMemoryBarrier const image_barrier = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = src_access,
         .dstAccessMask = dst_access,
         .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = image,
         .subresourceRange = whole_depth,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &image_barrier);
   };

   for (uint32_t mode_index = 0; mode_index < kModeCount; ++mode_index) {
      ModeCase const & mode_case = kModes[mode_index];
      bool const is_first = mode_index == 0;

      VkRenderingAttachmentInfoKHR depth_attachment = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
         .imageView = multisample.view,
         .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
         .resolveMode = mode_case.mode,
         .resolveImageView = destinations[mode_index].view,
         .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
         /* The first render fills the samples; the others must read back exactly what it left. */
         .loadOp = is_first ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      };
      depth_attachment.clearValue.depthStencil.depth = 1.0F;
      VkRenderingInfoKHR const rendering = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR,
         .renderArea = {{0, 0}, {kWidth, kHeight}},
         .layerCount = 1,
         .pDepthAttachment = &depth_attachment,
      };
      cmd_begin_rendering(command_buffer, &rendering);
      if (is_first) {
         for (uint32_t sample = 0; sample < kSampleCount; ++sample) {
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[sample]);
            vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(float), &kSampleDepths[sample]);
            vkCmdDraw(command_buffer, 6, 1, 0, 0);
         }
      }
      cmd_end_rendering(command_buffer);

      barrier(destinations[mode_index].image, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
              VK_ACCESS_TRANSFER_READ_BIT);
      barrier(multisample.image, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT);

      VkBufferImageCopy const readback_region = {
         .bufferOffset = image_bytes * mode_index,
         .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
         .imageExtent = {kWidth, kHeight, 1},
      };
      vkCmdCopyImageToBuffer(command_buffer, destinations[mode_index].image,
                             VK_IMAGE_LAYOUT_GENERAL, readback_buffer, 1, &readback_region);
   }

   VkMemoryBarrier const host_ready = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
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
   VkResult const wait_result = vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull);
   if (wait_result != VK_SUCCESS) {
      std::fprintf(stderr, "Fence wait failed with VkResult %d\n", wait_result);
      return 1;
   }

   uint32_t failures = 0;
   for (uint32_t mode_index = 0; mode_index < kModeCount; ++mode_index) {
      ModeCase const & mode_case = kModes[mode_index];
      float const expected = expected_depth(mode_case.mode, kSampleCount);
      float const * const pixels = readback_mapping + kWidth * kHeight * mode_index;
      uint32_t mismatches = 0;
      float first_bad = 0.0F;
      for (uint32_t texel = 0; texel < kWidth * kHeight; ++texel) {
         if (std::fabs(pixels[texel] - expected) <= 1.0F / 4096.0F)
            continue;
         if (mismatches == 0)
            first_bad = pixels[texel];
         ++mismatches;
      }
      if (mismatches == 0)
         continue;
      /* Naming the sample that was actually produced separates a mode that read the wrong sample
       * from one that read nothing.
       */
      int seen_sample = -1;
      for (uint32_t sample = 0; sample < kSampleCount; ++sample) {
         if (std::fabs(first_bad - kSampleDepths[sample]) <= 1.0F / 4096.0F)
            seen_sample = static_cast<int>(sample);
      }
      std::fprintf(stderr, "%s: %u/%u texels wrong, first is %f, expected %f%s", mode_case.name,
                   mismatches, kWidth * kHeight, first_bad, expected,
                   seen_sample >= 0 ? "" : "\n");
      if (seen_sample >= 0)
         std::fprintf(stderr, ", which is sample %d\n", seen_sample);
      ++failures;
   }
   std::printf("resolve_modes samples=%u modes=%u bad=%u %s\n", kSampleCount, kModeCount,
               failures,
               failures == 0 ? "PASS" : "FAIL");

   vkDeviceWaitIdle(device);
   vkDestroyFence(device, fence, nullptr);
   vkDestroyCommandPool(device, command_pool, nullptr);
   for (uint32_t sample = 0; sample < kSampleCount; ++sample)
      vkDestroyPipeline(device, pipelines[sample], nullptr);
   vkDestroyShaderModule(device, modules[0], nullptr);
   vkDestroyShaderModule(device, modules[1], nullptr);
   vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
   vkUnmapMemory(device, readback_memory);
   vkDestroyBuffer(device, readback_buffer, nullptr);
   vkFreeMemory(device, readback_memory, nullptr);
   vkDestroyImageView(device, multisample.view, nullptr);
   vkDestroyImage(device, multisample.image, nullptr);
   vkFreeMemory(device, multisample.memory, nullptr);
   for (Image const & destination : destinations) {
      vkDestroyImageView(device, destination.view, nullptr);
      vkDestroyImage(device, destination.image, nullptr);
      vkFreeMemory(device, destination.memory, nullptr);
   }
   vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr);
   return failures == 0 ? 0 : 1;
}
