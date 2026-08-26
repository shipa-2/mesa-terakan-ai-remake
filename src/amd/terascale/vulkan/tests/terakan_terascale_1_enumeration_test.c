/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* TeraScale 1 physical devices enumerate and report properties. On hardware-validated R700,
 * vkCreateDevice additionally performs the minimal logical-device bring-up, while R600 is still
 * refused cleanly. Queue submission remains disabled on both until the command stream is validated.
 *
 * Meaningful only on a machine with a TeraScale 1 card actually installed; there is no requirement
 * that one be present. When none is found, this reports that plainly and passes, since there is
 * nothing to check.
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define VK_CHECK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_result = (expression);                                                  \
      if (check_result != VK_SUCCESS) {                                                            \
         fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_result);               \
         return 1;                                                                                 \
      }                                                                                            \
   } while (0)

static bool
device_name_is_r700(char const * const device_name)
{
   return strstr(device_name, "RV770") != NULL || strstr(device_name, "RV730") != NULL ||
          strstr(device_name, "RV710") != NULL || strstr(device_name, "RV740") != NULL;
}

int
main(void)
{
   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-terascale-1-enumeration-test",
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo const instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application_info,
   };
   VkInstance instance;
   VK_CHECK(vkCreateInstance(&instance_info, NULL, &instance));

   uint32_t physical_device_count = 0;
   VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_device_count, NULL));
   VkPhysicalDevice physical_devices[8];
   if (physical_device_count > 8) {
      physical_device_count = 8;
   }
   VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices));

   uint32_t terascale_1_devices_checked = 0;
   uint32_t failures = 0;
   for (uint32_t device_index = 0; device_index < physical_device_count; ++device_index) {
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(physical_devices[device_index], &properties);
      if (strstr(properties.deviceName, "TeraScale 1") == NULL) {
         continue;
      }
      ++terascale_1_devices_checked;
      fprintf(stderr, "found %s (vendor=0x%04x device=0x%04x)\n", properties.deviceName,
              properties.vendorID, properties.deviceID);

      if (properties.vendorID != 0x1002) {
         fprintf(stderr, "  vendorID is 0x%04x, expected 0x1002 (ATI/AMD)\n", properties.vendorID);
         ++failures;
      }
      if (properties.apiVersion == 0) {
         fprintf(stderr, "  apiVersion is 0\n");
         ++failures;
      }

      float const priority = 1.0F;
      VkDeviceQueueCreateInfo const queue_info = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
         .queueFamilyIndex = 0,
         .queueCount = 1,
         .pQueuePriorities = &priority,
      };
      VkDeviceCreateInfo const device_info = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .queueCreateInfoCount = 1,
         .pQueueCreateInfos = &queue_info,
      };
      VkDevice device;
      VkResult const create_result =
         vkCreateDevice(physical_devices[device_index], &device_info, NULL, &device);
      if (device_name_is_r700(properties.deviceName)) {
         if (create_result != VK_SUCCESS) {
            fprintf(stderr, "  vkCreateDevice failed with %d on hardware-validated R700\n",
                    create_result);
            ++failures;
         } else {
            fprintf(stderr, "  minimal R700 vkCreateDevice succeeded\n");

            VkQueue queue;
            vkGetDeviceQueue(device, 0, 0, &queue);
            VkSubmitInfo const empty_submit = {
               .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            };
            VkResult const submit_result = vkQueueSubmit(queue, 1, &empty_submit, VK_NULL_HANDLE);
            if (submit_result != VK_ERROR_DEVICE_LOST) {
               fprintf(
                  stderr,
                  "  guarded R700 queue submission returned %d, expected VK_ERROR_DEVICE_LOST\n",
                  submit_result);
               ++failures;
            } else {
               fprintf(stderr, "  R700 queue submission remains safely disabled\n");
            }
         }
      } else if (create_result == VK_SUCCESS) {
         fprintf(stderr, "  vkCreateDevice unexpectedly succeeded on unvalidated TeraScale 1 %s\n",
                 properties.deviceName);
         ++failures;
      } else if (create_result != VK_ERROR_INITIALIZATION_FAILED) {
         fprintf(stderr,
                 "  vkCreateDevice failed with %d, expected VK_ERROR_INITIALIZATION_FAILED\n",
                 create_result);
         ++failures;
      }
      if (create_result == VK_SUCCESS) {
         vkDestroyDevice(device, NULL);
      }
   }

   if (terascale_1_devices_checked == 0) {
      printf("terascale_1_enumeration: no TeraScale 1 device present on this machine, nothing to "
             "check, PASS\n");
   } else {
      printf("terascale_1_enumeration: checked=%u bad=%u %s\n", terascale_1_devices_checked,
             failures, failures == 0 ? "PASS" : "FAIL");
   }

   vkDestroyInstance(instance, NULL);
   return failures == 0 ? 0 : 1;
}
