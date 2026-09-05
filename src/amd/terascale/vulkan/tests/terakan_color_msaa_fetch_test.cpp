/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Coverage for the FMASK/CMASK P0 item: can a multisample COLOR image be fetched per sample?
 *
 * terakan_depth_msaa_fetch already answered this for depth, which needs no compression metadata on
 * Evergreen (Terakan does not implement HTILE). Multisample color is different: FMASK/CMASK memory
 * layout and identity initialization are implemented (terakan_image.c computes and allocates an
 * FMASK surface alongside the color surface, and the color and meta descriptors both carry the
 * FMASK pointer), but per-sample texture reads through that path were, before this test, unproven
 * -- see the note in TODO.md: "per-sample texture reads and real color-target resolve coverage are
 * still required".
 *
 * Gives every sample of an R8G8B8A8_UNORM image its own distinct colour -- one draw per sample,
 * each confined to that sample by the pipeline's sample mask -- then reads every sample of every
 * texel with texelFetch on a sampler2DMS from a compute shader, mirroring
 * terakan_depth_msaa_fetch's structure. Sample i must read back sample i's own colour, so this
 * catches a fetch that lands on the wrong plane, not merely one that lands outside the surface.
 * (An earlier version cleared every sample to the same colour and could only detect the latter.)
 *
 * Two real bugs were found and fixed through this test, both described in TODO.md: the identity
 * FMASK fill constants did not match the shader's fixed 4-bit-per-sample decode, and the identity
 * initialization never ran when a render pass performed the initial layout transition. A third,
 * larger one was found the same way: CB colour compression and fast clear were enabled for every
 * multisample image with metadata, including sampled ones, so the CB was free to leave fragment
 * planes unwritten while a texture fetch -- which never consults CMASK -- read them anyway.
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
constexpr float kClearColor[4] = {0.75F, 0.25F, 0.5F, 1.0F};

uint32_t const color_msaa_fetch_spirv[] = {
#include "terakan_color_msaa_fetch.spv.h"
};
uint32_t const color_msaa_write_vertex_spirv[] = {
#include "terakan_color_msaa_write.vert.spv.h"
};
uint32_t const color_msaa_write_fragment_spirv[] = {
#include "terakan_color_msaa_write.frag.spv.h"

};

/* One distinct, exactly-UNORM8-representable colour per sample, so a fetch that returns the right
 * value can only have read that sample's own plane -- the whole point of an identity FMASK. A test
 * that gave every sample the same colour (as this one originally did) would pass just as happily
 * while reading some other sample's plane.
 */
void
sample_colour(uint32_t sample_index, float out_colour[4])
{
   out_colour[0] = (16.0F + float(sample_index) * 24.0F) / 255.0F;
   out_colour[1] = (200.0F - float(sample_index) * 20.0F) / 255.0F;
   out_colour[2] = (64.0F + float(sample_index) * 9.0F) / 255.0F;
   out_colour[3] = 1.0F;
}

