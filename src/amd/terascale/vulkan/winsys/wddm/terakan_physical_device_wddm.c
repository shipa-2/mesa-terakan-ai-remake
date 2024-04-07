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
#include "terakan_wddm_d3dkmthk.h"

#include "gallium/drivers/r600/evergreend.h"
#include "util/list.h"
#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_log.h"

#include <assert.h>
#include <dxgi.h>
#include <stdint.h>

static void
terakan_physical_device_wddm_destroy(struct terakan_physical_device * const device_base)
{
   struct terakan_physical_device_wddm * const device =
      container_of(device_base, struct terakan_physical_device_wddm, base);

   D3DKMT_CLOSEADAPTER const close_adapter_arguments = {.hAdapter = device->d3dkmt_adapter};
   D3DKMTCloseAdapter(&close_adapter_arguments);

   terakan_physical_device_finish(&device->base);

   vk_free(&device->base.vk.instance->alloc, device);
}

static VkResult
terakan_physical_device_wddm_try_create(struct terakan_instance * const instance,
                                        IDXGIAdapter * const dxgi_adapter,
                                        struct terakan_physical_device ** const device_out)
{
   VkResult result;

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

   struct terakan_physical_device_wddm * const device =
      vk_alloc(&instance->vk.alloc, sizeof(struct terakan_physical_device_wddm),
               alignof(struct terakan_physical_device_wddm), VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (device == NULL) {
      return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   D3DKMT_OPENADAPTERFROMLUID open_adapter_from_luid_arguments = {
      .AdapterLuid = adapter_desc.AdapterLuid,
   };
   NTSTATUS const open_adapter_from_luid_status =
      D3DKMTOpenAdapterFromLuid(&open_adapter_from_luid_arguments);
   if (!NT_SUCCESS(open_adapter_from_luid_status)) {
      result = vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                         "Failed to open the D3DKMT adapter, status 0x%08lX",
                         open_adapter_from_luid_status);
      goto fail_alloc;
   }
   device->d3dkmt_adapter = open_adapter_from_luid_arguments.hAdapter;

   uint32_t adapter_driver_private_data[0x10D8] = {};
   D3DKMT_QUERYADAPTERINFO const adapter_driver_private_data_query = {
      .hAdapter = device->d3dkmt_adapter,
      .Type = KMTQAITYPE_UMDRIVERPRIVATE,
      .pPrivateDriverData = adapter_driver_private_data,
      .PrivateDriverDataSize = sizeof(adapter_driver_private_data),
   };
   NTSTATUS const adapter_driver_private_data_query_status =
      D3DKMTQueryAdapterInfo(&adapter_driver_private_data_query);
   if (!NT_SUCCESS(adapter_driver_private_data_query_status)) {
      result = vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                         "Failed to query D3DKMT adapter driver private data, status 0x%08lX",
                         adapter_driver_private_data_query_status);
      goto fail_d3dkmt_adapter;
   }

   UINT const vram_host_visible_offset_dwords = 0x4E4;
   UINT const vram_non_host_visible_offset_dwords = 0x4E6;
   VkDeviceSize const vram_host_visible_bytes =
      (VkDeviceSize)(adapter_driver_private_data[vram_host_visible_offset_dwords] |
                     ((uint64_t)adapter_driver_private_data[vram_host_visible_offset_dwords + 1]
                      << 32));
   /* TODO(Triang3l): Remove ASSERTED when the base initialization method is called. */
   ASSERTED VkDeviceSize const vram_bytes =
      vram_host_visible_bytes +
      (VkDeviceSize)(adapter_driver_private_data[vram_non_host_visible_offset_dwords] |
                     ((uint64_t)adapter_driver_private_data[vram_non_host_visible_offset_dwords + 1]
                      << 32));
   assert(vram_bytes == adapter_desc.DedicatedVideoMemory);

   uint32_t const mc_arb_ramcfg = adapter_driver_private_data[0x558];
   uint32_t const gb_addr_config = adapter_driver_private_data[0x559];
   /* Also GB_BACKEND_MAP = adapter_driver_private_data[0x55A]. */
   /* TODO(Triang3l): Remove UNUSED when the base initialization method is called. */
   UNUSED struct terakan_physical_device_tiling_info const tiling_info = {
      .pipes_log2 = G_0098F8_NUM_PIPES(gb_addr_config),
      .banks_log2 = 2 + G_002760_NOOFBANK(mc_arb_ramcfg),
      /* TODO(Triang3l): Handle MC_ARB_RAMCFG::NOOFRANK in image surface logic and pass it because
       * it's not exposed by DRM Radeon 2.50.0, but is available on Windows.
       */
      .pipe_interleave_bytes_log2 = 8 + G_0098F8_PIPE_INTERLEAVE_SIZE(gb_addr_config),
      .bank_interleave_log2 = G_0098F8_BANK_INTERLEAVE_SIZE(gb_addr_config),
      .row_bytes_log2 = 10 + G_0098F8_ROW_SIZE(gb_addr_config),
   };

   /* TODO(Triang3l): Remove test quitting. */
   result = VK_ERROR_INCOMPATIBLE_DRIVER;
   goto fail_d3dkmt_adapter;

#if 0
   *device_out = &device->base.vk;
   return VK_SUCCESS;
#endif

fail_d3dkmt_adapter:
   D3DKMT_CLOSEADAPTER const close_adapter_arguments = {.hAdapter = device->d3dkmt_adapter};
   D3DKMTCloseAdapter(&close_adapter_arguments);
fail_alloc:
   vk_free(&instance->vk.alloc, device);
   return result;
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
