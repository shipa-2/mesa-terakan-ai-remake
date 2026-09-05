/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Regression coverage for the "Cover remaining copy, blit and resolve format/subresource
 * combinations" P0 item in TODO.md, specifically the vkCmdCopyImage slice of its acceptance
 * criteria: non-zero source/destination offsets, a partial (sub-full-extent) copy region, a copy
 * between two different mip levels, and a copy spanning multiple array layers. Every image used
 * here is single-sample: multisampled vkCmdCopyImage is a known, separately-tracked gap (see the
 * TODO(Triang3l) comment in terakan_meta_copy_image.c immediately above the meta-draw copy path,
 * which has no VK_SAMPLE_COUNT_1_BIT guard and silently falls into the single-sample-shaped
 * shader), not something this test attempts to exercise or fix.
 *
 * The source image is filled, level by level and layer by layer, with a value that encodes the
 * mip level, array layer, and in-level (x, y) position, so a readback can tell not just whether a
 * copy landed the right bytes but whether it landed them in the right place: a swapped offset or
 * an off-by-one extent shows up as a mismatch against a specific encoded neighbor, not just
 * "wrong number". The destination image starts filled with a sentinel value everywhere so a copy
 * that touches too much (or too little) of the destination is also visible.
 *
 * All cases pass as of this writing -- this closes what this item's acceptance criteria names
 * explicitly for image-to-image copies, not a bug found and fixed. The Vulkan CTS binary is not
 * installed on the test machine this driver is developed against (see
 * docs/terakan/FUNCTIONAL_COVERAGE.md), so the item as a whole remains open: this covers copy
 * subresource combinations, not the full copy/blit/resolve format matrix CTS would exercise.
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

#define VK_CHECK_VOID(expression)                                                                  \
   do {                                                                                            \
      VkResult const check_result = (expression);                                                  \
      if (check_result != VK_SUCCESS) {                                                            \
         std::fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_result);          \
         return;                                                                                   \
      }                                                                                            \
   } while (false)

