/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Reading an input attachment at the fragment's own position.
 *
 * subpassLoad carries no coordinate of its own. SPIR-V gives OpImageRead on SubpassData a constant
 * zero and the fragment's position is what it is meant to read, so a driver has to supply it --
 * nir_lower_input_attachments does that, and the driver was not running it. Every input attachment
 * read therefore returned the texel at (0, 0) for every fragment.
 *
 * dEQP-VK.renderpasses.renderpass1.suballocation.formats.*.input.* failed 2544 of its 5832 cases on
 * this, and the split was exact: every case whose subpass draws while reading an input attachment
 * failed -- `draw`, `clear_draw`, `draw_use_input_aspect` and `clear_draw_use_input_aspect` were 0
 * of 318 each -- while the cases that only clear passed in full.
 *
 * The source attachment here is filled with a value that differs per texel, so a read that ignores
 * the position produces one value everywhere and is unmistakable. The first texel is deliberately
 * not special: were it, a driver reading only (0, 0) would still match there and the failure would
 * be reported one texel later than it starts.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "terakan_test_device.h"


#define IMAGE_WIDTH 16u
#define IMAGE_HEIGHT 16u
#define TEXELS (IMAGE_WIDTH * IMAGE_HEIGHT)

#define CHECK_VK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_vk_result = (expression);                                               \
      if (check_vk_result != VK_SUCCESS) {                                                         \
         fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_vk_result);            \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

/* Distinct per texel, and distinct in every channel, so a read from the wrong place shows up
 * whichever component is looked at.
 */
