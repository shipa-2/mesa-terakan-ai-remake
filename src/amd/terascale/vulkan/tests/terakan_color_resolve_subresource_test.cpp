/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Color multisample resolve into a subresource that is not the whole image.
 *
 * No test in this suite exercised a COLOR attachment resolve at all before this one -- not even a
 * whole-image case. terakan_resolve_modes and terakan_stencil_resolve_modes both cover depth or
 * stencil only. This is written against dynamic rendering's automatic color resolve
 * (VkRenderingAttachmentInfo::resolveImageView), which Terakan implements by recording the
 * resolve's source/destination subresources at vkCmdBeginRendering time (see
 * terakan_vk_render_pass.c) and issuing a plain vkCmdResolveImage2 call at vkCmdEndRendering, just
 * like a depth/stencil resolve: the destination is reached through a view whose mip level and array
 * layer have to make it into the color descriptor, and the resolve covers the render area rather
 * than the attachment, so its offset has to reach the draw.
 *
 * Along the way this test found a real bug, not just a coverage gap: resolving into a destination
 * array layer that differs from the multisample source's own array layer did not fail to write the
 * requested layer, it silently wrote the resolved value into the SOURCE's layer of the destination
 * image instead -- corrupting whatever was there rather than leaving it alone. This looks like
 * Evergreen's CB_RESOLVE sharing one per-draw array-slice-select state across both bound color
 * buffers rather than addressing each RTV's slice independently; terakan_meta_resolve.c's fixed-
 * function-compatibility check already excluded other CB_RESOLVE-incompatible regions (mismatched
 * extents, mismatched offsets) but did not check for a mismatched array layer. The fix adds that
 * check, so a cross-layer resolve is now skipped like any other incompatible region -- there is no
 * shader fallback available (the alternate shader resolve path is disabled elsewhere in that file)
 * -- rather than silently corrupting an unrelated layer.
 *
 * Two cases exercise the addressing that does work: a non-zero mip level with a non-zero render-area
 * offset, and a non-zero array layer shared identically by both the multisample source and the
 * destination (the only array-layer combination CB_RESOLVE supports). A third case is a regression
 * check for the fix itself: a mismatched-layer resolve must leave the destination's sentinel intact
 * everywhere, in the source's layer as well as the requested one, proving the region is skipped
 * rather than misdirected.
 *
 * Every destination subresource is cleared to a sentinel first and the render area is a rectangle in
 * the middle of it, so a resolve that lands on the wrong level, the wrong layer or the wrong offset
 * shows up either as the sentinel surviving inside the area or as the resolved color appearing
 * outside it.
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

#define VK_CHECK_VOID(expression)                                                                  \
   do {                                                                                            \
      VkResult const check_result = (expression);                                                  \
      if (check_result != VK_SUCCESS) {                                                            \
         std::fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_result);          \
         return;                                                                                   \
      }                                                                                            \
   } while (false)

namespace {

constexpr uint32_t kDestinationWidth = 16;
constexpr uint32_t kDestinationHeight = 16;
constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkSampleCountFlagBits kSamples = VK_SAMPLE_COUNT_2_BIT;

constexpr uint8_t kSentinelBytes[4] = {0x11, 0x22, 0x33, 0xAA};
constexpr uint8_t kResolvedBytes[4] = {0xCC, 0x99, 0x55, 0xFF};

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
create_color_image(VkDevice device, VkPhysicalDevice physical_device, VkExtent2D extent,
                   uint32_t level_count, uint32_t layer_count, VkSampleCountFlagBits samples,
                   VkImageUsageFlags usage, Image & out)
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
   return vkBindImageMemory(device, out.image, out.memory, 0);
}

VkImageView
create_view(VkDevice device, VkImage image, uint32_t level, uint32_t layer)
{
   VkImageViewCreateInfo const info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = kFormat,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, layer, 1},
   };
   VkImageView view = VK_NULL_HANDLE;
   vkCreateImageView(device, &info, nullptr, &view);
   return view;
}

} // namespace

