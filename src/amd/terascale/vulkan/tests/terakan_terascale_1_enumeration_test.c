/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* TeraScale 1 physical devices enumerate and report properties. On hardware-validated R700,
 * vkCreateDevice additionally performs the minimal logical-device bring-up, while R600 is still
 * refused cleanly. Queue submission remains disabled on both until the command stream is validated.
 *
 * Meaningful only on a machine with a TeraScale 1 card actually installed; there is no requirement
 * that one be present. When none is found, this reports that plainly and passes, since there is
 * nothing to check.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t const application_vertex_spirv[] = {
#include "terakan_vertex_fetch_bounds.vert.spv.h"
};
static uint32_t const application_fragment_spirv[] = {
#include "terakan_vertex_fetch_bounds.frag.spv.h"
};

#define VK_CHECK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_result = (expression);                                                  \
      if (check_result != VK_SUCCESS) {                                                            \
         fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_result);               \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

static bool
device_name_is_r700(char const * const device_name)
{
   return strstr(device_name, "RV770") != NULL || strstr(device_name, "RV730") != NULL ||
          strstr(device_name, "RV710") != NULL || strstr(device_name, "RV740") != NULL;
}

static uint32_t
first_memory_type(uint32_t const memory_type_bits)
{
   for (uint32_t memory_type = 0; memory_type < 32; ++memory_type) {
      if (memory_type_bits & ((uint32_t)1 << memory_type)) {
         return memory_type;
      }
   }
   return UINT32_MAX;
}

static uint32_t
check_r700_image_layout(VkDevice const device, VkImageCreateInfo const * const image_info,
                        char const * const name, bool const check_linear_layout)
{
   uint32_t failures = 0;
   VkImage image = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;

   VkResult const create_result = vkCreateImage(device, image_info, NULL, &image);
   if (create_result != VK_SUCCESS) {
      fprintf(stderr, "  %s vkCreateImage failed with %d\n", name, create_result);
      return 1;
   }

   VkMemoryRequirements requirements;
   vkGetImageMemoryRequirements(device, image, &requirements);
   if (requirements.size == 0 || requirements.alignment < 256 ||
       (requirements.alignment & (requirements.alignment - 1)) != 0) {
      fprintf(stderr, "  %s invalid memory requirements: size=%llu alignment=%llu\n", name,
              (unsigned long long)requirements.size,
              (unsigned long long)requirements.alignment);
      ++failures;
      goto cleanup;
   }

   if (check_linear_layout) {
      VkImageSubresource const subresource = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      };
      VkSubresourceLayout layout;
      vkGetImageSubresourceLayout(device, image, &subresource, &layout);
      uint64_t const expected_size = layout.rowPitch * image_info->extent.height;
      if (layout.offset != 0 || layout.rowPitch != requirements.alignment ||
          layout.arrayPitch != layout.size || layout.depthPitch != layout.size ||
          layout.size != expected_size) {
         fprintf(stderr,
                 "  %s unexpected linear layout: offset=%llu row=%llu array=%llu depth=%llu "
                 "size=%llu expected_size=%llu requirement_alignment=%llu\n",
                 name, (unsigned long long)layout.offset, (unsigned long long)layout.rowPitch,
                 (unsigned long long)layout.arrayPitch, (unsigned long long)layout.depthPitch,
                 (unsigned long long)layout.size, (unsigned long long)expected_size,
                 (unsigned long long)requirements.alignment);
         ++failures;
      }
   }

   uint32_t const memory_type = first_memory_type(requirements.memoryTypeBits);
   if (memory_type == UINT32_MAX) {
      fprintf(stderr, "  %s has no compatible memory type\n", name);
      ++failures;
      goto cleanup;
   }
   VkMemoryAllocateInfo const allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = memory_type,
   };
   VkResult const allocate_result = vkAllocateMemory(device, &allocate_info, NULL, &memory);
   if (allocate_result != VK_SUCCESS) {
      fprintf(stderr, "  %s vkAllocateMemory failed with %d\n", name, allocate_result);
      ++failures;
      goto cleanup;
   }
   VkResult const bind_result = vkBindImageMemory(device, image, memory, 0);
   if (bind_result != VK_SUCCESS) {
      fprintf(stderr, "  %s vkBindImageMemory failed with %d\n", name, bind_result);
      ++failures;
   } else {
      fprintf(stderr, "  %s create/layout/allocate/bind succeeded (size=%llu alignment=%llu)\n",
              name, (unsigned long long)requirements.size,
              (unsigned long long)requirements.alignment);
   }

