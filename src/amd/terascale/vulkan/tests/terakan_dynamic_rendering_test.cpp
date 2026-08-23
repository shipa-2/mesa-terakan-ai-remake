/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* VK_KHR_dynamic_rendering used directly, without a VkRenderPass anywhere.
 *
 * Rendering is the driver's native path, since the common render pass implementation lowers
 * VkRenderPass onto vkCmdBeginRendering, so most of it is reached by every other test already.
 * What is not reached that way is the extension's own surface: attachments described per
 * recording rather than by a render pass object, a pipeline created from
 * VkPipelineRenderingCreateInfo instead of a render pass, and a render that is split across a
 * suspending and a resuming half.
 *
 * Three renders run against one image, each checked in two halves. A quad covers the left half, so
 * the right half reports what the load op did while the left half reports what the draw did.
 */

#include <vulkan/vulkan.h>

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

constexpr uint32_t kWidth = 32;
constexpr uint32_t kHeight = 32;
constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

struct Push {
   float color[4];
   float depth;
};

/* Exactly representable in UNORM8 so the comparison can be exact. */
struct Rgb {
   uint8_t r, g, b;
};
constexpr Rgb kBackground = {16, 32, 48};
constexpr Rgb kFirstDraw = {64, 80, 96};
constexpr Rgb kOccluded = {200, 210, 220};
constexpr Rgb kSecondDraw = {112, 128, 144};
constexpr Rgb kResumeBackground = {24, 40, 56};
constexpr Rgb kResumeDraw = {160, 176, 192};

