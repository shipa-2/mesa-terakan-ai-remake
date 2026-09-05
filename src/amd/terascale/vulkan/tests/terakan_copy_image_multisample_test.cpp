/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* A whole-surface multisample-to-multisample vkCmdCopyImage.
 *
 * This used to be a silent no-op. The meta-draw copy path binds the source through a plain
 * 2D_ARRAY resource descriptor regardless of sample count, and a #MemoryIntegrity check inside
 * descriptor creation rejects the mismatched dimensionality, so the copy loop skipped the region
 * and left the destination untouched. This test was written then to lock in that it was at least
 * safe rather than corrupting, and reported which of the two behaviours it saw.
 *
 * It now copies, through the CP DMA path rather than a meta draw: when two surfaces are laid out
 * identically and the whole of one is being copied to the other, the backing storage can be moved
 * verbatim, and terakan_image_surface_compute extends the surface size past the colour to cover
 * FMASK and CMASK, so the compression state travels with the samples it describes.
 *
 * The verdict is now that the copy must land, not merely that it must not corrupt. A destination
 * left holding the sentinel fails here rather than being reported as an acceptable gap.
 *
 * What this does not cover is a multisample copy that is not the whole of two identical surfaces
 * -- a subregion, a single array layer, or differing layouts. Those still take the meta-draw path
 * and are still skipped; they are the remaining 24 failures in
 * dEQP-VK.api.copy_and_blit.core.resolve_image and the subject of the TODO in
 * terakan_meta_copy_image.c.
 */

#include <vulkan/vulkan.h>

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
constexpr VkSampleCountFlagBits kSamples = VK_SAMPLE_COUNT_4_BIT;

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
};

VkResult
create_image(VkDevice device, VkPhysicalDevice physical_device, VkSampleCountFlagBits samples,
            VkImageUsageFlags usage, Image & out)
{
   VkImageCreateInfo const info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
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
   return vkBindImageMemory(device, out.image, out.memory, 0);
}

} // namespace

