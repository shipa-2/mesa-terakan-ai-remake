/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* vkCmdClearDepthStencilImage on a multisample image, checked per sample.
 *
 * Clears a 4-sample D32_SFLOAT_S8_UINT image outside a render pass and then reads every sample of
 * every texel back with texelFetch on a sampler2DMS and a usampler2DMS. The meta clear draws a
 * rectangle, and if it rasterizes single-sample it writes only one sample's worth of the image's
 * memory - one quarter of a 4x surface, in the tile-interleaved pattern the samples are laid out
 * in - while the rest keeps whatever was there. Against the driver before that was fixed this test
 * reports roughly three quarters of the surface unwritten.
 *
 * The image memory is filled with a distinctive pattern first, so an untouched surface reads as
 * that pattern rather than as whatever the previous owner left, and the failure is a mismatch
 * rather than a coin flip.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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

#define WIDTH 32u
#define HEIGHT 32u
#define SAMPLE_COUNT 4u

static float const clear_depth = 0.375F;
static uint32_t const clear_stencil = 0x33u;

static uint32_t const clear_multisample_spirv[] = {
#include "terakan_depth_stencil_clear_multisample.spv.h"

};

struct push_constants {
   uint32_t width;
   uint32_t height;
   uint32_t sample_count;
   float expected_depth;
   uint32_t expected_stencil;
};

struct results {
   uint32_t depth_mismatches;
   uint32_t stencil_mismatches;
   uint32_t depth_seen_bits;
   uint32_t stencil_seen;
};

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