static void
source_texel(unsigned const x, unsigned const y, uint8_t out[4])
{
   out[0] = (uint8_t)(x * 16u + 1u);
   out[1] = (uint8_t)(y * 16u + 2u);
   out[2] = (uint8_t)(x * 4u + y * 4u + 3u);
   out[3] = 255;
}

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
   for (uint32_t i = 0; i < physical_device_count; ++i) {
      VkPhysicalDeviceProperties properties;
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

   float const queue_priority = 1.0f;
   VkDeviceQueueCreateInfo const queue_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueCount = 1,
      .pQueuePriorities = &queue_priority,
   };
   VkDeviceCreateInfo const device_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_create_info,
   };
   VkDevice device;
   CHECK_VK(vkCreateDevice(physical_device, &device_create_info, NULL, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, 0, 0, &queue);

   /* Image 0 is the input attachment, image 1 the colour output. */
   VkImage images[2];
   VkDeviceMemory image_memories[2];
   VkImageView views[2];
   VkImageUsageFlags const usages[2] = {
      VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
   };
   for (int i = 0; i < 2; ++i) {
      VkImageCreateInfo const image_create_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .extent = {IMAGE_WIDTH, IMAGE_HEIGHT, 1},
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = usages[i],
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      CHECK_VK(vkCreateImage(device, &image_create_info, NULL, &images[i]));
      VkMemoryRequirements memory_requirements;
      vkGetImageMemoryRequirements(device, images[i], &memory_requirements);
      VkMemoryAllocateInfo const allocate_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = memory_requirements.size,
         .memoryTypeIndex = find_memory_type(physical_device, memory_requirements.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
      };
      CHECK_VK(vkAllocateMemory(device, &allocate_info, NULL, &image_memories[i]));
      CHECK_VK(vkBindImageMemory(device, images[i], image_memories[i], 0));
      VkImageViewCreateInfo const view_create_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = images[i],
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      CHECK_VK(vkCreateImageView(device, &view_create_info, NULL, &views[i]));
   }

   VkDeviceSize const buffer_size = (VkDeviceSize)TEXELS * 4;
   VkBuffer buffers[2];
   VkDeviceMemory buffer_memories[2];
   uint8_t * buffer_maps[2];
   VkBufferUsageFlags const buffer_usages[2] = {VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                VK_BUFFER_USAGE_TRANSFER_DST_BIT};
   for (int i = 0; i < 2; ++i) {
      VkBufferCreateInfo const buffer_create_info = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = buffer_size,
         .usage = buffer_usages[i],
      };
      CHECK_VK(vkCreateBuffer(device, &buffer_create_info, NULL, &buffers[i]));
      VkMemoryRequirements buffer_requirements;
      vkGetBufferMemoryRequirements(device, buffers[i], &buffer_requirements);
      VkMemoryAllocateInfo const buffer_allocate_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = buffer_requirements.size,
         .memoryTypeIndex =
            find_memory_type(physical_device, buffer_requirements.memoryTypeBits,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
      };
      CHECK_VK(vkAllocateMemory(device, &buffer_allocate_info, NULL, &buffer_memories[i]));
      CHECK_VK(vkBindBufferMemory(device, buffers[i], buffer_memories[i], 0));
      CHECK_VK(vkMapMemory(device, buffer_memories[i], 0, VK_WHOLE_SIZE, 0,
                           (void **)&buffer_maps[i]));
   }
   for (unsigned y = 0; y < IMAGE_HEIGHT; ++y) {
      for (unsigned x = 0; x < IMAGE_WIDTH; ++x) {
         source_texel(x, y, buffer_maps[0] + (y * IMAGE_WIDTH + x) * 4);
      }
   }
   memset(buffer_maps[1], 0xEE, buffer_size);

   VkAttachmentDescription const attachments[2] = {
      {
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
         .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
         .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
      },
      {
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
         .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
      },
   };
   VkAttachmentReference const input_reference = {0, VK_IMAGE_LAYOUT_GENERAL};
   VkAttachmentReference const colour_reference = {1, VK_IMAGE_LAYOUT_GENERAL};
   VkSubpassDescription const subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .inputAttachmentCount = 1,
      .pInputAttachments = &input_reference,
      .colorAttachmentCount = 1,
      .pColorAttachments = &colour_reference,
   };
   VkRenderPassCreateInfo const render_pass_create_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 2,
      .pAttachments = attachments,
      .subpassCount = 1,
      .pSubpasses = &subpass,
   };
   VkRenderPass render_pass;
   CHECK_VK(vkCreateRenderPass(device, &render_pass_create_info, NULL, &render_pass));

   VkFramebufferCreateInfo const framebuffer_create_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = render_pass,
      .attachmentCount = 2,
      .pAttachments = views,
      .width = IMAGE_WIDTH,
      .height = IMAGE_HEIGHT,
      .layers = 1,
   };
   VkFramebuffer framebuffer;
   CHECK_VK(vkCreateFramebuffer(device, &framebuffer_create_info, NULL, &framebuffer));

   VkDescriptorSetLayoutBinding const layout_binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
   };
   VkDescriptorSetLayoutCreateInfo const set_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &layout_binding,
   };
   VkDescriptorSetLayout set_layout;
   CHECK_VK(vkCreateDescriptorSetLayout(device, &set_layout_create_info, NULL, &set_layout));
   VkPipelineLayoutCreateInfo const pipeline_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout,
   };
   VkPipelineLayout pipeline_layout;
   CHECK_VK(vkCreatePipelineLayout(device, &pipeline_layout_create_info, NULL, &pipeline_layout));
   VkDescriptorPoolSize const pool_size = {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1};
   VkDescriptorPoolCreateInfo const pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
   };
   VkDescriptorPool descriptor_pool;
   CHECK_VK(vkCreateDescriptorPool(device, &pool_create_info, NULL, &descriptor_pool));
   VkDescriptorSetAllocateInfo const set_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = descriptor_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &set_layout,
   };
   VkDescriptorSet descriptor_set;
   CHECK_VK(vkAllocateDescriptorSets(device, &set_allocate_info, &descriptor_set));
   VkDescriptorImageInfo const input_image_info = {
      .imageView = views[0],
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
   };
   VkWriteDescriptorSet const write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = descriptor_set,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
      .pImageInfo = &input_image_info,
   };
   vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

   VkShaderModule const vertex_module = load_shader(device, argv[1]);
   VkShaderModule const fragment_module = load_shader(device, argv[2]);
   if (vertex_module == VK_NULL_HANDLE || fragment_module == VK_NULL_HANDLE) {
      return 1;
   }
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
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   VkViewport const viewport = {0.0f, 0.0f, (float)IMAGE_WIDTH, (float)IMAGE_HEIGHT, 0.0f, 1.0f};
   VkRect2D const scissor = {{0, 0}, {IMAGE_WIDTH, IMAGE_HEIGHT}};
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
   VkPipelineColorBlendAttachmentState const blend_attachment = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
   };
   VkPipelineColorBlendStateCreateInfo const colour_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_attachment,
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
      .layout = pipeline_layout,
      .renderPass = render_pass,
      .subpass = 0,
   };
   VkPipeline pipeline;
   CHECK_VK(
      vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, NULL, &pipeline));

   VkCommandPoolCreateInfo const command_pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
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
   VkCommandBufferBeginInfo const begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   CHECK_VK(vkBeginCommandBuffer(command_buffer, &begin_info));
   for (int i = 0; i < 2; ++i) {
      VkImageMemoryBarrier const to_general = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .image = images[i],
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_general);
   }
   VkBufferImageCopy const upload = {
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {IMAGE_WIDTH, IMAGE_HEIGHT, 1},
   };
   vkCmdCopyBufferToImage(command_buffer, buffers[0], images[0], VK_IMAGE_LAYOUT_GENERAL, 1,
                          &upload);
   VkMemoryBarrier const to_read = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &to_read, 0, NULL, 0, NULL);

   VkRenderPassBeginInfo const render_pass_begin_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = render_pass,
      .framebuffer = framebuffer,
      .renderArea = {{0, 0}, {IMAGE_WIDTH, IMAGE_HEIGHT}},
   };
   vkCmdBeginRenderPass(command_buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1,
                           &descriptor_set, 0, NULL);
   vkCmdDraw(command_buffer, 3, 1, 0, 0);
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
      .imageExtent = {IMAGE_WIDTH, IMAGE_HEIGHT, 1},
   };
   vkCmdCopyImageToBuffer(command_buffer, images[1], VK_IMAGE_LAYOUT_GENERAL, buffers[1], 1,
                          &download);
   VkMemoryBarrier const to_host = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, NULL, 0, NULL);
   CHECK_VK(vkEndCommandBuffer(command_buffer));

   VkFenceCreateInfo const fence_create_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   CHECK_VK(vkCreateFence(device, &fence_create_info, NULL, &fence));
   VkSubmitInfo const submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
   };
   CHECK_VK(vkQueueSubmit(queue, 1, &submit_info, fence));
   CHECK_VK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));

   bool failed = false;
   unsigned reported = 0;
   for (unsigned y = 0; y < IMAGE_HEIGHT; ++y) {
      for (unsigned x = 0; x < IMAGE_WIDTH; ++x) {
         uint8_t expected[4];
         source_texel(x, y, expected);
         uint8_t const * const got = buffer_maps[1] + (y * IMAGE_WIDTH + x) * 4;
         if (memcmp(got, expected, 4) != 0) {
            if (reported++ < 4) {
               fprintf(stderr,
                       "texel(%u,%u) = (%u,%u,%u,%u), expected (%u,%u,%u,%u)\n", x, y, got[0],
                       got[1], got[2], got[3], expected[0], expected[1], expected[2], expected[3]);
            }
            failed = true;
         }
      }
   }

   vkDeviceWaitIdle(device);
   if (failed) {
      return 1;
   }
   printf("subpassLoad reads the input attachment at the fragment's own position\n");
   return 0;
}
