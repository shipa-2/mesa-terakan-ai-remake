/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Regression coverage for the "stencil-only render targets on combined depth/stencil images" P0
 * item: a render that names ONLY a stencil attachment (`pDepthAttachment == NULL`,
 * `pStencilAttachment` naming a `D32_SFLOAT_S8_UINT` view) must write and read back the stencil
 * values it was asked to.
 *
 * This is the test that finally reproduced that bug deterministically, after two earlier
 * investigation passes failed to reproduce it at all. Two things made the difference, both of them
 * things TODO.md had explicitly recorded as untried:
 *
 *   - It draws through the rasterizer's STENCIL_REPLACE path rather than relying on a bare
 *     LOAD_OP_CLEAR. A clear alone had already been tried and passed, because the clear does not
 *     go through the same DB base-address programming a draw does.
 *   - It runs many renders back to back inside ONE command buffer with a different stencil
 *     reference each time, so a wrong result cannot be mistaken for an uninitialised read, and a
 *     stale value left by an earlier render in the same submission is distinguishable from a
 *     correct one.
 *
 * Root cause (fixed in terakan_hw_config_draw.c): when no depth attachment was bound,
 * terakan_hw_config_draw_set_db_depth_stencil_buffer() stored the two aspects' base addresses
 * SWAPPED, so the stencil-only emit path wrote the depth plane's address into
 * DB_STENCIL_READ_BASE/DB_STENCIL_WRITE_BASE and every stencil write landed in the depth plane.
 * Against the unfixed driver every iteration below reads back a constant unrelated byte.
 *
 * Both the broken shape (stencil alone) and the shape every existing stencil user works around it
 * with (a depth attachment bound alongside, which is also how VK_KHR_depth_stencil_resolve shapes
 * a real resolve) are covered, so a future change that fixes one by breaking the other is caught.
 */

#include <vulkan/vulkan.h>

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
constexpr uint32_t kIterations = 16;
constexpr VkFormat kFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;

/* Distinct per iteration, none of them zero (the clear value) or 0xFF (the readback fill), so a
 * render that wrote nothing at all is still caught.
 */
uint8_t
iteration_stencil(uint32_t iteration)
{
   return static_cast<uint8_t>(0x11 + iteration * 0x0D);
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

} // namespace

