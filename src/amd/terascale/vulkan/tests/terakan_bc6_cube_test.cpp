#include <vulkan/vulkan.h>

#include "util/format/u_format_bptc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
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

constexpr uint32_t kBaseSize = 128;
constexpr uint32_t kMipCount = 8;
constexpr uint32_t kFaceCount = 6;
constexpr uint32_t kOutputWidth = kMipCount * kFaceCount;

struct Buffer {
   VkBuffer buffer = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   uint8_t *mapping = nullptr;
};

struct Image {
   VkImage image = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;
};

uint32_t
find_memory_type(VkPhysicalDevice physical_device, uint32_t bits, VkMemoryPropertyFlags flags)
{
   VkPhysicalDeviceMemoryProperties properties;
   vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
   for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) &&
          (properties.memoryTypes[i].propertyFlags & flags) == flags)
         return i;
   }
   return UINT32_MAX;
}

std::vector<uint32_t>
read_spirv(char const *path)
{
   std::ifstream file(path, std::ios::binary | std::ios::ate);
   if (!file)
      return {};
   std::streamsize const byte_count = file.tellg();
   if (byte_count <= 0 || byte_count % sizeof(uint32_t))
      return {};
   file.seekg(0);
   std::vector<uint32_t> words(static_cast<size_t>(byte_count) / sizeof(uint32_t));
   file.read(reinterpret_cast<char *>(words.data()), byte_count);
   return file ? words : std::vector<uint32_t>{};
}

VkResult
create_host_buffer(VkDevice device, VkPhysicalDevice physical_device, VkDeviceSize size,
                   VkBufferUsageFlags usage, Buffer &out)
{
   VkBufferCreateInfo const info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkResult result = vkCreateBuffer(device, &info, nullptr, &out.buffer);
   if (result != VK_SUCCESS)
      return result;
   VkMemoryRequirements requirements;
   vkGetBufferMemoryRequirements(device, out.buffer, &requirements);
   uint32_t const memory_type =
      find_memory_type(physical_device, requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (memory_type == UINT32_MAX)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   VkMemoryAllocateInfo const allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = memory_type,
   };
   result = vkAllocateMemory(device, &allocation, nullptr, &out.memory);
   if (result == VK_SUCCESS)
      result = vkBindBufferMemory(device, out.buffer, out.memory, 0);
   if (result == VK_SUCCESS)
      result = vkMapMemory(device, out.memory, 0, size, 0,
                           reinterpret_cast<void **>(&out.mapping));
   return result;
}

VkResult
create_image(VkDevice device, VkPhysicalDevice physical_device, VkImageCreateInfo const &info,
             Image &out)
{
   VkResult result = vkCreateImage(device, &info, nullptr, &out.image);
   if (result != VK_SUCCESS)
      return result;
   VkMemoryRequirements requirements;
   vkGetImageMemoryRequirements(device, out.image, &requirements);
   uint32_t const memory_type =
      find_memory_type(physical_device, requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (memory_type == UINT32_MAX)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   VkMemoryAllocateInfo const allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = memory_type,
   };
   result = vkAllocateMemory(device, &allocation, nullptr, &out.memory);
   if (result == VK_SUCCESS)
      result = vkBindImageMemory(device, out.image, out.memory, 0);
   return result;
}

} // namespace

