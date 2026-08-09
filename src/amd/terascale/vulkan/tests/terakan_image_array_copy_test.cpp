/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define VK_CHECK(expr)                                                                            \
   do {                                                                                            \
      VkResult const vk_check_result = (expr);                                                     \
      if (vk_check_result != VK_SUCCESS) {                                                         \
         std::fprintf(stderr, "%s failed with VkResult %d\n", #expr, vk_check_result);             \
         return 1;                                                                                 \
      }                                                                                            \
   } while (false)

static uint32_t
find_memory_type(VkPhysicalDevice physical_device, uint32_t bits, VkMemoryPropertyFlags flags)
{
   VkPhysicalDeviceMemoryProperties properties;
   vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
   for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) &&
          (properties.memoryTypes[i].propertyFlags & flags) == flags) {
         return i;
      }
   }
   return UINT32_MAX;
}

static int
create_image(VkDevice device, VkPhysicalDevice physical_device, VkFormat format,
             VkImageUsageFlags usage, uint32_t width, uint32_t height, uint32_t layers,
             VkImage *image_out, VkDeviceMemory *memory_out)
{
   VkImageCreateInfo const image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {width, height, 1},
      .mipLevels = 1,
      .arrayLayers = layers,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkResult result = vkCreateImage(device, &image_info, nullptr, image_out);
   if (result != VK_SUCCESS)
      return result;
   VkMemoryRequirements requirements;
   vkGetImageMemoryRequirements(device, *image_out, &requirements);
   uint32_t const type =
      find_memory_type(physical_device, requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (type == UINT32_MAX)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   VkMemoryAllocateInfo const allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = type,
   };
   result = vkAllocateMemory(device, &allocate_info, nullptr, memory_out);
   if (result != VK_SUCCESS)
      return result;
   return vkBindImageMemory(device, *image_out, *memory_out, 0);
}

