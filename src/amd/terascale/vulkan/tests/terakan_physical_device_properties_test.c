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
   TEST_CHECK(bytes_are_nonzero(legacy_properties.pipelineCacheUUID, VK_UUID_SIZE));
   TEST_CHECK(has_device_extension(physical_device, VK_KHR_MAINTENANCE_3_EXTENSION_NAME));

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
   TEST_CHECK(subgroup.supportedStages == VK_SHADER_STAGE_COMPUTE_BIT);
   TEST_CHECK(subgroup.supportedOperations == VK_SUBGROUP_FEATURE_BASIC_BIT);
   TEST_CHECK(subgroup.subgroupSize == vulkan_1_1_properties.subgroupSize);
   TEST_CHECK(point_clipping.pointClippingBehavior == VK_POINT_CLIPPING_BEHAVIOR_ALL_CLIP_PLANES);
   TEST_CHECK(point_clipping.pointClippingBehavior == vulkan_1_1_properties.pointClippingBehavior);
   TEST_CHECK(multiview.maxMultiviewViewCount == 0);
   TEST_CHECK(multiview.maxMultiviewInstanceIndex == 0);
   TEST_CHECK(!protected_memory.protectedNoFault);

   TEST_CHECK(maintenance_3.maxPerSetDescriptors != 0);
   TEST_CHECK(maintenance_3.maxMemoryAllocationSize != 0);
   TEST_CHECK(maintenance_3.maxPerSetDescriptors == vulkan_1_1_properties.maxPerSetDescriptors);
   TEST_CHECK(maintenance_3.maxMemoryAllocationSize ==
              vulkan_1_1_properties.maxMemoryAllocationSize);
   TEST_CHECK(maintenance_3.maxPerSetDescriptors >=
              properties_2.properties.limits.maxDescriptorSetSamplers);
   TEST_CHECK(maintenance_3.maxPerSetDescriptors >=
              properties_2.properties.limits.maxDescriptorSetSampledImages);

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
   printf("\nVulkan 1.1 features:\n");
   printf("[SUPPORTED] shaderDrawParameters\n");
   printf("[UNSUPPORTED] 16-bit storage, multiview, variable pointers, protected memory, "
          "sampler YCbCr conversion\n");
   printf("\nVulkan 1.1 properties:\n");
   printf("subgroupSize=%" PRIu32 ", subgroupStages=COMPUTE, subgroupOperations=BASIC\n",
          subgroup.subgroupSize);
   printf("pointClippingBehavior=ALL_CLIP_PLANES\n");
   printf("maxPerSetDescriptors=%" PRIu32 "\n", maintenance_3.maxPerSetDescriptors);
   printf("maxMemoryAllocationSize=%" PRIu64 "\n", maintenance_3.maxMemoryAllocationSize);
   printf("\nErrors: 0\n");

out:
   if (instance != VK_NULL_HANDLE)
      vkDestroyInstance(instance, NULL);
   return result;
}
