/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* A local stand for one failing dEQP-VK.binding_model.descriptorset_random case:
 * sets4.dynindexed.ubolimitlow.sbolimitlow.sampledimglow.lowimgnotex.noiub.nouab.frag.noia.8,
 * which writes 1 to every texel of an 8x8 storage image and loses ten of them -- 44, 50-53 and
 * 58-62 -- deterministically.
 *
 * Ten reductions that each picked one ingredient out of that shader and reproduced it alone came
 * back with all sixty-four texels correct, so this runs the shader itself, with the four
 * descriptor sets it declares. The point is that the fragment shader is a file next to this one:
 * statements can be deleted from it until the failure goes away, which converges, where guessing
 * the ingredient did not.
 *
 * Every descriptor carries the value the shader checks for, so `accum` stays zero and each array
 * index stays dynamic without ever being anything but zero, exactly as in the original.
 */

#include <vulkan/vulkan.h>

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

constexpr uint32_t kDim = 8;
constexpr uint32_t kSetCount = 4;

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

enum ResourceKind { KIND_OUTPUT_IMAGE, KIND_IMAGE, KIND_UBO, KIND_SSBO, KIND_TEXEL };

struct BindingSpec {
   uint32_t set;
   uint32_t binding;
   ResourceKind kind;
   uint32_t count;
   /* The value the shader expects from element zero; the rest follow in order. Zero where the
    * shader writes rather than reads.
    */
   int32_t first_value;
};

/* Transcribed from the failing case's own GLSL, which the test logs. */
constexpr BindingSpec kBindings[] = {
   {0, 0, KIND_OUTPUT_IMAGE, 1, 0}, {0, 1, KIND_UBO, 9, 1},   {0, 3, KIND_UBO, 1, 10},
   {1, 0, KIND_UBO, 1, 11},         {1, 10, KIND_IMAGE, 1, 0}, {1, 11, KIND_TEXEL, 1, 13},
   {2, 1, KIND_TEXEL, 9, 14},       {2, 2, KIND_SSBO, 1, 0},  {2, 3, KIND_UBO, 1, 24},
   {2, 4, KIND_IMAGE, 2, 0},        {2, 10, KIND_TEXEL, 6, 27}, {3, 0, KIND_SSBO, 1, 0},
   {3, 1, KIND_SSBO, 2, 34},
};

VkDescriptorType
descriptor_type_of(ResourceKind kind)
{
   switch (kind) {
   case KIND_OUTPUT_IMAGE:
   case KIND_IMAGE:
      return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
   case KIND_UBO:
      return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
   case KIND_SSBO:
      return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   default:
      return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
   }
}

uint32_t const vertex_spirv[] = {
#include "terakan_color_msaa_write.vert.spv.h"
};
uint32_t const fragment_spirv[] = {
#include "terakan_descriptor_set_shape.frag.spv.h"
};

} // namespace