cleanup:
   vkDestroyImage(device, image, NULL);
   if (memory != VK_NULL_HANDLE) {
      vkFreeMemory(device, memory, NULL);
   }
   return failures;
}

static uint32_t
check_r700_image_layouts(VkDevice const device)
{
   VkImageCreateInfo const linear = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {37, 19, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImageCreateInfo optimal = linear;
   optimal.extent = (VkExtent3D){300, 50, 1};
   optimal.mipLevels = 3;
   optimal.tiling = VK_IMAGE_TILING_OPTIMAL;
   optimal.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;

   VkImageCreateInfo bc1 = optimal;
   bc1.format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
   bc1.extent = (VkExtent3D){1023, 511, 1};
   bc1.mipLevels = 4;

   VkImageCreateInfo expand_3x = linear;
   expand_3x.format = VK_FORMAT_R32G32B32_SFLOAT;
   expand_3x.extent = (VkExtent3D){81, 5, 1};
   expand_3x.mipLevels = 2;

   uint32_t failures = 0;
   failures += check_r700_image_layout(device, &linear, "linear RGBA8", true);
   failures += check_r700_image_layout(device, &optimal, "2D-tiled RGBA8 mip chain", false);
   failures += check_r700_image_layout(device, &bc1, "2D-tiled BC1 mip chain", false);
   failures +=
      check_r700_image_layout(device, &expand_3x, "linear R32G32B32 mip chain", false);
   return failures;
}

static uint32_t
check_r700_application_graphics_shader_compile(VkDevice const device)
{
   VkShaderModule vertex_module = VK_NULL_HANDLE, fragment_module = VK_NULL_HANDLE;
   VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
   VkRenderPass render_pass = VK_NULL_HANDLE;
   VkPipeline pipeline = VK_NULL_HANDLE;
   uint32_t failures = 0;

   VkShaderModuleCreateInfo const vertex_module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(application_vertex_spirv),
      .pCode = application_vertex_spirv,
   };
   VkResult result = vkCreateShaderModule(device, &vertex_module_info, NULL, &vertex_module);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  application vertex vkCreateShaderModule failed with %d\n", result);
      return 1;
   }
   VkShaderModuleCreateInfo const fragment_module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(application_fragment_spirv),
      .pCode = application_fragment_spirv,
   };
   result = vkCreateShaderModule(device, &fragment_module_info, NULL, &fragment_module);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  application fragment vkCreateShaderModule failed with %d\n", result);
      failures = 1;
      goto cleanup;
   }

   VkPipelineLayoutCreateInfo const pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
   };
   result = vkCreatePipelineLayout(device, &pipeline_layout_info, NULL, &pipeline_layout);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  application vkCreatePipelineLayout failed with %d\n", result);
      failures = 1;
      goto cleanup;
   }

   VkAttachmentDescription const attachment = {
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
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
   result = vkCreateRenderPass(device, &render_pass_info, NULL, &render_pass);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  application vkCreateRenderPass failed with %d\n", result);
      failures = 1;
      goto cleanup;
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
   VkVertexInputBindingDescription const vertex_binding = {
      .binding = 0,
      .stride = 16,
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
   };
   VkVertexInputAttributeDescription const vertex_attribute = {
      .location = 0,
      .binding = 0,
      .format = VK_FORMAT_R32G32B32A32_UINT,
   };
   VkPipelineVertexInputStateCreateInfo const vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &vertex_binding,
      .vertexAttributeDescriptionCount = 1,
      .pVertexAttributeDescriptions = &vertex_attribute,
   };
   VkPipelineInputAssemblyStateCreateInfo const input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   VkViewport const viewport = {.width = 1.0F, .height = 1.0F, .maxDepth = 1.0F};
   VkRect2D const scissor = {.extent = {1, 1}};
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
   VkPipelineColorBlendStateCreateInfo const blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_attachment,
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
      .pColorBlendState = &blend,
      .layout = pipeline_layout,
      .renderPass = render_pass,
   };
   result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "  R700 application VS/FS pipeline compilation failed with %d\n", result);
      failures = 1;
   } else {
      fprintf(stderr, "  R700 application VS/FS pipeline compilation succeeded\n");
   }

