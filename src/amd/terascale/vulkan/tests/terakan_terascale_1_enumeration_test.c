/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* TeraScale 1 (R600/R700) physical devices enumerate and report properties, but vkCreateDevice
 * refuses them cleanly: the hardware register configuration (SQ/CB/DB state setup, command stream
 * building) for that generation does not exist yet, only R8xx/Evergreen-and-later's does. This is
 * the first step of porting Terakan to R700, ported no further than this yet -- see the comment on
 * terakan_physical_device_chip_info::is_terascale_1 and terakan_CreateDevice.
 *
 * Meaningful only on a machine with a TeraScale 1 card actually installed; there is no requirement
 * that one be present; the check machines this project has been developed on so far have none.
 * When none is found, this reports that plainly and passes, since there is nothing to check.
 */

#include <vulkan/vulkan.h>

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
      if (create_result == VK_SUCCESS) {
         /* Not implemented yet, so success here would mean it silently started submitting command
          * streams built from state that was never actually computed for this generation --
          * exactly what terakan_CreateDevice's refusal exists to prevent.
          */
         fprintf(stderr, "  vkCreateDevice unexpectedly succeeded on %s\n", properties.deviceName);
         vkDestroyDevice(device, NULL);
         ++failures;
      } else if (create_result != VK_ERROR_INITIALIZATION_FAILED) {
         fprintf(stderr, "  vkCreateDevice failed with %d, expected VK_ERROR_INITIALIZATION_FAILED\n",
                 create_result);
         ++failures;
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
