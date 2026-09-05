/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Vertex attribute divisors that are not powers of two.
 *
 * The instance index is divided by the divisor in the fetch shader, using the multiply-high
 * algorithm util_compute_fast_udiv_info describes. The multiplication is the division -- the
 * shifts and the increment only condition its input and output -- and the driver requested the
 * surrounding operations without ever requesting the multiplication itself. The index therefore
 * came out shifted rather than divided, and every divisor that is not a power of two fetched the
 * wrong instance. Powers of two took a separate path that shifts on purpose and were fine, which is
 * why this went unnoticed: dEQP-VK.draw.renderpass.instanced passed all 96 of its cases at each of
 * the divisors 0, 1, 2 and 4, and failed all 96 at divisor 20.
 *
 * Each instance draws a line at its own row, coloured from an attribute the divisor selects, so a
 * wrong quotient shows up as a row holding another element's colour. Divisors one and two are
 * checked alongside three: they exercise the path that already worked, so a change that broke
 * division outright rather than only the non-power-of-two case moves them too.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "terakan_test_device.h"


#define TARGET_SIZE 16u
#define TEXELS (TARGET_SIZE * TARGET_SIZE)
#define INSTANCE_COUNT 12u
#define ELEMENT_COUNT 12u

#define CHECK_VK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_vk_result = (expression);                                               \
      if (check_vk_result != VK_SUCCESS) {                                                         \
         fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_vk_result);            \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

/* Distinct per element and never zero, so an unwritten row cannot be mistaken for a written one. */
static void
element_colour(unsigned const element, uint8_t out[4])
{
   out[0] = (uint8_t)(element * 20u + 11u);
   out[1] = (uint8_t)(element * 7u + 29u);
   out[2] = (uint8_t)(255u - element * 13u);
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
   char const * const instance_extensions[1] = {
      VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
   VkInstanceCreateInfo const instance_create_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application_info,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = instance_extensions,
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
   char const * const device_extensions[1] = {VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME};
   VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT divisor_features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
      .vertexAttributeInstanceRateDivisor = VK_TRUE,
   };
   VkDeviceCreateInfo const device_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &divisor_features,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_create_info,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = device_extensions,
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

   VkDeviceSize const vertex_buffer_size = (VkDeviceSize)ELEMENT_COUNT * 4 * sizeof(float);
   VkBufferCreateInfo const vertex_buffer_create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = vertex_buffer_size,
      .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
   };
   VkBuffer vertex_buffer;
   CHECK_VK(vkCreateBuffer(device, &vertex_buffer_create_info, NULL, &vertex_buffer));
   VkMemoryRequirements vertex_requirements;
   vkGetBufferMemoryRequirements(device, vertex_buffer, &vertex_requirements);
   VkMemoryAllocateInfo const vertex_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = vertex_requirements.size,
      .memoryTypeIndex = find_memory_type(physical_device, vertex_requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   VkDeviceMemory vertex_memory;
   CHECK_VK(vkAllocateMemory(device, &vertex_allocate_info, NULL, &vertex_memory));
   CHECK_VK(vkBindBufferMemory(device, vertex_buffer, vertex_memory, 0));
   float * vertex_map;
   CHECK_VK(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0, (void **)&vertex_map));
   for (unsigned element = 0; element < ELEMENT_COUNT; ++element) {
      uint8_t colour[4];
      element_colour(element, colour);
      for (unsigned channel = 0; channel < 4; ++channel) {
         vertex_map[element * 4 + channel] = (float)colour[channel] / 255.0f;
      }
   }

   VkDeviceSize const readback_size = (VkDeviceSize)TEXELS * 4;
   VkBufferCreateInfo const readback_create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = readback_size,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
   };
   VkBuffer readback;
   CHECK_VK(vkCreateBuffer(device, &readback_create_info, NULL, &readback));
   VkMemoryRequirements readback_requirements;
   vkGetBufferMemoryRequirements(device, readback, &readback_requirements);
   VkMemoryAllocateInfo const readback_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = readback_requirements.size,
      .memoryTypeIndex = find_memory_type(physical_device, readback_requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   VkDeviceMemory readback_memory;
   CHECK_VK(vkAllocateMemory(device, &readback_allocate_info, NULL, &readback_memory));
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
      return TERAKAN_TEST_DEVICE_NOT_FOUND_STATUS;
   }

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

   static uint32_t const divisors[3] = {1, 2, 3};
   bool failed = false;
   for (int divisor_index = 0; divisor_index < 3; ++divisor_index) {
      uint32_t const divisor = divisors[divisor_index];

      VkVertexInputBindingDescription const binding = {
         .binding = 0,
         .stride = 4 * sizeof(float),
         .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE,
      };
      VkVertexInputBindingDivisorDescriptionEXT const binding_divisor = {
         .binding = 0,
         .divisor = divisor,
      };
      VkPipelineVertexInputDivisorStateCreateInfoEXT const divisor_state = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
         .vertexBindingDivisorCount = 1,
         .pVertexBindingDivisors = &binding_divisor,
      };
      VkVertexInputAttributeDescription const attribute = {
         .location = 0,
         .binding = 0,
         .format = VK_FORMAT_R32G32B32A32_SFLOAT,
         .offset = 0,
      };
      VkPipelineVertexInputStateCreateInfo const vertex_input = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
         .pNext = &divisor_state,
         .vertexBindingDescriptionCount = 1,
         .pVertexBindingDescriptions = &binding,
         .vertexAttributeDescriptionCount = 1,
         .pVertexAttributeDescriptions = &attribute,
      };
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
      CHECK_VK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, NULL,
                                         &pipeline));

      memset(readback_map, 0xEE, readback_size);
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
      VkClearValue const clear_value = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}}};
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
      VkDeviceSize const vertex_offset = 0;
      vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer, &vertex_offset);
      vkCmdDraw(command_buffer, 2, INSTANCE_COUNT, 0, 0);
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

      unsigned reported = 0;
      for (unsigned instance = 0; instance < INSTANCE_COUNT; ++instance) {
         uint8_t expected[4];
         element_colour(instance / divisor, expected);
         uint8_t const * const got = readback_map + (instance * TARGET_SIZE + TARGET_SIZE / 2) * 4;
         /* The attribute goes through a UNORM attachment, so one unit of rounding is allowed. */
         bool matches = true;
         for (unsigned channel = 0; channel < 4; ++channel) {
            if (abs((int)got[channel] - (int)expected[channel]) > 1) {
               matches = false;
            }
         }
         if (!matches) {
            if (reported++ < 3) {
               fprintf(stderr,
                       "divisor %u: instance %u got (%u,%u,%u,%u), expected element %u "
                       "(%u,%u,%u,%u)\n",
                       divisor, instance, got[0], got[1], got[2], got[3], instance / divisor,
                       expected[0], expected[1], expected[2], expected[3]);
            }
            failed = true;
         }
      }

      vkDeviceWaitIdle(device);
      vkDestroyPipeline(device, pipeline, NULL);
   }

   if (failed) {
      return 1;
   }
   printf("Instance attribute divisors select the right element\n");
   return 0;
}