int
main(void)
{
   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-depth-stencil-clear-multisample-test",
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
      return TERAKAN_TEST_DEVICE_NOT_FOUND_STATUS;
   }

   VkFormat const format = VK_FORMAT_D32_SFLOAT_S8_UINT;
   VkImageFormatProperties image_format_properties;
   VK_CHECK(vkGetPhysicalDeviceImageFormatProperties(
      physical_device, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
         VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      0, &image_format_properties));
   if (!(image_format_properties.sampleCounts & VK_SAMPLE_COUNT_4_BIT)) {
      fprintf(stderr, "D32_SFLOAT_S8_UINT does not support 4 samples\n");
      return 1;
   }

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
   };
   VkDevice device;
   VK_CHECK(vkCreateDevice(physical_device, &device_info, NULL, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, 0, 0, &queue);

   VkImageCreateInfo const image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {WIDTH, HEIGHT, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_4_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT,
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

   VkImageViewCreateInfo depth_view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = format,
      .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
   };
   VkImageView depth_view;
   VK_CHECK(vkCreateImageView(device, &depth_view_info, NULL, &depth_view));
   VkImageViewCreateInfo stencil_view_info = depth_view_info;
   stencil_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
   VkImageView stencil_view;
   VK_CHECK(vkCreateImageView(device, &stencil_view_info, NULL, &stencil_view));

   /* Reaches every sample, unlike the clear under test, so what the clear misses is known. */
   float const preclear_depth = 0.875F;
   uint32_t const preclear_stencil = 0x77u;
   VkAttachmentDescription const attachment = {
      .format = format,
      .samples = VK_SAMPLE_COUNT_4_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
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
   VkImageViewCreateInfo attachment_view_info = depth_view_info;
   attachment_view_info.subresourceRange.aspectMask =
      VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
   VkImageView attachment_view;
   VK_CHECK(vkCreateImageView(device, &attachment_view_info, NULL, &attachment_view));
   VkFramebufferCreateInfo const framebuffer_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = render_pass,
      .attachmentCount = 1,
      .pAttachments = &attachment_view,
      .width = WIDTH,
      .height = HEIGHT,
      .layers = 1,
   };
   VkFramebuffer framebuffer;
   VK_CHECK(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));

   VkBufferCreateInfo const results_buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = sizeof(struct results),
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
   };
   VkBuffer results_buffer;
   VK_CHECK(vkCreateBuffer(device, &results_buffer_info, NULL, &results_buffer));
   VkMemoryRequirements results_requirements;
   vkGetBufferMemoryRequirements(device, results_buffer, &results_requirements);
   VkMemoryAllocateInfo const results_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = results_requirements.size,
      .memoryTypeIndex = find_memory_type(physical_device, results_requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   VkDeviceMemory results_memory;
   VK_CHECK(vkAllocateMemory(device, &results_allocate_info, NULL, &results_memory));
   VK_CHECK(vkBindBufferMemory(device, results_buffer, results_memory, 0));
   struct results * results;
   VK_CHECK(vkMapMemory(device, results_memory, 0, VK_WHOLE_SIZE, 0, (void **)&results));
   memset(results, 0, sizeof(*results));

   VkShaderModuleCreateInfo const module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(clear_multisample_spirv),
      .pCode = clear_multisample_spirv,
   };
   VkShaderModule module;
   VK_CHECK(vkCreateShaderModule(device, &module_info, NULL, &module));

   VkDescriptorSetLayoutBinding const bindings[3] = {
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      {
         .binding = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      {
         .binding = 2,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
   };
   VkDescriptorSetLayoutCreateInfo const set_layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 3,
      .pBindings = bindings,
   };
   VkDescriptorSetLayout set_layout;
   VK_CHECK(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout));
   VkPushConstantRange const push_constant_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                                    sizeof(struct push_constants)};
   VkPipelineLayoutCreateInfo const pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push_constant_range,
   };
   VkPipelineLayout pipeline_layout;
   VK_CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, NULL, &pipeline_layout));
   VkComputePipelineCreateInfo const pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage =
         {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = "main",
         },
      .layout = pipeline_layout,
   };
   VkPipeline pipeline;
   VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline));

   VkSamplerCreateInfo const sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
   };
   VkSampler sampler;
   VK_CHECK(vkCreateSampler(device, &sampler_info, NULL, &sampler));

   VkDescriptorPoolSize const pool_sizes[2] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
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
   VkDescriptorImageInfo const depth_descriptor = {
      .sampler = sampler,
      .imageView = depth_view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
   };
   VkDescriptorImageInfo const stencil_descriptor = {
      .sampler = sampler,
      .imageView = stencil_view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
   };
   VkDescriptorBufferInfo const results_descriptor = {
      .buffer = results_buffer,
      .offset = 0,
      .range = sizeof(struct results),
   };
   VkWriteDescriptorSet const writes[3] = {
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_set,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &depth_descriptor,
      },
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_set,
         .dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &stencil_descriptor,
      },
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_set,
         .dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &results_descriptor,
      },
   };
   vkUpdateDescriptorSets(device, 3, writes, 0, NULL);

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

   VkImageSubresourceRange const whole_image = {
      VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
   VkImageMemoryBarrier const to_general = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .image = image,
      .subresourceRange = whole_image,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &to_general);

   VkClearValue const preclear_value = {.depthStencil = {preclear_depth, preclear_stencil}};
   VkRenderPassBeginInfo const render_pass_begin_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = render_pass,
      .framebuffer = framebuffer,
      .renderArea = {{0, 0}, {WIDTH, HEIGHT}},
      .clearValueCount = 1,
      .pClearValues = &preclear_value,
   };
   vkCmdBeginRenderPass(command_buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdEndRenderPass(command_buffer);

   VkMemoryBarrier const preclear_to_clear = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &preclear_to_clear, 0, NULL, 0,
                        NULL);

   VkClearDepthStencilValue const clear_value = {clear_depth, clear_stencil};
   vkCmdClearDepthStencilImage(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL, &clear_value, 1,
                               &whole_image);

   VkMemoryBarrier const clear_to_shader = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &clear_to_shader, 0, NULL, 0,
                        NULL);

   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                           &descriptor_set, 0, NULL);
   struct push_constants const constants = {
      .width = WIDTH,
      .height = HEIGHT,
      .sample_count = SAMPLE_COUNT,
      .expected_depth = clear_depth,
      .expected_stencil = clear_stencil,
   };
   vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                      sizeof(constants), &constants);
   vkCmdDispatch(command_buffer, (WIDTH + 7) / 8, (HEIGHT + 7) / 8, 1);

   VkMemoryBarrier const shader_to_host = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &shader_to_host, 0, NULL, 0, NULL);
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

   uint32_t const total = WIDTH * HEIGHT * SAMPLE_COUNT;
   bool const passed = results->depth_mismatches == 0 && results->stencil_mismatches == 0;
   printf("depth: %u of %u samples not %.4f\n", results->depth_mismatches, total, clear_depth);
   printf("stencil: %u of %u samples not 0x%02x\n", results->stencil_mismatches, total,
          clear_stencil);
   if (!passed) {
      union {
         uint32_t bits;
         float value;
      } const depth_seen = {.bits = results->depth_seen_bits};
      fprintf(stderr,
              "The multisample clear did not reach every sample: saw depth %f, stencil 0x%02x\n",
              depth_seen.value, results->stencil_seen);
   }

   vkDeviceWaitIdle(device);
   return passed ? 0 : 1;
}
