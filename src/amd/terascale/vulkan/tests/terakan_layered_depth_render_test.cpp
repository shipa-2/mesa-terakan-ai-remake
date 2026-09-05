/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Rendering into individual array layers of a layered depth image.
 *
 * This is the shape an omni light's shadow map has: one depth image with six layers, each cube face
 * rendered by its own render pass through a single-layer view. Nothing else in this suite covered
 * it, and it was broken in a way that only showed up in real applications.
 *
 * DB_DEPTH_VIEW's SLICE_START/SLICE_MAX is the only thing that selects which layer a depth render
 * targets -- the base addresses deliberately point at the surface rather than at the slice, because
 * DB indexes slices from the base itself. terakan_hw_config_draw_set_db_depth_stencil_buffer()'s
 * early-out for redundant state compared every field of the descriptor except that one, so two
 * consecutive layers of the same image were indistinguishable to it: same buffer object, same
 * z_info, same size, same slice, same base addresses. The update was skipped, DB_DEPTH_VIEW kept
 * whatever layer it already held, and every face rendered into face zero while the rest of the
 * image kept whatever the previous owner of that memory left behind.
 *
 * The whole image is cleared to a sentinel first, so a layer that never gets written is a
 * deterministic value rather than stale VRAM -- otherwise this test would reproduce the original
 * bug only as often as the garbage happened to be wrong, which in the application was a bit over
 * half the time. Each layer is then cleared to a value only it should hold, so a failure names
 * which layer received which layer's value.
 */

#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "terakan_test_device.h"


#define VK_CHECK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_result = (expression);                                                  \
      if (check_result != VK_SUCCESS) {                                                            \
         std::fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_result);          \
         return 1;                                                                                 \
      }                                                                                            \
   } while (false)