int
main(int argc, char **argv)
{
   bool const corrupt_expectation =
      argc == 4 && std::strcmp(argv[3], "--corrupt-expectation") == 0;
   bool const single_level_views =
      argc == 4 && std::strcmp(argv[3], "--single-level-views") == 0;
   bool const array_view = argc == 4 && std::strcmp(argv[3], "--2d-array-view") == 0;
   if (argc < 3 || argc > 4 ||
       (argc == 4 && !corrupt_expectation && !single_level_views && !array_view)) {
      std::fprintf(stderr,
                   "usage: %s VERT_SPV FRAG_SPV"
                   " [--corrupt-expectation|--single-level-views|--2d-array-view]\n",
                   argv[0]);
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
      .pApplicationName = "terakan-bc6-cube-test",
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

   std::vector<uint8_t> compressed_data;
   std::vector<VkBufferImageCopy> upload_regions;
   std::array<std::array<float, 4>, kOutputWidth> expected = {};
   for (uint32_t mip = 0; mip < kMipCount; ++mip) {
      uint32_t const size = std::max(1u, kBaseSize >> mip);
      uint32_t const block_count = ((size + 3) / 4) * ((size + 3) / 4);
      for (uint32_t face = 0; face < kFaceCount; ++face) {
         std::array<float, 4 * 4 * 4> source_pixels;
         std::array<float, 4> const source_color = {
            0.05F + 0.12F * static_cast<float>(face),
            0.04F + 0.09F * static_cast<float>(mip),
            0.03F + 0.035F * static_cast<float>(face + mip),
            1.0F,
         };
         for (uint32_t pixel = 0; pixel < 16; ++pixel)
            std::copy(source_color.begin(), source_color.end(),
                      source_pixels.begin() + pixel * 4);

         std::array<uint8_t, 16> block;
         util_format_bptc_rgb_ufloat_pack_rgba_float(
            block.data(), 16, source_pixels.data(), 4 * 4 * sizeof(float), 4, 4);
         std::array<float, 4 * 4 * 4> decoded_pixels;
         util_format_bptc_rgb_ufloat_unpack_rgba_float(
            decoded_pixels.data(), 4 * 4 * sizeof(float), block.data(), 16, 4, 4);
         std::copy_n(decoded_pixels.begin(), 4, expected[mip * kFaceCount + face].begin());

         VkDeviceSize const offset = compressed_data.size();
         for (uint32_t block_index = 0; block_index < block_count; ++block_index)
            compressed_data.insert(compressed_data.end(), block.begin(), block.end());
         upload_regions.push_back({
            .bufferOffset = offset,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, face, 1},
            .imageExtent = {size, size, 1},
         });
      }
   }
   /* Mesa's software BC6H decoder disagrees with the R8xx/R9xx hardware decoder for the blocks
    * generated for levels 5 and 6 above. These values are the hardware oracle captured with the
    * upstream r600 OpenGL driver on the same CAICOS device. Terakan must match that established
    * driver; the other 36 blocks are also checked against the independent software decoder.
    */
   std::array<std::array<float, 3>, 12> const r600_bc6_reference = {{
      {{0.25732F, 0.37231F, 0.25732F}}, {{0.37598F, 0.40503F, 0.37598F}},
      {{0.47729F, 0.50781F, 0.47339F}}, {{0.54785F, 0.55762F, 0.53223F}},
      {{0.60156F, 0.61328F, 0.63672F}}, {{0.67285F, 0.72607F, 0.60059F}},
      {{0.54199F, 0.20581F, 0.54199F}}, {{0.66260F, 0.26025F, 0.66260F}},
      {{0.74561F, 0.32471F, 0.74561F}}, {{0.63672F, 0.59131F, 0.66699F}},
      {{0.65186F, 0.63672F, 0.69727F}}, {{0.66699F, 0.68213F, 0.74268F}},
   }};
   for (uint32_t sample = 0; sample < r600_bc6_reference.size(); ++sample)
      std::copy(r600_bc6_reference[sample].begin(), r600_bc6_reference[sample].end(),
                expected[(5 * kFaceCount) + sample].begin());
   if (corrupt_expectation)
      expected[0][0] += 0.25F;

   Buffer upload;
   VK_CHECK(create_host_buffer(device, physical_device, compressed_data.size(),
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT, upload));
   std::memcpy(upload.mapping, compressed_data.data(), compressed_data.size());
   Buffer readback;
   VK_CHECK(create_host_buffer(device, physical_device,
                               kOutputWidth * 4 * sizeof(float),
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback));
   std::memset(readback.mapping, 0xA5, kOutputWidth * 4 * sizeof(float));

   Image cube;
   VkImageCreateInfo const cube_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_BC6H_UFLOAT_BLOCK,
      .extent = {kBaseSize, kBaseSize, 1},
      .mipLevels = kMipCount,
      .arrayLayers = kFaceCount,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VK_CHECK(create_image(device, physical_device, cube_info, cube));
   Image output;
   VkImageCreateInfo const output_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R32G32B32A32_SFLOAT,
      .extent = {kOutputWidth, 1, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VK_CHECK(create_image(device, physical_device, output_info, output));

   std::array<VkImageView, kMipCount> cube_views;
   for (uint32_t mip = 0; mip < kMipCount; ++mip) {
      VkImageViewCreateInfo const cube_view_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = cube.image,
         .viewType = array_view ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE,
         .format = VK_FORMAT_BC6H_UFLOAT_BLOCK,
         .subresourceRange = {
            VK_IMAGE_ASPECT_COLOR_BIT,
            single_level_views ? mip : 0,
            single_level_views ? 1u : kMipCount,
            0,
            kFaceCount,
         },
      };
      VK_CHECK(vkCreateImageView(device, &cube_view_info, nullptr, &cube_views[mip]));
   }
   VkImageView output_view;
   VkImageViewCreateInfo const output_view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = output.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R32G32B32A32_SFLOAT,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   VK_CHECK(vkCreateImageView(device, &output_view_info, nullptr, &output_view));

   VkSampler sampler;
   VkSamplerCreateInfo const sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = static_cast<float>(kMipCount - 1),
   };
   VK_CHECK(vkCreateSampler(device, &sampler_info, nullptr, &sampler));

   VkAttachmentDescription const attachment = {
      .format = VK_FORMAT_R32G32B32A32_SFLOAT,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
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
      .pAttachments = &output_view,
      .width = kOutputWidth,
      .height = 1,
      .layers = 1,
   };
   VkFramebuffer framebuffer;
   VK_CHECK(vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer));

   VkDescriptorSetLayoutBinding const descriptor_binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
   };
   VkDescriptorSetLayoutCreateInfo const descriptor_layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &descriptor_binding,
   };
   VkDescriptorSetLayout descriptor_layout;
   VK_CHECK(vkCreateDescriptorSetLayout(device, &descriptor_layout_info, nullptr,
                                         &descriptor_layout));
   VkPushConstantRange const push_range = {
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      .offset = 0,
      .size = 4 * sizeof(float),
   };
   VkPipelineLayoutCreateInfo const pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &descriptor_layout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push_range,
   };
   VkPipelineLayout pipeline_layout;
   VK_CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout));
   VkDescriptorPoolSize const pool_size = {
      .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = kMipCount,
   };
   VkDescriptorPoolCreateInfo const descriptor_pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = kMipCount,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
   };
   VkDescriptorPool descriptor_pool;
   VK_CHECK(vkCreateDescriptorPool(device, &descriptor_pool_info, nullptr, &descriptor_pool));
   std::array<VkDescriptorSetLayout, kMipCount> descriptor_layouts;
   descriptor_layouts.fill(descriptor_layout);
   VkDescriptorSetAllocateInfo const descriptor_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = descriptor_pool,
      .descriptorSetCount = kMipCount,
      .pSetLayouts = descriptor_layouts.data(),
   };
   std::array<VkDescriptorSet, kMipCount> descriptor_sets;
   VK_CHECK(vkAllocateDescriptorSets(device, &descriptor_allocate_info, descriptor_sets.data()));
   std::array<VkDescriptorImageInfo, kMipCount> descriptor_images;
   std::array<VkWriteDescriptorSet, kMipCount> descriptor_writes;
   for (uint32_t mip = 0; mip < kMipCount; ++mip) {
      descriptor_images[mip] = {
         .sampler = sampler,
         .imageView = cube_views[mip],
         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      };
      descriptor_writes[mip] = {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_sets[mip],
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &descriptor_images[mip],
      };
   }
   vkUpdateDescriptorSets(device, kMipCount, descriptor_writes.data(), 0, nullptr);

   VkShaderModule modules[2];
   std::array<std::vector<uint32_t> const *, 2> const shader_code = {
      &vertex_spirv, &fragment_spirv};
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
       .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = modules[0],
       .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = modules[1],
       .pName = "main"},
   };
   VkPipelineVertexInputStateCreateInfo const vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
   };
   VkPipelineInputAssemblyStateCreateInfo const input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
   };
   VkPipelineViewportStateCreateInfo const viewport_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .scissorCount = 1,
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
      .attachmentCount = 1,
      .pAttachments = &blend_attachment,
   };
   VkDynamicState const dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
   VkPipelineDynamicStateCreateInfo const dynamic_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 2,
      .pDynamicStates = dynamic_states,
   };
   VkGraphicsPipelineCreateInfo const pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2,
      .pStages = stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport_state,
      .pRasterizationState = &rasterization,
      .pMultisampleState = &multisample,
      .pColorBlendState = &color_blend,
      .pDynamicState = &dynamic_state,
      .layout = pipeline_layout,
      .renderPass = render_pass,
   };
   VkPipeline pipeline;
   VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                      &pipeline));

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

   VkImageMemoryBarrier const cube_to_upload = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = cube.image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kMipCount, 0, kFaceCount},
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                        &cube_to_upload);
   vkCmdCopyBufferToImage(command_buffer, upload.buffer, cube.image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, upload_regions.size(),
                          upload_regions.data());
   VkImageMemoryBarrier const cube_to_sample = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = cube.image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kMipCount, 0, kFaceCount},
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                        &cube_to_sample);

   VkClearValue const clear = {.color = {.float32 = {0.0F, 0.0F, 0.0F, 0.0F}}};
   VkRenderPassBeginInfo const render_begin = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = render_pass,
      .framebuffer = framebuffer,
      .renderArea = {{0, 0}, {kOutputWidth, 1}},
      .clearValueCount = 1,
      .pClearValues = &clear,
   };
   vkCmdBeginRenderPass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   std::array<std::array<float, 3>, kFaceCount> const directions = {{
      {{1.0F, 0.0F, 0.0F}},
      {{-1.0F, 0.0F, 0.0F}},
      {{0.0F, 1.0F, 0.0F}},
      {{0.0F, -1.0F, 0.0F}},
      {{0.0F, 0.0F, 1.0F}},
      {{0.0F, 0.0F, -1.0F}},
   }};
   for (uint32_t mip = 0; mip < kMipCount; ++mip) {
      vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0,
                              1, &descriptor_sets[mip], 0, nullptr);
      for (uint32_t face = 0; face < kFaceCount; ++face) {
         uint32_t const x = mip * kFaceCount + face;
         VkViewport const viewport = {
            .x = static_cast<float>(x),
            .y = 0.0F,
            .width = 1.0F,
            .height = 1.0F,
            .minDepth = 0.0F,
            .maxDepth = 1.0F,
         };
         VkRect2D const scissor = {{static_cast<int32_t>(x), 0}, {1, 1}};
         std::array<float, 4> const params = {
            directions[face][0], directions[face][1],
            array_view ? static_cast<float>(face) : directions[face][2],
            single_level_views ? 0.0F : static_cast<float>(mip),
         };
         vkCmdSetViewport(command_buffer, 0, 1, &viewport);
         vkCmdSetScissor(command_buffer, 0, 1, &scissor);
         vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                            sizeof(params), params.data());
         vkCmdDraw(command_buffer, 1, 1, 0, 0);
      }
   }
   vkCmdEndRenderPass(command_buffer);
   VkImageMemoryBarrier const output_ready = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = output.image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                        &output_ready);
   VkBufferImageCopy const readback_region = {
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {kOutputWidth, 1, 1},
   };
   vkCmdCopyImageToBuffer(command_buffer, output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          readback.buffer, 1, &readback_region);
   VkMemoryBarrier const host_ready = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_ready, 0, nullptr, 0, nullptr);
   VK_CHECK(vkEndCommandBuffer(command_buffer));
   VkSubmitInfo const submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
   };
   VK_CHECK(vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE));
   VK_CHECK(vkQueueWaitIdle(queue));

   float const *actual = reinterpret_cast<float const *>(readback.mapping);
   uint32_t mismatches = 0;
   float max_error = 0.0F;
   for (uint32_t i = 0; i < kOutputWidth; ++i) {
      bool pixel_matches = true;
      for (uint32_t channel = 0; channel < 4; ++channel) {
         float const error = std::abs(actual[i * 4 + channel] - expected[i][channel]);
         max_error = std::max(max_error, error);
         pixel_matches &= std::isfinite(actual[i * 4 + channel]) && error <= 0.02F;
      }
      if (!pixel_matches) {
         ++mismatches;
         uint32_t closest_expected = 0;
         float closest_error = INFINITY;
         for (uint32_t candidate = 0; candidate < kOutputWidth; ++candidate) {
            float candidate_error = 0.0F;
            for (uint32_t channel = 0; channel < 3; ++channel)
               candidate_error +=
                  std::abs(actual[i * 4 + channel] - expected[candidate][channel]);
            if (candidate_error < closest_error) {
               closest_error = candidate_error;
               closest_expected = candidate;
            }
         }
         std::printf("mip=%u face=%u actual=(%.5f %.5f %.5f %.5f)"
                     " expected=(%.5f %.5f %.5f %.5f)"
                     " closest=mip%u/face%u error=%.5f FAIL\n",
                     i / kFaceCount, i % kFaceCount, actual[i * 4 + 0],
                     actual[i * 4 + 1], actual[i * 4 + 2], actual[i * 4 + 3],
                     expected[i][0], expected[i][1], expected[i][2], expected[i][3],
                     closest_expected / kFaceCount, closest_expected % kFaceCount,
                     closest_error);
      }
   }
   std::printf("bc6_cube samples=%u mismatches=%u max_error=%.6f %s\n", kOutputWidth,
               mismatches, max_error, mismatches == 0 ? "PASS" : "FAIL");

   vkDeviceWaitIdle(device);
   vkDestroyCommandPool(device, command_pool, nullptr);
   vkDestroyPipeline(device, pipeline, nullptr);
   vkDestroyShaderModule(device, modules[0], nullptr);
   vkDestroyShaderModule(device, modules[1], nullptr);
   vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
   vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
   vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
   vkDestroyFramebuffer(device, framebuffer, nullptr);
   vkDestroyRenderPass(device, render_pass, nullptr);
   vkDestroySampler(device, sampler, nullptr);
   for (VkImageView cube_view : cube_views)
      vkDestroyImageView(device, cube_view, nullptr);
   vkDestroyImageView(device, output_view, nullptr);
   vkDestroyImage(device, cube.image, nullptr);
   vkFreeMemory(device, cube.memory, nullptr);
   vkDestroyImage(device, output.image, nullptr);
   vkFreeMemory(device, output.memory, nullptr);
   vkDestroyBuffer(device, upload.buffer, nullptr);
   vkUnmapMemory(device, upload.memory);
   vkFreeMemory(device, upload.memory, nullptr);
   vkDestroyBuffer(device, readback.buffer, nullptr);
   vkUnmapMemory(device, readback.memory);
   vkFreeMemory(device, readback.memory, nullptr);
   vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr);
   return mismatches == 0 ? 0 : 1;
}
