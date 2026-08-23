/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Cache and barrier coherency across a repeated frame chain.
 *
 * The existing tests each exercise one hazard in isolation and all pass, while real applications
 * still show frame-to-frame corruption, so this one tests the composition instead: many frames of
 * clear, sample, render and copy recorded into a single command buffer, with only the barriers a
 * correct application would issue.
 *
 * --depth replaces the colour producer with a depth-only render pass, which is the shape a shadow
 * map has and the one applications lean on hardest: the frame structure Buckshot Roulette records
 * is dominated by depth-only passes that are then sampled. It exercises the DB-to-texture path that
 * the colour chain never touches.
 *
 * Each frame clears attachment A to a colour unique to that frame, transitions it to be sampled,
 * samples it into attachment B, transitions B for transfer, and copies B into its own slice of a
 * readback buffer. Any frame that observes a neighbouring frame's colour has seen through a
 * barrier, and the failure names the frame it actually saw.
 */

#include <vulkan/vulkan.h>

#include <array>
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

constexpr uint32_t kWidth = 16;
constexpr uint32_t kHeight = 16;
constexpr uint32_t kFrameCount = 24;

/* Distinct, exactly representable in UNORM8, and different in every channel so a partial stale
 * read is still caught.
 */
uint8_t
frame_channel(uint32_t frame, uint32_t channel)
{
   return static_cast<uint8_t>(8u + frame * 9u + channel * 3u);
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
      .format = info.format,
      .subresourceRange = {aspect, 0, 1, 0, 1},
   };
   return vkCreateImageView(device, &view_info, nullptr, &out.view);
}

} // namespace

