/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Depth resolve into a subresource that is not the whole image.
 *
 * terakan_depth_resolve and terakan_depth_stencil_resolve both resolve a whole single-level,
 * single-layer image, which leaves the addressing untested: the destination is reached through a
 * view, so its mip level and array layer have to reach the depth/stencil descriptor, and the
 * resolve covers the render area rather than the attachment, so its offset has to reach the draw.
 *
 * The destination is cleared to a sentinel first and the render area is a rectangle in the middle
 * of it, so a resolve that lands on the wrong level, the wrong layer or the wrong offset shows up
 * either as the sentinel surviving inside the area or as the resolved value appearing outside it.
 *
 * This is written against dynamic rendering, which describes the resolve destination by view and
 * needs no render pass or framebuffer object.
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

/* The destination's level 1 is what the resolve targets, so it is the level that has to match the
 * multisample source's size.
 */
constexpr uint32_t kDestinationWidth = 16;
constexpr uint32_t kDestinationHeight = 16;
constexpr uint32_t kResolveLevel = 1;
constexpr uint32_t kResolveLayer = 1;
constexpr uint32_t kWidth = kDestinationWidth >> kResolveLevel;
constexpr uint32_t kHeight = kDestinationHeight >> kResolveLevel;
constexpr VkFormat kFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkSampleCountFlagBits kSamples = VK_SAMPLE_COUNT_2_BIT;

constexpr VkRect2D kRenderArea = {{2, 2}, {4, 4}};

/* Both exactly representable, and far enough apart that no filtering could confuse them. */
constexpr float kSentinelDepth = 0.25F;
constexpr float kResolvedDepth = 0.75F;

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
create_depth_image(VkDevice device, VkPhysicalDevice physical_device, VkExtent2D extent,
                   uint32_t level_count, uint32_t layer_count, VkSampleCountFlagBits samples,
                   VkImageUsageFlags usage, uint32_t view_level, uint32_t view_layer, Image & out)
{
   VkImageCreateInfo const info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = kFormat,
      .extent = {extent.width, extent.height, 1},
      .mipLevels = level_count,
      .arrayLayers = layer_count,
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
      .format = kFormat,
      .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, view_level, 1, view_layer, 1},
   };
   return vkCreateImageView(device, &view_info, nullptr, &out.view);
}

} // namespace

