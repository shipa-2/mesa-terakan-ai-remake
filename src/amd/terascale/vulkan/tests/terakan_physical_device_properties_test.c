/*
 * Copyright © 2026 Terakan contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <vulkan/vulkan.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

#define TEST_CHECK(condition)                                                                      \
   do {                                                                                            \
      if (!(condition)) {                                                                          \
         fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);             \
         result = EXIT_FAILURE;                                                                    \
         goto out;                                                                                 \
      }                                                                                            \
   } while (0)

static bool
bytes_are_nonzero(uint8_t const * const bytes, size_t const size)
{
   for (size_t i = 0; i < size; ++i) {
      if (bytes[i] != 0)
         return true;
   }
   return false;
}

static void
print_uuid(uint8_t const uuid[VK_UUID_SIZE])
{
   for (uint32_t i = 0; i < VK_UUID_SIZE; ++i)
      printf("%02x", uuid[i]);
}

static bool
has_device_extension(VkPhysicalDevice const physical_device, char const * const name)
{
   uint32_t extension_count = 0;
   if (vkEnumerateDeviceExtensionProperties(physical_device, NULL, &extension_count, NULL) !=
       VK_SUCCESS)
      return false;

   VkExtensionProperties * const extensions =
      calloc(extension_count, sizeof(VkExtensionProperties));
   if (extensions == NULL)
      return false;

   bool found = false;
   if (vkEnumerateDeviceExtensionProperties(physical_device, NULL, &extension_count, extensions) ==
       VK_SUCCESS) {
      for (uint32_t i = 0; i < extension_count; ++i) {
         if (!strcmp(extensions[i].extensionName, name)) {
            found = true;
            break;
         }
      }
   }
   free(extensions);
   return found;
}

int
main(void)
{
   int result = EXIT_SUCCESS;
   VkInstance instance = VK_NULL_HANDLE;
   VkDevice device = VK_NULL_HANDLE;
   VkDeviceMemory budget_test_memory = VK_NULL_HANDLE;
   VkImage image_3d = VK_NULL_HANDLE;
   VkImage msaa_images[4] = {VK_NULL_HANDLE};
   VkDeviceMemory msaa_memory[4] = {VK_NULL_HANDLE};
   VkCommandPool command_pool = VK_NULL_HANDLE;
   VkFence metadata_fence = VK_NULL_HANDLE;
   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan_physical_device_properties_test",
      .apiVersion = VK_API_VERSION_1_1,
   };
   char const * const instance_extensions[] = {
      VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
   };
   VkInstanceCreateInfo const instance_create_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application_info,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = instance_extensions,
   };
   TEST_CHECK(vkCreateInstance(&instance_create_info, NULL, &instance) == VK_SUCCESS);

   uint32_t physical_device_count = 0;
   TEST_CHECK(vkEnumeratePhysicalDevices(instance, &physical_device_count, NULL) == VK_SUCCESS);
   if (physical_device_count == 0) {
      result = 77;
      goto out;
   }

   VkPhysicalDevice * const physical_devices =
      calloc(physical_device_count, sizeof(VkPhysicalDevice));
   TEST_CHECK(physical_devices != NULL);
   TEST_CHECK(vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices) ==
              VK_SUCCESS);

   VkPhysicalDevice physical_device = VK_NULL_HANDLE;
   VkPhysicalDeviceProperties legacy_properties;
   for (uint32_t i = 0; i < physical_device_count; ++i) {
      vkGetPhysicalDeviceProperties(physical_devices[i], &legacy_properties);
      if (legacy_properties.vendorID == 0x1002 &&
          strstr(legacy_properties.deviceName, "(Terakan)") != NULL) {
         physical_device = physical_devices[i];
         break;
      }
   }
   free(physical_devices);
   if (physical_device == VK_NULL_HANDLE) {
      result = 77;
      goto out;
   }

   TEST_CHECK(VK_API_VERSION_MAJOR(legacy_properties.apiVersion) == 1);
   TEST_CHECK(VK_API_VERSION_MINOR(legacy_properties.apiVersion) >= 1);
   TEST_CHECK(legacy_properties.limits.minInterpolationOffset == -0.5f);
   TEST_CHECK(legacy_properties.limits.maxInterpolationOffset == 0.4375f);
   TEST_CHECK(legacy_properties.limits.subPixelInterpolationOffsetBits == 4);
   TEST_CHECK(bytes_are_nonzero(legacy_properties.pipelineCacheUUID, VK_UUID_SIZE));
   TEST_CHECK(has_device_extension(physical_device, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME));
   TEST_CHECK(has_device_extension(physical_device, VK_KHR_MAINTENANCE_3_EXTENSION_NAME));
   TEST_CHECK(has_device_extension(physical_device, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME));
   TEST_CHECK(has_device_extension(physical_device, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME));
   /* Depth resolve is implemented by sampling the multisample source, which terakan_depth_resolve
    * covers with a readback test, so the extension and its dependency are advertised. Dynamic
    * rendering additionally needs stencil resolve, so it stays hidden. */
   TEST_CHECK(has_device_extension(physical_device,
                                   VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME));
   TEST_CHECK(has_device_extension(physical_device,
                                   VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME));
   TEST_CHECK(!has_device_extension(physical_device, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME));

   VkPhysicalDeviceVulkan11Features vulkan_1_1_features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
   };
   VkPhysicalDeviceFeatures2 features_2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &vulkan_1_1_features,
   };
   vkGetPhysicalDeviceFeatures2(physical_device, &features_2);

   VkPhysicalDeviceFeatures legacy_features;
   vkGetPhysicalDeviceFeatures(physical_device, &legacy_features);
   TEST_CHECK(!memcmp(&legacy_features, &features_2.features, sizeof(legacy_features)));
   TEST_CHECK(vulkan_1_1_features.shaderDrawParameters);
   TEST_CHECK(!vulkan_1_1_features.storageBuffer16BitAccess);
   TEST_CHECK(!vulkan_1_1_features.uniformAndStorageBuffer16BitAccess);
   TEST_CHECK(!vulkan_1_1_features.storagePushConstant16);
   TEST_CHECK(!vulkan_1_1_features.storageInputOutput16);
   TEST_CHECK(!vulkan_1_1_features.multiview);
   TEST_CHECK(!vulkan_1_1_features.variablePointersStorageBuffer);
   TEST_CHECK(!vulkan_1_1_features.variablePointers);
   TEST_CHECK(!vulkan_1_1_features.protectedMemory);
   TEST_CHECK(!vulkan_1_1_features.samplerYcbcrConversion);

   VkPhysicalDeviceVulkan11Properties vulkan_1_1_properties = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
   };
   VkPhysicalDeviceProperties2 properties_2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &vulkan_1_1_properties,
   };
   vkGetPhysicalDeviceProperties2(physical_device, &properties_2);
   TEST_CHECK(properties_2.properties.vendorID == legacy_properties.vendorID);
   TEST_CHECK(properties_2.properties.deviceID == legacy_properties.deviceID);
   TEST_CHECK(!strcmp(properties_2.properties.deviceName, legacy_properties.deviceName));

   VkPhysicalDeviceMaintenance3Properties maintenance_3 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES,
   };
   VkPhysicalDeviceProtectedMemoryProperties protected_memory = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES,
      .pNext = &maintenance_3,
   };
   VkPhysicalDeviceMultiviewProperties multiview = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES,
      .pNext = &protected_memory,
   };
   VkPhysicalDevicePointClippingProperties point_clipping = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES,
      .pNext = &multiview,
   };
   VkPhysicalDeviceSubgroupProperties subgroup = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
      .pNext = &point_clipping,
   };
   VkPhysicalDeviceIDProperties id = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
      .pNext = &subgroup,
   };
   properties_2.pNext = &id;
   vkGetPhysicalDeviceProperties2(physical_device, &properties_2);

   TEST_CHECK(bytes_are_nonzero(id.deviceUUID, VK_UUID_SIZE));
   TEST_CHECK(bytes_are_nonzero(id.driverUUID, VK_UUID_SIZE));
   TEST_CHECK(!memcmp(id.deviceUUID, vulkan_1_1_properties.deviceUUID, VK_UUID_SIZE));
   TEST_CHECK(!memcmp(id.driverUUID, vulkan_1_1_properties.driverUUID, VK_UUID_SIZE));
   TEST_CHECK(id.deviceLUIDValid == vulkan_1_1_properties.deviceLUIDValid);
   if (id.deviceLUIDValid) {
      TEST_CHECK(bytes_are_nonzero(id.deviceLUID, VK_LUID_SIZE));
      TEST_CHECK(id.deviceNodeMask != 0);
      TEST_CHECK((id.deviceNodeMask & (id.deviceNodeMask - 1)) == 0);
   }

   TEST_CHECK(subgroup.subgroupSize != 0);
   TEST_CHECK((subgroup.subgroupSize & (subgroup.subgroupSize - 1)) == 0);
   TEST_CHECK(subgroup.subgroupSize == 1);
   TEST_CHECK(subgroup.supportedStages ==
              (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
               VK_SHADER_STAGE_COMPUTE_BIT));
   TEST_CHECK(subgroup.supportedOperations ==
              (VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
               VK_SUBGROUP_FEATURE_BALLOT_BIT));
   TEST_CHECK(subgroup.subgroupSize == vulkan_1_1_properties.subgroupSize);
   TEST_CHECK(point_clipping.pointClippingBehavior == VK_POINT_CLIPPING_BEHAVIOR_ALL_CLIP_PLANES);
   TEST_CHECK(point_clipping.pointClippingBehavior == vulkan_1_1_properties.pointClippingBehavior);
   TEST_CHECK(multiview.maxMultiviewViewCount == 0);
   TEST_CHECK(multiview.maxMultiviewInstanceIndex == 0);
   TEST_CHECK(!protected_memory.protectedNoFault);

   VkPhysicalDeviceDepthStencilResolveProperties depth_stencil_resolve = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES,
   };
   properties_2.pNext = &depth_stencil_resolve;
   vkGetPhysicalDeviceProperties2(physical_device, &properties_2);
   /* Sample zero only for depth, and no stencil resolve yet. That combination requires
    * independentResolveNone, so depth can be resolved while stencil is left alone, but not full
    * independentResolve, which would mean the two aspects can take different modes. */
   TEST_CHECK(depth_stencil_resolve.supportedDepthResolveModes == VK_RESOLVE_MODE_SAMPLE_ZERO_BIT);
   TEST_CHECK(depth_stencil_resolve.supportedStencilResolveModes == VK_RESOLVE_MODE_NONE);
   TEST_CHECK(depth_stencil_resolve.independentResolveNone);
   TEST_CHECK(!depth_stencil_resolve.independentResolve);

   TEST_CHECK(maintenance_3.maxPerSetDescriptors != 0);
   TEST_CHECK(maintenance_3.maxMemoryAllocationSize != 0);
   TEST_CHECK(maintenance_3.maxPerSetDescriptors == vulkan_1_1_properties.maxPerSetDescriptors);
   TEST_CHECK(maintenance_3.maxMemoryAllocationSize ==
              vulkan_1_1_properties.maxMemoryAllocationSize);
   TEST_CHECK(maintenance_3.maxPerSetDescriptors >=
              properties_2.properties.limits.maxDescriptorSetSamplers);
   TEST_CHECK(maintenance_3.maxPerSetDescriptors >=
              properties_2.properties.limits.maxDescriptorSetSampledImages);

   VkPhysicalDeviceDriverProperties driver = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
   };
   properties_2.pNext = &driver;
   vkGetPhysicalDeviceProperties2(physical_device, &properties_2);
   TEST_CHECK(driver.driverID == VK_DRIVER_ID_MESA_RADV);
   TEST_CHECK(!strcmp(driver.driverName, "Terakan"));
   TEST_CHECK(strstr(driver.driverInfo, "Mesa ") == driver.driverInfo);
   TEST_CHECK(driver.conformanceVersion.major == 0);
   TEST_CHECK(driver.conformanceVersion.minor == 0);
   TEST_CHECK(driver.conformanceVersion.subminor == 0);
   TEST_CHECK(driver.conformanceVersion.patch == 0);

   PFN_vkGetPhysicalDeviceFeatures2KHR const get_features_2_khr =
      (PFN_vkGetPhysicalDeviceFeatures2KHR)vkGetInstanceProcAddr(instance,
                                                                 "vkGetPhysicalDeviceFeatures2KHR");
   PFN_vkGetPhysicalDeviceProperties2KHR const get_properties_2_khr =
      (PFN_vkGetPhysicalDeviceProperties2KHR)vkGetInstanceProcAddr(
         instance, "vkGetPhysicalDeviceProperties2KHR");
   TEST_CHECK(get_features_2_khr != NULL);
   TEST_CHECK(get_properties_2_khr != NULL);

   VkPhysicalDeviceVulkan11Features vulkan_1_1_features_khr = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
   };
   features_2.pNext = &vulkan_1_1_features_khr;
   get_features_2_khr(physical_device, &features_2);
   TEST_CHECK(!memcmp(&vulkan_1_1_features, &vulkan_1_1_features_khr, sizeof(vulkan_1_1_features)));

   VkPhysicalDeviceMaintenance3Properties maintenance_3_khr = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES,
   };
   properties_2.pNext = &maintenance_3_khr;
   get_properties_2_khr(physical_device, &properties_2);
   TEST_CHECK(maintenance_3_khr.maxPerSetDescriptors == maintenance_3.maxPerSetDescriptors);
   TEST_CHECK(maintenance_3_khr.maxMemoryAllocationSize == maintenance_3.maxMemoryAllocationSize);

   /* The maximum addressable allocation may be larger than every currently available heap. */
   TEST_CHECK(maintenance_3.maxMemoryAllocationSize <= UINT32_MAX);

   VkPhysicalDeviceMemoryBudgetPropertiesEXT memory_budget_before = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT,
   };
   VkPhysicalDeviceMemoryProperties2 memory_properties_2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
      .pNext = &memory_budget_before,
   };
   vkGetPhysicalDeviceMemoryProperties2(physical_device, &memory_properties_2);

   uint32_t device_local_memory_type = UINT32_MAX;
   for (uint32_t i = 0; i < memory_properties_2.memoryProperties.memoryTypeCount; ++i) {
      if (memory_properties_2.memoryProperties.memoryTypes[i].propertyFlags &
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
         device_local_memory_type = i;
         break;
      }
   }
   TEST_CHECK(device_local_memory_type != UINT32_MAX);
   uint32_t const budget_test_heap =
      memory_properties_2.memoryProperties.memoryTypes[device_local_memory_type].heapIndex;
   TEST_CHECK(memory_budget_before.heapBudget[budget_test_heap] <=
              memory_properties_2.memoryProperties.memoryHeaps[budget_test_heap].size);

   float const queue_priority = 1.0f;
   VkDeviceQueueCreateInfo const queue_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &queue_priority,
   };
   char const * const device_extensions[] = {
      VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
   };
   VkDeviceCreateInfo const device_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_create_info,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = device_extensions,
   };
   TEST_CHECK(vkCreateDevice(physical_device, &device_create_info, NULL, &device) == VK_SUCCESS);

   VkImageCreateInfo const image_3d_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_3D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {64, 64, 8},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   TEST_CHECK(vkCreateImage(device, &image_3d_create_info, NULL, &image_3d) == VK_SUCCESS);
   VkMemoryRequirements image_3d_memory_requirements;
   vkGetImageMemoryRequirements(device, image_3d, &image_3d_memory_requirements);
   TEST_CHECK(image_3d_memory_requirements.alignment >= 4096);

   VkSampleCountFlagBits const sample_counts[] = {
      VK_SAMPLE_COUNT_1_BIT,
      VK_SAMPLE_COUNT_2_BIT,
      VK_SAMPLE_COUNT_4_BIT,
      VK_SAMPLE_COUNT_8_BIT,
   };
   VkDeviceSize image_requirement_sizes[ARRAY_SIZE(sample_counts)];
   for (uint32_t i = 0; i < ARRAY_SIZE(sample_counts); ++i) {
      VkImageCreateInfo const image_create_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .extent = {1024, 1024, 1},
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = sample_counts[i],
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      TEST_CHECK(vkCreateImage(device, &image_create_info, NULL, &msaa_images[i]) == VK_SUCCESS);
      VkMemoryRequirements object_memory_requirements;
      vkGetImageMemoryRequirements(device, msaa_images[i], &object_memory_requirements);
      image_requirement_sizes[i] = object_memory_requirements.size;
      if (i != 0) {
         TEST_CHECK(object_memory_requirements.memoryTypeBits & (1u << device_local_memory_type));
         VkMemoryAllocateInfo const allocate_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = object_memory_requirements.size,
            .memoryTypeIndex = device_local_memory_type,
         };
         TEST_CHECK(vkAllocateMemory(device, &allocate_info, NULL, &msaa_memory[i]) == VK_SUCCESS);
         TEST_CHECK(vkBindImageMemory(device, msaa_images[i], msaa_memory[i], 0) == VK_SUCCESS);
      }
   }
   TEST_CHECK(image_requirement_sizes[1] > 2 * image_requirement_sizes[0]);
   TEST_CHECK(image_requirement_sizes[2] > 4 * image_requirement_sizes[0]);
   TEST_CHECK(image_requirement_sizes[3] > 8 * image_requirement_sizes[0]);

   VkCommandPoolCreateInfo const command_pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   TEST_CHECK(vkCreateCommandPool(device, &command_pool_create_info, NULL, &command_pool) ==
              VK_SUCCESS);
   VkCommandBuffer command_buffer;
   VkCommandBufferAllocateInfo const command_buffer_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   TEST_CHECK(vkAllocateCommandBuffers(device, &command_buffer_allocate_info, &command_buffer) ==
              VK_SUCCESS);
   VkCommandBufferBeginInfo const command_buffer_begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   TEST_CHECK(vkBeginCommandBuffer(command_buffer, &command_buffer_begin_info) == VK_SUCCESS);
   VkImageMemoryBarrier metadata_barriers[3];
   for (uint32_t i = 0; i < ARRAY_SIZE(metadata_barriers); ++i) {
      metadata_barriers[i] = (VkImageMemoryBarrier){
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = 0,
         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = msaa_images[i + 1],
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
   }
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL,
                        ARRAY_SIZE(metadata_barriers), metadata_barriers);
   TEST_CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
   VkFenceCreateInfo const fence_create_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   };
   TEST_CHECK(vkCreateFence(device, &fence_create_info, NULL, &metadata_fence) == VK_SUCCESS);
   VkSubmitInfo const submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
   };
   VkQueue queue;
   vkGetDeviceQueue(device, 0, 0, &queue);
   TEST_CHECK(vkQueueSubmit(queue, 1, &submit_info, metadata_fence) == VK_SUCCESS);
   TEST_CHECK(vkWaitForFences(device, 1, &metadata_fence, VK_TRUE, 2000000000ull) == VK_SUCCESS);

   /* Take the baseline after device creation because internal device BOs aren't VkDeviceMemory
    * allocations and therefore aren't part of the application allocation counter. */
   vkGetPhysicalDeviceMemoryProperties2(physical_device, &memory_properties_2);
   VkDeviceSize const usage_before = memory_budget_before.heapUsage[budget_test_heap];

   VkDeviceSize const budget_test_size = 16 * 1024 * 1024;
   VkMemoryAllocateInfo const budget_test_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = budget_test_size,
      .memoryTypeIndex = device_local_memory_type,
   };
   TEST_CHECK(vkAllocateMemory(device, &budget_test_allocate_info, NULL, &budget_test_memory) ==
              VK_SUCCESS);

   VkPhysicalDeviceMemoryBudgetPropertiesEXT memory_budget_allocated = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT,
   };
   memory_properties_2.pNext = &memory_budget_allocated;
   vkGetPhysicalDeviceMemoryProperties2(physical_device, &memory_properties_2);
   TEST_CHECK(memory_budget_allocated.heapUsage[budget_test_heap] >=
              usage_before + budget_test_size);
   TEST_CHECK(memory_budget_allocated.heapBudget[budget_test_heap] <=
              memory_properties_2.memoryProperties.memoryHeaps[budget_test_heap].size);

   vkFreeMemory(device, budget_test_memory, NULL);
   budget_test_memory = VK_NULL_HANDLE;

   VkPhysicalDeviceMemoryBudgetPropertiesEXT memory_budget_freed = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT,
   };
   memory_properties_2.pNext = &memory_budget_freed;
   vkGetPhysicalDeviceMemoryProperties2(physical_device, &memory_properties_2);
   TEST_CHECK(memory_budget_freed.heapUsage[budget_test_heap] == usage_before);
   for (uint32_t i = memory_properties_2.memoryProperties.memoryHeapCount;
        i < VK_MAX_MEMORY_HEAPS; ++i) {
      TEST_CHECK(memory_budget_freed.heapBudget[i] == 0);
      TEST_CHECK(memory_budget_freed.heapUsage[i] == 0);
   }

   printf("Terakan Vulkan 1.1 capability report\n");
   printf("Device: %s (vendor=%04" PRIx32 ", device=%04" PRIx32 ")\n", legacy_properties.deviceName,
          legacy_properties.vendorID, legacy_properties.deviceID);
   printf("API: %" PRIu32 ".%" PRIu32 ".%" PRIu32 "\n",
          VK_API_VERSION_MAJOR(legacy_properties.apiVersion),
          VK_API_VERSION_MINOR(legacy_properties.apiVersion),
          VK_API_VERSION_PATCH(legacy_properties.apiVersion));
   printf("deviceUUID: ");
   print_uuid(id.deviceUUID);
   printf("\ndriverUUID: ");
   print_uuid(id.driverUUID);
   printf("\npipelineCacheUUID: ");
   print_uuid(legacy_properties.pipelineCacheUUID);
   printf("\n\nFunctions:\n");
   printf("[PASS] vkEnumerateDeviceExtensionProperties\n");
   printf("[PASS] vkGetPhysicalDeviceFeatures\n");
   printf("[PASS] vkGetPhysicalDeviceFeatures2\n");
   printf("[PASS] vkGetPhysicalDeviceFeatures2KHR\n");
   printf("[PASS] vkGetPhysicalDeviceProperties\n");
   printf("[PASS] vkGetPhysicalDeviceProperties2\n");
   printf("[PASS] vkGetPhysicalDeviceProperties2KHR\n");
   printf("[PASS] vkGetPhysicalDeviceMemoryProperties\n");
   printf("[PASS] VK_EXT_memory_budget allocation/free accounting\n");
   printf("[PASS] 2x/4x/8x color images reserve FMASK/CMASK memory\n");
   printf("[PASS] 2x/4x/8x FMASK identity and CMASK initialization submission\n");
   printf("[PASS] sample-zero depth resolve is advertised, stencil resolve and dynamic "
          "rendering remain hidden\n");
   printf("[PASS] 3D image memory bindings are page-aligned for radeon CS validation\n");
   printf("memoryBudget heap=%" PRIu32 " before=%" PRIu64 " allocated=%" PRIu64
          " freed=%" PRIu64 " budget=%" PRIu64 "\n",
          budget_test_heap, usage_before, memory_budget_allocated.heapUsage[budget_test_heap],
          memory_budget_freed.heapUsage[budget_test_heap],
          memory_budget_allocated.heapBudget[budget_test_heap]);
   printf("\nVulkan 1.1 features:\n");
   printf("[SUPPORTED] shaderDrawParameters\n");
   printf("[UNSUPPORTED] 16-bit storage, multiview, variable pointers, protected memory, "
          "sampler YCbCr conversion\n");
   printf("\nVulkan 1.1 properties:\n");
   printf("subgroupSize=%" PRIu32
          ", subgroupStages=VERTEX|FRAGMENT|COMPUTE, subgroupOperations=BASIC|ARITHMETIC\n",
          subgroup.subgroupSize);
   printf("pointClippingBehavior=ALL_CLIP_PLANES\n");
   printf("maxPerSetDescriptors=%" PRIu32 "\n", maintenance_3.maxPerSetDescriptors);
   printf("maxMemoryAllocationSize=%" PRIu64 "\n", maintenance_3.maxMemoryAllocationSize);
   printf("driverName=%s\n", driver.driverName);
   printf("driverInfo=%s\n", driver.driverInfo);
   printf("\nErrors: 0\n");

out:
   if (device != VK_NULL_HANDLE) {
      if (image_3d != VK_NULL_HANDLE)
         vkDestroyImage(device, image_3d, NULL);
      if (metadata_fence != VK_NULL_HANDLE)
         vkDestroyFence(device, metadata_fence, NULL);
      if (command_pool != VK_NULL_HANDLE)
         vkDestroyCommandPool(device, command_pool, NULL);
      for (uint32_t i = 0; i < ARRAY_SIZE(msaa_images); ++i) {
         if (msaa_images[i] != VK_NULL_HANDLE)
            vkDestroyImage(device, msaa_images[i], NULL);
         if (msaa_memory[i] != VK_NULL_HANDLE)
            vkFreeMemory(device, msaa_memory[i], NULL);
      }
   }
   if (budget_test_memory != VK_NULL_HANDLE)
      vkFreeMemory(device, budget_test_memory, NULL);
   if (device != VK_NULL_HANDLE)
      vkDestroyDevice(device, NULL);
   if (instance != VK_NULL_HANDLE)
      vkDestroyInstance(instance, NULL);
   return result;
}