int
main(int argc, char ** argv)
{
   /* --compute produces each frame from a compute shader writing a storage image instead of a
    * render pass clear, which is the shape real applications use and the one the driver's compute
    * RAT coherency covers least.
    */
   bool const use_compute = argc == 5 && std::strcmp(argv[4], "--compute") == 0;
   bool const use_depth = argc == 5 && std::strcmp(argv[4], "--depth") == 0;
   if (argc != 3 && !(argc == 5 && (use_compute || use_depth))) {
      std::fprintf(stderr, "usage: %s VERT_SPV FRAG_SPV [COMP_SPV --compute | DEPTH_FRAG_SPV --depth]\n",
                   argv[0]);
      return 2;
   }
   std::vector<uint32_t> const vertex_spirv = read_spirv(argv[1]);
   /* In depth mode the second pass samples a depth texture, so it needs its own fragment shader. */
   std::vector<uint32_t> const fragment_spirv = read_spirv(use_depth ? argv[3] : argv[2]);
   std::vector<uint32_t> const compute_spirv = use_compute ? read_spirv(argv[3])
                                                           : std::vector<uint32_t>{1};
   if (compute_spirv.empty()) {
      std::fprintf(stderr, "Unable to read the compute SPIR-V\n");
      return 2;
   }
   if (vertex_spirv.empty() || fragment_spirv.empty()) {
      std::fprintf(stderr, "Unable to read shader SPIR-V\n");
      return 2;
   }

   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-frame-chain-test",
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
   std::fprintf(stderr, "device=%s queue_family=%u frames=%u\n", properties.deviceName,
                queue_family, kFrameCount);

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

   Image source;
   VK_CHECK(create_image(device, physical_device,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                            VK_IMAGE_USAGE_STORAGE_BIT,
                         VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, source));
   Image target;
   VK_CHECK(create_image(device, physical_device,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, target));
   /* The depth producer. Created unconditionally so the teardown stays uniform. */
   Image depth;
   VK_CHECK(create_image(device, physical_device,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, depth));

   VkDeviceSize const frame_bytes = kWidth * kHeight * 4;
   VkBufferCreateInfo const readback_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = frame_bytes * kFrameCount,
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
   std::memset(readback_mapping, 0xA5, frame_bytes * kFrameCount);

   /* One render pass description reused for both passes of every frame. */
   VkAttachmentDescription const attachment = {
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .samples = VK_SAMPLE_COUNT_1_BIT,
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

   VkFramebuffer source_framebuffer, target_framebuffer;
   VkFramebufferCreateInfo framebuffer_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = render_pass,
      .attachmentCount = 1,
      .pAttachments = &source.view,
      .width = kWidth,
      .height = kHeight,
      .layers = 1,
   };
   VK_CHECK(vkCreateFramebuffer(device, &framebuffer_info, nullptr, &source_framebuffer));
   framebuffer_info.pAttachments = &target.view;
   VK_CHECK(vkCreateFramebuffer(device, &framebuffer_info, nullptr, &target_framebuffer));

   /* A depth-only pass with no colour attachments at all, matching what the shadow passes of a
    * real frame look like to the driver.
    */
   VkRenderPass depth_render_pass = VK_NULL_HANDLE;
   VkFramebuffer depth_framebuffer = VK_NULL_HANDLE;
   if (use_depth) {
      VkAttachmentDescription const depth_attachment = {
         .format = VK_FORMAT_D32_SFLOAT,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
      };
      VkAttachmentReference const depth_reference = {
         .attachment = 0,
         .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
      };
      VkSubpassDescription const depth_subpass = {
         .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
         .pDepthStencilAttachment = &depth_reference,
      };
      VkRenderPassCreateInfo const depth_render_pass_info = {
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
         .attachmentCount = 1,
         .pAttachments = &depth_attachment,
         .subpassCount = 1,
         .pSubpasses = &depth_subpass,
      };
      VK_CHECK(vkCreateRenderPass(device, &depth_render_pass_info, nullptr, &depth_render_pass));
      VkFramebufferCreateInfo const depth_framebuffer_info = {
         .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
         .renderPass = depth_render_pass,
         .attachmentCount = 1,
         .pAttachments = &depth.view,
         .width = kWidth,
         .height = kHeight,
         .layers = 1,
      };
      VK_CHECK(vkCreateFramebuffer(device, &depth_framebuffer_info, nullptr, &depth_framebuffer));
   }

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

   VkDescriptorSetLayoutBinding const layout_binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
   };
   VkDescriptorSetLayoutCreateInfo const set_layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &layout_binding,
   };
   VkDescriptorSetLayout set_layout;
   VK_CHECK(vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &set_layout));
   VkPipelineLayoutCreateInfo const pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout,
   };
   VkPipelineLayout pipeline_layout;
   VK_CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout));

   VkDescriptorPoolSize const pool_size = {
      .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1,
   };
   VkDescriptorPoolCreateInfo const pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
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
      .imageView = use_depth ? depth.view : source.view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
   };
   VkWriteDescriptorSet const descriptor_write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = descriptor_set,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .pImageInfo = &image_descriptor,
   };
   vkUpdateDescriptorSets(device, 1, &descriptor_write, 0, nullptr);

   VkShaderModule modules[2];
   std::array<std::vector<uint32_t> const *, 2> const shader_code = {&vertex_spirv,
                                                                     &fragment_spirv};
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
   VkPipelineColorBlendAttachmentState const blend_attachment = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
   };
   VkPipelineColorBlendStateCreateInfo const color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &blend_attachment,
   };
   VkGraphicsPipelineCreateInfo const pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2, .pStages = stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport_state,
      .pRasterizationState = &rasterization,
      .pMultisampleState = &multisample,
      .pColorBlendState = &color_blend,
      .layout = pipeline_layout,
      .renderPass = render_pass,
   };
   VkPipeline pipeline;
   VK_CHECK(
      vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline));

   /* Compute path: its own set layout with a storage image, plus a pipeline. */
   VkDescriptorSetLayout compute_set_layout = VK_NULL_HANDLE;
   VkPipelineLayout compute_pipeline_layout = VK_NULL_HANDLE;
   VkPipeline compute_pipeline = VK_NULL_HANDLE;
   VkDescriptorSet compute_descriptor_set = VK_NULL_HANDLE;
   VkShaderModule compute_module = VK_NULL_HANDLE;
   VkDescriptorPool compute_descriptor_pool = VK_NULL_HANDLE;
   struct ComputePushConstants { uint32_t frame, width, height; };
   if (use_compute) {
      VkDescriptorSetLayoutBinding const compute_binding = {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      };
      VkDescriptorSetLayoutCreateInfo const compute_set_layout_info = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
         .bindingCount = 1,
         .pBindings = &compute_binding,
      };
      VK_CHECK(vkCreateDescriptorSetLayout(device, &compute_set_layout_info, nullptr,
                                           &compute_set_layout));
      VkPushConstantRange const compute_push_range = {
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
         .offset = 0,
         .size = sizeof(ComputePushConstants),
      };
      VkPipelineLayoutCreateInfo const compute_pipeline_layout_info = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
         .setLayoutCount = 1,
         .pSetLayouts = &compute_set_layout,
         .pushConstantRangeCount = 1,
         .pPushConstantRanges = &compute_push_range,
      };
      VK_CHECK(vkCreatePipelineLayout(device, &compute_pipeline_layout_info, nullptr,
                                      &compute_pipeline_layout));
      VkDescriptorPoolSize const compute_pool_size = {
         .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = 1,
      };
      VkDescriptorPoolCreateInfo const compute_pool_info = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
         .maxSets = 1,
         .poolSizeCount = 1,
         .pPoolSizes = &compute_pool_size,
      };
      VK_CHECK(vkCreateDescriptorPool(device, &compute_pool_info, nullptr,
                                      &compute_descriptor_pool));
      VkDescriptorSetAllocateInfo const compute_set_allocate_info = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
         .descriptorPool = compute_descriptor_pool,
         .descriptorSetCount = 1,
         .pSetLayouts = &compute_set_layout,
      };
      VK_CHECK(vkAllocateDescriptorSets(device, &compute_set_allocate_info,
                                        &compute_descriptor_set));
      VkDescriptorImageInfo const storage_descriptor = {
         .imageView = source.view,
         .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      };
      VkWriteDescriptorSet const compute_write = {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = compute_descriptor_set,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo = &storage_descriptor,
      };
      vkUpdateDescriptorSets(device, 1, &compute_write, 0, nullptr);

      VkShaderModuleCreateInfo const compute_module_info = {
         .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
         .codeSize = compute_spirv.size() * sizeof(uint32_t),
         .pCode = compute_spirv.data(),
      };
      VK_CHECK(vkCreateShaderModule(device, &compute_module_info, nullptr, &compute_module));
      VkComputePipelineCreateInfo const compute_pipeline_info = {
         .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
         .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                   .module = compute_module,
                   .pName = "main"},
         .layout = compute_pipeline_layout,
      };
      VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute_pipeline_info, nullptr,
                                        &compute_pipeline));
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

   for (uint32_t frame = 0; frame < kFrameCount; ++frame) {
      /* Pass 1: give the source a colour that only this frame uses. */
      VkClearValue const clear = {.color = {.float32 = {frame_channel(frame, 0) / 255.0F,
                                                        frame_channel(frame, 1) / 255.0F,
                                                        frame_channel(frame, 2) / 255.0F, 1.0F}}};
      VkRenderPassBeginInfo render_begin = {
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
         .renderPass = render_pass,
         .framebuffer = source_framebuffer,
         .renderArea = {{0, 0}, {kWidth, kHeight}},
         .clearValueCount = 1,
         .pClearValues = &clear,
      };
      if (use_depth) {
         /* Exactly representable in UNORM8 after the sampling shader writes it back out. */
         VkClearValue const depth_clear = {
            .depthStencil = {.depth = frame_channel(frame, 0) / 255.0F},
         };
         VkRenderPassBeginInfo const depth_begin = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = depth_render_pass,
            .framebuffer = depth_framebuffer,
            .renderArea = {{0, 0}, {kWidth, kHeight}},
            .clearValueCount = 1,
            .pClearValues = &depth_clear,
         };
         vkCmdBeginRenderPass(command_buffer, &depth_begin, VK_SUBPASS_CONTENTS_INLINE);
         vkCmdEndRenderPass(command_buffer);
      } else if (use_compute) {
         vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline);
         vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                 compute_pipeline_layout, 0, 1, &compute_descriptor_set, 0,
                                 nullptr);
         ComputePushConstants const push = {frame, kWidth, kHeight};
         vkCmdPushConstants(command_buffer, compute_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                            0, sizeof(push), &push);
         vkCmdDispatch(command_buffer, (kWidth + 7) / 8, (kHeight + 7) / 8, 1);
      } else {
         vkCmdBeginRenderPass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
         vkCmdEndRenderPass(command_buffer);
      }

      VkAccessFlags producer_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      VkPipelineStageFlags producer_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      if (use_compute) {
         producer_access = VK_ACCESS_SHADER_WRITE_BIT;
         producer_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      } else if (use_depth) {
         producer_access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
         producer_stage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
      }
      VkImageMemoryBarrier const source_to_sampled = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = producer_access,
         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = use_depth ? depth.image : source.image,
         .subresourceRange = {use_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                              0, 1, 0, 1},
      };
      vkCmdPipelineBarrier(command_buffer, producer_stage, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &source_to_sampled);

      /* Pass 2: sample it straight through into the target. */
      VkClearValue const target_clear = {.color = {.float32 = {0.0F, 0.0F, 0.0F, 0.0F}}};
      render_begin.framebuffer = target_framebuffer;
      render_begin.pClearValues = &target_clear;
      vkCmdBeginRenderPass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0,
                              1, &descriptor_set, 0, nullptr);
      vkCmdDraw(command_buffer, 3, 1, 0, 0);
      vkCmdEndRenderPass(command_buffer);

      VkImageMemoryBarrier const target_to_transfer = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = target.image,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &target_to_transfer);

      VkBufferImageCopy const region = {
         .bufferOffset = frame_bytes * frame,
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageExtent = {kWidth, kHeight, 1},
      };
      vkCmdCopyImageToBuffer(command_buffer, target.image, VK_IMAGE_LAYOUT_GENERAL,
                             readback_buffer, 1, &region);

      /* The next frame overwrites both images, so the copy has to be done with them first. */
      VkMemoryBarrier const transfer_done = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                           0, 1, &transfer_done, 0, nullptr, 0, nullptr);
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

   /* The depth producer feeds one value through all three channels, the colour producer a
    * different value per channel.
    */
   auto const expected_channel = [use_depth](uint32_t frame, uint32_t channel) {
      return frame_channel(frame, use_depth ? 0 : channel);
   };

   uint32_t bad_frames = 0;
   for (uint32_t frame = 0; frame < kFrameCount; ++frame) {
      uint8_t const * const pixels = readback_mapping + frame_bytes * frame;
      uint32_t mismatches = 0;
      uint8_t first_bad[4] = {};
      for (uint32_t texel = 0; texel < kWidth * kHeight; ++texel) {
         uint8_t const * const pixel = pixels + texel * 4;
         bool matches = true;
         for (uint32_t channel = 0; channel < 3; ++channel)
            matches &= pixel[channel] == expected_channel(frame, channel);
         if (!matches) {
            if (mismatches == 0)
               std::memcpy(first_bad, pixel, 4);
            ++mismatches;
         }
      }
      if (mismatches != 0) {
         /* Name the frame whose colour was actually observed, which says how far the stale data
          * came from.
          */
         int seen_frame = -1;
         for (uint32_t candidate = 0; candidate < kFrameCount; ++candidate) {
            if (first_bad[0] == expected_channel(candidate, 0) &&
                first_bad[1] == expected_channel(candidate, 1) &&
                first_bad[2] == expected_channel(candidate, 2)) {
               seen_frame = static_cast<int>(candidate);
               break;
            }
         }
         std::fprintf(stderr,
                      "frame %u: %u/%u texels wrong, first was (%u,%u,%u) which is %s\n", frame,
                      mismatches, kWidth * kHeight, first_bad[0], first_bad[1], first_bad[2],
                      seen_frame >= 0 ? "another frame's colour" : "not any frame's colour");
         if (seen_frame >= 0)
            std::fprintf(stderr, "  that colour belongs to frame %d\n", seen_frame);
         ++bad_frames;
      }
   }
   std::printf("frame_chain frames=%u bad=%u %s\n", kFrameCount, bad_frames,
               bad_frames == 0 ? "PASS" : "FAIL");

   vkDeviceWaitIdle(device);
   vkDestroyFence(device, fence, nullptr);
   vkDestroyCommandPool(device, command_pool, nullptr);
   vkDestroyPipeline(device, pipeline, nullptr);
   vkDestroyShaderModule(device, modules[0], nullptr);
   vkDestroyShaderModule(device, modules[1], nullptr);
   vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
   vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
   vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
   vkDestroySampler(device, sampler, nullptr);
   vkDestroyFramebuffer(device, source_framebuffer, nullptr);
   vkDestroyFramebuffer(device, target_framebuffer, nullptr);
   vkDestroyFramebuffer(device, depth_framebuffer, nullptr);
   vkDestroyRenderPass(device, render_pass, nullptr);
   vkDestroyRenderPass(device, depth_render_pass, nullptr);
   for (Image const & image : {source, target, depth}) {
      vkDestroyImageView(device, image.view, nullptr);
      vkDestroyImage(device, image.image, nullptr);
      vkFreeMemory(device, image.memory, nullptr);
   }
   vkUnmapMemory(device, readback_memory);
   vkDestroyBuffer(device, readback_buffer, nullptr);
   vkFreeMemory(device, readback_memory, nullptr);
   vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr);
   return bad_frames == 0 ? 0 : 1;
}
