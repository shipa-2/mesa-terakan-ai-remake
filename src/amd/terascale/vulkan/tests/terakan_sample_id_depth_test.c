/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Per-sample fragment invocation writing per-sample depth, and reading it back.
 *
 * A single draw covers a 4-sample D32_SFLOAT image with a fragment shader that writes
 * `gl_FragDepth` from `gl_SampleID`. The pipeline does not set `sampleShadingEnable`: section
 * "Sample Shading" of the Vulkan 1.4.349 specification enables per-sample invocation anyway for a
 * fragment shader whose interface includes the SampleId built-in, so each sample must end up with
 * its own depth. A compute shader then reads all four back with `texelFetch(sampler2DMS, ...)`.
 *
 * This covers both halves of what the depth and stencil min/max resolve needs, and covers them in
 * a way `terakan_depth_msaa_fetch` cannot: that test clears its image to one value and expects
 * every sample to equal it, so it passes whether or not the sample index is honoured. Before the
 * per-sample invocation fix this test reads 0.1 - sample 0's depth - from all four samples.
 */

#include <vulkan/vulkan.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "terakan_test_device.h"


#define VK_CHECK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_result = (expression);                                                  \
      if (check_result != VK_SUCCESS) {                                                            \
         fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_result);                \
         return 1;                                                                                 \
      }                                                                                            \
   } while (false)

#define SAMPLE_COUNT 4u
#define WIDTH 8u
#define HEIGHT 8u

/* The same expression the fragment shader evaluates. */
static float
sample_depth(uint32_t const sample_index)
{
   return 0.1F + 0.2F * (float)sample_index;
}

static uint32_t
find_memory_type(VkPhysicalDevice const physical_device, uint32_t const bits,
                 VkMemoryPropertyFlags const flags)
{
   VkPhysicalDeviceMemoryProperties properties;
   vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
   for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) {
         return i;
      }
   }
   return UINT32_MAX;
}

static VkShaderModule
load_shader_module(VkDevice const device, char const * const path)
{
   FILE * const file = fopen(path, "rb");
   if (file == NULL) {
      fprintf(stderr, "Failed to open %s\n", path);
      return VK_NULL_HANDLE;
   }
   fseek(file, 0, SEEK_END);
   long const size = ftell(file);
   fseek(file, 0, SEEK_SET);
   uint32_t * const code = malloc((size_t)size);
   if (code == NULL || fread(code, 1, (size_t)size, file) != (size_t)size) {
      fprintf(stderr, "Failed to read %s\n", path);
      free(code);
      fclose(file);
      return VK_NULL_HANDLE;
   }
   fclose(file);
   VkShaderModuleCreateInfo const module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = (size_t)size,
      .pCode = code,
   };
   VkShaderModule module = VK_NULL_HANDLE;
   vkCreateShaderModule(device, &module_info, NULL, &module);
   free(code);
   return module;
}

