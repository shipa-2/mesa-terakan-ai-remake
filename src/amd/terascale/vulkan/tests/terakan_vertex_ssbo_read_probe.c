/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* A characterization tool, not a pass/fail test, which is why it is built but not run by
 * bin/terakan-test.
 *
 * `dEQP-VK.synchronization.op.single_queue.*read_ssbo_vertex*` fails every case whatever wrote the
 * data, while the matching write-from-vertex cases pass. `terakan_instance_dynamic_ssbo` reads a
 * `readonly` storage buffer from a vertex shader and passes, so the read path works when nothing
 * else writes the buffer -- what those cases add is a second agent writing it earlier in the same
 * command buffer.
 *
 * So this reproduces exactly that, in one draw: a writer fills the buffer, a barrier makes the
 * write available to shader reads, and then the vertex shader and the fragment shader each read
 * the same word. The two are reported side by side, because the fragment stage's read is known to
 * work and is therefore the control. `TERAKAN_PROBE_WRITER` selects the writer: `fill` (the
 * default) for vkCmdFillBuffer, `copy` for vkCmdCopyBuffer, and `none` to write the value from the
 * host before anything is submitted, which is the case that has no second agent at all.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "terakan_test_device.h"

#define CK(e)                                                                                      \
   do {                                                                                            \
      VkResult const r = (e);                                                                      \
      if (r) {                                                                                     \
         fprintf(stderr, "%s -> %d\n", #e, r);                                                     \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

static uint32_t const vertex_spirv[] = {
#include "terakan_vertex_ssbo_read.vert.spv.h"
};
static uint32_t const fragment_spirv[] = {
#include "terakan_vertex_ssbo_read.frag.spv.h"
};

#define EXPECTED_VALUE 0x5A5A5A5Au

static uint32_t
memory_type(VkPhysicalDevice const physical_device, uint32_t const bits,
            VkMemoryPropertyFlags const flags)
{
   VkPhysicalDeviceMemoryProperties properties;
   vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
   for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
      if ((bits & (1u << index)) &&
          (properties.memoryTypes[index].propertyFlags & flags) == flags) {
         return index;
      }
   }
   return UINT32_MAX;
}

int
main(void)
{
   char const * const writer = getenv("TERAKAN_PROBE_WRITER") != NULL
                                  ? getenv("TERAKAN_PROBE_WRITER")
                                  : "fill";
   bool const write_with_fill = strcmp(writer, "fill") == 0;
   bool const write_with_copy = strcmp(writer, "copy") == 0;
   printf("writer=%s expected=0x%08x\n", writer, EXPECTED_VALUE);

   VkApplicationInfo const application_info = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                               .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo const instance_create_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application_info};
   VkInstance instance;
   CK(vkCreateInstance(&instance_create_info, NULL, &instance));

   uint32_t physical_device_count = 8;
   VkPhysicalDevice physical_devices[8];
   CK(vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices));
   VkPhysicalDevice physical_device = VK_NULL_HANDLE;
   VkPhysicalDeviceProperties properties;
   for (uint32_t index = 0; index < physical_device_count; ++index) {
      vkGetPhysicalDeviceProperties(physical_devices[index], &properties);
      if (terakan_test_device_matches(properties.deviceName)) {
         physical_device = physical_devices[index];
         break;
      }
   }
   if (physical_device == VK_NULL_HANDLE) {
      fprintf(stderr, "No Terakan device found\n");
      return 77;
   }
   fprintf(stderr, "device=%s\n", properties.deviceName);

   float const queue_priority = 1.0f;
   VkDeviceQueueCreateInfo const queue_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueCount = 1,
      .pQueuePriorities = &queue_priority};
   VkDeviceCreateInfo const device_create_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                                  .queueCreateInfoCount = 1,
                                                  .pQueueCreateInfos = &queue_create_info};
   VkDevice device;
   CK(vkCreateDevice(physical_device, &device_create_info, NULL, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, 0, 0, &queue);

   /* The storage buffer both stages read. */
   VkDeviceSize const source_size = 4096;
   VkBufferCreateInfo const source_create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = source_size,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
   VkBuffer source_buffer;
   CK(vkCreateBuffer(device, &source_create_info, NULL, &source_buffer));
   VkMemoryRequirements source_requirements;
   vkGetBufferMemoryRequirements(device, source_buffer, &source_requirements);
   VkMemoryAllocateInfo const source_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = source_requirements.size,
      .memoryTypeIndex = memory_type(physical_device, source_requirements.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
   VkDeviceMemory source_memory;
   CK(vkAllocateMemory(device, &source_allocate_info, NULL, &source_memory));
   CK(vkBindBufferMemory(device, source_buffer, source_memory, 0));
   uint32_t * source_map;
   CK(vkMapMemory(device, source_memory, 0, VK_WHOLE_SIZE, 0, (void **)&source_map));
   /* Whatever the writer, the buffer starts holding something the read must not return. */
   for (VkDeviceSize index = 0; index < source_size / 4; ++index) {
      source_map[index] = 0xDEADBEEFu;
   }
   if (!write_with_fill && !write_with_copy) {
      for (VkDeviceSize index = 0; index < source_size / 4; ++index) {
         source_map[index] = EXPECTED_VALUE;
      }
   }

   /* The source of a copy, when that is the writer. */
   VkBufferCreateInfo const staging_create_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                                   .size = source_size,
                                                   .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
   VkBuffer staging_buffer;
   CK(vkCreateBuffer(device, &staging_create_info, NULL, &staging_buffer));
   VkMemoryRequirements staging_requirements;
   vkGetBufferMemoryRequirements(device, staging_buffer, &staging_requirements);
   VkMemoryAllocateInfo const staging_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = staging_requirements.size,
      .memoryTypeIndex = memory_type(physical_device, staging_requirements.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
   VkDeviceMemory staging_memory;
   CK(vkAllocateMemory(device, &staging_allocate_info, NULL, &staging_memory));
   CK(vkBindBufferMemory(device, staging_buffer, staging_memory, 0));
   uint32_t * staging_map;
   CK(vkMapMemory(device, staging_memory, 0, VK_WHOLE_SIZE, 0, (void **)&staging_map));
   for (VkDeviceSize index = 0; index < source_size / 4; ++index) {
      staging_map[index] = EXPECTED_VALUE;
   }

   /* A 1x1 integer colour attachment holding both stages' answers. */
   VkImageCreateInfo const target_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R32G32B32A32_UINT,
      .extent = {1, 1, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
   VkImage target;
   CK(vkCreateImage(device, &target_create_info, NULL, &target));
   VkMemoryRequirements target_requirements;
   vkGetImageMemoryRequirements(device, target, &target_requirements);
   VkMemoryAllocateInfo const target_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = target_requirements.size,
      .memoryTypeIndex = memory_type(physical_device, target_requirements.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
   VkDeviceMemory target_memory;
   CK(vkAllocateMemory(device, &target_allocate_info, NULL, &target_memory));
   CK(vkBindImageMemory(device, target, target_memory, 0));
   VkImageViewCreateInfo const target_view_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = target,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R32G32B32A32_UINT,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VkImageView target_view;
   CK(vkCreateImageView(device, &target_view_create_info, NULL, &target_view));

   VkBufferCreateInfo const readback_create_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                                    .size = 16,
                                                    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
   VkBuffer readback_buffer;
   CK(vkCreateBuffer(device, &readback_create_info, NULL, &readback_buffer));
   VkMemoryRequirements readback_requirements;
   vkGetBufferMemoryRequirements(device, readback_buffer, &readback_requirements);
   VkMemoryAllocateInfo const readback_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = readback_requirements.size,
      .memoryTypeIndex = memory_type(physical_device, readback_requirements.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
   VkDeviceMemory readback_memory;
   CK(vkAllocateMemory(device, &readback_allocate_info, NULL, &readback_memory));
   CK(vkBindBufferMemory(device, readback_buffer, readback_memory, 0));
   uint32_t * readback_map;
   CK(vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0, (void **)&readback_map));
   memset(readback_map, 0, 16);

   VkDescriptorSetLayoutBinding const binding = {
      0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
   VkDescriptorSetLayoutCreateInfo const set_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &binding};
   VkDescriptorSetLayout set_layout;
   CK(vkCreateDescriptorSetLayout(device, &set_layout_create_info, NULL, &set_layout));
   VkPipelineLayoutCreateInfo const pipeline_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout};
   VkPipelineLayout pipeline_layout;
   CK(vkCreatePipelineLayout(device, &pipeline_layout_create_info, NULL, &pipeline_layout));

   VkShaderModuleCreateInfo const vertex_module_create_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(vertex_spirv),
      .pCode = vertex_spirv};
   VkShaderModule vertex_module;
   CK(vkCreateShaderModule(device, &vertex_module_create_info, NULL, &vertex_module));
   VkShaderModuleCreateInfo const fragment_module_create_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(fragment_spirv),
      .pCode = fragment_spirv};
   VkShaderModule fragment_module;
   CK(vkCreateShaderModule(device, &fragment_module_create_info, NULL, &fragment_module));

   VkAttachmentDescription const attachment = {
      .format = VK_FORMAT_R32G32B32A32_UINT,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
   VkAttachmentReference const attachment_reference = {0,
                                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription const subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                         .colorAttachmentCount = 1,
                                         .pColorAttachments = &attachment_reference};
   VkRenderPassCreateInfo const render_pass_create_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass};
   VkRenderPass render_pass;
   CK(vkCreateRenderPass(device, &render_pass_create_info, NULL, &render_pass));
   VkFramebufferCreateInfo const framebuffer_create_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = render_pass,
      .attachmentCount = 1,
      .pAttachments = &target_view,
      .width = 1,
      .height = 1,
      .layers = 1};
   VkFramebuffer framebuffer;
   CK(vkCreateFramebuffer(device, &framebuffer_create_info, NULL, &framebuffer));

   uint32_t const specialization_data[2] = {(uint32_t)(source_size / 4u), EXPECTED_VALUE};
   VkSpecializationMapEntry const specialization_entries[2] = {
      {0, 0, sizeof(uint32_t)}, {1, sizeof(uint32_t), sizeof(uint32_t)}};
   VkSpecializationInfo const specialization = {.mapEntryCount = 2,
                                                .pMapEntries = specialization_entries,
                                                .dataSize = sizeof(specialization_data),
                                                .pData = specialization_data};
   VkPipelineShaderStageCreateInfo const stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = vertex_module,
       .pName = "main",
       .pSpecializationInfo = &specialization},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = fragment_module,
       .pName = "main",
       .pSpecializationInfo = &specialization}};
   VkPipelineVertexInputStateCreateInfo const vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo const input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport const viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
   VkRect2D const scissor = {{0, 0}, {1, 1}};
   VkPipelineViewportStateCreateInfo const viewport_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .pViewports = &viewport,
      .scissorCount = 1,
      .pScissors = &scissor};
   VkPipelineRasterizationStateCreateInfo const rasterization = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo const multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState const blend_attachment = {.colorWriteMask = 0xF};
   VkPipelineColorBlendStateCreateInfo const color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_attachment};
   VkGraphicsPipelineCreateInfo const pipeline_create_info = {
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
      .renderPass = render_pass};
   VkPipeline pipeline;
   CK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, NULL, &pipeline));

   VkDescriptorPoolSize const pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
   VkDescriptorPoolCreateInfo const pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size};
   VkDescriptorPool descriptor_pool;
   CK(vkCreateDescriptorPool(device, &pool_create_info, NULL, &descriptor_pool));
   VkDescriptorSetAllocateInfo const set_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = descriptor_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &set_layout};
   VkDescriptorSet descriptor_set;
   CK(vkAllocateDescriptorSets(device, &set_allocate_info, &descriptor_set));
   VkDescriptorBufferInfo const source_info = {
      .buffer = source_buffer, .offset = 0, .range = VK_WHOLE_SIZE};
   VkWriteDescriptorSet const write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = descriptor_set,
                                       .dstBinding = 0,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .pBufferInfo = &source_info};
   vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

   VkCommandPoolCreateInfo const command_pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
   VkCommandPool command_pool;
   CK(vkCreateCommandPool(device, &command_pool_create_info, NULL, &command_pool));
   VkCommandBufferAllocateInfo const command_buffer_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = command_pool,
      .commandBufferCount = 1};
   VkCommandBuffer command_buffer;
   CK(vkAllocateCommandBuffers(device, &command_buffer_allocate_info, &command_buffer));
   VkFenceCreateInfo const fence_create_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   CK(vkCreateFence(device, &fence_create_info, NULL, &fence));

   VkCommandBufferBeginInfo const begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   CK(vkBeginCommandBuffer(command_buffer, &begin_info));

   if (write_with_fill) {
      vkCmdFillBuffer(command_buffer, source_buffer, 0, VK_WHOLE_SIZE, EXPECTED_VALUE);
   } else if (write_with_copy) {
      VkBufferCopy const region = {.size = source_size};
      vkCmdCopyBuffer(command_buffer, staging_buffer, source_buffer, 1, &region);
   }
   if (write_with_fill || write_with_copy) {
      VkBufferMemoryBarrier const to_shader_read = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .buffer = source_buffer,
         .offset = 0,
         .size = VK_WHOLE_SIZE};
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 0, NULL, 1, &to_shader_read, 0, NULL);
   }

   VkClearValue const clear_value = {.color = {.uint32 = {0xFFFFFFFFu, 0xFFFFFFFFu, 0u, 0u}}};
   VkRenderPassBeginInfo const render_pass_begin_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = render_pass,
      .framebuffer = framebuffer,
      .renderArea = {{0, 0}, {1, 1}},
      .clearValueCount = 1,
      .pClearValues = &clear_value};
   vkCmdBeginRenderPass(command_buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1,
                           &descriptor_set, 0, NULL);
   vkCmdDraw(command_buffer, 3, 1, 0, 0);
   vkCmdEndRenderPass(command_buffer);

   VkBufferImageCopy const readback_region = {
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {1, 1, 1}};
   vkCmdCopyImageToBuffer(command_buffer, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          readback_buffer, 1, &readback_region);
   VkMemoryBarrier const to_host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                    .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, NULL, 0, NULL);
   CK(vkEndCommandBuffer(command_buffer));

   CK(vkResetFences(device, 1, &fence));
   VkSubmitInfo const submit_info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                     .commandBufferCount = 1,
                                     .pCommandBuffers = &command_buffer};
   CK(vkQueueSubmit(queue, 1, &submit_info, fence));
   if (vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull) != VK_SUCCESS) {
      fprintf(stderr, "fence wait failed\n");
      return 1;
   }

   printf("vertex stage   first wrong element %s\n",
          readback_map[0] == 0xFFFFFFFFu ? "none" : "");
   if (readback_map[0] != 0xFFFFFFFFu) {
      printf("               index %u, holds 0x%08x in memory  <-- WRONG\n", readback_map[0],
             source_map[readback_map[0]]);
   }
   printf("fragment stage first wrong element %s\n",
          readback_map[1] == 0xFFFFFFFFu ? "none" : "");
   if (readback_map[1] != 0xFFFFFFFFu) {
      printf("               index %u, holds 0x%08x in memory  <-- WRONG\n", readback_map[1],
             source_map[readback_map[1]]);
   }
   printf("buffer element 0 as the fragment stage read it 0x%08x, in memory 0x%08x\n",
          readback_map[2], source_map[0]);

   vkDeviceWaitIdle(device);
   return 0;
}