int
main()
{
   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-depth-resolve-subresource-test",
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
      return TERAKAN_TEST_DEVICE_NOT_FOUND_STATUS;
   }
   std::fprintf(stderr, "device=%s queue_family=%u resolve=%ux%u level %u layer %u\n",
                properties.deviceName, queue_family, kWidth, kHeight, kResolveLevel, kResolveLayer);

   float const priority = 1.0F;
   VkDeviceQueueCreateInfo const queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = queue_family,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamic_rendering_feature = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR,
      .dynamicRendering = VK_TRUE,
   };
   char const * const device_extension = VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME;
   VkDeviceCreateInfo const device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &dynamic_rendering_feature,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = &device_extension,
   };
   VkDevice device;
   VK_CHECK(vkCreateDevice(physical_device, &device_info, nullptr, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, queue_family, 0, &queue);

   auto const cmd_begin_rendering = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
      vkGetDeviceProcAddr(device, "vkCmdBeginRenderingKHR"));
   auto const cmd_end_rendering = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
      vkGetDeviceProcAddr(device, "vkCmdEndRenderingKHR"));
   if (cmd_begin_rendering == nullptr || cmd_end_rendering == nullptr) {
      std::fprintf(stderr, "The dynamic rendering entry points are missing\n");
      return 1;
   }

   Image multisample;
   VK_CHECK(create_depth_image(device, physical_device, {kWidth, kHeight}, 1, 1, kSamples,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT,
                               0, 0, multisample));
   Image destination;
   VK_CHECK(create_depth_image(device, physical_device, {kDestinationWidth, kDestinationHeight}, 2,
                               2, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               kResolveLevel, kResolveLayer, destination));

   VkDeviceSize const readback_bytes = kWidth * kHeight * sizeof(float);
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

   /* Every level and layer of the destination gets the sentinel, so a resolve landing on the wrong
    * one is still visible as the sentinel surviving where the resolved value was expected.
    */
   VkClearDepthStencilValue const sentinel = {.depth = kSentinelDepth};
   VkImageSubresourceRange const whole_destination = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 2, 0, 2};
   vkCmdClearDepthStencilImage(command_buffer, destination.image, VK_IMAGE_LAYOUT_GENERAL,
                               &sentinel, 1, &whole_destination);

   VkImageMemoryBarrier const cleared = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = destination.image,
      .subresourceRange = whole_destination,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                        &cleared);

   /* The multisample attachment is only cleared: with sample-zero resolve every sample holds the
    * same value, so the clear alone determines what a correct resolve must produce.
    */
   VkRenderingAttachmentInfoKHR depth_attachment = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
      .imageView = multisample.view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT,
      .resolveImageView = destination.view,
      .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
   };
   depth_attachment.clearValue.depthStencil.depth = kResolvedDepth;
   VkRenderingInfoKHR const rendering = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR,
      .renderArea = kRenderArea,
      .layerCount = 1,
      .pDepthAttachment = &depth_attachment,
   };
   cmd_begin_rendering(command_buffer, &rendering);
   cmd_end_rendering(command_buffer);

   VkImageMemoryBarrier const resolved = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = destination.image,
      .subresourceRange = whole_destination,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &resolved);

   VkBufferImageCopy const readback_region = {
      .bufferOffset = 0,
      .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, kResolveLevel, kResolveLayer, 1},
      .imageExtent = {kWidth, kHeight, 1},
   };
   vkCmdCopyImageToBuffer(command_buffer, destination.image, VK_IMAGE_LAYOUT_GENERAL,
                          readback_buffer, 1, &readback_region);

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

   uint32_t inside_wrong = 0;
   uint32_t outside_wrong = 0;
   for (uint32_t y = 0; y < kHeight; ++y) {
      for (uint32_t x = 0; x < kWidth; ++x) {
         bool const inside = x >= static_cast<uint32_t>(kRenderArea.offset.x) &&
                             y >= static_cast<uint32_t>(kRenderArea.offset.y) &&
                             x < kRenderArea.offset.x + kRenderArea.extent.width &&
                             y < kRenderArea.offset.y + kRenderArea.extent.height;
         float const want = inside ? kResolvedDepth : kSentinelDepth;
         float const actual = readback_mapping[y * kWidth + x];
         if (std::fabs(actual - want) <= 1.0F / 4096.0F)
            continue;
         uint32_t & counter = inside ? inside_wrong : outside_wrong;
         if (counter == 0) {
            std::fprintf(stderr, "(%u,%u) %s the render area is %f, expected %f\n", x, y,
                         inside ? "inside" : "outside", actual, want);
         }
         ++counter;
      }
   }
   uint32_t const failures = inside_wrong + outside_wrong;
   if (failures != 0) {
      std::fprintf(stderr, "%u of %u inside wrong, %u of %u outside wrong\n", inside_wrong,
                   kRenderArea.extent.width * kRenderArea.extent.height, outside_wrong,
                   kWidth * kHeight - kRenderArea.extent.width * kRenderArea.extent.height);
   }
   std::printf("depth_resolve_subresource level=%u layer=%u bad=%u %s\n", kResolveLevel,
               kResolveLayer, failures, failures == 0 ? "PASS" : "FAIL");

   vkDeviceWaitIdle(device);
   vkDestroyFence(device, fence, nullptr);
   vkDestroyCommandPool(device, command_pool, nullptr);
   vkUnmapMemory(device, readback_memory);
   vkDestroyBuffer(device, readback_buffer, nullptr);
   vkFreeMemory(device, readback_memory, nullptr);
   for (Image const & image : {multisample, destination}) {
      vkDestroyImageView(device, image.view, nullptr);
      vkDestroyImage(device, image.image, nullptr);
      vkFreeMemory(device, image.memory, nullptr);
   }
   vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr);
   return failures == 0 ? 0 : 1;
}