int
main(int argc, char ** argv)
{
   if (argc != 4) {
      fprintf(stderr, "Usage: %s <vertex.spv> <fragment.spv> <compute.spv>\n", argv[0]);
      return 1;
   }

   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-sample-id-depth-test",
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo const instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application_info,
   };
   VkInstance instance;
   VK_CHECK(vkCreateInstance(&instance_info, NULL, &instance));

   uint32_t physical_device_count = 8;
   VkPhysicalDevice physical_devices[8];
   VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices));
   VkPhysicalDevice physical_device = VK_NULL_HANDLE;
   for (uint32_t i = 0; i < physical_device_count; ++i) {
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(physical_devices[i], &properties);
      if (terakan_test_device_matches(properties.deviceName)) {
         physical_device = physical_devices[i];
         break;
      }
   }
   if (physical_device == VK_NULL_HANDLE) {
      fprintf(stderr, "No Terakan device\n");
      return 1;
   }

   /* Sample shading is not requested by the pipeline, but the queried support is what makes the
    * SampleId built-in usable at all.
    */
   VkPhysicalDeviceFeatures features;
   vkGetPhysicalDeviceFeatures(physical_device, &features);
   if (!features.sampleRateShading) {
      fprintf(stderr, "sampleRateShading is not supported\n");
      return 1;
   }
   VkPhysicalDeviceFeatures const enabled_features = {.sampleRateShading = VK_TRUE};

   float const queue_priority = 1.0F;
   VkDeviceQueueCreateInfo const queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueCount = 1,
      .pQueuePriorities = &queue_priority,
   };
   VkDeviceCreateInfo const device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
      .pEnabledFeatures = &enabled_features,
   };
   VkDevice device;
   VK_CHECK(vkCreateDevice(physical_device, &device_info, NULL, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, 0, 0, &queue);

   VkImageCreateInfo const image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_D32_SFLOAT,
      .extent = {WIDTH, HEIGHT, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_4_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage image;
   VK_CHECK(vkCreateImage(device, &image_info, NULL, &image));
   VkMemoryRequirements image_requirements;
   vkGetImageMemoryRequirements(device, image, &image_requirements);
   VkMemoryAllocateInfo const image_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = image_requirements.size,
      .memoryTypeIndex = find_memory_type(physical_device, image_requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
   };
   VkDeviceMemory image_memory;
   VK_CHECK(vkAllocateMemory(device, &image_allocate_info, NULL, &image_memory));
   VK_CHECK(vkBindImageMemory(device, image, image_memory, 0));
   VkImageViewCreateInfo const view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_D32_SFLOAT,
      .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
   };
   VkImageView view;
   VK_CHECK(vkCreateImageView(device, &view_info, NULL, &view));

   VkDeviceSize const readback_size = SAMPLE_COUNT * sizeof(float);
   VkBufferCreateInfo const buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = readback_size,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
   };
   VkBuffer buffer;
   VK_CHECK(vkCreateBuffer(device, &buffer_info, NULL, &buffer));
   VkMemoryRequirements buffer_requirements;
   vkGetBufferMemoryRequirements(device, buffer, &buffer_requirements);
   VkMemoryAllocateInfo const buffer_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = buffer_requirements.size,
      .memoryTypeIndex = find_memory_type(physical_device, buffer_requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   VkDeviceMemory buffer_memory;
   VK_CHECK(vkAllocateMemory(device, &buffer_allocate_info, NULL, &buffer_memory));
   VK_CHECK(vkBindBufferMemory(device, buffer, buffer_memory, 0));
   float * readback;
   VK_CHECK(vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, (void **)&readback));
   memset(readback, 0, (size_t)readback_size);

   VkAttachmentDescription const attachment = {
      .format = VK_FORMAT_D32_SFLOAT,
      .samples = VK_SAMPLE_COUNT_4_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
      .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
   };
   VkAttachmentReference const attachment_reference = {0, VK_IMAGE_LAYOUT_GENERAL};
   VkSubpassDescription const subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .pDepthStencilAttachment = &attachment_reference,
   };
   VkRenderPassCreateInfo const render_pass_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
   };
   VkRenderPass render_pass;
   VK_CHECK(vkCreateRenderPass(device, &render_pass_info, NULL, &render_pass));
   VkFramebufferCreateInfo const framebuffer_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = render_pass,
      .attachmentCount = 1,
      .pAttachments = &view,
      .width = WIDTH,
      .height = HEIGHT,
      .layers = 1,
   };
   VkFramebuffer framebuffer;
   VK_CHECK(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));

   VkShaderModule const vertex_module = load_shader_module(device, argv[1]);
   VkShaderModule const fragment_module = load_shader_module(device, argv[2]);
   VkShaderModule const compute_module = load_shader_module(device, argv[3]);
   if (vertex_module == VK_NULL_HANDLE || fragment_module == VK_NULL_HANDLE ||
       compute_module == VK_NULL_HANDLE) {
      return 1;
   }

   VkPipelineLayoutCreateInfo const graphics_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
   };
   VkPipelineLayout graphics_layout;
   VK_CHECK(vkCreatePipelineLayout(device, &graphics_layout_info, NULL, &graphics_layout));

   VkPipelineShaderStageCreateInfo const graphics_stages[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vertex_module,
         .pName = "main",
      },
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fragment_module,
         .pName = "main",
      },
   };
   VkPipelineVertexInputStateCreateInfo const vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
   };
   VkPipelineInputAssemblyStateCreateInfo const input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   VkViewport const viewport = {0.0F, 0.0F, (float)WIDTH, (float)HEIGHT, 0.0F, 1.0F};
   VkRect2D const scissor = {{0, 0}, {WIDTH, HEIGHT}};
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
      .lineWidth = 1.0F,
   };
   /* Deliberately leaves `sampleShadingEnable` false - the shader's use of SampleId is what has to
    * make the invocation per-sample.
    */
   VkPipelineMultisampleStateCreateInfo const multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_4_BIT,
   };
   VkPipelineDepthStencilStateCreateInfo const depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE,
      .depthWriteEnable = VK_TRUE,
      .depthCompareOp = VK_COMPARE_OP_ALWAYS,
   };
   VkPipelineColorBlendStateCreateInfo const color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
   };
   VkGraphicsPipelineCreateInfo const graphics_pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2,
      .pStages = graphics_stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport_state,
      .pRasterizationState = &rasterization,
      .pMultisampleState = &multisample,
      .pDepthStencilState = &depth_stencil,
      .pColorBlendState = &color_blend,
      .layout = graphics_layout,
      .renderPass = render_pass,
      .subpass = 0,
   };
   VkPipeline graphics_pipeline;
   VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphics_pipeline_info, NULL,
                                      &graphics_pipeline));

   VkDescriptorSetLayoutBinding const bindings[2] = {
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
      .pBindings = bindings,
   };
   VkDescriptorSetLayout set_layout;
   VK_CHECK(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout));
   VkPushConstantRange const compute_push_constants = {VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                                       sizeof(uint32_t)};
   VkPipelineLayoutCreateInfo const compute_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &compute_push_constants,
   };
   VkPipelineLayout compute_layout;
   VK_CHECK(vkCreatePipelineLayout(device, &compute_layout_info, NULL, &compute_layout));
   VkComputePipelineCreateInfo const compute_pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage =
         {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = compute_module,
            .pName = "main",
         },
      .layout = compute_layout,
   };
   VkPipeline compute_pipeline;
   VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute_pipeline_info, NULL,
                                     &compute_pipeline));

   VkSamplerCreateInfo const sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
   };
   VkSampler sampler;
   VK_CHECK(vkCreateSampler(device, &sampler_info, NULL, &sampler));
   VkDescriptorPoolSize const pool_sizes[2] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
   };
   VkDescriptorPoolCreateInfo const pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 2,
      .pPoolSizes = pool_sizes,
   };
   VkDescriptorPool descriptor_pool;
   VK_CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool));
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
      .imageView = view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
   };
   VkDescriptorBufferInfo const buffer_descriptor = {
      .buffer = buffer,
      .offset = 0,
      .range = readback_size,
   };
   VkWriteDescriptorSet const writes[2] = {
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
   vkUpdateDescriptorSets(device, 2, writes, 0, NULL);

   VkCommandPoolCreateInfo const command_pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
   };
   VkCommandPool command_pool;
   VK_CHECK(vkCreateCommandPool(device, &command_pool_info, NULL, &command_pool));
   VkCommandBufferAllocateInfo const command_buffer_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = command_pool,
      .commandBufferCount = 1,
   };
   VkCommandBuffer command_buffer;
   VK_CHECK(vkAllocateCommandBuffers(device, &command_buffer_info, &command_buffer));
   VkCommandBufferBeginInfo const begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info));

   VkImageMemoryBarrier const initial_barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .image = image,
      .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1,
                        &initial_barrier);

   VkClearValue const clear_value = {.depthStencil = {1.0F, 0}};
   VkRenderPassBeginInfo const render_pass_begin_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = render_pass,
      .framebuffer = framebuffer,
      .renderArea = {{0, 0}, {WIDTH, HEIGHT}},
      .clearValueCount = 1,
      .pClearValues = &clear_value,
   };
   vkCmdBeginRenderPass(command_buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
   vkCmdDraw(command_buffer, 3, 1, 0, 0);
   vkCmdEndRenderPass(command_buffer);

   VkMemoryBarrier const depth_to_shader_barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &depth_to_shader_barrier, 0,
                        NULL, 0, NULL);
   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline);
   vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, compute_layout, 0, 1,
                           &descriptor_set, 0, NULL);
   uint32_t const sample_count = SAMPLE_COUNT;
   vkCmdPushConstants(command_buffer, compute_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                      sizeof(sample_count), &sample_count);
   vkCmdDispatch(command_buffer, 1, 1, 1);
   VkMemoryBarrier const shader_to_host_barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &shader_to_host_barrier, 0, NULL, 0,
                        NULL);
   VK_CHECK(vkEndCommandBuffer(command_buffer));

   VkFenceCreateInfo const fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   VK_CHECK(vkCreateFence(device, &fence_info, NULL, &fence));
   VkSubmitInfo const submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
   };
   VK_CHECK(vkQueueSubmit(queue, 1, &submit_info, fence));
   VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ULL));

   /* D32_SFLOAT stores the depth exactly, but the fixed-point viewport transform of the depth
    * range still costs a little precision.
    */
   float const tolerance = 1.0F / 4096.0F;
   bool passed = true;
   for (uint32_t sample_index = 0; sample_index < SAMPLE_COUNT; ++sample_index) {
      float const expected = sample_depth(sample_index);
      float const got = readback[sample_index];
      bool const sample_passed = fabsf(got - expected) <= tolerance;
      passed = passed && sample_passed;
      printf("sample %u: expected %.6f, got %.6f%s\n", sample_index, expected, got,
             sample_passed ? "" : "  <- MISMATCH");
   }
   if (!passed) {
      fprintf(stderr, "Per-sample depth was not written or not fetched per sample\n");
   }

   vkDeviceWaitIdle(device);
   return passed ? 0 : 1;
}
