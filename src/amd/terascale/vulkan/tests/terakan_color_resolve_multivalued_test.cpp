/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Colour-target resolve of a genuinely multi-valued multisample surface.
 *
 * Every other resolve test in this suite resolves a surface whose samples all hold the same value
 * -- they clear the multisample image and resolve that. Such a test passes for
 * VK_RESOLVE_MODE_AVERAGE no matter what the hardware does with the individual samples, because
 * the average of N copies of one value is that value: it cannot tell a real average from "read
 * sample zero", from "read one arbitrary sample", or from a resolve that silently ignores FMASK.
 *
 * This one gives every sample its own distinct colour first, with one draw per sample confined to
 * that sample by the pipeline's sample mask (the same technique terakan_color_msaa_fetch and
 * terakan_resolve_modes use), and then checks the resolved single-sample result against the actual
 * arithmetic mean of those colours. The colours are chosen so the mean is not equal to any
 * individual sample's value, so a resolve that returns one sample instead of averaging is caught.
 *
 * This is the resolve half of the FMASK/CMASK P0 item's acceptance criteria ("per-sample reads and
 * resolved reads pass for 2x/4x/8x images"). Note the multisample image deliberately does NOT
 * declare VK_IMAGE_USAGE_SAMPLED_BIT, so it keeps CB colour compression and fast clear enabled --
 * unlike terakan_color_msaa_fetch, which has to disable them to be sampled. That makes this test
 * cover the compressed CB write and CB_RESOLVE path specifically, which nothing else does.
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

uint32_t const write_vertex_spirv[] = {
#include "terakan_color_msaa_write.vert.spv.h"
};
uint32_t const write_fragment_spirv[] = {
#include "terakan_color_msaa_write.frag.spv.h"

};

/* Spread widely enough that the mean lands far from every individual sample: at two samples the
 * mean of 0.125 and 0.875 is 0.5, which neither sample holds, so "resolve returned some sample"
 * and "resolve averaged" are different answers.
 */
void
sample_colour(uint32_t sample_index, uint32_t sample_count, float out_colour[4])
{
   float const t = sample_count > 1 ? float(sample_index) / float(sample_count - 1) : 0.0F;
   out_colour[0] = 0.125F + t * 0.750F;
   out_colour[1] = 0.875F - t * 0.750F;
   out_colour[2] = 0.250F + t * 0.500F;
   out_colour[3] = 1.0F;
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

struct Image {
   VkImage image = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   VkImageView view = VK_NULL_HANDLE;
};

VkResult
create_image(VkDevice device, VkPhysicalDevice physical_device, VkSampleCountFlagBits samples,
            VkImageUsageFlags usage, Image & out)
{
   VkImageCreateInfo const info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
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
      .format = info.format,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   return vkCreateImageView(device, &view_info, nullptr, &out.view);
}

} // namespace

