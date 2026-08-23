/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* vkCmdBlitImage on 3D images, where the depth axis is scaled and signed like the other two.
 *
 * Two-dimensional blits already pass, including the mirrored ones. The depth axis was the part
 * left behind: the destination slice count was forced to equal the source slice count and each
 * destination slice sampled the source slice with the same index, so a region whose two depth
 * ranges differ in size produced the wrong slices, and one whose ranges differ in direction was
 * not mirrored at all.
 *
 * The source is a 3D image whose every slice holds a distinct colour, so the check is simply which
 * source slice each destination slice ended up holding. The blit keeps x and y one to one, which
 * leaves the depth mapping as the only thing under test.
 *
 * A layered 2D array blit runs last. It shares the defect's other half: the sampled blit drew
 * several destination slices in one draw while the pixel shader sampled a constant source array
 * layer, so every layer of a multi-layer array blit received the first source layer.
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
constexpr uint32_t kDepth = 8;
constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

/* Distinct per slice and exactly representable in UNORM8. */
uint8_t
slice_channel(uint32_t slice, uint32_t channel)
{
   return static_cast<uint8_t>(16u + slice * 20u + channel * 4u);
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

struct Image {
   VkImage image = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;
};

VkResult
create_3d_image(VkDevice device, VkPhysicalDevice physical_device, VkImageUsageFlags usage,
                Image & out)
{
   VkImageCreateInfo const info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_3D,
      .format = kFormat,
      .extent = {kWidth, kHeight, kDepth},
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

/* Each case names, per destination slice, the source slice Vulkan's depth scaling selects. */
struct Case {
   char const * name;
   int32_t src_z0, src_z1;
   int32_t dst_z0, dst_z1;
   uint32_t written_slices;
   uint32_t expected_source[kDepth];
};

constexpr Case kCases[] = {
   /* Eight source slices into four destination slices: each takes the centre of its pair. */
   {"depth minified", 0, 8, 0, 4, 4, {1, 3, 5, 7}},
   /* Two source slices into eight: each source slice covers four destination slices. */
   {"depth magnified", 0, 2, 0, 8, 8, {0, 0, 0, 0, 1, 1, 1, 1}},
   /* Equal sizes but a reversed destination range, which must mirror. */
   {"depth mirrored", 0, 4, 4, 0, 4, {3, 2, 1, 0}},
};
constexpr uint32_t kCaseCount = sizeof(kCases) / sizeof(kCases[0]);

constexpr uint32_t kArrayLayers = 4;

VkResult
create_array_image(VkDevice device, VkPhysicalDevice physical_device, VkImageUsageFlags usage,
                   Image & out)
{
   VkImageCreateInfo const info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = kFormat,
      .extent = {kWidth, kHeight, 1},
      .mipLevels = 1,
      .arrayLayers = kArrayLayers,
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
      .pApplicationName = "terakan-blit-3d-test",
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

   Image source;
   VK_CHECK(create_3d_image(device, physical_device,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT,
                            source));
   Image destination;
   VK_CHECK(create_3d_image(device, physical_device,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                            destination));

   Image array_source;
   VK_CHECK(create_array_image(device, physical_device,
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT,
                               array_source));
   Image array_destination;
   VK_CHECK(create_array_image(device, physical_device,
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                               array_destination));

   VkDeviceSize const slice_bytes = kWidth * kHeight * 4;
   VkDeviceSize const image_bytes = slice_bytes * kDepth;
   /* One upload copy of the source followed by one readback per case. */
   VkDeviceSize const array_readback_offset = image_bytes * (kCaseCount + 1);
   VkDeviceSize const buffer_bytes = array_readback_offset + slice_bytes * kArrayLayers;
   VkBufferCreateInfo const buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = buffer_bytes,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer buffer;
   VK_CHECK(vkCreateBuffer(device, &buffer_info, nullptr, &buffer));
   VkMemoryRequirements buffer_requirements;
   vkGetBufferMemoryRequirements(device, buffer, &buffer_requirements);
   uint32_t const buffer_memory_type =
      find_memory_type(physical_device, buffer_requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (buffer_memory_type == UINT32_MAX) {
      std::fprintf(stderr, "No host-visible coherent memory for the staging buffer\n");
      return 1;
   }
   VkMemoryAllocateInfo const buffer_allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = buffer_requirements.size,
      .memoryTypeIndex = buffer_memory_type,
   };
   VkDeviceMemory buffer_memory;
   VK_CHECK(vkAllocateMemory(device, &buffer_allocation, nullptr, &buffer_memory));
   VK_CHECK(vkBindBufferMemory(device, buffer, buffer_memory, 0));
   uint8_t * mapping;
   VK_CHECK(vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0,
                        reinterpret_cast<void **>(&mapping)));
   std::memset(mapping, 0xA5, buffer_bytes);
   for (uint32_t slice = 0; slice < kDepth; ++slice) {
      for (uint32_t texel = 0; texel < kWidth * kHeight; ++texel) {
         uint8_t * const pixel = mapping + slice_bytes * slice + texel * 4;
         pixel[0] = slice_channel(slice, 0);
         pixel[1] = slice_channel(slice, 1);
         pixel[2] = slice_channel(slice, 2);
         pixel[3] = 255;
      }
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

   VkImageSubresourceRange const whole_image = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   auto const full_barrier = [&](VkImage image, VkAccessFlags src_access,
                                 VkAccessFlags dst_access) {
      VkImageMemoryBarrier const barrier = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = src_access,
         .dstAccessMask = dst_access,
         .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = image,
         .subresourceRange = whole_image,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &barrier);
   };

   VkBufferImageCopy const upload_region = {
      .bufferOffset = 0,
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {kWidth, kHeight, kDepth},
   };
   vkCmdCopyBufferToImage(command_buffer, buffer, source.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                          &upload_region);
   full_barrier(source.image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

   for (uint32_t case_index = 0; case_index < kCaseCount; ++case_index) {
      Case const & test_case = kCases[case_index];

      /* Clear first, so a destination slice the blit fails to write cannot pass by holding what an
       * earlier case left there.
       */
      VkClearColorValue const sentinel = {.float32 = {1.0F, 0.0F, 1.0F, 1.0F}};
      vkCmdClearColorImage(command_buffer, destination.image, VK_IMAGE_LAYOUT_GENERAL, &sentinel, 1,
                           &whole_image);
      full_barrier(destination.image, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

      VkImageBlit const blit = {
         .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .srcOffsets = {{0, 0, test_case.src_z0}, {kWidth, kHeight, test_case.src_z1}},
         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .dstOffsets = {{0, 0, test_case.dst_z0}, {kWidth, kHeight, test_case.dst_z1}},
      };
      vkCmdBlitImage(command_buffer, source.image, VK_IMAGE_LAYOUT_GENERAL, destination.image,
                     VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_NEAREST);
      full_barrier(destination.image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT);

      VkBufferImageCopy const readback_region = {
         .bufferOffset = image_bytes * (case_index + 1),
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageExtent = {kWidth, kHeight, kDepth},
      };
      vkCmdCopyImageToBuffer(command_buffer, destination.image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1,
                             &readback_region);
      full_barrier(destination.image, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
   }

   /* The layered array blit. The x axis is minified so the region cannot be turned into a copy,
    * which is what puts it on the sampled path where the layer selection lives.
    */
   {
      VkBufferImageCopy const array_upload = {
         .bufferOffset = 0,
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, kArrayLayers},
         .imageExtent = {kWidth, kHeight, 1},
      };
      vkCmdCopyBufferToImage(command_buffer, buffer, array_source.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                             &array_upload);
      VkImageSubresourceRange const all_layers = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                                  kArrayLayers};
      VkImageMemoryBarrier const uploaded = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = array_source.image,
         .subresourceRange = all_layers,
      };
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &uploaded);

      VkClearColorValue const sentinel = {.float32 = {1.0F, 0.0F, 1.0F, 1.0F}};
      vkCmdClearColorImage(command_buffer, array_destination.image, VK_IMAGE_LAYOUT_GENERAL,
                           &sentinel, 1, &all_layers);
      VkImageMemoryBarrier cleared = uploaded;
      cleared.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      cleared.image = array_destination.image;
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &cleared);

      VkImageBlit const array_blit = {
         .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, kArrayLayers},
         .srcOffsets = {{0, 0, 0}, {kWidth, kHeight, 1}},
         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, kArrayLayers},
         .dstOffsets = {{0, 0, 0}, {kWidth / 2, kHeight, 1}},
      };
      vkCmdBlitImage(command_buffer, array_source.image, VK_IMAGE_LAYOUT_GENERAL,
                     array_destination.image, VK_IMAGE_LAYOUT_GENERAL, 1, &array_blit,
                     VK_FILTER_NEAREST);
      VkImageMemoryBarrier blitted = cleared;
      blitted.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      blitted.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &blitted);

      VkBufferImageCopy const array_readback = {
         .bufferOffset = array_readback_offset,
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, kArrayLayers},
         .imageExtent = {kWidth, kHeight, 1},
      };
      vkCmdCopyImageToBuffer(command_buffer, array_destination.image, VK_IMAGE_LAYOUT_GENERAL,
                             buffer, 1, &array_readback);
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

   uint32_t failures = 0;
   for (uint32_t case_index = 0; case_index < kCaseCount; ++case_index) {
      Case const & test_case = kCases[case_index];
      uint8_t const * const case_pixels = mapping + image_bytes * (case_index + 1);
      for (uint32_t slice = 0; slice < test_case.written_slices; ++slice) {
         uint32_t const want_source = test_case.expected_source[slice];
         uint8_t const * const pixel = case_pixels + slice_bytes * slice;
         bool matches = true;
         for (uint32_t channel = 0; channel < 3; ++channel)
            matches &= pixel[channel] == slice_channel(want_source, channel);
         if (matches)
            continue;

         /* Naming the slice actually seen says what went wrong: an unscaled mapping, a mapping
          * that was not mirrored, or nothing written at all.
          */
         int seen = -1;
         for (uint32_t candidate = 0; candidate < kDepth; ++candidate) {
            if (pixel[0] == slice_channel(candidate, 0) &&
                pixel[1] == slice_channel(candidate, 1) &&
                pixel[2] == slice_channel(candidate, 2)) {
               seen = static_cast<int>(candidate);
               break;
            }
         }
         if (seen >= 0) {
            std::fprintf(stderr, "%s: destination slice %u holds source slice %d, expected %u\n",
                         test_case.name, slice, seen, want_source);
         } else {
            std::fprintf(stderr,
                         "%s: destination slice %u holds (%u,%u,%u), which is no source slice; "
                         "expected source slice %u\n",
                         test_case.name, slice, pixel[0], pixel[1], pixel[2], want_source);
         }
         ++failures;
      }
   }
   for (uint32_t layer = 0; layer < kArrayLayers; ++layer) {
      /* Only the minified half was written, so the check reads the first texel of the row. */
      uint8_t const * const pixel = mapping + array_readback_offset + slice_bytes * layer;
      bool matches = true;
      for (uint32_t channel = 0; channel < 3; ++channel)
         matches &= pixel[channel] == slice_channel(layer, channel);
      if (matches)
         continue;
      int seen = -1;
      for (uint32_t candidate = 0; candidate < kDepth; ++candidate) {
         if (pixel[0] == slice_channel(candidate, 0) && pixel[1] == slice_channel(candidate, 1) &&
             pixel[2] == slice_channel(candidate, 2)) {
            seen = static_cast<int>(candidate);
            break;
         }
      }
      std::fprintf(stderr, "layered array: destination layer %u holds source layer %d\n", layer,
                   seen);
      ++failures;
   }

   std::printf("blit_3d cases=%u bad_slices=%u %s\n", kCaseCount, failures,
               failures == 0 ? "PASS" : "FAIL");

   vkDeviceWaitIdle(device);
   vkDestroyFence(device, fence, nullptr);
   vkDestroyCommandPool(device, command_pool, nullptr);
   vkUnmapMemory(device, buffer_memory);
   vkDestroyBuffer(device, buffer, nullptr);
   vkFreeMemory(device, buffer_memory, nullptr);
   for (Image const & image : {source, destination, array_source, array_destination}) {
      vkDestroyImage(device, image.image, nullptr);
      vkFreeMemory(device, image.memory, nullptr);
   }
   vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr);
   return failures == 0 ? 0 : 1;
}