int
main()
{
   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-copy-image-multisample-noop-test",
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
      /* Which generation this run is for is `terakan_test_device_matches`'s decision, not each
       * test's; see terakan_test_device.h.
       */
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
   std::fprintf(stderr, "device=%s queue_family=%u\n", properties.deviceName, queue_family);

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

   VkCommandPoolCreateInfo const command_pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = queue_family,
   };
   VkCommandPool command_pool;
   VK_CHECK(vkCreateCommandPool(device, &command_pool_info, nullptr, &command_pool));

   auto run_command = [&](auto && record) -> VkResult {
      VkCommandBufferAllocateInfo const allocate_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = command_pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      };
      VkCommandBuffer cmd;
      VkResult result = vkAllocateCommandBuffers(device, &allocate_info, &cmd);
      if (result != VK_SUCCESS)
         return result;
      VkCommandBufferBeginInfo const begin_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      };
      result = vkBeginCommandBuffer(cmd, &begin_info);
      if (result != VK_SUCCESS)
         return result;
      record(cmd);
      result = vkEndCommandBuffer(cmd);
      if (result != VK_SUCCESS)
         return result;
      VkSubmitInfo const submit_info = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &cmd,
      };
      result = vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE);
      if (result != VK_SUCCESS)
         return result;
      result = vkQueueWaitIdle(queue);
      vkFreeCommandBuffers(device, command_pool, 1, &cmd);
      return result;
   };

   Image src_image, dst_image, resolved_image;
   VK_CHECK(create_image(device, physical_device, kSamples,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         src_image));
   VK_CHECK(create_image(device, physical_device, kSamples,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         dst_image));
   VK_CHECK(create_image(device, physical_device, VK_SAMPLE_COUNT_1_BIT,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         resolved_image));

   VkDeviceSize const readback_bytes = kWidth * kHeight * 4;
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
   VkMemoryAllocateInfo const readback_allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = readback_requirements.size,
      .memoryTypeIndex = readback_memory_type,
   };
   VkDeviceMemory readback_memory;
   VK_CHECK(vkAllocateMemory(device, &readback_allocation, nullptr, &readback_memory));
   VK_CHECK(vkBindBufferMemory(device, readback_buffer, readback_memory, 0));
   uint8_t * readback_mapping;
   VK_CHECK(vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0,
                        reinterpret_cast<void **>(&readback_mapping)));
   std::memset(readback_mapping, 0xA5, readback_bytes);

   VK_CHECK(run_command([&](VkCommandBuffer cmd) {
      VkImageMemoryBarrier init_barriers[3] = {};
      VkImage const images[3] = {src_image.image, dst_image.image, resolved_image.image};
      for (uint32_t i = 0; i < 3; ++i) {
         init_barriers[i] = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[i],
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
         };
      }
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           0, nullptr, 0, nullptr, 3, init_barriers);

      /* The source gets a bright, unmistakable colour; the destination gets a different sentinel
       * colour. If the copy silently does nothing (the finding this test locks in), the
       * destination's resolved readback stays the sentinel everywhere. If a future change makes
       * the copy actually work, it should read back as the source colour instead -- also a pass,
       * see the check below. Only a third, garbage colour is a failure.
       */
      VkClearColorValue const src_clear = {.float32 = {1.0F, 0.0F, 0.0F, 1.0F}};
      VkClearColorValue const dst_sentinel = {.float32 = {0.0F, 1.0F, 0.0F, 1.0F}};
      VkImageSubresourceRange const range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdClearColorImage(cmd, src_image.image, VK_IMAGE_LAYOUT_GENERAL, &src_clear, 1, &range);
      vkCmdClearColorImage(cmd, dst_image.image, VK_IMAGE_LAYOUT_GENERAL, &dst_sentinel, 1, &range);

      VkMemoryBarrier const cleared = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
      };
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                           &cleared, 0, nullptr, 0, nullptr);

      VkImageCopy const region = {
         .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .srcOffset = {0, 0, 0},
         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .dstOffset = {0, 0, 0},
         .extent = {kWidth, kHeight, 1},
      };
      vkCmdCopyImage(cmd, src_image.image, VK_IMAGE_LAYOUT_GENERAL, dst_image.image,
                     VK_IMAGE_LAYOUT_GENERAL, 1, &region);

      VkMemoryBarrier const copy_done = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      };
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 1, &copy_done, 0,
                           nullptr, 0, nullptr);

      /* Resolve (average) the destination down to a single-sample image to read it back: every
       * sample holds the same value either way (clear or unmodified copy target), so averaging
       * does not itself introduce ambiguity.
       */
      VkImageResolve const resolve_region = {
         .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .srcOffset = {0, 0, 0},
         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .dstOffset = {0, 0, 0},
         .extent = {kWidth, kHeight, 1},
      };
      vkCmdResolveImage(cmd, dst_image.image, VK_IMAGE_LAYOUT_GENERAL, resolved_image.image,
                        VK_IMAGE_LAYOUT_GENERAL, 1, &resolve_region);

      VkImageMemoryBarrier const resolved_to_transfer = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = resolved_image.image,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &resolved_to_transfer);

      VkBufferImageCopy const readback_region = {
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageExtent = {kWidth, kHeight, 1},
      };
      vkCmdCopyImageToBuffer(cmd, resolved_image.image, VK_IMAGE_LAYOUT_GENERAL, readback_buffer, 1,
                             &readback_region);

      VkMemoryBarrier const host_ready = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      };
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                           &host_ready, 0, nullptr, 0, nullptr);
   }));

   uint32_t untouched = 0, copied = 0, corrupted = 0;
   for (uint32_t texel = 0; texel < kWidth * kHeight; ++texel) {
      uint8_t const * const pixel = &readback_mapping[texel * 4];
      bool const is_sentinel = pixel[0] < 32 && pixel[1] > 224 && pixel[2] < 32;
      bool const is_copied = pixel[0] > 224 && pixel[1] < 32 && pixel[2] < 32;
      if (is_sentinel)
         ++untouched;
      else if (is_copied)
         ++copied;
      else
         ++corrupted;
   }
   std::printf(
      "copy_image_multisample texels=%u untouched=%u copied=%u corrupted=%u %s\n",
      kWidth * kHeight, untouched, copied, corrupted,
      (corrupted == 0 && copied == kWidth * kHeight) ? "PASS" : "FAIL");
   if (corrupted == 0 && copied != kWidth * kHeight) {
      std::printf("  %s\n", untouched == kWidth * kHeight
                                ? "the destination still holds the sentinel: the copy did nothing"
                                : "some texels copied and some did not");
   }

   vkDeviceWaitIdle(device);
   vkDestroyCommandPool(device, command_pool, nullptr);
   vkUnmapMemory(device, readback_memory);
   vkDestroyBuffer(device, readback_buffer, nullptr);
   vkFreeMemory(device, readback_memory, nullptr);
   for (Image const & image : {src_image, dst_image, resolved_image}) {
      vkDestroyImage(device, image.image, nullptr);
      vkFreeMemory(device, image.memory, nullptr);
   }
   vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr);
   return (corrupted == 0 && copied == kWidth * kHeight) ? 0 : 1;
}