int
main(int argc, char ** argv)
{
   if (argc != 3) {
      std::fprintf(stderr, "usage: %s VERT_SPV FRAG_SPV\n", argv[0]);
      return 2;
   }
   std::vector<uint32_t> const vertex_spirv = read_spirv(argv[1]);
   std::vector<uint32_t> const fragment_spirv = read_spirv(argv[2]);
   if (vertex_spirv.empty() || fragment_spirv.empty()) {
      std::fprintf(stderr, "Unable to read shader SPIR-V\n");
      return 2;
   }

   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-stencil-only-render-test",
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
      /* Which generation this run is for is `terakan_test_device_matches`'s decision, not each
       * test's; see terakan_test_device.h.
       */
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
   std::fprintf(stderr, "device=%s queue_family=%u iterations=%u\n", properties.deviceName,
                queue_family, kIterations);

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

   VkImageCreateInfo const image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = kFormat,
      .extent = {kWidth, kHeight, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
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

   VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = kFormat,
      .subresourceRange = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1},
   };
   VkImageView stencil_view;
   VK_CHECK(vkCreateImageView(device, &view_info, nullptr, &stencil_view));
   view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
   VkImageView depth_view;
   VK_CHECK(vkCreateImageView(device, &view_info, nullptr, &depth_view));

   /* Two passes over the same image: one genuinely stencil-only, one with a depth attachment
    * bound alongside (the shape existing stencil users rely on).
    */
   constexpr uint32_t kPassCount = 2;
   VkDeviceSize const image_bytes = kWidth * kHeight;
   VkDeviceSize const readback_bytes = image_bytes * kIterations * kPassCount;
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
   uint8_t * readback_mapping;
   VK_CHECK(vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0,
                        reinterpret_cast<void **>(&readback_mapping)));
   std::memset(readback_mapping, 0xFF, readback_bytes);

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
   VkPipelineMultisampleStateCreateInfo const multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
   };
   VkPipelineColorBlendStateCreateInfo const color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
   };
   VkStencilOpState const stencil_op = {
      .failOp = VK_STENCIL_OP_REPLACE,
      .passOp = VK_STENCIL_OP_REPLACE,
      .depthFailOp = VK_STENCIL_OP_REPLACE,
      .compareOp = VK_COMPARE_OP_ALWAYS,
      .compareMask = 0xFF,
      .writeMask = 0xFF,
      .reference = 0,
   };
   VkPipelineDepthStencilStateCreateInfo const depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .stencilTestEnable = VK_TRUE,
      .front = stencil_op,
      .back = stencil_op,
   };
   VkDynamicState const dynamic_states[1] = {VK_DYNAMIC_STATE_STENCIL_REFERENCE};
   VkPipelineDynamicStateCreateInfo const dynamic_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 1,
      .pDynamicStates = dynamic_states,
   };

   /* Pass 0 declares no depth attachment format at all, matching its render; pass 1 declares one,
    * matching its own. A pipeline's rendering info has to agree with the render it is used in.
    */
   VkPipeline pipelines[kPassCount];
   for (uint32_t pass = 0; pass < kPassCount; ++pass) {
      VkPipelineRenderingCreateInfoKHR const pipeline_rendering = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
         .depthAttachmentFormat = pass == 0 ? VK_FORMAT_UNDEFINED : kFormat,
         .stencilAttachmentFormat = kFormat,
      };
      VkGraphicsPipelineCreateInfo const pipeline_info = {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .pNext = &pipeline_rendering,
         .stageCount = 2, .pStages = stages,
         .pVertexInputState = &vertex_input,
         .pInputAssemblyState = &input_assembly,
         .pViewportState = &viewport_state,
         .pRasterizationState = &rasterization,
         .pMultisampleState = &multisample,
         .pDepthStencilState = &depth_stencil,
         .pColorBlendState = &color_blend,
         .pDynamicState = &dynamic_state,
         .layout = pipeline_layout,
         .renderPass = VK_NULL_HANDLE,
      };
      VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                         &pipelines[pass]));
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

   VkImageMemoryBarrier const to_general = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1},
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                        &to_general);

   for (uint32_t pass = 0; pass < kPassCount; ++pass) {
      for (uint32_t iteration = 0; iteration < kIterations; ++iteration) {
         VkRenderingAttachmentInfoKHR stencil_attachment = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
            .imageView = stencil_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .resolveMode = VK_RESOLVE_MODE_NONE,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         };
         stencil_attachment.clearValue.depthStencil.stencil = 0;
         VkRenderingAttachmentInfoKHR depth_attachment = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
            .imageView = depth_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .resolveMode = VK_RESOLVE_MODE_NONE,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         };
         depth_attachment.clearValue.depthStencil.depth = 1.0F;
         VkRenderingInfoKHR const rendering = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR,
            .renderArea = {{0, 0}, {kWidth, kHeight}},
            .layerCount = 1,
            .pDepthAttachment = pass == 0 ? nullptr : &depth_attachment,
            .pStencilAttachment = &stencil_attachment,
         };
         cmd_begin_rendering(command_buffer, &rendering);
         vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[pass]);
         vkCmdSetStencilReference(command_buffer, VK_STENCIL_FACE_FRONT_AND_BACK,
                                  iteration_stencil(iteration));
         float const position_depth = 0.5F;
         vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                            sizeof(float), &position_depth);
         vkCmdDraw(command_buffer, 6, 1, 0, 0);
         cmd_end_rendering(command_buffer);

         VkImageMemoryBarrier const to_transfer = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1},
         };
         vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                              &to_transfer);

         VkBufferImageCopy const region = {
            .bufferOffset = image_bytes * (pass * kIterations + iteration),
            .imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1},
            .imageExtent = {kWidth, kHeight, 1},
         };
         vkCmdCopyImageToBuffer(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL, readback_buffer, 1,
                                &region);

         VkImageMemoryBarrier const back_to_attachment = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1},
         };
         vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr,
                              1, &back_to_attachment);
      }
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

   char const * const pass_names[kPassCount] = {"stencil-only", "with-depth-bound"};
   uint32_t total_bad_iterations = 0;
   for (uint32_t pass = 0; pass < kPassCount; ++pass) {
      uint32_t bad_iterations = 0;
      for (uint32_t iteration = 0; iteration < kIterations; ++iteration) {
         uint8_t const * const texels =
            readback_mapping + image_bytes * (pass * kIterations + iteration);
         uint8_t const expected = iteration_stencil(iteration);
         uint32_t mismatches = 0;
         uint8_t first_bad = 0;
         for (uint32_t i = 0; i < image_bytes; ++i) {
            if (texels[i] != expected) {
               if (mismatches == 0)
                  first_bad = texels[i];
               ++mismatches;
            }
         }
         if (mismatches != 0) {
            if (bad_iterations < 4) {
               std::fprintf(stderr,
                            "%s iteration %u: %u/%llu texels wrong, first was 0x%02X, expected "
                            "0x%02X\n",
                            pass_names[pass], iteration, mismatches,
                            (unsigned long long)image_bytes, first_bad, expected);
            }
            ++bad_iterations;
         }
      }
      std::printf("stencil_only_render %s iterations=%u bad=%u %s\n", pass_names[pass], kIterations,
                  bad_iterations, bad_iterations == 0 ? "PASS" : "FAIL");
      total_bad_iterations += bad_iterations;
   }

   vkDeviceWaitIdle(device);
   vkDestroyFence(device, fence, nullptr);
   vkDestroyCommandPool(device, command_pool, nullptr);
   for (VkPipeline pipeline : pipelines)
      vkDestroyPipeline(device, pipeline, nullptr);
   vkDestroyShaderModule(device, modules[0], nullptr);
   vkDestroyShaderModule(device, modules[1], nullptr);
   vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
   vkDestroyImageView(device, stencil_view, nullptr);
   vkDestroyImageView(device, depth_view, nullptr);
   vkDestroyImage(device, image, nullptr);
   vkFreeMemory(device, image_memory, nullptr);
   vkUnmapMemory(device, readback_memory);
   vkDestroyBuffer(device, readback_buffer, nullptr);
   vkFreeMemory(device, readback_memory, nullptr);
   vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr);
   return total_bad_iterations == 0 ? 0 : 1;
}