namespace {

constexpr uint32_t kWidth = 32;
constexpr uint32_t kHeight = 32;
constexpr uint32_t kLayerCount = 6; /* A cube map's six faces. */
/* Not equal to any layer's own value, so an unwritten layer is distinguishable from a wrong one. */
constexpr float kSentinelDepth = 0.5F;

float
layer_depth(uint32_t layer)
{
   return 0.1F + float(layer) * 0.15F;
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

} // namespace

int
main()
{
   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-layered-depth-render-test",
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
      if (!terakan_test_device_matches(properties.deviceName))
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
   std::fprintf(stderr, "device=%s queue_family=%u layers=%u\n", properties.deviceName, queue_family,
                kLayerCount);

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

   VkImageCreateInfo const image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_D32_SFLOAT,
      .extent = {kWidth, kHeight, 1},
      .mipLevels = 1,
      .arrayLayers = kLayerCount,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage image;
   VK_CHECK(vkCreateImage(device, &image_info, nullptr, &image));
   VkMemoryRequirements image_requirements;
   vkGetImageMemoryRequirements(device, image, &image_requirements);
   VkMemoryAllocateInfo const image_allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = image_requirements.size,
      .memoryTypeIndex = find_memory_type(physical_device, image_requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
   };
   VkDeviceMemory image_memory;
   VK_CHECK(vkAllocateMemory(device, &image_allocation, nullptr, &image_memory));
   VK_CHECK(vkBindImageMemory(device, image, image_memory, 0));

   /* One single-layer view per layer, which is how a cube face is rendered. */
   VkImageView layer_views[kLayerCount];
   for (uint32_t layer = 0; layer < kLayerCount; ++layer) {
      VkImageViewCreateInfo const view_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = image,
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = VK_FORMAT_D32_SFLOAT,
         .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, layer, 1},
      };
      VK_CHECK(vkCreateImageView(device, &view_info, nullptr, &layer_views[layer]));
   }

   VkDeviceSize const layer_bytes = kWidth * kHeight * sizeof(float);
   VkDeviceSize const readback_bytes = layer_bytes * kLayerCount;
   VkBufferCreateInfo const readback_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = readback_bytes,
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
   float * readback_mapping;
   VK_CHECK(vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0,
                        reinterpret_cast<void **>(&readback_mapping)));

   VkAttachmentDescription const attachment = {
      .format = VK_FORMAT_D32_SFLOAT,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
      .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
   };
   VkAttachmentReference const depth_reference = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
   };
   VkSubpassDescription const subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .pDepthStencilAttachment = &depth_reference,
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
   VkFramebuffer layer_framebuffers[kLayerCount];
   for (uint32_t layer = 0; layer < kLayerCount; ++layer) {
      VkFramebufferCreateInfo const framebuffer_info = {
         .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
         .renderPass = render_pass,
         .attachmentCount = 1,
         .pAttachments = &layer_views[layer],
         .width = kWidth,
         .height = kHeight,
         .layers = 1,
      };
      VK_CHECK(vkCreateFramebuffer(device, &framebuffer_info, nullptr, &layer_framebuffers[layer]));
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

   VkImageSubresourceRange const whole_image = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, kLayerCount};
   VkImageMemoryBarrier const to_general = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = whole_image,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_general);

   /* Every layer starts at the sentinel, so "never written" is a known value. */
   VkClearDepthStencilValue const sentinel = {.depth = kSentinelDepth, .stencil = 0};
   vkCmdClearDepthStencilImage(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL, &sentinel, 1,
                               &whole_image);
   VkMemoryBarrier const cleared = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 1, &cleared, 0, nullptr, 0,
                        nullptr);

   /* One render pass per layer, back to back, exactly as the six faces of a cube shadow map are
    * recorded. Nothing but the layer differs between them, which is what defeated the redundant
    * state check.
    */
   for (uint32_t layer = 0; layer < kLayerCount; ++layer) {
      VkClearValue clear_value = {};
      clear_value.depthStencil.depth = layer_depth(layer);
      VkRenderPassBeginInfo const render_begin = {
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
         .renderPass = render_pass,
         .framebuffer = layer_framebuffers[layer],
         .renderArea = {{0, 0}, {kWidth, kHeight}},
         .clearValueCount = 1,
         .pClearValues = &clear_value,
      };
      vkCmdBeginRenderPass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdEndRenderPass(command_buffer);
   }

   VkImageMemoryBarrier const to_transfer = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = whole_image,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_transfer);

   for (uint32_t layer = 0; layer < kLayerCount; ++layer) {
      VkBufferImageCopy const region = {
         .bufferOffset = layer_bytes * layer,
         .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, layer, 1},
         .imageExtent = {kWidth, kHeight, 1},
      };
      vkCmdCopyImageToBuffer(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL, readback_buffer, 1,
                             &region);
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

   uint32_t bad_layers = 0;
   for (uint32_t layer = 0; layer < kLayerCount; ++layer) {
      float const expected = layer_depth(layer);
      float const * const texels = readback_mapping + (layer_bytes / sizeof(float)) * layer;
      uint32_t mismatches = 0;
      float first_bad = 0.0F;
      for (uint32_t texel = 0; texel < kWidth * kHeight; ++texel) {
         if (std::fabs(texels[texel] - expected) > 1.0e-5F) {
            if (mismatches == 0)
               first_bad = texels[texel];
            ++mismatches;
         }
      }
      if (mismatches != 0) {
         /* Name what the layer actually holds: the sentinel means the layer was never rendered to,
          * and another layer's value means the render landed on the wrong layer.
          */
         char const * meaning = "an unexpected value";
         if (std::fabs(first_bad - kSentinelDepth) <= 1.0e-5F) {
            meaning = "the sentinel, so this layer was never rendered to";
         } else {
            for (uint32_t other = 0; other < kLayerCount; ++other) {
               if (other != layer && std::fabs(first_bad - layer_depth(other)) <= 1.0e-5F) {
                  meaning = "another layer's value, so the render landed on the wrong layer";
                  break;
               }
            }
         }
         std::fprintf(stderr, "layer %u: %u/%u texels wrong, first was %.3f (expected %.3f) -- %s\n",
                      layer, mismatches, kWidth * kHeight, first_bad, expected, meaning);
         ++bad_layers;
      }
   }
   std::printf("layered_depth_render layers=%u bad=%u %s\n", kLayerCount, bad_layers,
               bad_layers == 0 ? "PASS" : "FAIL");

   vkDeviceWaitIdle(device);
   vkDestroyFence(device, fence, nullptr);
   vkDestroyCommandPool(device, command_pool, nullptr);
   for (uint32_t layer = 0; layer < kLayerCount; ++layer) {
      vkDestroyFramebuffer(device, layer_framebuffers[layer], nullptr);
      vkDestroyImageView(device, layer_views[layer], nullptr);
   }
   vkDestroyRenderPass(device, render_pass, nullptr);
   vkDestroyImage(device, image, nullptr);
   vkFreeMemory(device, image_memory, nullptr);
   vkUnmapMemory(device, readback_memory);
   vkDestroyBuffer(device, readback_buffer, nullptr);
   vkFreeMemory(device, readback_memory, nullptr);
   vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr);
   return bad_layers == 0 ? 0 : 1;
}