struct PushConstants {
   uint32_t width;
   uint32_t height;
   uint32_t sample_count;
};

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
main(int argc, char ** argv)
{
   /* --samples=N selects the multisample count to probe (2, 4 or 8), so 4x/8x can be checked
    * without a separate binary.
    */
   uint32_t sample_count = 2;
   if (argc == 2 && std::strncmp(argv[1], "--samples=", 10) == 0) {
      sample_count = static_cast<uint32_t>(std::atoi(argv[1] + 10));
   }
   VkSampleCountFlagBits const samples = sample_count == 8   ? VK_SAMPLE_COUNT_8_BIT
                                         : sample_count == 4 ? VK_SAMPLE_COUNT_4_BIT
                                                             : VK_SAMPLE_COUNT_2_BIT;
   sample_count = sample_count == 8 ? 8 : sample_count == 4 ? 4 : 2;

   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-color-msaa-fetch-test",
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
      return TERAKAN_TEST_DEVICE_NOT_FOUND_STATUS;
   }

   VkSampleCountFlags const color_sample_counts =
      properties.limits.framebufferColorSampleCounts & properties.limits.sampledImageColorSampleCounts;
   if (!(color_sample_counts & samples)) {
      std::fprintf(stderr, "%ux color sampling is not advertised, nothing to probe\n",
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

   VkImageCreateInfo const color_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {kWidth, kHeight, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = samples,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage color_image;
   VK_CHECK(vkCreateImage(device, &color_info, nullptr, &color_image));
   VkMemoryRequirements color_requirements;
   vkGetImageMemoryRequirements(device, color_image, &color_requirements);
   uint32_t const color_memory_type = find_memory_type(
      physical_device, color_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (color_memory_type == UINT32_MAX) {
      std::fprintf(stderr, "No device-local memory for the color image\n");
      return 1;
   }
   VkMemoryAllocateInfo const color_allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = color_requirements.size,
      .memoryTypeIndex = color_memory_type,
   };
   VkDeviceMemory color_memory;
   VK_CHECK(vkAllocateMemory(device, &color_allocation, nullptr, &color_memory));
   VK_CHECK(vkBindImageMemory(device, color_image, color_memory, 0));

   VkImageViewCreateInfo const color_view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = color_image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = color_info.format,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   VkImageView color_view;
   VK_CHECK(vkCreateImageView(device, &color_view_info, nullptr, &color_view));

   uint32_t const output_count = kWidth * kHeight * sample_count;
   VkBufferCreateInfo const output_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = output_count * sizeof(float) * 4,
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
   if (output_memory_type == UINT32_MAX) {
      std::fprintf(stderr, "No host-visible coherent memory for the output buffer\n");
      return 1;
   }
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
   for (uint32_t i = 0; i < output_count * 4; ++i)
      output_mapping[i] = -1.0F;

   VkSamplerCreateInfo const sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
   };
   VkSampler sampler;
   VK_CHECK(vkCreateSampler(device, &sampler_info, nullptr, &sampler));

   VkDescriptorSetLayoutBinding const layout_bindings[] = {
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
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
   VkDescriptorSetLayoutCreateInfo const set_layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2,
      .pBindings = layout_bindings,
   };
   VkDescriptorSetLayout set_layout;
   VK_CHECK(vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &set_layout));
   VkPushConstantRange const push_range = {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset = 0,
      .size = sizeof(PushConstants),
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

   VkDescriptorImageInfo const image_descriptor = {
      .sampler = sampler,
      .imageView = color_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
   };
   VkDescriptorBufferInfo const buffer_descriptor = {
      .buffer = output_buffer,
      .offset = 0,
      .range = VK_WHOLE_SIZE,
   };
   VkWriteDescriptorSet const descriptor_writes[] = {
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_set,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &image_descriptor,
      },
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_set,
         .dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &buffer_descriptor,
      },
   };
   vkUpdateDescriptorSets(device, 2, descriptor_writes, 0, nullptr);

   VkShaderModuleCreateInfo const module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(color_msaa_fetch_spirv),
      .pCode = color_msaa_fetch_spirv,
   };
   VkShaderModule shader_module;
   VK_CHECK(vkCreateShaderModule(device, &module_info, nullptr, &shader_module));
   VkComputePipelineCreateInfo const pipeline_info = {
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
   VK_CHECK(
      vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline));

   /* The render pass exists only to clear the multisample color image to a known value. */
   VkAttachmentDescription const attachment = {
      .format = color_info.format,
      .samples = samples,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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
      .pAttachments = &color_view,
      .width = kWidth,
      .height = kHeight,
      .layers = 1,
   };
   VkFramebuffer framebuffer;
   VK_CHECK(vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer));

   /* One graphics pipeline per sample: its sample mask confines the draw to that sample, so each
    * sample's plane ends up holding a value no other sample has.
    */
   VkShaderModuleCreateInfo const write_vertex_module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(color_msaa_write_vertex_spirv),
      .pCode = color_msaa_write_vertex_spirv,
   };
   VkShaderModule write_vertex_module;
   VK_CHECK(vkCreateShaderModule(device, &write_vertex_module_info, nullptr, &write_vertex_module));
   VkShaderModuleCreateInfo const write_fragment_module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(color_msaa_write_fragment_spirv),
      .pCode = color_msaa_write_fragment_spirv,
   };
   VkShaderModule write_fragment_module;
   VK_CHECK(
      vkCreateShaderModule(device, &write_fragment_module_info, nullptr, &write_fragment_module));
   VkPushConstantRange const write_push_range = {
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      .offset = 0,
      .size = sizeof(float) * 4,
   };
   VkPipelineLayoutCreateInfo const write_pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &write_push_range,
   };
   VkPipelineLayout write_pipeline_layout;
   VK_CHECK(vkCreatePipelineLayout(device, &write_pipeline_layout_info, nullptr,
                                   &write_pipeline_layout));
   VkPipelineShaderStageCreateInfo const write_stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = write_vertex_module, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = write_fragment_module, .pName = "main"},
   };
   VkPipelineVertexInputStateCreateInfo const write_vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
   };
   VkPipelineInputAssemblyStateCreateInfo const write_input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   VkViewport const write_viewport = {0.0F, 0.0F, (float)kWidth, (float)kHeight, 0.0F, 1.0F};
   VkRect2D const write_scissor = {{0, 0}, {kWidth, kHeight}};
   VkPipelineViewportStateCreateInfo const write_viewport_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1, .pViewports = &write_viewport,
      .scissorCount = 1, .pScissors = &write_scissor,
   };
   VkPipelineRasterizationStateCreateInfo const write_rasterization = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0F,
   };
   VkPipelineColorBlendAttachmentState const write_blend_attachment = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
   };
   VkPipelineColorBlendStateCreateInfo const write_color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &write_blend_attachment,
   };
   std::vector<VkPipeline> write_pipelines(sample_count);
   std::vector<VkSampleMask> write_sample_masks(sample_count);
   for (uint32_t sample_index = 0; sample_index < sample_count; ++sample_index) {
      write_sample_masks[sample_index] = 1u << sample_index;
      VkPipelineMultisampleStateCreateInfo const write_multisample = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
         .rasterizationSamples = samples,
         .pSampleMask = &write_sample_masks[sample_index],
      };
      VkGraphicsPipelineCreateInfo const write_pipeline_info = {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .stageCount = 2, .pStages = write_stages,
         .pVertexInputState = &write_vertex_input,
         .pInputAssemblyState = &write_input_assembly,
         .pViewportState = &write_viewport_state,
         .pRasterizationState = &write_rasterization,
         .pMultisampleState = &write_multisample,
         .pColorBlendState = &write_color_blend,
         .layout = write_pipeline_layout,
         .renderPass = render_pass,
      };
      VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &write_pipeline_info, nullptr,
                                         &write_pipelines[sample_index]));
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

   VkClearValue const clear = {
      .color = {.float32 = {kClearColor[0], kClearColor[1], kClearColor[2], kClearColor[3]}}};
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
      sample_colour(sample_index, colour);
      vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        write_pipelines[sample_index]);
      vkCmdPushConstants(command_buffer, write_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(colour), colour);
      vkCmdDraw(command_buffer, 6, 1, 0, 0);
   }
   vkCmdEndRenderPass(command_buffer);

   VkImageMemoryBarrier const color_ready = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = color_image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                        &color_ready);

   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                           &descriptor_set, 0, nullptr);
   PushConstants const push_constants = {kWidth, kHeight, sample_count};
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

   VkFenceCreateInfo const fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   VK_CHECK(vkCreateFence(device, &fence_info, nullptr, &fence));
   VkSubmitInfo const submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
   };
   VK_CHECK(vkQueueSubmit(queue, 1, &submit_info, fence));
   VkResult const wait_result = vkWaitForFences(device, 1, &fence, VK_TRUE, 3000000000ull);
   if (wait_result != VK_SUCCESS) {
      std::fprintf(stderr, "Fence wait failed with VkResult %d\n", wait_result);
      return 1;
   }

   uint32_t mismatches = 0;
   for (uint32_t y = 0; y < kHeight; ++y) {
      for (uint32_t x = 0; x < kWidth; ++x) {
         for (uint32_t sample_index = 0; sample_index < sample_count; ++sample_index) {
            float const * const actual =
               &output_mapping[((y * kWidth + x) * sample_count + sample_index) * 4];
            float expected[4];
            sample_colour(sample_index, expected);
            bool ok = true;
            for (uint32_t channel = 0; channel < 4; ++channel)
               /* One UNORM8 quantization step is 1/255; allow a little more slack than that. */
               ok &= std::fabs(actual[channel] - expected[channel]) <= 1.0F / 128.0F;
            if (!ok) {
               if (mismatches < 8) {
                  std::fprintf(stderr,
                               "color(%u,%u) sample %u = (%.4f,%.4f,%.4f,%.4f), expected "
                               "(%.4f,%.4f,%.4f,%.4f) FAIL\n",
                               x, y, sample_index, actual[0], actual[1], actual[2], actual[3],
                               expected[0], expected[1], expected[2], expected[3]);
               }
               ++mismatches;
            }
         }
      }
   }
   std::printf("color_msaa_fetch samples=%u values=%u mismatches=%u %s\n", sample_count,
               output_count, mismatches, mismatches == 0 ? "PASS" : "FAIL");

   vkDeviceWaitIdle(device);
   vkDestroyFence(device, fence, nullptr);
   vkDestroyCommandPool(device, command_pool, nullptr);
   vkDestroyFramebuffer(device, framebuffer, nullptr);
   vkDestroyRenderPass(device, render_pass, nullptr);
   vkDestroyPipeline(device, pipeline, nullptr);
   for (VkPipeline write_pipeline : write_pipelines)
      vkDestroyPipeline(device, write_pipeline, nullptr);
   vkDestroyPipelineLayout(device, write_pipeline_layout, nullptr);
   vkDestroyShaderModule(device, write_vertex_module, nullptr);
   vkDestroyShaderModule(device, write_fragment_module, nullptr);
   vkDestroyShaderModule(device, shader_module, nullptr);
   vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
   vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
   vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
   vkDestroySampler(device, sampler, nullptr);
   vkDestroyImageView(device, color_view, nullptr);
   vkDestroyImage(device, color_image, nullptr);
   vkFreeMemory(device, color_memory, nullptr);
   vkUnmapMemory(device, output_memory);
   vkDestroyBuffer(device, output_buffer, nullptr);
   vkFreeMemory(device, output_memory, nullptr);
   vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr);
   return mismatches == 0 ? 0 : 1;
}