int
main(int argc, char **argv)
{
   bool const r32_mode =
      argc == 2 && (std::strcmp(argv[1], "--r32") == 0 ||
                    std::strcmp(argv[1], "--r32-full") == 0);
   bool const r32_full_mode =
      argc == 2 && std::strcmp(argv[1], "--r32-full") == 0;
   bool const hdr_mode =
      argc == 2 && (std::strcmp(argv[1], "--hdr") == 0 ||
                    std::strcmp(argv[1], "--hdr-full") == 0);
   bool const hdr_full_mode =
      argc == 2 && std::strcmp(argv[1], "--hdr-full") == 0;
   if (argc > 2 || (argc == 2 && !hdr_mode && !r32_mode)) {
      std::fprintf(stderr,
                   "Usage: %s [--hdr|--hdr-full|--r32|--r32-full]\n", argv[0]);
      return 2;
   }

   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "Terakan image array copy test",
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
   for (VkPhysicalDevice candidate : physical_devices) {
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(candidate, &properties);
      if (!std::strstr(properties.deviceName, "Terakan"))
         continue;
      uint32_t count = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
      std::vector<VkQueueFamilyProperties> families(count);
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
      for (uint32_t i = 0; i < count; ++i) {
         if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            physical_device = candidate;
            queue_family = i;
            std::fprintf(stderr, "device=%s queue_family=%u\n", properties.deviceName, i);
            break;
         }
      }
      if (physical_device)
         break;
   }
   if (!physical_device) {
      std::fprintf(stderr, "Terakan graphics device not found\n");
      return 1;
   }

   float const priority = 1.0f;
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

   uint32_t const image_width = hdr_full_mode ? 1920 : r32_full_mode ? 512 : r32_mode ? 64 : 136;
   uint32_t const image_height = hdr_full_mode ? 1080 : r32_full_mode ? 512 : r32_mode ? 64 : 136;
   VkDeviceSize const payload_size =
      (hdr_mode || r32_mode) ? image_width * image_height * sizeof(uint32_t) : 34 * 34 * 8;
   std::vector<uint32_t> const test_layers =
      hdr_full_mode ? std::vector<uint32_t>{0}
                    : std::vector<uint32_t>{0, 1, 7, 8, 255, 480, 899};
   VkDeviceSize const slot_size = (payload_size + 255) & ~VkDeviceSize(255);
   VkDeviceSize const buffer_size = slot_size * (test_layers.size() + 1);

   VkBufferCreateInfo const buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = buffer_size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer buffer;
   VK_CHECK(vkCreateBuffer(device, &buffer_info, nullptr, &buffer));
   VkMemoryRequirements buffer_requirements;
   vkGetBufferMemoryRequirements(device, buffer, &buffer_requirements);
   uint32_t const host_type =
      find_memory_type(physical_device, buffer_requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (host_type == UINT32_MAX) {
      std::fprintf(stderr, "No coherent host memory\n");
      return 1;
   }
   VkMemoryAllocateInfo const buffer_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = buffer_requirements.size,
      .memoryTypeIndex = host_type,
   };
   VkDeviceMemory buffer_memory;
   VK_CHECK(vkAllocateMemory(device, &buffer_allocate_info, nullptr, &buffer_memory));
   VK_CHECK(vkBindBufferMemory(device, buffer, buffer_memory, 0));
   uint8_t *mapping;
   VK_CHECK(vkMapMemory(device, buffer_memory, 0, buffer_size, 0,
                        reinterpret_cast<void **>(&mapping)));
   if (hdr_mode || r32_mode) {
      auto *pixels = reinterpret_cast<uint32_t *>(mapping);
      for (uint32_t y = 0; y < image_height; ++y) {
         for (uint32_t x = 0; x < image_width; ++x) {
            if (r32_mode) {
               pixels[y * image_width + x] =
                  0x6d2b79f5u ^ (x * 0x9e3779b9u) ^ (y * 0x85ebca6bu);
            } else {
               uint32_t const r = (15u << 6) | ((x * 13 + y * 7) & 63);
               uint32_t const g = (14u << 6) | ((x * 3 + y * 11) & 63);
               uint32_t const b = (13u << 5) | ((x * 5 + y * 17) & 31);
               pixels[y * image_width + x] = r | (g << 11) | (b << 22);
            }
         }
      }
   } else {
      for (VkDeviceSize i = 0; i < payload_size; ++i)
         mapping[i] = static_cast<uint8_t>((i * 37 + (i >> 3) * 11 + 0x5d) & 0xff);
   }
   std::memset(mapping + slot_size, 0xa5, buffer_size - slot_size);

   VkImage source, destination;
   VkDeviceMemory source_memory, destination_memory;
   VkFormat const image_format =
      r32_mode ? VK_FORMAT_R32_UINT
               : hdr_mode ? VK_FORMAT_B10G11R11_UFLOAT_PACK32
                          : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
   VkImageUsageFlags const image_usage =
      (r32_mode ? 0 : VK_IMAGE_USAGE_TRANSFER_SRC_BIT) |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
      (r32_mode ? 0 : VK_IMAGE_USAGE_SAMPLED_BIT) |
      (hdr_mode ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0) |
      (r32_mode ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
   uint32_t const destination_layers = hdr_full_mode ? 1 : 900;
   VK_CHECK(static_cast<VkResult>(
      create_image(device, physical_device, image_format, image_usage, image_width,
                   image_height, 1, &source, &source_memory)));
   VK_CHECK(static_cast<VkResult>(
      create_image(device, physical_device, image_format, image_usage, image_width,
                   image_height, destination_layers, &destination, &destination_memory)));

   VkCommandPoolCreateInfo const pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = queue_family,
   };
   VkCommandPool pool;
   VK_CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &pool));
   VkCommandBufferAllocateInfo const command_buffer_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer command_buffer;
   VK_CHECK(vkAllocateCommandBuffers(device, &command_buffer_info, &command_buffer));
   VkCommandBufferBeginInfo const begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info));

   std::array<VkImageMemoryBarrier, 2> initial_barriers = {};
   for (uint32_t i = 0; i < initial_barriers.size(); ++i) {
      initial_barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      initial_barriers[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      initial_barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      initial_barriers[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      initial_barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      initial_barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      initial_barriers[i].image = i ? destination : source;
      initial_barriers[i].subresourceRange = {
         VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, i ? destination_layers : 1u};
   }
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                        initial_barriers.size(), initial_barriers.data());

   VkBufferImageCopy upload = {
      .bufferOffset = 0,
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {image_width, image_height, 1},
   };
   vkCmdCopyBufferToImage(command_buffer, buffer, source,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload);
   VkImageMemoryBarrier source_ready = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = source,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                        &source_ready);

   for (uint32_t layer : test_layers) {
      VkImageCopy copy = {
         .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1},
         .extent = {image_width, image_height, 1},
      };
      vkCmdCopyImage(command_buffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
   }
   VkImageMemoryBarrier destination_ready = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = destination,
      .subresourceRange = {
         VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, destination_layers},
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                        &destination_ready);
   for (uint32_t i = 0; i < test_layers.size(); ++i) {
      VkBufferImageCopy download = {
         .bufferOffset = slot_size * (i + 1),
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, test_layers[i], 1},
         .imageExtent = {image_width, image_height, 1},
      };
      vkCmdCopyImageToBuffer(command_buffer, destination,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &download);
   }
   VkMemoryBarrier host_ready = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_ready, 0, nullptr, 0, nullptr);
   VK_CHECK(vkEndCommandBuffer(command_buffer));
   VkSubmitInfo const submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
   };
   VK_CHECK(vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE));
   VK_CHECK(vkQueueWaitIdle(queue));

   bool passed = true;
   for (uint32_t i = 0; i < test_layers.size(); ++i) {
      uint8_t const *actual = mapping + slot_size * (i + 1);
      size_t mismatch = payload_size;
      for (size_t byte = 0; byte < payload_size; ++byte) {
         if (actual[byte] != mapping[byte]) {
            mismatch = byte;
            break;
         }
      }
      if (mismatch == payload_size) {
         std::printf("PASS layer=%u bytes=%zu\n", test_layers[i],
                     static_cast<size_t>(payload_size));
      } else {
         std::printf("FAIL layer=%u byte=%zu expected=%02x actual=%02x\n", test_layers[i],
                     mismatch, mapping[mismatch], actual[mismatch]);
         passed = false;
      }
   }

   vkDeviceWaitIdle(device);
   vkDestroyCommandPool(device, pool, nullptr);
   vkDestroyImage(device, source, nullptr);
   vkDestroyImage(device, destination, nullptr);
   vkFreeMemory(device, source_memory, nullptr);
   vkFreeMemory(device, destination_memory, nullptr);
   vkDestroyBuffer(device, buffer, nullptr);
   vkUnmapMemory(device, buffer_memory);
   vkFreeMemory(device, buffer_memory, nullptr);
   vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr);
   return passed ? 0 : 1;
}
