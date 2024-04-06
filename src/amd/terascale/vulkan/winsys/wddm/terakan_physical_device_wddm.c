/*
 * Copyright © 2024 Vitaliy Triang3l Kuzmin
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

#include "terakan_physical_device_wddm.h"

#include "terakan_instance.h"

#include "util/list.h"
#include "util/macros.h"
#include "vk_log.h"

#include <dxgi.h>

static VkResult
terakan_physical_device_wddm_try_create(struct terakan_instance * const instance,
                                        IDXGIAdapter * const dxgi_adapter,
                                        struct terakan_physical_device ** const device_out)
{
   DXGI_ADAPTER_DESC adapter_desc;
   HRESULT const adapter_get_desc_result =
      dxgi_adapter->lpVtbl->GetDesc(dxgi_adapter, &adapter_desc);
   if (FAILED(adapter_get_desc_result)) {
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "Failed to get the DXGI adapter description, result 0x%08lX",
                       adapter_get_desc_result);
   }

   /* Check if the GPU is supported. */
   if (adapter_desc.VendorId != TERAKAN_PHYSICAL_DEVICE_VENDOR_ID_ATI) {
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }
   enum radeon_family const chip_family =
      terakan_physical_device_get_chip_family(adapter_desc.DeviceId);
   if (!terakan_physical_device_is_chip_family_supported(chip_family)) {
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }
   /* TODO(Triang3l): Research and implement R9xx support and virtual memory usage. */
   if (chip_family >= CHIP_CAYMAN) {
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }
   /* TODO(Triang3l): Research allocation on chips with shared memory. */
   switch (chip_family) {
   case CHIP_PALM:
   case CHIP_SUMO:
   case CHIP_SUMO2:
   case CHIP_ARUBA:
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   default:
      break;
   }

   /* TODO(Triang3l): Create the physical device. */

   return VK_ERROR_INCOMPATIBLE_DRIVER;
}

VkResult
terakan_physical_device_wddm_enumerate(struct vk_instance * const instance_base)
{
   VkResult result = VK_SUCCESS;

   struct terakan_instance * const instance =
      container_of(instance_base, struct terakan_instance, vk);

   static IID const IID_IDXGIFactory = {0x7b7166ec,
                                        0x21c7,
                                        0x44ae,
                                        {0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69}};
   IDXGIFactory * dxgi_factory;
   HRESULT const dxgi_factory_create_result = CreateDXGIFactory(&IID_IDXGIFactory, &dxgi_factory);
   if (FAILED(dxgi_factory_create_result)) {
      return vk_errorf(
         instance,
         dxgi_factory_create_result == E_OUTOFMEMORY ? VK_ERROR_OUT_OF_HOST_MEMORY
                                                     : VK_ERROR_INITIALIZATION_FAILED,
         "Failed to create a DXGI factory to enumerate Terakan-compatible adapters, result 0x%08lX",
         dxgi_factory_create_result);
   }

   IDXGIAdapter * dxgi_adapter;
   for (UINT adapter_index = 0;
        SUCCEEDED(dxgi_factory->lpVtbl->EnumAdapters(dxgi_factory, adapter_index, &dxgi_adapter));
        ++adapter_index) {
      struct terakan_physical_device * physical_device;
      result = terakan_physical_device_wddm_try_create(instance, dxgi_adapter, &physical_device);
      dxgi_adapter->lpVtbl->Release(dxgi_adapter);
      if (result == VK_ERROR_INCOMPATIBLE_DRIVER) {
         /* Adapter not compatible with Terakan, skip. */
         result = VK_SUCCESS;
         continue;
      }
      if (result != VK_SUCCESS) {
         /* Error creating the physical device, report the error. */
         break;
      }
      list_addtail(&physical_device->vk.link, &instance->vk.physical_devices.list);
   }

   dxgi_factory->lpVtbl->Release(dxgi_factory);

   if (result != VK_SUCCESS) {
      list_for_each_entry_safe (struct vk_physical_device, physical_device,
                                &instance->vk.physical_devices.list, link) {
         list_del(&physical_device->link);
         instance->vk.physical_devices.destroy(physical_device);
      }
   }

   return result;
}