Push
make_push(Rgb color, float depth)
{
   return Push{{color.r / 255.0F, color.g / 255.0F, color.b / 255.0F, 1.0F}, depth};
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
create_image(VkDevice device, VkPhysicalDevice physical_device, VkImageUsageFlags usage,
             VkFormat format, VkImageAspectFlags aspect, Image & out)
{
   VkImageCreateInfo const info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {kWidth, kHeight, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
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
      .format = format,
      .subresourceRange = {aspect, 0, 1, 0, 1},
   };
   return vkCreateImageView(device, &view_info, nullptr, &out.view);
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
      .pApplicationName = "terakan-dynamic-rendering-test",
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
      if (!std::strstr(properties.deviceName, "Terakan"))
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

   /* The point of the test is that the extension is actually advertised, not only that the code
    * behind it works, so a missing extension is a failure rather than a skip.
    */
   uint32_t extension_count = 0;
   VK_CHECK(vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count,
                                                 nullptr));
   std::vector<VkExtensionProperties> extensions(extension_count);
   VK_CHECK(vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count,
                                                 extensions.data()));
   bool dynamic_rendering_advertised = false;
   for (VkExtensionProperties const & extension : extensions) {
      if (std::strcmp(extension.extensionName, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) == 0)
         dynamic_rendering_advertised = true;
   }
   if (!dynamic_rendering_advertised) {
      std::fprintf(stderr, "%s is not advertised\n", VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
      return 1;
   }
   std::fprintf(stderr, "device=%s queue_family=%u\n", properties.deviceName, queue_family);

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

   Image color;
   VK_CHECK(create_image(device, physical_device,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT, color));
   Image depth;
   VK_CHECK(create_image(device, physical_device, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                         kDepthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, depth));

   constexpr uint32_t kRenderCount = 3;
   VkDeviceSize const image_bytes = kWidth * kHeight * 4;
   VkBufferCreateInfo const readback_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = image_bytes * kRenderCount,
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
   std::memset(readback_mapping, 0xA5, image_bytes * kRenderCount);

   VkPushConstantRange const push_range = {
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
      .offset = 0,
      .size = sizeof(Push),
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
   VkPipelineDepthStencilStateCreateInfo const depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE,
      .depthWriteEnable = VK_TRUE,
      .depthCompareOp = VK_COMPARE_OP_LESS,
   };
   VkPipelineColorBlendAttachmentState const blend_attachment = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
   };
   VkPipelineColorBlendStateCreateInfo const color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &blend_attachment,
   };
   /* No VkRenderPass: the pipeline learns its attachment formats from here instead. */
   VkPipelineRenderingCreateInfoKHR const pipeline_rendering = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &kColorFormat,
      .depthAttachmentFormat = kDepthFormat,
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
      .layout = pipeline_layout,
      .renderPass = VK_NULL_HANDLE,
   };
   VkPipeline pipeline;
   VK_CHECK(
      vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline));

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

   auto const color_attachment = [&](VkAttachmentLoadOp load_op, Rgb clear) {
      VkRenderingAttachmentInfoKHR info = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
         .imageView = color.view,
         .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
         .loadOp = load_op,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      };
      info.clearValue.color.float32[0] = clear.r / 255.0F;
      info.clearValue.color.float32[1] = clear.g / 255.0F;
      info.clearValue.color.float32[2] = clear.b / 255.0F;
      info.clearValue.color.float32[3] = 1.0F;
      return info;
   };
   auto const depth_attachment = [&](VkAttachmentLoadOp load_op) {
      VkRenderingAttachmentInfoKHR info = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
         .imageView = depth.view,
         .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
         .loadOp = load_op,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      };
      info.clearValue.depthStencil.depth = 0.5F;
      return info;
   };

   auto const draw = [&](Push const & push) {
      vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      vkCmdPushConstants(command_buffer, pipeline_layout,
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(push), &push);
      vkCmdDraw(command_buffer, 6, 1, 0, 0);
   };
   auto const copy_out = [&](uint32_t slot) {
      VkImageMemoryBarrier const to_transfer = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = color.image,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &to_transfer);
      VkBufferImageCopy const region = {
         .bufferOffset = image_bytes * slot,
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageExtent = {kWidth, kHeight, 1},
      };
      vkCmdCopyImageToBuffer(command_buffer, color.image, VK_IMAGE_LAYOUT_GENERAL, readback_buffer,
                             1, &region);
      VkMemoryBarrier const transfer_done = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
         .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                           0, 1, &transfer_done, 0, nullptr, 0, nullptr);
   };

   VkRenderingAttachmentInfoKHR color_info;
   VkRenderingAttachmentInfoKHR depth_info;
   VkRenderingInfoKHR rendering = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR,
      .renderArea = {{0, 0}, {kWidth, kHeight}},
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_info,
      .pDepthAttachment = &depth_info,
   };

   /* Render 0: clear both attachments, draw in front of the cleared depth, then draw behind it.
    * The second draw must be rejected, which is what proves the depth attachment named in
    * VkRenderingInfo is really bound and really cleared.
    */
   color_info = color_attachment(VK_ATTACHMENT_LOAD_OP_CLEAR, kBackground);
   depth_info = depth_attachment(VK_ATTACHMENT_LOAD_OP_CLEAR);
   rendering.flags = 0;
   cmd_begin_rendering(command_buffer, &rendering);
   draw(make_push(kFirstDraw, 0.25F));
   draw(make_push(kOccluded, 0.75F));
   cmd_end_rendering(command_buffer);
   copy_out(0);

   /* Render 1: load instead of clear. The right half has to still hold what render 0 stored. */
   color_info = color_attachment(VK_ATTACHMENT_LOAD_OP_LOAD, kBackground);
   depth_info = depth_attachment(VK_ATTACHMENT_LOAD_OP_CLEAR);
   rendering.flags = 0;
   cmd_begin_rendering(command_buffer, &rendering);
   draw(make_push(kSecondDraw, 0.25F));
   cmd_end_rendering(command_buffer);
   copy_out(1);

   /* Render 2: one render split in half. The suspending half only clears, the resuming half only
    * draws, and together they must behave as a single render.
    */
   color_info = color_attachment(VK_ATTACHMENT_LOAD_OP_CLEAR, kResumeBackground);
   depth_info = depth_attachment(VK_ATTACHMENT_LOAD_OP_CLEAR);
   rendering.flags = VK_RENDERING_SUSPENDING_BIT_KHR;
   cmd_begin_rendering(command_buffer, &rendering);
   cmd_end_rendering(command_buffer);
   color_info = color_attachment(VK_ATTACHMENT_LOAD_OP_LOAD, kResumeBackground);
   depth_info = depth_attachment(VK_ATTACHMENT_LOAD_OP_LOAD);
   rendering.flags = VK_RENDERING_RESUMING_BIT_KHR;
   cmd_begin_rendering(command_buffer, &rendering);
   draw(make_push(kResumeDraw, 0.25F));
   cmd_end_rendering(command_buffer);
   copy_out(2);

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

   struct Expectation {
      char const * name;
      Rgb left;
      Rgb right;
   };
   Expectation const expectations[kRenderCount] = {
      {"clear and depth test", kFirstDraw, kBackground},
      {"load preserves the stored contents", kSecondDraw, kBackground},
      {"suspend and resume", kResumeDraw, kResumeBackground},
   };

   uint32_t failures = 0;
   for (uint32_t render = 0; render < kRenderCount; ++render) {
      Expectation const & expectation = expectations[render];
      uint8_t const * const pixels = readback_mapping + image_bytes * render;
      uint32_t mismatches = 0;
      for (uint32_t y = 0; y < kHeight; ++y) {
         for (uint32_t x = 0; x < kWidth; ++x) {
            uint8_t const * const pixel = pixels + (y * kWidth + x) * 4;
            Rgb const & want = x < kWidth / 2 ? expectation.left : expectation.right;
            if (pixel[0] == want.r && pixel[1] == want.g && pixel[2] == want.b)
               continue;
            if (mismatches == 0) {
               std::fprintf(stderr,
                            "render %u (%s): (%u,%u) is (%u,%u,%u), expected (%u,%u,%u)\n", render,
                            expectation.name, x, y, pixel[0], pixel[1], pixel[2], want.r, want.g,
                            want.b);
            }
            ++mismatches;
         }
      }
      if (mismatches != 0) {
         std::fprintf(stderr, "render %u (%s): %u/%u texels wrong\n", render, expectation.name,
                      mismatches, kWidth * kHeight);
         ++failures;
      }
   }
   std::printf("dynamic_rendering renders=%u bad=%u %s\n", kRenderCount, failures,
               failures == 0 ? "PASS" : "FAIL");

   vkDeviceWaitIdle(device);
   vkDestroyFence(device, fence, nullptr);
   vkDestroyCommandPool(device, command_pool, nullptr);
   vkDestroyPipeline(device, pipeline, nullptr);
   vkDestroyShaderModule(device, modules[0], nullptr);
   vkDestroyShaderModule(device, modules[1], nullptr);
   vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
   for (Image const & image : {color, depth}) {
      vkDestroyImageView(device, image.view, nullptr);
      vkDestroyImage(device, image.image, nullptr);
      vkFreeMemory(device, image.memory, nullptr);
   }
   vkUnmapMemory(device, readback_memory);
   vkDestroyBuffer(device, readback_buffer, nullptr);
   vkFreeMemory(device, readback_memory, nullptr);
   vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr);
   return failures == 0 ? 0 : 1;
}
