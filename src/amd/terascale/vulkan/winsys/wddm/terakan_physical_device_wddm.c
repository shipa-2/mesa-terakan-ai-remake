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

#include "terakan_device_wddm.h"
#include "terakan_instance.h"
#include "terakan_queue_wddm.h"
#include "terakan_sync_completion.h"
#include "terakan_wddm_d3dkmthk.h"

#include "gallium/drivers/r600/evergreend.h"
#include "util/list.h"
#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_log.h"

#include <assert.h>
#include <dxgi.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>

static void
terakan_physical_device_wddm_get_winsys_extensions(
   struct terakan_physical_device const * const device_base,
   struct vk_device_extension_table * const extensions, UNUSED struct vk_features * const features,
   struct vk_properties * const properties)
{
   struct terakan_physical_device_wddm const * const device =
      container_of(device_base, struct terakan_physical_device_wddm const, base);

   uint32_t const pci_domain = 0;

   /* VK_KHR_external_memory_capabilities (#72, Vulkan 1.1, instance). */
   /* Same as ac_compute_device_uuid as of July 2023. */
   uint32_t const device_uuid_u32[] = {
      pci_domain,
      device->adapter_address.BusNumber,
      device->adapter_address.DeviceNumber,
      device->adapter_address.FunctionNumber,
   };
   static_assert(sizeof(device_uuid_u32) <= sizeof(properties->deviceUUID),
                 "Computed device UUID must fit into the Vulkan UUID field.");
   memcpy(properties->deviceUUID, device_uuid_u32, sizeof(device_uuid_u32));
   static_assert(
      sizeof(LUID) == VK_LUID_SIZE,
      "Using memcpy to copy the adapter LUID, the size must match between WDDM and Vulkan.");
   memcpy(properties->deviceLUID, &device->adapter_luid, VK_LUID_SIZE);

   /* VK_EXT_pci_bus_info (#213). */
   extensions->EXT_pci_bus_info = true;
   properties->pciDomain = pci_domain;
   properties->pciBus = device->adapter_address.BusNumber;
   properties->pciDevice = device->adapter_address.DeviceNumber;
   properties->pciFunction = device->adapter_address.FunctionNumber;
}

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

static struct terakan_physical_device_winsys_fn const terakan_physical_device_wddm_fn = {
   .get_winsys_extensions = terakan_physical_device_wddm_get_winsys_extensions,
   .create_device = terakan_device_wddm_create,
   .destroy = terakan_physical_device_wddm_destroy,
};