namespace {

constexpr uint32_t kBaseWidth = 16;
constexpr uint32_t kBaseHeight = 16;
constexpr uint32_t kMipLevels = 3; /* 16x16, 8x8, 4x4 */
constexpr uint32_t kArrayLayers = 2;
constexpr uint32_t kSentinel = 0xDEADBEEFu;

uint32_t
mip_extent(uint32_t base, uint32_t level)
{
   uint32_t extent = base >> level;
   return extent < 1 ? 1 : extent;
}

/* Encodes a texel's expected identity so a readback mismatch names exactly what landed where
 * instead of just "wrong".
 */
uint32_t
encode_texel(uint32_t mip, uint32_t layer, uint32_t x, uint32_t y)
{
   return (mip << 28) | (layer << 24) | (y << 12) | x;
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

struct Buffer {
   VkBuffer buffer = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   void * mapping = nullptr;
};

VkResult
create_host_buffer(VkDevice device, VkPhysicalDevice physical_device, VkDeviceSize size,
                   VkBufferUsageFlags usage, Buffer & out)
{
   VkBufferCreateInfo const info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkResult result = vkCreateBuffer(device, &info, nullptr, &out.buffer);
   if (result != VK_SUCCESS)
      return result;
   VkMemoryRequirements requirements;
   vkGetBufferMemoryRequirements(device, out.buffer, &requirements);
   uint32_t const memory_type =
      find_memory_type(physical_device, requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
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
   result = vkBindBufferMemory(device, out.buffer, out.memory, 0);
   if (result != VK_SUCCESS)
      return result;
   return vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapping);
}

struct Image {
   VkImage image = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;
};

VkResult
create_image(VkDevice device, VkPhysicalDevice physical_device, VkFormat format,
            uint32_t mip_levels, uint32_t array_layers, VkImageUsageFlags usage, Image & out)
{
   VkImageCreateInfo const info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {kBaseWidth, kBaseHeight, 1},
      .mipLevels = mip_levels,
      .arrayLayers = array_layers,
      .samples = VK_SAMPLE_COUNT_1_BIT,
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
      .pApplicationName = "terakan-copy-image-subresource-test",
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
      return TERAKAN_TEST_DEVICE_NOT_FOUND_STATUS;
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

   Image src_image, dst_image;
   VK_CHECK(create_image(device, physical_device, VK_FORMAT_R32_UINT, kMipLevels, kArrayLayers,
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         src_image));
   VK_CHECK(create_image(device, physical_device, VK_FORMAT_R32_UINT, kMipLevels, kArrayLayers,
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         dst_image));

   /* One upload buffer big enough for the largest (base) level of one layer, reused per
    * level/layer upload; one sentinel-filled buffer covering an entire dst level/layer, reused to
    * initialize every dst subresource; one readback buffer sized the same way.
    */
   VkDeviceSize const max_level_bytes = kBaseWidth * kBaseHeight * 4;
   Buffer upload, sentinel_fill, readback;
   VK_CHECK(create_host_buffer(device, physical_device, max_level_bytes,
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT, upload));
   VK_CHECK(create_host_buffer(device, physical_device, max_level_bytes,
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT, sentinel_fill));
   VK_CHECK(create_host_buffer(device, physical_device, max_level_bytes,
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback));
   {
      uint32_t * const words = static_cast<uint32_t *>(sentinel_fill.mapping);
      for (uint32_t i = 0; i < kBaseWidth * kBaseHeight; ++i)
         words[i] = kSentinel;
   }

   VK_CHECK(run_command([&](VkCommandBuffer cmd) {
      VkImageMemoryBarrier init_barriers[2] = {};
      init_barriers[0] = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = src_image.image,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kMipLevels, 0, kArrayLayers},
      };
      init_barriers[1] = init_barriers[0];
      init_barriers[1].image = dst_image.image;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           0, nullptr, 0, nullptr, 2, init_barriers);

      /* Fill the destination image, every level and layer, with the sentinel so an
       * over-eager or misplaced copy is visible against it.
       */
      for (uint32_t mip = 0; mip < kMipLevels; ++mip) {
         for (uint32_t layer = 0; layer < kArrayLayers; ++layer) {
            VkBufferImageCopy const region = {
               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1},
               .imageExtent = {mip_extent(kBaseWidth, mip), mip_extent(kBaseHeight, mip), 1},
            };
            vkCmdCopyBufferToImage(cmd, sentinel_fill.buffer, dst_image.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
         }
      }
   }));

   /* Fill the source image, one level/layer at a time (each with its own upload payload, since
    * the encoded value depends on mip/layer/position), then transition both images to
    * TRANSFER_SRC so the copies below can read either.
    */
   for (uint32_t mip = 0; mip < kMipLevels; ++mip) {
      uint32_t const width = mip_extent(kBaseWidth, mip);
      uint32_t const height = mip_extent(kBaseHeight, mip);
      for (uint32_t layer = 0; layer < kArrayLayers; ++layer) {
         uint32_t * const words = static_cast<uint32_t *>(upload.mapping);
         for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x)
               words[y * width + x] = encode_texel(mip, layer, x, y);
         }
         VK_CHECK(run_command([&](VkCommandBuffer cmd) {
            VkBufferImageCopy const region = {
               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1},
               .imageExtent = {width, height, 1},
            };
            vkCmdCopyBufferToImage(cmd, upload.buffer, src_image.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
         }));
      }
   }

   VK_CHECK(run_command([&](VkCommandBuffer cmd) {
      VkImageMemoryBarrier to_src[2] = {};
      to_src[0] = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = src_image.image,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kMipLevels, 0, kArrayLayers},
      };
      to_src[1] = to_src[0];
      to_src[1].image = dst_image.image;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                           nullptr, 0, nullptr, 2, to_src);
   }));

   struct CopyCase {
      char const * name;
      VkImageCopy region;
      /* Where, within dst level/layer, to check: expected texels come from the corresponding
       * src offset; expected sentinel is everywhere else in the touched dst subresource.
       */
      uint32_t dst_mip;
      uint32_t dst_layer;
   };

   int32_t const src_off_x = 3, src_off_y = 2;
   std::vector<CopyCase> cases = {
      /* Non-zero offsets, partial extent, across array layers (layer 0 -> layer 1), same mip. */
      {"offset_partial_cross_layer",
       VkImageCopy{
          .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
          .srcOffset = {src_off_x, src_off_y, 0},
          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1},
          .dstOffset = {7, 1, 0},
          .extent = {6, 5, 1},
       },
       0, 1},
      /* Across mip levels: 4x4 region from mip 0 into mip 1 (8x8), same layer. */
      {"cross_mip_level",
       VkImageCopy{
          .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
          .srcOffset = {2, 2, 0},
          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1},
          .dstOffset = {1, 1, 0},
          .extent = {4, 4, 1},
       },
       1, 0},
      /* Multiple array layers in one region (layerCount = 2), full mip-2 (4x4) extent, layer 0. */
      {"multi_layer_region",
       VkImageCopy{
          .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 0, kArrayLayers},
          .srcOffset = {0, 0, 0},
          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 0, kArrayLayers},
          .dstOffset = {0, 0, 0},
          .extent = {4, 4, 1},
       },
       2, 0 /* checked separately below for both layers */},
   };

   std::vector<VkImageCopy> regions;
   for (auto const & c : cases)
      regions.push_back(c.region);

   VK_CHECK(run_command([&](VkCommandBuffer cmd) {
      vkCmdCopyImage(cmd, src_image.image, VK_IMAGE_LAYOUT_GENERAL, dst_image.image,
                     VK_IMAGE_LAYOUT_GENERAL, static_cast<uint32_t>(regions.size()),
                     regions.data());
   }));

   uint32_t total_mismatches = 0;

   auto check_subresource = [&](char const * case_name, uint32_t mip, uint32_t layer,
                                int32_t copy_dst_x, int32_t copy_dst_y, uint32_t copy_w,
                                uint32_t copy_h, int32_t copy_src_x, int32_t copy_src_y,
                                uint32_t copy_src_mip, uint32_t copy_src_layer) {
      uint32_t const width = mip_extent(kBaseWidth, mip);
      uint32_t const height = mip_extent(kBaseHeight, mip);
      VK_CHECK_VOID(run_command([&](VkCommandBuffer cmd) {
         VkBufferImageCopy const region = {
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1},
            .imageExtent = {width, height, 1},
         };
         vkCmdCopyImageToBuffer(cmd, dst_image.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1,
                                &region);
      }));
      uint32_t const * const words = static_cast<uint32_t const *>(readback.mapping);
      uint32_t mismatches = 0;
      for (uint32_t y = 0; y < height; ++y) {
         for (uint32_t x = 0; x < width; ++x) {
            bool const in_copied_region = static_cast<int32_t>(x) >= copy_dst_x &&
                                          static_cast<int32_t>(x) < copy_dst_x + static_cast<int32_t>(copy_w) &&
                                          static_cast<int32_t>(y) >= copy_dst_y &&
                                          static_cast<int32_t>(y) < copy_dst_y + static_cast<int32_t>(copy_h);
            uint32_t expected;
            if (in_copied_region) {
               uint32_t const src_x = static_cast<uint32_t>(copy_src_x + (static_cast<int32_t>(x) - copy_dst_x));
               uint32_t const src_y = static_cast<uint32_t>(copy_src_y + (static_cast<int32_t>(y) - copy_dst_y));
               expected = encode_texel(copy_src_mip, copy_src_layer, src_x, src_y);
            } else {
               expected = kSentinel;
            }
            uint32_t const actual = words[y * width + x];
            if (actual != expected) {
               if (mismatches < 8) {
                  std::fprintf(stderr,
                               "%s dst(mip=%u,layer=%u,%u,%u) = 0x%08x, expected 0x%08x FAIL\n",
                               case_name, mip, layer, x, y, actual, expected);
               }
               ++mismatches;
            }
         }
      }
      std::printf("%s (mip=%u,layer=%u) texels=%u mismatches=%u %s\n", case_name, mip, layer,
                  width * height, mismatches, mismatches == 0 ? "PASS" : "FAIL");
      total_mismatches += mismatches;
   };

   check_subresource("offset_partial_cross_layer", 0, 1, 7, 1, 6, 5, src_off_x, src_off_y, 0, 0);
   check_subresource("cross_mip_level", 1, 0, 1, 1, 4, 4, 2, 2, 0, 0);
   check_subresource("multi_layer_region_layer0", 2, 0, 0, 0, 4, 4, 0, 0, 2, 0);
   check_subresource("multi_layer_region_layer1", 2, 1, 0, 0, 4, 4, 0, 0, 2, 1);

   return total_mismatches == 0 ? 0 : 1;
}
