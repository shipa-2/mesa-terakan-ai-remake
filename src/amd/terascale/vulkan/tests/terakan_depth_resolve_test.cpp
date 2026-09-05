/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Readback test for VK_KHR_depth_stencil_resolve in VK_RESOLVE_MODE_SAMPLE_ZERO_BIT.
 *
 * Clears a 2x multisample depth attachment to a known value through a render pass created with
 * vkCreateRenderPass2, resolves it into a single-sample destination declared through
 * VkSubpassDescriptionDepthStencilResolve, and checks every resolved texel against the value.
 *
 * The destination starts filled with a different value, so a resolve that never runs, or that
 * writes nothing, fails rather than passing on leftover contents.
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

constexpr uint32_t kWidth = 8;
constexpr uint32_t kHeight = 8;
constexpr float kClearDepth = 0.375F;
constexpr float kDestinationInitialDepth = 0.875F;
constexpr uint32_t kClearStencil = 0x5Au;
constexpr uint32_t kDestinationInitialStencil = 0xA3u;

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

struct Image {
   VkImage image = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   VkImageView view = VK_NULL_HANDLE;
};

VkResult
create_depth_image(VkDevice device, VkPhysicalDevice physical_device, VkSampleCountFlagBits samples,
                   VkImageUsageFlags usage, VkFormat format, VkImageAspectFlags aspects, Image & out)
{
   VkImageCreateInfo const info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {kWidth, kHeight, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = samples,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkResult result = vkCreateImage(device, &info, nullptr, &out.image);
   if (result != VK_SUCCESS)
      return result;
   VkMemoryRequirements requirements;
   vkGetImageMemoryRequirements(device, out.image, &requirements);
   uint32_t const memory_type = find_memory_type(physical_device, requirements.memoryTypeBits,
                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (memory_type == UINT32_MAX)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   VkMemoryAllocateInfo const allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = memory_type,
   };
   result = vkAllocateMemory(device, &allocation, nullptr, &out.memory);
   if (result != VK_SUCCESS)
      return result;
   result = vkBindImageMemory(device, out.image, out.memory, 0);
   if (result != VK_SUCCESS)
      return result;
   VkImageViewCreateInfo const view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = out.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = info.format,
      .subresourceRange = {aspects, 0, 1, 0, 1},
   };
   return vkCreateImageView(device, &view_info, nullptr, &out.view);
}

} // namespace