static VkResult
terakan_physical_device_wddm_try_create(struct terakan_instance * const instance,
                                        IDXGIAdapter * const dxgi_adapter,
                                        struct terakan_physical_device_wddm ** const device_out)
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

   device->adapter_luid = adapter_desc.AdapterLuid;

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

   D3DKMT_QUERYADAPTERINFO const adapter_address_query = {
      .hAdapter = device->d3dkmt_adapter,
      .Type = KMTQAITYPE_ADAPTERADDRESS,
      .pPrivateDriverData = &device->adapter_address,
      .PrivateDriverDataSize = sizeof(device->adapter_address),
   };
   NTSTATUS const adapter_address_query_status = D3DKMTQueryAdapterInfo(&adapter_address_query);
   if (!NT_SUCCESS(adapter_address_query_status)) {
      result = vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                         "Failed to query the D3DKMT adapter PCI address, status 0x%08lX",
                         adapter_address_query_status);
      goto fail_d3dkmt_adapter;
   }

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
                         "Failed to query the D3DKMT adapter driver private data, status 0x%08lX",
                         adapter_driver_private_data_query_status);
      goto fail_d3dkmt_adapter;
   }

   /* TODO(Triang3l): Verify the memory amounts on linked physical adapters and multi-GPU graphics
    * cards.
    */
   UINT const vram_host_visible_offset_dwords = 0x4E4;
   UINT const vram_non_host_visible_offset_dwords = 0x4E6;
   VkDeviceSize const vram_host_visible_bytes =
      (VkDeviceSize)(adapter_driver_private_data[vram_host_visible_offset_dwords] |
                     ((uint64_t)adapter_driver_private_data[vram_host_visible_offset_dwords + 1]
                      << 32));
   VkDeviceSize const vram_bytes =
      vram_host_visible_bytes +
      (VkDeviceSize)(adapter_driver_private_data[vram_non_host_visible_offset_dwords] |
                     ((uint64_t)adapter_driver_private_data[vram_non_host_visible_offset_dwords + 1]
                      << 32));
   assert(vram_bytes == adapter_desc.DedicatedVideoMemory);

   uint32_t const mc_arb_ramcfg = adapter_driver_private_data[0x558];
   uint32_t const gb_addr_config = adapter_driver_private_data[0x559];
   /* Also GB_BACKEND_MAP = adapter_driver_private_data[0x55A]. */
   struct terakan_physical_device_tiling_info const tiling_info = {
      .pipes_log2 = G_0098F8_NUM_PIPES(gb_addr_config),
      .banks_log2 = 2 + G_002760_NOOFBANK(mc_arb_ramcfg),
      /* TODO(Triang3l): Handle MC_ARB_RAMCFG::NOOFRANK in image surface logic and pass it because
       * it's not exposed by DRM Radeon 2.50.0, but is available on Windows.
       */
      .pipe_interleave_bytes_log2 = 8 + G_0098F8_PIPE_INTERLEAVE_SIZE(gb_addr_config),
      .bank_interleave_log2 = G_0098F8_BANK_INTERLEAVE_SIZE(gb_addr_config),
      .row_bytes_log2 = 10 + G_0098F8_ROW_SIZE(gb_addr_config),
   };

   struct terakan_physical_device_submission_info_gfx const submission_info_gfx = {
      .base =
         {
            .relocation_type = TERAKAN_QUEUE_RELOCATION_TYPE_WDDM_PATCH,
            .submission_outer_reserved_amount =
               {
                  .bo_references = TERAKAN_QUEUE_WDDM_SUBMISSION_RESERVED_BO_REFERENCES,
                  .relocations = TERAKAN_QUEUE_WDDM_SUBMISSION_RESERVED_RELOCATIONS,
               },
         },
      .need_sq_alu_const_mode_control = true,
   };

   uint32_t const clock_crystal_frequency_hz = adapter_driver_private_data[0x4E1];

   size_t sync_type_count = 0;
   assert(sync_type_count < ARRAY_SIZE(device->sync_types));
   device->sync_types[sync_type_count++] = &terakan_sync_completion_type;
   device->sync_type_binary = vk_sync_binary_get_type(&terakan_sync_completion_type);
   assert(sync_type_count < ARRAY_SIZE(device->sync_types));
   device->sync_types[sync_type_count++] = &device->sync_type_binary.sync;
   assert(sync_type_count < ARRAY_SIZE(device->sync_types));
   device->sync_types[sync_type_count++] = NULL;

   SYSTEM_INFO system_info;
   GetSystemInfo(&system_info);
   result = terakan_physical_device_init(
      &device->base, instance, &terakan_physical_device_wddm_fn, adapter_desc.DeviceId,
      system_info.dwAllocationGranularity, (VkDeviceSize)adapter_desc.SharedSystemMemory,
      vram_bytes, vram_host_visible_bytes, UINT32_MAX & ~(system_info.dwAllocationGranularity - 1),
      system_info.dwAllocationGranularity, &tiling_info, &submission_info_gfx,
      clock_crystal_frequency_hz, device->sync_types);
   if (result != VK_SUCCESS) {
      goto fail_d3dkmt_adapter;
   }

   *device_out = device;
   return VK_SUCCESS;

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
      struct terakan_physical_device_wddm * physical_device;
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
      list_addtail(&physical_device->base.vk.link, &instance->vk.physical_devices.list);
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
