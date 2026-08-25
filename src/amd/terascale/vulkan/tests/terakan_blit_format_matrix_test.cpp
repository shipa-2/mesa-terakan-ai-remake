/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Regression coverage for the "meta blit format coverage" P0 item in TODO.md: RGBA<->BGRA (a
 * format-converting blit between two 4x8-bit formats with a different channel order in memory,
 * both straight and mirrored on X, exercising the acceptance criteria's "reversed source/
 * destination axes" alongside format conversion in the same blit) and R32 (a single
 * 32-bit-channel format, a distinct pixel shader export/texture fetch shape from the
 * 8-bit-per-channel packed formats every other blit test in this driver uses).
 *
 * All three cases pass as of this writing -- this closes a real, previously-untested slice of the
 * format matrix named in the acceptance criteria, not a bug found and fixed. The Vulkan CTS binary
 * is not installed on the test machine this driver is developed against (see
 * docs/terakan/FUNCTIONAL_COVERAGE.md), so the item as a whole remains open: this covers what its
 * acceptance criteria names explicitly, not the full format matrix CTS would exercise.
 */

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

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
            VkImageUsageFlags usage, Image & out)
{
   VkImageCreateInfo const info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {kWidth, kHeight, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
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
      .pApplicationName = "terakan-blit-format-matrix-test",
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
      if (!std::strstr(properties.deviceName, "Terakan"))
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

   uint32_t total_mismatches = 0;

   /* --- RGBA <-> BGRA format-converting blit --- */
   {
      Image src_image, dst_image;
      VK_CHECK(create_image(device, physical_device, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            src_image));
      VK_CHECK(create_image(device, physical_device, VK_FORMAT_B8G8R8A8_UNORM,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            dst_image));

      Buffer upload, readback;
      VkDeviceSize const pixel_bytes = kWidth * kHeight * 4;
      VK_CHECK(create_host_buffer(device, physical_device, pixel_bytes,
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT, upload));
      VK_CHECK(create_host_buffer(device, physical_device, pixel_bytes,
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback));

      /* Logical (R, G, B, A) per pixel, distinct per position. Stored in memory as R,G,B,A bytes
       * (matching VK_FORMAT_R8G8B8A8_UNORM's channel order).
       */
      uint8_t * const upload_bytes = static_cast<uint8_t *>(upload.mapping);
      for (uint32_t y = 0; y < kHeight; ++y) {
         for (uint32_t x = 0; x < kWidth; ++x) {
            uint8_t * const texel = &upload_bytes[(y * kWidth + x) * 4];
            texel[0] = static_cast<uint8_t>(16 + x * 24); /* R */
            texel[1] = static_cast<uint8_t>(16 + y * 24); /* G */
            texel[2] = 128;                               /* B */
            texel[3] = 255;                                /* A */
         }
      }

      VK_CHECK(run_command([&](VkCommandBuffer cmd) {
         VkImageMemoryBarrier barriers[2] = {};
         barriers[0] = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = src_image.image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
         };
         barriers[1] = barriers[0];
         barriers[1].image = dst_image.image;
         vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, nullptr, 0, nullptr, 2, barriers);

         VkBufferImageCopy const upload_region = {
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {kWidth, kHeight, 1},
         };
         vkCmdCopyBufferToImage(cmd, upload.buffer, src_image.image,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload_region);

         VkImageMemoryBarrier const to_src = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = src_image.image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
         };
         vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                              0, nullptr, 0, nullptr, 1, &to_src);

         /* Mirrored on X (reversed source/destination axis) as well as format-converting. */
         VkImageBlit const blit_region = {
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffsets = {{0, 0, 0}, {kWidth, kHeight, 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffsets = {{static_cast<int32_t>(kWidth), 0, 0}, {0, kHeight, 1}},
         };
         vkCmdBlitImage(cmd, src_image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_image.image,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit_region, VK_FILTER_NEAREST);

         VkImageMemoryBarrier const dst_to_src = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = dst_image.image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
         };
         vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                              0, nullptr, 0, nullptr, 1, &dst_to_src);

         VkBufferImageCopy const readback_region = {
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {kWidth, kHeight, 1},
         };
         vkCmdCopyImageToBuffer(cmd, dst_image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                readback.buffer, 1, &readback_region);
      }));

      uint8_t const * const readback_bytes = static_cast<uint8_t const *>(readback.mapping);
      uint32_t mismatches = 0;
      for (uint32_t y = 0; y < kHeight; ++y) {
         for (uint32_t x = 0; x < kWidth; ++x) {
            /* Destination X is mirrored: dest column x holds source column (kWidth - 1 - x). */
            uint32_t const src_x = kWidth - 1 - x;
            uint8_t const * const texel = &readback_bytes[(y * kWidth + x) * 4];
            /* B8G8R8A8_UNORM memory order is B, G, R, A. */
            uint8_t const b = texel[0], g = texel[1], r = texel[2], a = texel[3];
            uint8_t const expected_r = static_cast<uint8_t>(16 + src_x * 24);
            uint8_t const expected_g = static_cast<uint8_t>(16 + y * 24);
            uint8_t const expected_b = 128, expected_a = 255;
            if (r != expected_r || g != expected_g || b != expected_b || a != expected_a) {
               if (mismatches < 8) {
                  std::fprintf(stderr,
                               "rgba_to_bgra_mirrored(%u,%u) = (%u,%u,%u,%u), expected "
                               "(%u,%u,%u,%u) FAIL\n",
                               x, y, r, g, b, a, expected_r, expected_g, expected_b, expected_a);
               }
               ++mismatches;
            }
         }
      }
      std::printf("rgba_to_bgra_mirrored_blit texels=%u mismatches=%u %s\n", kWidth * kHeight,
                  mismatches, mismatches == 0 ? "PASS" : "FAIL");
      total_mismatches += mismatches;
   }

   /* --- R32_UINT identity-format blit --- */
   {
      Image src_image, dst_image;
      VK_CHECK(create_image(device, physical_device, VK_FORMAT_R32_UINT,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            src_image));
      VK_CHECK(create_image(device, physical_device, VK_FORMAT_R32_UINT,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            dst_image));

      Buffer upload, readback;
      VkDeviceSize const pixel_bytes = kWidth * kHeight * 4;
      VK_CHECK(create_host_buffer(device, physical_device, pixel_bytes,
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT, upload));
      VK_CHECK(create_host_buffer(device, physical_device, pixel_bytes,
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback));

      uint32_t * const upload_words = static_cast<uint32_t *>(upload.mapping);
      for (uint32_t y = 0; y < kHeight; ++y) {
         for (uint32_t x = 0; x < kWidth; ++x) {
            upload_words[y * kWidth + x] = 0x10000000u + y * kWidth + x;
         }
      }

      VK_CHECK(run_command([&](VkCommandBuffer cmd) {
         VkImageMemoryBarrier barriers[2] = {};
         barriers[0] = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = src_image.image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
         };
         barriers[1] = barriers[0];
         barriers[1].image = dst_image.image;
         vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, nullptr, 0, nullptr, 2, barriers);

         VkBufferImageCopy const upload_region = {
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {kWidth, kHeight, 1},
         };
         vkCmdCopyBufferToImage(cmd, upload.buffer, src_image.image,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload_region);

         VkImageMemoryBarrier const to_src = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = src_image.image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
         };
         vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                              0, nullptr, 0, nullptr, 1, &to_src);

         VkImageBlit const blit_region = {
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffsets = {{0, 0, 0}, {kWidth, kHeight, 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffsets = {{0, 0, 0}, {kWidth, kHeight, 1}},
         };
         vkCmdBlitImage(cmd, src_image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_image.image,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit_region, VK_FILTER_NEAREST);

         VkImageMemoryBarrier const dst_to_src = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = dst_image.image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
         };
         vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                              0, nullptr, 0, nullptr, 1, &dst_to_src);

         VkBufferImageCopy const readback_region = {
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {kWidth, kHeight, 1},
         };
         vkCmdCopyImageToBuffer(cmd, dst_image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                readback.buffer, 1, &readback_region);
      }));

      uint32_t const * const readback_words = static_cast<uint32_t const *>(readback.mapping);
      uint32_t mismatches = 0;
      for (uint32_t y = 0; y < kHeight; ++y) {
         for (uint32_t x = 0; x < kWidth; ++x) {
            uint32_t const actual = readback_words[y * kWidth + x];
            uint32_t const expected = 0x10000000u + y * kWidth + x;
            if (actual != expected) {
               if (mismatches < 8) {
                  std::fprintf(stderr, "r32_blit(%u,%u) = 0x%08x, expected 0x%08x FAIL\n", x, y,
                               actual, expected);
               }
               ++mismatches;
            }
         }
      }
      std::printf("r32_uint_blit texels=%u mismatches=%u %s\n", kWidth * kHeight, mismatches,
                  mismatches == 0 ? "PASS" : "FAIL");
      total_mismatches += mismatches;
   }

   return total_mismatches == 0 ? 0 : 1;
}