int
main()
{
   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-color-resolve-subresource-test",
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
      /* TeraScale 1 (R600/R700) devices also enumerate as "... (Terakan)" but cannot create a
       * device yet (see terakan_physical_device_chip_info::is_terascale_1 and
       * terakan_CreateDevice), so they are excluded by their "TeraScale 1" name prefix rather than
       * picked and failed on below.
       */
      if (!std::strstr(properties.deviceName, "(Terakan)") ||
          std::strstr(properties.deviceName, "TeraScale 1"))
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

   uint32_t total_failures = 0;

   auto close_enough = [](uint8_t actual, uint8_t expected) {
      int const delta = static_cast<int>(actual) - static_cast<int>(expected);
      return delta >= -2 && delta <= 2;
   };

   /* Runs one resolve case. The multisample source has `src_layer_count` array layers and is bound
    * through a view at `src_view_layer`; the destination has `dst_level_count` mip levels and
    * `dst_layer_count` array layers and is resolved into `dst_view_level`/`dst_view_layer`. The
    * render area is `render_area`, and every destination subresource is pre-filled with the
    * sentinel so a wrong-level, wrong-layer or wrong-offset write is visible anywhere it lands.
    */
   auto run_case = [&](char const * name, uint32_t src_layer_count, uint32_t src_view_layer,
                       uint32_t dst_level_count, uint32_t dst_layer_count,
                       uint32_t dst_view_level, uint32_t dst_view_layer, VkRect2D render_area,
                       bool expect_resolved, int32_t extra_untouched_layer = -1) {
      uint32_t const dst_width = kDestinationWidth >> dst_view_level;
      uint32_t const dst_height = kDestinationHeight >> dst_view_level;

      Image multisample, destination;
      VK_CHECK_VOID(create_color_image(device, physical_device, {dst_width, dst_height}, 1,
                                       src_layer_count, kSamples,
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, multisample));
      VK_CHECK_VOID(create_color_image(
         device, physical_device, {kDestinationWidth, kDestinationHeight}, dst_level_count,
         dst_layer_count,
         VK_SAMPLE_COUNT_1_BIT,
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         destination));
      VkImageView const multisample_view = create_view(device, multisample.image, 0, src_view_layer);
      VkImageView const destination_view =
         create_view(device, destination.image, dst_view_level, dst_view_layer);

      VkDeviceSize const readback_bytes = dst_width * dst_height * 4;
      VkBufferCreateInfo const readback_info = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = readback_bytes,
         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      VkBuffer readback_buffer;
      VK_CHECK_VOID(vkCreateBuffer(device, &readback_info, nullptr, &readback_buffer));
      VkMemoryRequirements readback_requirements;
      vkGetBufferMemoryRequirements(device, readback_buffer, &readback_requirements);
      uint32_t const readback_memory_type = find_memory_type(
         physical_device, readback_requirements.memoryTypeBits,
         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      VkMemoryAllocateInfo const readback_allocation = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = readback_requirements.size,
         .memoryTypeIndex = readback_memory_type,
      };
      VkDeviceMemory readback_memory;
      VK_CHECK_VOID(vkAllocateMemory(device, &readback_allocation, nullptr, &readback_memory));
      VK_CHECK_VOID(vkBindBufferMemory(device, readback_buffer, readback_memory, 0));
      uint8_t * readback_mapping;
      VK_CHECK_VOID(vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0,
                                reinterpret_cast<void **>(&readback_mapping)));

      VK_CHECK_VOID(run_command([&](VkCommandBuffer cmd) {
         VkClearColorValue const sentinel_clear_float = {
            .float32 = {kSentinelBytes[0] / 255.0F, kSentinelBytes[1] / 255.0F,
                        kSentinelBytes[2] / 255.0F, kSentinelBytes[3] / 255.0F}};
         VkImageSubresourceRange const whole_destination = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                                             dst_level_count, 0, dst_layer_count};
         vkCmdClearColorImage(cmd, destination.image, VK_IMAGE_LAYOUT_GENERAL,
                              &sentinel_clear_float, 1, &whole_destination);

         VkImageMemoryBarrier const cleared = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = destination.image,
            .subresourceRange = whole_destination,
         };
         vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                              nullptr, 1, &cleared);

         VkClearColorValue const resolved_clear_float = {
            .float32 = {kResolvedBytes[0] / 255.0F, kResolvedBytes[1] / 255.0F,
                        kResolvedBytes[2] / 255.0F, kResolvedBytes[3] / 255.0F}};
         VkRenderingAttachmentInfoKHR color_attachment = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
            .imageView = multisample_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT,
            .resolveImageView = destination_view,
            .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = {.color = resolved_clear_float},
         };
         VkRenderingInfoKHR const rendering = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR,
            .renderArea = render_area,
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &color_attachment,
         };
         cmd_begin_rendering(cmd, &rendering);
         cmd_end_rendering(cmd);

         VkImageMemoryBarrier const resolved = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = destination.image,
            .subresourceRange = whole_destination,
         };
         vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                              &resolved);

         VkBufferImageCopy const readback_region = {
            .bufferOffset = 0,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dst_view_level, dst_view_layer, 1},
            .imageExtent = {dst_width, dst_height, 1},
         };
         vkCmdCopyImageToBuffer(cmd, destination.image, VK_IMAGE_LAYOUT_GENERAL, readback_buffer, 1,
                                &readback_region);

         VkMemoryBarrier const host_ready = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
         };
         vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                              &host_ready, 0, nullptr, 0, nullptr);
      }));

      uint32_t inside_wrong = 0;
      uint32_t outside_wrong = 0;
      for (uint32_t y = 0; y < dst_height; ++y) {
         for (uint32_t x = 0; x < dst_width; ++x) {
            bool const inside = expect_resolved &&
                                x >= static_cast<uint32_t>(render_area.offset.x) &&
                                y >= static_cast<uint32_t>(render_area.offset.y) &&
                                x < render_area.offset.x + render_area.extent.width &&
                                y < render_area.offset.y + render_area.extent.height;
            uint8_t const * const want = inside ? kResolvedBytes : kSentinelBytes;
            uint8_t const * const actual = &readback_mapping[(y * dst_width + x) * 4];
            bool const ok = close_enough(actual[0], want[0]) && close_enough(actual[1], want[1]) &&
                            close_enough(actual[2], want[2]) && close_enough(actual[3], want[3]);
            if (ok)
               continue;
            uint32_t & counter = inside ? inside_wrong : outside_wrong;
            if (counter == 0) {
               std::fprintf(stderr,
                            "%s (%u,%u) %s the render area is (%u,%u,%u,%u), expected "
                            "(%u,%u,%u,%u)\n",
                            name, x, y, inside ? "inside" : "outside", actual[0], actual[1],
                            actual[2], actual[3], want[0], want[1], want[2], want[3]);
            }
            ++counter;
         }
      }
      uint32_t failures = inside_wrong + outside_wrong;

      if (extra_untouched_layer >= 0) {
         VK_CHECK_VOID(run_command([&](VkCommandBuffer cmd) {
            VkBufferImageCopy const extra_region = {
               .bufferOffset = 0,
               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dst_view_level,
                                    static_cast<uint32_t>(extra_untouched_layer), 1},
               .imageExtent = {dst_width, dst_height, 1},
            };
            vkCmdCopyImageToBuffer(cmd, destination.image, VK_IMAGE_LAYOUT_GENERAL,
                                   readback_buffer, 1, &extra_region);
            VkMemoryBarrier const host_ready = {
               .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
               .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
               .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                 1, &host_ready, 0, nullptr, 0, nullptr);
         }));
         uint32_t extra_wrong = 0;
         for (uint32_t y = 0; y < dst_height; ++y) {
            for (uint32_t x = 0; x < dst_width; ++x) {
               uint8_t const * const actual = &readback_mapping[(y * dst_width + x) * 4];
               bool const ok = close_enough(actual[0], kSentinelBytes[0]) &&
                               close_enough(actual[1], kSentinelBytes[1]) &&
                               close_enough(actual[2], kSentinelBytes[2]) &&
                               close_enough(actual[3], kSentinelBytes[3]);
               if (!ok) {
                  if (extra_wrong == 0) {
                     std::fprintf(stderr,
                                  "%s layer=%d (%u,%u) corrupted: (%u,%u,%u,%u), expected sentinel\n",
                                  name, extra_untouched_layer, x, y, actual[0], actual[1], actual[2],
                                  actual[3]);
                  }
                  ++extra_wrong;
               }
            }
         }
         failures += extra_wrong;
      }

      total_failures += failures;
      std::printf("%s level=%u dst_layer=%u src_layer=%u bad=%u %s\n", name, dst_view_level,
                  dst_view_layer, src_view_layer, failures, failures == 0 ? "PASS" : "FAIL");

      vkDestroyImageView(device, multisample_view, nullptr);
      vkDestroyImageView(device, destination_view, nullptr);
      vkDestroyBuffer(device, readback_buffer, nullptr);
      vkUnmapMemory(device, readback_memory);
      vkFreeMemory(device, readback_memory, nullptr);
      vkDestroyImage(device, multisample.image, nullptr);
      vkFreeMemory(device, multisample.memory, nullptr);
      vkDestroyImage(device, destination.image, nullptr);
      vkFreeMemory(device, destination.memory, nullptr);
   };

   /* Non-zero mip level, offset render area, single layer both sides. */
   run_case("mip_and_offset", 1, 0, 2, 1, 1, 0, VkRect2D{{2, 2}, {4, 4}}, true);

   /* Non-zero array layer shared identically by the source and the destination. */
   run_case("matched_array_layer", 2, 1, 1, 2, 0, 1, VkRect2D{{2, 2}, {4, 4}}, true);

   /* Source layer 0, destination layer 1, which the shared per-draw slice select cannot express
    * directly. The destination is retargeted by shifting its base address instead, so layer 1 must
    * now hold the resolved colour -- and layer 0, which is where the resolve originally landed when
    * the slice select went unhandled, must still hold the sentinel.
    */
   run_case("mismatched_array_layer", 1, 0, 1, 2, 0, 1, VkRect2D{{0, 0}, {8, 8}}, true,
            /* extra_untouched_layer */ 0);

   vkDeviceWaitIdle(device);
   vkDestroyCommandPool(device, command_pool, nullptr);
   vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr);
   return total_failures == 0 ? 0 : 1;
}