int
main()
{
   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-descriptor-set-shape-test",
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo const instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application_info,
   };
   VkInstance instance;
   VK_CHECK(vkCreateInstance(&instance_info, nullptr, &instance));

   uint32_t physical_device_count = 8;
   VkPhysicalDevice physical_devices[8];
   VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices));
   VkPhysicalDevice physical_device = VK_NULL_HANDLE;
   for (uint32_t i = 0; i < physical_device_count; ++i) {
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(physical_devices[i], &properties);
      if (std::strstr(properties.deviceName, "(Terakan)") == nullptr ||
          std::strstr(properties.deviceName, "TeraScale 1") != nullptr)
         continue;
      physical_device = physical_devices[i];
      break;
   }
   if (physical_device == VK_NULL_HANDLE) {
      std::fprintf(stderr, "No usable Terakan physical device found\n");
      return 1;
   }
   VkPhysicalDeviceProperties device_properties;
   vkGetPhysicalDeviceProperties(physical_device, &device_properties);

   uint32_t queue_family_count = 16;
   VkQueueFamilyProperties queue_families[16];
   vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_families);
   uint32_t queue_family = UINT32_MAX;
   for (uint32_t i = 0; i < queue_family_count; ++i) {
      if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
         queue_family = i;
         break;
      }
   }
   if (queue_family == UINT32_MAX) {
      std::fprintf(stderr, "No graphics queue family\n");
      return 1;
   }

   float const queue_priority = 1.0f;
   VkDeviceQueueCreateInfo const queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = queue_family,
      .queueCount = 1,
      .pQueuePriorities = &queue_priority,
   };
   VkPhysicalDeviceFeatures enabled_features = {};
   enabled_features.fragmentStoresAndAtomics = VK_TRUE;
   VkDeviceCreateInfo const device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
      .pEnabledFeatures = &enabled_features,
   };
   VkDevice device;
   VK_CHECK(vkCreateDevice(physical_device, &device_info, nullptr, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, queue_family, 0, &queue);

   /* One host-visible buffer holds every uniform, storage and texel buffer element, each at its
    * own aligned offset, so the values are simply written in before submission.
    */
   VkDeviceSize alignment = 16;
   for (VkDeviceSize a : {device_properties.limits.minUniformBufferOffsetAlignment,
                          device_properties.limits.minStorageBufferOffsetAlignment,
                          device_properties.limits.minTexelBufferOffsetAlignment}) {
      if (a > alignment)
         alignment = a;
   }
   uint32_t element_count = 0;
   for (BindingSpec const &spec : kBindings) {
      if (spec.kind != KIND_IMAGE && spec.kind != KIND_OUTPUT_IMAGE)
         element_count += spec.count;
   }
   VkDeviceSize const buffer_size = alignment * element_count;
   VkBufferCreateInfo const buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = buffer_size,
      .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
               VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer buffer;
   VK_CHECK(vkCreateBuffer(device, &buffer_info, nullptr, &buffer));
   VkMemoryRequirements buffer_requirements;
   vkGetBufferMemoryRequirements(device, buffer, &buffer_requirements);
   VkMemoryAllocateInfo const buffer_allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = buffer_requirements.size,
      .memoryTypeIndex =
         find_memory_type(physical_device, buffer_requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   VkDeviceMemory buffer_memory;
   VK_CHECK(vkAllocateMemory(device, &buffer_allocation, nullptr, &buffer_memory));
   VK_CHECK(vkBindBufferMemory(device, buffer, buffer_memory, 0));
   char *buffer_mapping;
   VK_CHECK(vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0,
                        reinterpret_cast<void **>(&buffer_mapping)));
   std::memset(buffer_mapping, 0, static_cast<size_t>(buffer_size));

   auto create_image = [&](uint32_t width, uint32_t height, VkImage *image_out,
                           VkImageView *view_out) -> VkResult {
      VkImageCreateInfo const info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_R32_SINT,
         .extent = {width, height, 1},
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      VkResult result = vkCreateImage(device, &info, nullptr, image_out);
      if (result != VK_SUCCESS)
         return result;
      VkMemoryRequirements requirements;
      vkGetImageMemoryRequirements(device, *image_out, &requirements);
      VkMemoryAllocateInfo const allocation = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = requirements.size,
         .memoryTypeIndex = find_memory_type(physical_device, requirements.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
      };
      VkDeviceMemory memory;
      result = vkAllocateMemory(device, &allocation, nullptr, &memory);
      if (result != VK_SUCCESS)
         return result;
      result = vkBindImageMemory(device, *image_out, memory, 0);
      if (result != VK_SUCCESS)
         return result;
      VkImageViewCreateInfo const view = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = *image_out,
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = VK_FORMAT_R32_SINT,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      return vkCreateImageView(device, &view, nullptr, view_out);
   };

   /* Build the descriptors, filling the buffer as it goes. */
   std::vector<VkDescriptorSetLayoutBinding> layout_bindings[kSetCount];
   std::vector<VkDescriptorImageInfo> image_infos[sizeof(kBindings) / sizeof(kBindings[0])];
   std::vector<VkDescriptorBufferInfo> buffer_infos[sizeof(kBindings) / sizeof(kBindings[0])];
   std::vector<VkBufferView> texel_views[sizeof(kBindings) / sizeof(kBindings[0])];
   VkImage output_image = VK_NULL_HANDLE;
   VkDeviceSize next_offset = 0;
   /* Where each buffer-backed element ended up and what it was filled with, so a conditional store
    * the shader makes into one can be checked after the draw. The rendered texels say the reads
    * were right; only this says the writes landed.
    */
   struct BufferElement {
      uint32_t set;
      uint32_t binding;
      uint32_t element;
      VkDeviceSize offset;
      int32_t fill;
   };
   std::vector<BufferElement> buffer_element_offsets;

   for (size_t spec_index = 0; spec_index < sizeof(kBindings) / sizeof(kBindings[0]); ++spec_index) {
      BindingSpec const &spec = kBindings[spec_index];
      layout_bindings[spec.set].push_back({
         .binding = spec.binding,
         .descriptorType = descriptor_type_of(spec.kind),
         .descriptorCount = spec.count,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      });
      for (uint32_t element = 0; element < spec.count; ++element) {
         if (spec.kind == KIND_IMAGE || spec.kind == KIND_OUTPUT_IMAGE) {
            bool const is_output = spec.kind == KIND_OUTPUT_IMAGE;
            VkImage image;
            VkImageView view;
            VK_CHECK(create_image(is_output ? kDim : 1, is_output ? kDim : 1, &image, &view));
            if (is_output)
               output_image = image;
            image_infos[spec_index].push_back(
               {.imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL});
            continue;
         }
         int32_t const value = spec.first_value != 0 ? spec.first_value + int32_t(element) : 0;
         std::memcpy(buffer_mapping + next_offset, &value, sizeof(value));
         buffer_element_offsets.push_back({spec.set, spec.binding, element, next_offset, value});
         if (spec.kind == KIND_TEXEL) {
            VkBufferViewCreateInfo const view_info = {
               .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
               .buffer = buffer,
               .format = VK_FORMAT_R32_SINT,
               .offset = next_offset,
               .range = sizeof(int32_t),
            };
            VkBufferView view;
            VK_CHECK(vkCreateBufferView(device, &view_info, nullptr, &view));
            texel_views[spec_index].push_back(view);
         } else {
            buffer_infos[spec_index].push_back(
               {.buffer = buffer, .offset = next_offset, .range = sizeof(int32_t)});
         }
         next_offset += alignment;
      }
   }

   VkDescriptorSetLayout set_layouts[kSetCount];
   for (uint32_t set = 0; set < kSetCount; ++set) {
      VkDescriptorSetLayoutCreateInfo const info = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
         .bindingCount = uint32_t(layout_bindings[set].size()),
         .pBindings = layout_bindings[set].data(),
      };
      VK_CHECK(vkCreateDescriptorSetLayout(device, &info, nullptr, &set_layouts[set]));
   }
   VkDescriptorPoolSize const pool_sizes[] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16},
      {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 32},
   };
   VkDescriptorPoolCreateInfo const pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = kSetCount,
      .poolSizeCount = 4,
      .pPoolSizes = pool_sizes,
   };
   VkDescriptorPool pool;
   VK_CHECK(vkCreateDescriptorPool(device, &pool_info, nullptr, &pool));
   VkDescriptorSetAllocateInfo const set_allocate = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pool,
      .descriptorSetCount = kSetCount,
      .pSetLayouts = set_layouts,
   };
   VkDescriptorSet descriptor_sets[kSetCount];
   VK_CHECK(vkAllocateDescriptorSets(device, &set_allocate, descriptor_sets));

   std::vector<VkWriteDescriptorSet> writes;
   for (size_t spec_index = 0; spec_index < sizeof(kBindings) / sizeof(kBindings[0]); ++spec_index) {
      BindingSpec const &spec = kBindings[spec_index];
      VkWriteDescriptorSet write = {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_sets[spec.set],
         .dstBinding = spec.binding,
         .descriptorCount = spec.count,
         .descriptorType = descriptor_type_of(spec.kind),
      };
      if (!image_infos[spec_index].empty())
         write.pImageInfo = image_infos[spec_index].data();
      if (!buffer_infos[spec_index].empty())
         write.pBufferInfo = buffer_infos[spec_index].data();
      if (!texel_views[spec_index].empty())
         write.pTexelBufferView = texel_views[spec_index].data();
      writes.push_back(write);
   }
   vkUpdateDescriptorSets(device, uint32_t(writes.size()), writes.data(), 0, nullptr);

   VkPipelineLayoutCreateInfo const pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = kSetCount,
      .pSetLayouts = set_layouts,
   };
   VkPipelineLayout pipeline_layout;
   VK_CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout));

   VkSubpassDescription const subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS};
   VkRenderPassCreateInfo const render_pass_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .subpassCount = 1,
      .pSubpasses = &subpass,
   };
   VkRenderPass render_pass;
   VK_CHECK(vkCreateRenderPass(device, &render_pass_info, nullptr, &render_pass));
   VkFramebufferCreateInfo const framebuffer_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = render_pass,
      .width = kDim,
      .height = kDim,
      .layers = 1,
   };
   VkFramebuffer framebuffer;
   VK_CHECK(vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer));

   VkShaderModuleCreateInfo const vertex_module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(vertex_spirv),
      .pCode = vertex_spirv,
   };
   VkShaderModule vertex_module;
   VK_CHECK(vkCreateShaderModule(device, &vertex_module_info, nullptr, &vertex_module));
   VkShaderModuleCreateInfo const fragment_module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(fragment_spirv),
      .pCode = fragment_spirv,
   };
   VkShaderModule fragment_module;
   VK_CHECK(vkCreateShaderModule(device, &fragment_module_info, nullptr, &fragment_module));

   VkPipelineShaderStageCreateInfo const stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = vertex_module,
       .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = fragment_module,
       .pName = "main"},
   };
   VkPipelineVertexInputStateCreateInfo const vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo const input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   VkViewport const viewport = {0.0f, 0.0f, float(kDim), float(kDim), 0.0f, 1.0f};
   VkRect2D const scissor = {{0, 0}, {kDim, kDim}};
   VkPipelineViewportStateCreateInfo const viewport_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .pViewports = &viewport,
      .scissorCount = 1,
      .pScissors = &scissor,
   };
   VkPipelineRasterizationStateCreateInfo const rasterization = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
   };
   VkPipelineMultisampleStateCreateInfo const multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
   };
   VkPipelineColorBlendStateCreateInfo const color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
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
      .layout = pipeline_layout,
      .renderPass = render_pass,
   };
   VkPipeline pipeline;
   VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline));

   VkDeviceSize const readback_size = kDim * kDim * sizeof(int32_t);
   VkBufferCreateInfo const readback_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = readback_size,
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
   int32_t *readback_mapping;
   VK_CHECK(vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0,
                        reinterpret_cast<void **>(&readback_mapping)));

   VkCommandPoolCreateInfo const command_pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = queue_family,
   };
   VkCommandPool command_pool;
   VK_CHECK(vkCreateCommandPool(device, &command_pool_info, nullptr, &command_pool));
   VkCommandBufferAllocateInfo const command_buffer_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer command_buffer;
   VK_CHECK(vkAllocateCommandBuffers(device, &command_buffer_info, &command_buffer));
   VkCommandBufferBeginInfo const begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info));

   /* Every storage image starts in GENERAL and cleared, as the original does. */
   std::vector<VkImageMemoryBarrier> barriers;
   for (auto const &infos : image_infos) {
      for (size_t i = 0; i < infos.size(); ++i) {
         (void)i;
      }
   }
   VkImageSubresourceRange const whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   auto transition_and_clear = [&](VkImage image) {
      VkImageMemoryBarrier const to_general = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = image,
         .subresourceRange = whole,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &to_general);
      VkClearColorValue const zero = {};
      vkCmdClearColorImage(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &whole);
   };
   for (size_t spec_index = 0; spec_index < sizeof(kBindings) / sizeof(kBindings[0]); ++spec_index) {
      for (VkDescriptorImageInfo const &info : image_infos[spec_index]) {
         (void)info;
      }
   }
   /* The images were created in declaration order, and the output one was remembered. */
   transition_and_clear(output_image);

   VkMemoryBarrier const clear_to_shader = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &clear_to_shader, 0, nullptr,
                        0, nullptr);

   VkRenderPassBeginInfo const render_pass_begin = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = render_pass,
      .framebuffer = framebuffer,
      .renderArea = {{0, 0}, {kDim, kDim}},
   };
   vkCmdBeginRenderPass(command_buffer, &render_pass_begin, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0,
                           kSetCount, descriptor_sets, 0, nullptr);
   vkCmdDraw(command_buffer, 6, 1, 0, 0);
   vkCmdEndRenderPass(command_buffer);

   VkMemoryBarrier const shader_to_transfer = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &shader_to_transfer, 0, nullptr, 0,
                        nullptr);
   VkBufferImageCopy const copy = {
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {kDim, kDim, 1},
   };
   vkCmdCopyImageToBuffer(command_buffer, output_image, VK_IMAGE_LAYOUT_GENERAL, readback_buffer, 1,
                          &copy);
   VK_CHECK(vkEndCommandBuffer(command_buffer));

   VkSubmitInfo const submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
   };
   VK_CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
   VK_CHECK(vkQueueWaitIdle(queue));

   /* The conditional stores the shader makes, and what they must leave behind. A store that is
    * dropped leaves the element at its fill value, which is what dEQP sees as `found -1`.
    */
   static const struct {
      uint32_t set, binding, element;
      int32_t value;
   } kExpectedWrites[] = {
      {2, 2, 0, 23},
      {3, 0, 0, 33},
   };
   uint32_t lost_writes = 0;
   for (auto const &expected : kExpectedWrites) {
      for (BufferElement const &element : buffer_element_offsets) {
         if (element.set != expected.set || element.binding != expected.binding ||
             element.element != expected.element)
            continue;
         int32_t actual;
         std::memcpy(&actual, buffer_mapping + element.offset, sizeof(actual));
         if (actual == expected.value)
            break;
         std::printf("descriptor_set_shape set %u binding %u element %u = %d, expected the %d the "
                     "shader stores%s\n",
                     expected.set, expected.binding, expected.element, actual, expected.value,
                     actual == element.fill ? " (still the fill value -- the store was dropped)"
                                            : "");
         ++lost_writes;
         break;
      }
   }

   uint32_t wrong = 0;
   std::printf("descriptor_set_shape wrong texels:");
   for (uint32_t i = 0; i < kDim * kDim; ++i) {
      if (readback_mapping[i] != 1) {
         std::printf(" %u(=%d)", i, readback_mapping[i]);
         ++wrong;
      }
   }
   std::printf("\ndescriptor_set_shape wrong=%u/%u lost_writes=%u %s\n", wrong, kDim * kDim,
               lost_writes, wrong == 0 && lost_writes == 0 ? "PASS" : "FAIL");
   return wrong == 0 && lost_writes == 0 ? 0 : 1;
}