int
main(int argc, char ** argv)
{
   /* --stencil resolves a combined depth/stencil format and checks both aspects. */
   bool const with_stencil = argc == 2 && std::strcmp(argv[1], "--stencil") == 0;
   if (argc > 2 || (argc == 2 && !with_stencil)) {
      std::fprintf(stderr, "usage: %s [--stencil]\n", argv[0]);
      return 2;
   }
   VkFormat const format =
      with_stencil ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT;
   VkImageAspectFlags const resolved_aspects =
      VK_IMAGE_ASPECT_DEPTH_BIT | (with_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-depth-resolve-test",
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

   VkPhysicalDeviceDepthStencilResolveProperties resolve_properties = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES,
   };
   VkPhysicalDeviceProperties2 properties_2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &resolve_properties,
   };
   vkGetPhysicalDeviceProperties2(physical_device, &properties_2);
   if (!(resolve_properties.supportedDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)) {
      std::fprintf(stderr, "Sample-zero depth resolve is not advertised, nothing to test\n");
      return 77;
   }
   std::fprintf(stderr, "device=%s queue_family=%u\n", properties.deviceName, queue_family);

   float const priority = 1.0F;
   VkDeviceQueueCreateInfo const queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = queue_family,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   char const * const device_extensions[] = {
      VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
      VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME,
   };
   VkDeviceCreateInfo const device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
      .enabledExtensionCount = 2,
      .ppEnabledExtensionNames = device_extensions,
   };
   VkDevice device;
   VK_CHECK(vkCreateDevice(physical_device, &device_info, nullptr, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, queue_family, 0, &queue);

   Image multisample_depth;
   VK_CHECK(create_depth_image(device, physical_device, VK_SAMPLE_COUNT_2_BIT,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT,
                               format, resolved_aspects, multisample_depth));
   Image resolved_depth;
   VK_CHECK(create_depth_image(device, physical_device, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               format, resolved_aspects, resolved_depth));

   VkBufferCreateInfo const readback_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = kWidth * kHeight * (sizeof(float) + 1),
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer readback_buffer;
   VK_CHECK(vkCreateBuffer(device, &readback_info, nullptr, &readback_buffer));
   VkMemoryRequirements readback_requirements;
   vkGetBufferMemoryRequirements(device, readback_buffer, &readback_requirements);
   uint32_t const readback_memory_type =
      find_memory_type(physical_device, readback_requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (readback_memory_type == UINT32_MAX) {
      std::fprintf(stderr, "No host-visible coherent memory for the readback buffer\n");
      return 1;
   }
   VkMemoryAllocateInfo const readback_allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = readback_requirements.size,
      .memoryTypeIndex = readback_memory_type,
   };
   VkDeviceMemory readback_memory;
   VK_CHECK(vkAllocateMemory(device, &readback_allocation, nullptr, &readback_memory));
   VK_CHECK(vkBindBufferMemory(device, readback_buffer, readback_memory, 0));
   float * readback_mapping;
   VK_CHECK(vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0,
                        reinterpret_cast<void **>(&readback_mapping)));
   for (uint32_t i = 0; i < kWidth * kHeight; ++i)
      readback_mapping[i] = -1.0F;

   /* Attachment 0 is the multisample source, attachment 1 the resolve destination. The
    * destination is loaded rather than cleared so its distinct starting value survives if the
    * resolve fails to write.
    */
   VkAttachmentDescription2 const attachments[] = {
      {
         .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
         .format = format,
         .samples = VK_SAMPLE_COUNT_2_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
      },
      {
         .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
         .format = format,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
         .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      },
   };
   VkAttachmentReference2 const depth_reference = {
      .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
      .aspectMask = resolved_aspects,
   };
   VkAttachmentReference2 const resolve_reference = {
      .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
      .attachment = 1,
      .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
      .aspectMask = resolved_aspects,
   };
   VkSubpassDescriptionDepthStencilResolve const depth_stencil_resolve = {
      .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE,
      .depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT,
      .stencilResolveMode =
         with_stencil ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE,
      .pDepthStencilResolveAttachment = &resolve_reference,
   };
   VkSubpassDescription2 const subpass = {
      .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,
      .pNext = &depth_stencil_resolve,
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .pDepthStencilAttachment = &depth_reference,
   };
   VkRenderPassCreateInfo2 const render_pass_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,
      .attachmentCount = 2,
      .pAttachments = attachments,
      .subpassCount = 1,
      .pSubpasses = &subpass,
   };
   /* vkCreateRenderPass2 is core in Vulkan 1.2, so at the 1.1 this test requests it is only
    * reachable through the VK_KHR_create_renderpass2 entry point.
    */
   auto const create_render_pass_2 = reinterpret_cast<PFN_vkCreateRenderPass2KHR>(
      vkGetDeviceProcAddr(device, "vkCreateRenderPass2KHR"));
   if (create_render_pass_2 == nullptr) {
      std::fprintf(stderr, "vkCreateRenderPass2KHR is unavailable\n");
      return 1;
   }
   VkRenderPass render_pass;
   VK_CHECK(create_render_pass_2(device, &render_pass_info, nullptr, &render_pass));

   VkImageView const framebuffer_attachments[] = {multisample_depth.view, resolved_depth.view};
   VkFramebufferCreateInfo const framebuffer_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = render_pass,
      .attachmentCount = 2,
      .pAttachments = framebuffer_attachments,
      .width = kWidth,
      .height = kHeight,
      .layers = 1,
   };
   VkFramebuffer framebuffer;
   VK_CHECK(vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer));

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

   /* Put a distinct value into the destination first, so a resolve that writes nothing is
    * distinguishable from one that writes the expected value.
    */
   VkImageMemoryBarrier const destination_to_clear = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = resolved_depth.image,
      .subresourceRange = {resolved_aspects, 0, 1, 0, 1},
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                        &destination_to_clear);
   VkClearDepthStencilValue const destination_initial = {kDestinationInitialDepth,
                                                        kDestinationInitialStencil};
   VkImageSubresourceRange const destination_range = {resolved_aspects, 0, 1, 0, 1};
   vkCmdClearDepthStencilImage(command_buffer, resolved_depth.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &destination_initial, 1,
                               &destination_range);
   VkImageMemoryBarrier const destination_to_attachment = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = resolved_depth.image,
      .subresourceRange = {resolved_aspects, 0, 1, 0, 1},
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                        &destination_to_attachment);

   VkClearValue const clear = {.depthStencil = {kClearDepth, kClearStencil}};
   VkRenderPassBeginInfo const render_begin = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = render_pass,
      .framebuffer = framebuffer,
      .renderArea = {{0, 0}, {kWidth, kHeight}},
      .clearValueCount = 1,
      .pClearValues = &clear,
   };
   vkCmdBeginRenderPass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdEndRenderPass(command_buffer);

   VkImageMemoryBarrier const resolved_ready = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = resolved_depth.image,
      .subresourceRange = {resolved_aspects, 0, 1, 0, 1},
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                        &resolved_ready);
   VkBufferImageCopy const readback_region = {
      .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
      .imageExtent = {kWidth, kHeight, 1},
   };
   vkCmdCopyImageToBuffer(command_buffer, resolved_depth.image,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buffer, 1,
                          &readback_region);
   if (with_stencil) {
      /* The stencil aspect lands after the depth values in the same buffer, one byte per texel. */
      VkBufferImageCopy const stencil_region = {
         .bufferOffset = kWidth * kHeight * sizeof(float),
         .imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1},
         .imageExtent = {kWidth, kHeight, 1},
      };
      vkCmdCopyImageToBuffer(command_buffer, resolved_depth.image,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buffer, 1,
                             &stencil_region);
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
   VkResult const wait_result = vkWaitForFences(device, 1, &fence, VK_TRUE, 3000000000ull);
   if (wait_result != VK_SUCCESS) {
      std::fprintf(stderr, "Fence wait failed with VkResult %d\n", wait_result);
      return 1;
   }

   uint32_t mismatches = 0;
   uint32_t untouched = 0;
   for (uint32_t y = 0; y < kHeight; ++y) {
      for (uint32_t x = 0; x < kWidth; ++x) {
         float const actual = readback_mapping[y * kWidth + x];
         if (std::fabs(actual - kDestinationInitialDepth) <= 1.0e-6F)
            ++untouched;
         if (!(std::fabs(actual - kClearDepth) <= 1.0e-6F)) {
            if (mismatches < 8) {
               std::fprintf(stderr, "resolved(%u,%u) = %.7f, expected %.7f FAIL\n", x, y, actual,
                            kClearDepth);
            }
            ++mismatches;
         }
      }
   }
   if (untouched != 0) {
      std::fprintf(stderr, "%u texels still hold the destination's initial value\n", untouched);
   }
   if (with_stencil) {
      uint8_t const * const stencil_mapping =
         reinterpret_cast<uint8_t const *>(readback_mapping) + kWidth * kHeight * sizeof(float);
      for (uint32_t y = 0; y < kHeight; ++y) {
         for (uint32_t x = 0; x < kWidth; ++x) {
            uint32_t const actual = stencil_mapping[y * kWidth + x];
            if (actual != kClearStencil) {
               if (mismatches < 8) {
                  std::fprintf(stderr, "resolved stencil(%u,%u) = 0x%02X, expected 0x%02X FAIL\n",
                               x, y, actual, kClearStencil);
               }
               ++mismatches;
            }
         }
      }
   }
   std::printf("depth_resolve aspects=%s texels=%u mismatches=%u %s\n",
               with_stencil ? "depth+stencil" : "depth", kWidth * kHeight, mismatches,
               mismatches == 0 ? "PASS" : "FAIL");

   vkDeviceWaitIdle(device);
   vkDestroyFence(device, fence, nullptr);
   vkDestroyCommandPool(device, command_pool, nullptr);
   vkDestroyFramebuffer(device, framebuffer, nullptr);
   vkDestroyRenderPass(device, render_pass, nullptr);
   for (Image const & image : {multisample_depth, resolved_depth}) {
      vkDestroyImageView(device, image.view, nullptr);
      vkDestroyImage(device, image.image, nullptr);
      vkFreeMemory(device, image.memory, nullptr);
   }
   vkUnmapMemory(device, readback_memory);
   vkDestroyBuffer(device, readback_buffer, nullptr);
   vkFreeMemory(device, readback_memory, nullptr);
   vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr);
   return mismatches == 0 ? 0 : 1;
}