cleanup:
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyRenderPass(device, render_pass, NULL);
   vkDestroyPipelineLayout(device, pipeline_layout, NULL);
   vkDestroyShaderModule(device, fragment_module, NULL);
   vkDestroyShaderModule(device, vertex_module, NULL);
   return failures;
}

int
main(void)
{
   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-terascale-1-enumeration-test",
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo const instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application_info,
   };
   VkInstance instance;
   VK_CHECK(vkCreateInstance(&instance_info, NULL, &instance));

   uint32_t physical_device_count = 0;
   VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_device_count, NULL));
   VkPhysicalDevice physical_devices[8];
   if (physical_device_count > 8) {
      physical_device_count = 8;
   }
   VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices));

   uint32_t terascale_1_devices_checked = 0;
   uint32_t failures = 0;
   for (uint32_t device_index = 0; device_index < physical_device_count; ++device_index) {
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(physical_devices[device_index], &properties);
      if (strstr(properties.deviceName, "TeraScale 1") == NULL) {
         continue;
      }
      ++terascale_1_devices_checked;
      fprintf(stderr, "found %s (vendor=0x%04x device=0x%04x)\n", properties.deviceName,
              properties.vendorID, properties.deviceID);

      if (properties.vendorID != 0x1002) {
         fprintf(stderr, "  vendorID is 0x%04x, expected 0x1002 (ATI/AMD)\n", properties.vendorID);
         ++failures;
      }
      if (properties.apiVersion == 0) {
         fprintf(stderr, "  apiVersion is 0\n");
         ++failures;
      }

      float const priority = 1.0F;
      VkDeviceQueueCreateInfo const queue_info = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
         .queueFamilyIndex = 0,
         .queueCount = 1,
         .pQueuePriorities = &priority,
      };
      VkDeviceCreateInfo const device_info = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .queueCreateInfoCount = 1,
         .pQueueCreateInfos = &queue_info,
      };
      VkDevice device;
      VkResult const create_result =
         vkCreateDevice(physical_devices[device_index], &device_info, NULL, &device);
      if (device_name_is_r700(properties.deviceName)) {
         if (create_result != VK_SUCCESS) {
            fprintf(stderr, "  vkCreateDevice failed with %d on hardware-validated R700\n",
                    create_result);
            ++failures;
         } else {
            fprintf(stderr, "  minimal R700 vkCreateDevice succeeded\n");

            failures += check_r700_image_layouts(device);
            failures += check_r700_application_graphics_shader_compile(device);

            VkQueue queue;
            vkGetDeviceQueue(device, 0, 0, &queue);
            VkSubmitInfo const empty_submit = {
               .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            };
            VkResult const submit_result = vkQueueSubmit(queue, 1, &empty_submit, VK_NULL_HANDLE);
            if (submit_result != VK_ERROR_DEVICE_LOST) {
               fprintf(
                  stderr,
                  "  guarded R700 queue submission returned %d, expected VK_ERROR_DEVICE_LOST\n",
                  submit_result);
               ++failures;
            } else {
               fprintf(stderr, "  R700 queue submission remains safely disabled\n");
            }
         }
      } else if (create_result == VK_SUCCESS) {
         fprintf(stderr, "  vkCreateDevice unexpectedly succeeded on unvalidated TeraScale 1 %s\n",
                 properties.deviceName);
         ++failures;
      } else if (create_result != VK_ERROR_INITIALIZATION_FAILED) {
         fprintf(stderr,
                 "  vkCreateDevice failed with %d, expected VK_ERROR_INITIALIZATION_FAILED\n",
                 create_result);
         ++failures;
      }
      if (create_result == VK_SUCCESS) {
         vkDestroyDevice(device, NULL);
      }
   }

   if (terascale_1_devices_checked == 0) {
      printf("terascale_1_enumeration: no TeraScale 1 device present on this machine, nothing to "
             "check, PASS\n");
   } else {
      printf("terascale_1_enumeration: checked=%u bad=%u %s\n", terascale_1_devices_checked,
             failures, failures == 0 ? "PASS" : "FAIL");
   }

   vkDestroyInstance(instance, NULL);
   return failures == 0 ? 0 : 1;
}
