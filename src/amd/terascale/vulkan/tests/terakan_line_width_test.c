/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Line width, static and dynamic.
 *
 * PA_SU_LINE_CNTL was emitted as a constant with a `TODO: Expose line width configuration` beside
 * it, so every line came out one pixel wide while wideLines was advertised as supported. Neither
 * the pipeline's lineWidth nor vkCmdSetLineWidth reached the hardware.
 * dEQP-VK.dynamic_state failed 13 of its 122 supported cases on this, all of them line width ones.
 *
 * A horizontal line is drawn across the middle of the target and the covered rows are counted. The
 * count is the width, so a driver ignoring the width reports one row whatever was asked for. Width
 * one is checked alongside, which is the case that passed before and would still pass if the value
 * were plumbed but scaled wrongly -- the eighths-of-a-pixel encoding makes that an easy mistake,
 * and a width of four arriving as thirty-two or as one half would both show up here.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "terakan_test_device.h"


#define TARGET_SIZE 32u
#define TEXELS (TARGET_SIZE * TARGET_SIZE)
#define WIDE_LINE_WIDTH 4u

#define CHECK_VK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_vk_result = (expression);                                               \
      if (check_vk_result != VK_SUCCESS) {                                                         \
         fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_vk_result);            \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

static uint32_t
find_memory_type(VkPhysicalDevice const physical_device, uint32_t const memory_type_bits,
                 VkMemoryPropertyFlags const properties)
{
   VkPhysicalDeviceMemoryProperties memory_properties;
   vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
   for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
      if ((memory_type_bits & (1u << i)) != 0 &&
          (memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
         return i;
      }
   }
   return UINT32_MAX;
}

static VkShaderModule
load_shader(VkDevice const device, char const * const path)
{
   FILE * const file = fopen(path, "rb");
   if (file == NULL) {
      fprintf(stderr, "Cannot open %s\n", path);
      return VK_NULL_HANDLE;
   }
   fseek(file, 0, SEEK_END);
   long const size = ftell(file);
   fseek(file, 0, SEEK_SET);
   uint32_t * const code = malloc((size_t)size);
   if (code == NULL || fread(code, 1, (size_t)size, file) != (size_t)size) {
      fclose(file);
      free(code);
      return VK_NULL_HANDLE;
   }
   fclose(file);
   VkShaderModuleCreateInfo const create_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = (size_t)size,
      .pCode = code,
   };
   VkShaderModule module = VK_NULL_HANDLE;
   vkCreateShaderModule(device, &create_info, NULL, &module);
   free(code);
   return module;
}