int
main(int argc, char ** argv)
{
   uint32_t sample_count = 2;
   if (argc == 2 && std::strncmp(argv[1], "--samples=", 10) == 0)
      sample_count = static_cast<uint32_t>(std::atoi(argv[1] + 10));
   VkSampleCountFlagBits const samples = sample_count == 8   ? VK_SAMPLE_COUNT_8_BIT
                                         : sample_count == 4 ? VK_SAMPLE_COUNT_4_BIT
                                                             : VK_SAMPLE_COUNT_2_BIT;
   sample_count = sample_count == 8 ? 8 : sample_count == 4 ? 4 : 2;

   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-color-resolve-multivalued-test",
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
   if (!(properties.limits.framebufferColorSampleCounts & samples)) {
      std::fprintf(stderr, "%ux colour rendering is not advertised, nothing to probe\n",
                   sample_count);
      return 77;
   }
   std::fprintf(stderr, "device=%s queue_family=%u samples=%u\n", properties.deviceName,
                queue_family, sample_count);

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

   /* No SAMPLED usage: this keeps CB compression and fast clear enabled for the multisample image,
    * which is exactly the path this test is here to cover.
    */
   Image multisample, resolved;
   VK_CHECK(create_image(device, physical_device, samples,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         multisample));
   VK_CHECK(create_image(device, physical_device, VK_SAMPLE_COUNT_1_BIT,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         resolved));

   VkDeviceSize const readback_bytes = kWidth * kHeight * 4;
   VkBufferCreateInfo const readback_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = readback_bytes,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer readback_buffer;
   VK_CHECK(vkCreateBuffer(device, &readback_info, nullptr, &readback_buffer));
   VkMemoryRequirements readback_requirements;
   vkGetBufferMemoryRequirements(device, readback_buffer, &readback_requirements);
   VkMemoryAllocateInfo const readback_allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = readback_requirements.size,
      .memoryTypeIndex =
         find_memory_type(physical_device, readback_requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   VkDeviceMemory readback_memory;
   VK_CHECK(vkAllocateMemory(device, &readback_allocation, nullptr, &readback_memory));
   VK_CHECK(vkBindBufferMemory(device, readback_buffer, readback_memory, 0));
   uint8_t * readback_mapping;
   VK_CHECK(vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0,
                        reinterpret_cast<void **>(&readback_mapping)));
   std::memset(readback_mapping, 0xA5, readback_bytes);

   VkAttachmentDescription const attachment = {
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .samples = samples,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
   };
   VkAttachmentReference const color_reference = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
   };
   VkSubpassDescription const subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_reference,
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
      .pAttachments = &multisample.view,
      .width = kWidth,
      .height = kHeight,
      .layers = 1,
   };
   VkFramebuffer framebuffer;
   VK_CHECK(vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer));

   VkShaderModuleCreateInfo const vertex_module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(write_vertex_spirv),
      .pCode = write_vertex_spirv,
   };
   VkShaderModule vertex_module;
   VK_CHECK(vkCreateShaderModule(device, &vertex_module_info, nullptr, &vertex_module));
   VkShaderModuleCreateInfo const fragment_module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(write_fragment_spirv),
      .pCode = write_fragment_spirv,
   };
   VkShaderModule fragment_module;
   VK_CHECK(vkCreateShaderModule(device, &fragment_module_info, nullptr, &fragment_module));
   VkPushConstantRange const push_range = {
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(float) * 4,
   };
   VkPipelineLayoutCreateInfo const pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push_range,
   };
   VkPipelineLayout pipeline_layout;
   VK_CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout));
   VkPipelineShaderStageCreateInfo const stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex_module, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment_module, .pName = "main"},
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
   VkPipelineColorBlendAttachmentState const blend_attachment = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
   };
   VkPipelineColorBlendStateCreateInfo const color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &blend_attachment,
   };
   std::vector<VkPipeline> pipelines(sample_count);
   std::vector<VkSampleMask> sample_masks(sample_count);
   for (uint32_t sample_index = 0; sample_index < sample_count; ++sample_index) {
      sample_masks[sample_index] = 1u << sample_index;
      VkPipelineMultisampleStateCreateInfo const multisample_state = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
         .rasterizationSamples = samples,
         .pSampleMask = &sample_masks[sample_index],
      };
      VkGraphicsPipelineCreateInfo const pipeline_info = {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .stageCount = 2, .pStages = stages,
         .pVertexInputState = &vertex_input,
         .pInputAssemblyState = &input_assembly,
         .pViewportState = &viewport_state,
         .pRasterizationState = &rasterization,
         .pMultisampleState = &multisample_state,
         .pColorBlendState = &color_blend,
         .layout = pipeline_layout,
         .renderPass = render_pass,
      };
      VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                         &pipelines[sample_index]));
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

   VkImageMemoryBarrier const resolved_to_general = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = resolved.image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                        &resolved_to_general);

   VkClearValue const clear = {.color = {.float32 = {0.0F, 0.0F, 0.0F, 1.0F}}};
   VkRenderPassBeginInfo const render_begin = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = render_pass,
      .framebuffer = framebuffer,
      .renderArea = {{0, 0}, {kWidth, kHeight}},
      .clearValueCount = 1,
      .pClearValues = &clear,
   };
   vkCmdBeginRenderPass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
   for (uint32_t sample_index = 0; sample_index < sample_count; ++sample_index) {
      float colour[4];
      sample_colour(sample_index, sample_count, colour);
      vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[sample_index]);
      vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(colour), colour);
      vkCmdDraw(command_buffer, 6, 1, 0, 0);
   }
   vkCmdEndRenderPass(command_buffer);

   VkImageMemoryBarrier const multisample_ready = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = multisample.image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &multisample_ready);

   VkImageResolve const resolve_region = {
      .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .srcOffset = {0, 0, 0},
      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .dstOffset = {0, 0, 0},
      .extent = {kWidth, kHeight, 1},
   };
   vkCmdResolveImage(command_buffer, multisample.image, VK_IMAGE_LAYOUT_GENERAL, resolved.image,
                     VK_IMAGE_LAYOUT_GENERAL, 1, &resolve_region);

   VkImageMemoryBarrier const resolved_to_transfer = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = resolved.image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                        &resolved_to_transfer);

   VkBufferImageCopy const readback_region = {
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {kWidth, kHeight, 1},
   };
   vkCmdCopyImageToBuffer(command_buffer, resolved.image, VK_IMAGE_LAYOUT_GENERAL, readback_buffer,
                          1, &readback_region);

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

   float expected[4] = {};
   for (uint32_t sample_index = 0; sample_index < sample_count; ++sample_index) {
      float colour[4];
      sample_colour(sample_index, sample_count, colour);
      for (uint32_t channel = 0; channel < 4; ++channel)
         expected[channel] += colour[channel] / float(sample_count);
   }
   /* Report what a "returned one sample instead of averaging" result would look like, so a failure
    * says which of the two it was.
    */
   float sample_zero[4];
   sample_colour(0, sample_count, sample_zero);

   uint32_t mismatches = 0;
   for (uint32_t y = 0; y < kHeight; ++y) {
      for (uint32_t x = 0; x < kWidth; ++x) {
         uint8_t const * const texel = &readback_mapping[(y * kWidth + x) * 4];
         bool ok = true;
         for (uint32_t channel = 0; channel < 4; ++channel) {
            float const actual = float(texel[channel]) / 255.0F;
            /* Two UNORM8 steps of slack: the hardware's averaging rounds per channel. */
            ok &= std::fabs(actual - expected[channel]) <= 2.5F / 255.0F;
         }
         if (!ok) {
            if (mismatches < 4) {
               std::fprintf(stderr,
                            "resolved(%u,%u) = (%.4f,%.4f,%.4f,%.4f), expected mean "
                            "(%.4f,%.4f,%.4f,%.4f) [sample 0 would be (%.4f,%.4f,%.4f,%.4f)]\n",
                            x, y, texel[0] / 255.0F, texel[1] / 255.0F, texel[2] / 255.0F,
                            texel[3] / 255.0F, expected[0], expected[1], expected[2], expected[3],
                            sample_zero[0], sample_zero[1], sample_zero[2], sample_zero[3]);
            }
            ++mismatches;
         }
      }
   }
   std::printf("color_resolve_multivalued samples=%u texels=%u mismatches=%u %s\n", sample_count,
               kWidth * kHeight, mismatches, mismatches == 0 ? "PASS" : "FAIL");

   vkDeviceWaitIdle(device);
   vkDestroyFence(device, fence, nullptr);
   vkDestroyCommandPool(device, command_pool, nullptr);
   for (VkPipeline pipeline : pipelines)
      vkDestroyPipeline(device, pipeline, nullptr);
   vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
   vkDestroyShaderModule(device, vertex_module, nullptr);
   vkDestroyShaderModule(device, fragment_module, nullptr);
   vkDestroyFramebuffer(device, framebuffer, nullptr);
   vkDestroyRenderPass(device, render_pass, nullptr);
   for (Image const & image : {multisample, resolved}) {
      vkDestroyImageView(device, image.view, nullptr);
      vkDestroyImage(device, image.image, nullptr);
      vkFreeMemory(device, image.memory, nullptr);
   }
   vkUnmapMemory(device, readback_memory);
   vkDestroyBuffer(device, readback_buffer, nullptr);
   vkFreeMemory(device, readback_memory, nullptr);
   vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr);
   return mismatches == 0 ? 0 : 1;
}