int
main(int argc, char ** argv)
{
   if (argc < 3) {
      fprintf(stderr, "usage: %s VERT_SPV FRAG_SPV\n", argv[0]);
      return 2;
   }

   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo const instance_create_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application_info,
   };
   VkInstance instance;
   CHECK_VK(vkCreateInstance(&instance_create_info, NULL, &instance));

   uint32_t physical_device_count = 8;
   VkPhysicalDevice physical_devices[8];
   CHECK_VK(vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices));
   VkPhysicalDevice physical_device = VK_NULL_HANDLE;
   VkPhysicalDeviceProperties properties;
   for (uint32_t i = 0; i < physical_device_count; ++i) {
      vkGetPhysicalDeviceProperties(physical_devices[i], &properties);
      if (terakan_test_device_matches(properties.deviceName)) {
         physical_device = physical_devices[i];
         break;
      }
   }
   if (physical_device == VK_NULL_HANDLE) {
      fprintf(stderr, "No Terakan device found\n");
      return 77;
   }
   VkPhysicalDeviceFeatures supported_features;
   vkGetPhysicalDeviceFeatures(physical_device, &supported_features);
   if (!supported_features.wideLines ||
       properties.limits.lineWidthRange[1] < (float)WIDE_LINE_WIDTH) {
      fprintf(stderr, "wideLines up to %u is not supported\n", WIDE_LINE_WIDTH);
      return 77;
   }

   float const queue_priority = 1.0f;
   VkDeviceQueueCreateInfo const queue_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueCount = 1,
      .pQueuePriorities = &queue_priority,
   };
   VkPhysicalDeviceFeatures const enabled_features = {.wideLines = VK_TRUE};
   VkDeviceCreateInfo const device_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_create_info,
      .pEnabledFeatures = &enabled_features,
   };
   VkDevice device;
   CHECK_VK(vkCreateDevice(physical_device, &device_create_info, NULL, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, 0, 0, &queue);

   VkImageCreateInfo const image_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {TARGET_SIZE, TARGET_SIZE, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage image;
   CHECK_VK(vkCreateImage(device, &image_create_info, NULL, &image));
   VkMemoryRequirements image_requirements;
   vkGetImageMemoryRequirements(device, image, &image_requirements);
   VkMemoryAllocateInfo const image_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = image_requirements.size,
      .memoryTypeIndex = find_memory_type(physical_device, image_requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
   };
   VkDeviceMemory image_memory;
   CHECK_VK(vkAllocateMemory(device, &image_allocate_info, NULL, &image_memory));
   CHECK_VK(vkBindImageMemory(device, image, image_memory, 0));
   VkImageViewCreateInfo const view_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   VkImageView view;
   CHECK_VK(vkCreateImageView(device, &view_create_info, NULL, &view));

   VkDeviceSize const buffer_size = (VkDeviceSize)TEXELS * 4;
   VkBufferCreateInfo const buffer_create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = buffer_size,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
   };
   VkBuffer readback;
   CHECK_VK(vkCreateBuffer(device, &buffer_create_info, NULL, &readback));
   VkMemoryRequirements buffer_requirements;
   vkGetBufferMemoryRequirements(device, readback, &buffer_requirements);
   VkMemoryAllocateInfo const buffer_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = buffer_requirements.size,
      .memoryTypeIndex = find_memory_type(physical_device, buffer_requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   VkDeviceMemory readback_memory;
   CHECK_VK(vkAllocateMemory(device, &buffer_allocate_info, NULL, &readback_memory));
   CHECK_VK(vkBindBufferMemory(device, readback, readback_memory, 0));
   uint8_t * readback_map;
   CHECK_VK(vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0, (void **)&readback_map));

   VkAttachmentDescription const attachment = {
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
      .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
   };
   VkAttachmentReference const colour_reference = {0, VK_IMAGE_LAYOUT_GENERAL};
   VkSubpassDescription const subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &colour_reference,
   };
   VkRenderPassCreateInfo const render_pass_create_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
   };
   VkRenderPass render_pass;
   CHECK_VK(vkCreateRenderPass(device, &render_pass_create_info, NULL, &render_pass));
   VkFramebufferCreateInfo const framebuffer_create_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = render_pass,
      .attachmentCount = 1,
      .pAttachments = &view,
      .width = TARGET_SIZE,
      .height = TARGET_SIZE,
      .layers = 1,
   };
   VkFramebuffer framebuffer;
   CHECK_VK(vkCreateFramebuffer(device, &framebuffer_create_info, NULL, &framebuffer));

   VkPipelineLayoutCreateInfo const pipeline_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
   };
   VkPipelineLayout pipeline_layout;
   CHECK_VK(vkCreatePipelineLayout(device, &pipeline_layout_create_info, NULL, &pipeline_layout));

   VkShaderModule const vertex_module = load_shader(device, argv[1]);
   VkShaderModule const fragment_module = load_shader(device, argv[2]);
   if (vertex_module == VK_NULL_HANDLE || fragment_module == VK_NULL_HANDLE) {
      return 1;
   }

   /* Three pipelines: width one, width four baked in, and width four set dynamically. */
   struct width_case {
      char const * name;
      float pipeline_width;
      bool dynamic;
      unsigned expected_rows;
   };
   static struct width_case const cases[3] = {
      {"static width 1", 1.0f, false, 1},
      {"static width 4", (float)WIDE_LINE_WIDTH, false, WIDE_LINE_WIDTH},
      {"dynamic width 4", 1.0f, true, WIDE_LINE_WIDTH},
   };

   VkCommandPoolCreateInfo const command_pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
   };
   VkCommandPool command_pool;
   CHECK_VK(vkCreateCommandPool(device, &command_pool_create_info, NULL, &command_pool));
   VkCommandBufferAllocateInfo const command_buffer_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = command_pool,
      .commandBufferCount = 1,
   };
   VkCommandBuffer command_buffer;
   CHECK_VK(vkAllocateCommandBuffers(device, &command_buffer_allocate_info, &command_buffer));
   VkFenceCreateInfo const fence_create_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   CHECK_VK(vkCreateFence(device, &fence_create_info, NULL, &fence));

   bool failed = false;
   for (int case_index = 0; case_index < 3; ++case_index) {
      struct width_case const * const width_case = &cases[case_index];

      VkPipelineShaderStageCreateInfo const stages[2] = {
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
         .topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
      };
      VkViewport const viewport = {0.0f, 0.0f, (float)TARGET_SIZE, (float)TARGET_SIZE, 0.0f, 1.0f};
      VkRect2D const scissor = {{0, 0}, {TARGET_SIZE, TARGET_SIZE}};
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
         .lineWidth = width_case->pipeline_width,
      };
      VkPipelineMultisampleStateCreateInfo const multisample = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
         .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
      };
      VkPipelineColorBlendAttachmentState const blend_attachment = {
         .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
      };
      VkPipelineColorBlendStateCreateInfo const colour_blend = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
         .attachmentCount = 1,
         .pAttachments = &blend_attachment,
      };
      VkDynamicState const dynamic_states[1] = {VK_DYNAMIC_STATE_LINE_WIDTH};
      VkPipelineDynamicStateCreateInfo const dynamic_state = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
         .dynamicStateCount = 1,
         .pDynamicStates = dynamic_states,
      };
      VkGraphicsPipelineCreateInfo const pipeline_create_info = {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .stageCount = 2,
         .pStages = stages,
         .pVertexInputState = &vertex_input,
         .pInputAssemblyState = &input_assembly,
         .pViewportState = &viewport_state,
         .pRasterizationState = &rasterization,
         .pMultisampleState = &multisample,
         .pColorBlendState = &colour_blend,
         .pDynamicState = width_case->dynamic ? &dynamic_state : NULL,
         .layout = pipeline_layout,
         .renderPass = render_pass,
         .subpass = 0,
      };
      VkPipeline pipeline;
      CHECK_VK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, NULL,
                                         &pipeline));

      memset(readback_map, 0xEE, buffer_size);
      VkCommandBufferBeginInfo const begin_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      };
      CHECK_VK(vkResetCommandBuffer(command_buffer, 0));
      CHECK_VK(vkBeginCommandBuffer(command_buffer, &begin_info));
      VkImageMemoryBarrier const to_general = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .image = image,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &to_general);
      VkClearValue const clear_value = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}}};
      VkRenderPassBeginInfo const render_pass_begin_info = {
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
         .renderPass = render_pass,
         .framebuffer = framebuffer,
         .renderArea = {{0, 0}, {TARGET_SIZE, TARGET_SIZE}},
         .clearValueCount = 1,
         .pClearValues = &clear_value,
      };
      vkCmdBeginRenderPass(command_buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      if (width_case->dynamic) {
         vkCmdSetLineWidth(command_buffer, (float)WIDE_LINE_WIDTH);
      }
      vkCmdDraw(command_buffer, 2, 1, 0, 0);
      vkCmdEndRenderPass(command_buffer);
      VkMemoryBarrier const to_transfer = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &to_transfer, 0, NULL, 0, NULL);
      VkBufferImageCopy const download = {
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageExtent = {TARGET_SIZE, TARGET_SIZE, 1},
      };
      vkCmdCopyImageToBuffer(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL, readback, 1,
                             &download);
      VkMemoryBarrier const to_host = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, NULL, 0, NULL);
      CHECK_VK(vkEndCommandBuffer(command_buffer));
      CHECK_VK(vkResetFences(device, 1, &fence));
      VkSubmitInfo const submit_info = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &command_buffer,
      };
      CHECK_VK(vkQueueSubmit(queue, 1, &submit_info, fence));
      CHECK_VK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));

      /* A row counts as covered when the line reaches the middle column, which any width does. */
      unsigned covered_rows = 0;
      for (unsigned y = 0; y < TARGET_SIZE; ++y) {
         if (readback_map[(y * TARGET_SIZE + TARGET_SIZE / 2) * 4] != 0) {
            ++covered_rows;
         }
      }
      if (covered_rows != width_case->expected_rows) {
         fprintf(stderr, "%s: %u rows covered, expected %u\n", width_case->name, covered_rows,
                 width_case->expected_rows);
         failed = true;
      }

      vkDeviceWaitIdle(device);
      vkDestroyPipeline(device, pipeline, NULL);
   }

   if (failed) {
      return 1;
   }
   printf("Lines are as wide as the pipeline or vkCmdSetLineWidth asks\n");
   return 0;
}
