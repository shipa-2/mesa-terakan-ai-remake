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

#include "terakan_bo_wddm.h"

#include "terakan_device_wddm.h"
#include "terakan_wddm_d3dkmthk.h"

#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_log.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

enum {
   TERAKAN_BO_WDDM_MEMORY_TYPE_NONE = 0,
   TERAKAN_BO_WDDM_MEMORY_TYPE_DEVICE_HOST_VISIBLE = 1,
   TERAKAN_BO_WDDM_MEMORY_TYPE_DEVICE_NON_HOST_VISIBLE = 2,
   TERAKAN_BO_WDDM_MEMORY_TYPE_HOST_WRITE_COMBINED = 3,
   TERAKAN_BO_WDDM_MEMORY_TYPE_HOST_CACHED = 4,
};

static bool
terakan_bo_wddm_set_tiling(UNUSED struct terakan_bo * const bo_base,
                           UNUSED struct terakan_bo_tiling const * const tiling)
{
   /* TODO(Triang3l): Pass dedicated allocation info to allocation creation since tiling is likely
    * specified in private data at allocation creation time. For now this empty implementation is
    * there just not to crash because set_tiling is done for all dedicated allocations.
    */
   return true;
}

static void *
terakan_bo_wddm_map_impl(struct terakan_bo * const bo_base)
{
   struct terakan_bo_wddm const * const bo =
      container_of(bo_base, struct terakan_bo_wddm const, base);
   struct terakan_device_wddm const * const device =
      container_of(bo->base.device, struct terakan_device_wddm const, base);

   /* Arguments matching what is sent in the IgnoreSync case during Direct3D 11 execution. */
   D3DKMT_LOCK lock_arguments = {
      .hDevice = device->d3dkmt_device,
      .hAllocation = bo->allocation,
      .Flags =
         {
            .DonotWait = 1,
            .IgnoreSync = 1,
         },
   };

   NTSTATUS const lock_status = D3DKMTLock(&lock_arguments);
   if (!NT_SUCCESS(lock_status)) {
      vk_loge(VK_LOG_OBJS(device), "Failed to lock a D3DKMT memory allocation");
      return NULL;
   }

   return lock_arguments.pData;
}

static void
terakan_bo_wddm_unmap_impl(struct terakan_bo * const bo_base)
{
   struct terakan_bo_wddm const * const bo =
      container_of(bo_base, struct terakan_bo_wddm const, base);
   struct terakan_device_wddm const * const device =
      container_of(bo->base.device, struct terakan_device_wddm const, base);

   D3DKMT_UNLOCK const unlock_arguments = {
      .hDevice = device->d3dkmt_device,
      .NumAllocations = 1,
      .phAllocations = &bo->allocation,
   };
   D3DKMTUnlock(&unlock_arguments);
}

static void
terakan_bo_wddm_create_reference(void * const bo_reference_ptr,
                                 struct terakan_bo const * const bo_base,
                                 UNUSED bool const is_reading, bool const is_writing,
                                 UNUSED enum terakan_bo_priority const priority)
{
   struct terakan_bo_wddm const * const bo =
      container_of(bo_base, struct terakan_bo_wddm const, base);

   *(D3DDDI_ALLOCATIONLIST *)bo_reference_ptr = (D3DDDI_ALLOCATIONLIST){
      .hAllocation = bo->allocation,
      .WriteOperation = (UINT)is_writing,
   };
}

static void
terakan_bo_wddm_update_reference(void * const bo_reference_ptr,
                                 struct terakan_bo const * const bo_base,
                                 UNUSED bool const is_reading, bool const is_writing,
                                 UNUSED enum terakan_bo_priority const priority)
{
   D3DDDI_ALLOCATIONLIST * const bo_reference = (D3DDDI_ALLOCATIONLIST *)bo_reference_ptr;

   struct terakan_bo_wddm const * const bo =
      container_of(bo_base, struct terakan_bo_wddm const, base);

   assert(bo_reference->hAllocation == bo->allocation);

   if (is_writing) {
      bo_reference->WriteOperation = 1;
   }
}

static void
terakan_bo_wddm_free_impl(struct terakan_bo * const bo_base,
                          VkAllocationCallbacks const * const allocator)
{
   struct terakan_bo_wddm * const bo = container_of(bo_base, struct terakan_bo_wddm, base);
   struct terakan_device_wddm const * const device =
      container_of(bo->base.device, struct terakan_device_wddm const, base);

   D3DKMT_DESTROYALLOCATION const destroy_allocation_arguments = {
      .hDevice = device->d3dkmt_device,
      .phAllocationList = &bo->allocation,
      .AllocationCount = 1,
   };
   D3DKMTDestroyAllocation(&destroy_allocation_arguments);

   vk_free2(&device->base.vk.alloc, allocator, bo);
}

/* Can't be used with TERAKAN_BO_WDDM_MEMORY_TYPE_DEVICE_NON_HOST_VISIBLE among the allocation's
 * memory types.
 */
#define TERAKAN_BO_WDDM_ALLOCATION_PRIVATE_DATA_MEMORY_TYPE_FLAG_HOST_VISIBLE ((uint32_t)1 << 29)

struct terakan_bo_wddm_allocation_private_data_header {
   uint32_t private_data_size_bytes;

   /* 0x1 with only the header and terakan_bo_wddm_allocation_private_data_unknown_struct in
    * the end.
    * 0x1 | 0xC for private data with some additional data (possibly Direct3D 11 resource
    * information) in between.
    */
   uint32_t flags;

   uint32_t unknown_0x8_0x80;
   uint32_t unknown_0xC_0;

   /* The lower bits are OR of (1 << (each memory type - 1)), not including unused (0) memory types.
    * May include TERAKAN_BO_WDDM_ALLOCATION_PRIVATE_DATA_MEMORY_TYPE_FLAG_HOST_VISIBLE.
    */
   uint32_t memory_type_flags;

   uint32_t unknown_0x14_0;

   uint32_t alignment_bytes;

   /* 0x5 for the basic header + unknown_struct.
    * See terakan_bo_wddm_allocation_private_data comments for additional bits.
    */
   uint32_t struct_flags;

   uint8_t memory_type_priority[4];
   uint32_t preferred_memory_type;

   uint32_t unknown_0x28_0[0x10];

   /* Not aligned to the allocation alignment, although in Direct3D 11 may be padded to the
    * resource's size requirement (just like naturally in Vulkan).
    */
   uint32_t size_bytes;

   /* For basic header + unknown_struct private data, zeros, but contains some values for private
    * data with other structures.
    */
   uint32_t unknown_0x6C[0x11];

   uint32_t unknown_struct_offset_bytes;

   uint32_t unknown_0xB4_0[0x4];
};

static_assert(
   sizeof(struct terakan_bo_wddm_allocation_private_data_header) == 0xC4,
   "The sizes of private data structures must match those expected by the kernel-mode driver.");

struct terakan_bo_wddm_allocation_private_data_unknown_struct {
   uint32_t struct_size_bytes;

   uint32_t unknown_0x4_0[0x44];
};

static_assert(
   sizeof(struct terakan_bo_wddm_allocation_private_data_unknown_struct) == 0x114,
   "The sizes of private data structures must match those expected by the kernel-mode driver.");

struct terakan_bo_wddm_create_allocation_private_driver_data {
   uint32_t unknown_0x0_0[0x3];
   uint32_t unknown_0xC_0x78;
   uint32_t unknown_0x10_0[0xC];
};

static_assert(
   sizeof(struct terakan_bo_wddm_create_allocation_private_driver_data) == 0x40,
   "The sizes of private data structures must match those expected by the kernel-mode driver.");

static VkResult
terakan_bo_wddm_allocate_device_memory(
   struct terakan_device * const device_base, VkDeviceSize const size, VkDeviceSize const alignment,
   VkMemoryPropertyFlags const flags,
   UNUSED VkExternalMemoryHandleTypeFlags const export_handle_types,
   VkAllocationCallbacks const * const allocator, VkSystemAllocationScope const scope,
   struct terakan_bo ** const bo_out)
{
   struct terakan_device_wddm * const device =
      container_of(device_base, struct terakan_device_wddm, base);

   struct terakan_bo_wddm * const bo =
      vk_alloc2(&device->base.vk.alloc, allocator, sizeof(struct terakan_bo_wddm),
                alignof(struct terakan_bo_wddm), scope);
   if (bo == NULL) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   alignas(uint32_t) char
      allocation_private_data[sizeof(struct terakan_bo_wddm_allocation_private_data_header) +
                              sizeof(struct terakan_bo_wddm_allocation_private_data_unknown_struct)];
   uint32_t allocation_private_data_size_bytes = 0;

   assert(sizeof(allocation_private_data) - allocation_private_data_size_bytes >=
          sizeof(struct terakan_bo_wddm_allocation_private_data_header));
   struct terakan_bo_wddm_allocation_private_data_header * const allocation_private_data_header =
      (struct terakan_bo_wddm_allocation_private_data_header *)(allocation_private_data +
                                                                allocation_private_data_size_bytes);
   allocation_private_data_size_bytes +=
      sizeof(struct terakan_bo_wddm_allocation_private_data_header);

   *allocation_private_data_header = (struct terakan_bo_wddm_allocation_private_data_header){
      .flags = 0b1,
      .unknown_0x8_0x80 = 0x80,
      .alignment_bytes = (uint32_t)alignment,
      .struct_flags = 0x5,
      .size_bytes = (uint32_t)size,
   };

   if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      allocation_private_data_header->memory_type_flags |=
         TERAKAN_BO_WDDM_ALLOCATION_PRIVATE_DATA_MEMORY_TYPE_FLAG_HOST_VISIBLE;
      if (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) {
         /* Matching a D3D11_USAGE_STAGING allocation. */
         allocation_private_data_header->memory_type_priority[0] =
            TERAKAN_BO_WDDM_MEMORY_TYPE_HOST_CACHED;
      } else {
         if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            /* Matching a small D3D11_USAGE_DYNAMIC allocation. */
            allocation_private_data_header->memory_type_priority[0] =
               TERAKAN_BO_WDDM_MEMORY_TYPE_DEVICE_HOST_VISIBLE;
            allocation_private_data_header->memory_type_priority[1] =
               TERAKAN_BO_WDDM_MEMORY_TYPE_DEVICE_HOST_VISIBLE;
            allocation_private_data_header->memory_type_priority[2] =
               TERAKAN_BO_WDDM_MEMORY_TYPE_HOST_WRITE_COMBINED;
            allocation_private_data_header->memory_type_priority[3] =
               TERAKAN_BO_WDDM_MEMORY_TYPE_HOST_CACHED;
         } else {
            allocation_private_data_header->memory_type_priority[0] =
               TERAKAN_BO_WDDM_MEMORY_TYPE_HOST_WRITE_COMBINED;
            allocation_private_data_header->memory_type_priority[1] =
               TERAKAN_BO_WDDM_MEMORY_TYPE_HOST_CACHED;
         }
      }
   } else {
      /* Matching a D3D11_USAGE_DEFAULT allocation. */
      allocation_private_data_header->memory_type_priority[0] =
         TERAKAN_BO_WDDM_MEMORY_TYPE_DEVICE_NON_HOST_VISIBLE;
      allocation_private_data_header->memory_type_priority[1] =
         TERAKAN_BO_WDDM_MEMORY_TYPE_DEVICE_HOST_VISIBLE;
      allocation_private_data_header->memory_type_priority[2] =
         TERAKAN_BO_WDDM_MEMORY_TYPE_HOST_WRITE_COMBINED;
      allocation_private_data_header->memory_type_priority[3] =
         TERAKAN_BO_WDDM_MEMORY_TYPE_HOST_CACHED;
   }
   uint32_t used_memory_types = 0b0;
   for (size_t memory_type_priority_index = 0;
        memory_type_priority_index <
        ARRAY_SIZE(allocation_private_data_header->memory_type_priority);
        ++memory_type_priority_index) {
      used_memory_types |= (uint32_t)1 << allocation_private_data_header
                                             ->memory_type_priority[memory_type_priority_index];
   }
   /* Flags expected by the kernel-mode driver are for memory types minus 1 (not including the
    * unused types).
    */
   allocation_private_data_header->memory_type_flags |= used_memory_types >> 1;
   allocation_private_data_header->preferred_memory_type =
      allocation_private_data_header->memory_type_priority[0];

   /* Direct3D 11 buffers have a 0x60-byte structure in between indicated by the structure flag
    * 0x200.
    * It contains the buffer size in multiple places (in some matching the allocation size that's
    * aligned to the requirement of the bind flags, like to 256 bytes for an unordered access
    * buffer, in some being the original size specified in the buffer description) and some unknown
    * data.
    * Some values near the end of the header are also nonzero with it.
    *
    * Direct3D 11 textures have some structure in between containing information about each mip
    * level (appears to be 0x60 bytes per level), and some other structure flags (including 0x200)
    * are also set for them.
    */

   allocation_private_data_header->unknown_struct_offset_bytes = allocation_private_data_size_bytes;
   assert(sizeof(allocation_private_data) - allocation_private_data_size_bytes >=
          sizeof(struct terakan_bo_wddm_allocation_private_data_unknown_struct));
   struct terakan_bo_wddm_allocation_private_data_unknown_struct * const
      allocation_private_data_unknown_struct =
         (struct terakan_bo_wddm_allocation_private_data_unknown_struct *)(allocation_private_data +
                                                                           allocation_private_data_size_bytes);
   allocation_private_data_size_bytes +=
      sizeof(struct terakan_bo_wddm_allocation_private_data_unknown_struct);

   *allocation_private_data_unknown_struct =
      (struct terakan_bo_wddm_allocation_private_data_unknown_struct){
         .struct_size_bytes = sizeof(*allocation_private_data_unknown_struct),
      };

   allocation_private_data_header->private_data_size_bytes = allocation_private_data_size_bytes;

   D3DDDI_ALLOCATIONINFO allocation_info = {
      .pPrivateDriverData = allocation_private_data,
      .PrivateDriverDataSize = allocation_private_data_size_bytes,
   };

   struct terakan_bo_wddm_create_allocation_private_driver_data const
      create_allocation_private_driver_data = {
         .unknown_0xC_0x78 = 0x78,
      };
   D3DKMT_CREATEALLOCATION create_allocation_arguments = {
      .hDevice = device->d3dkmt_device,
      .pPrivateDriverData = &create_allocation_private_driver_data,
      .PrivateDriverDataSize = sizeof(create_allocation_private_driver_data),
      .NumAllocations = 1,
      .pAllocationInfo = &allocation_info,
   };
   NTSTATUS const create_allocation_status = D3DKMTCreateAllocation(&create_allocation_arguments);
   if (!NT_SUCCESS(create_allocation_status)) {
      vk_free2(&device->base.vk.alloc, allocator, bo);
      vk_loge(VK_LOG_OBJS(device),
              "Failed to create a D3DKMT memory allocation, size: %" PRIu64 " bytes, "
              "alignment: %" PRIu64 " bytes, memory property flags: 0x%" PRIX32 ", status 0x%08lX",
              size, alignment, flags, create_allocation_status);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   terakan_bo_init(&bo->base, &device->base);

   bo->allocation = allocation_info.hAllocation;

   *bo_out = &bo->base;
   return VK_SUCCESS;
}

struct terakan_bo_winsys_fn const terakan_bo_wddm_fn = {
   .set_tiling = terakan_bo_wddm_set_tiling,
   .map_impl = terakan_bo_wddm_map_impl,
   .unmap_impl = terakan_bo_wddm_unmap_impl,
   .create_reference = terakan_bo_wddm_create_reference,
   .update_reference = terakan_bo_wddm_update_reference,
   .free_impl = terakan_bo_wddm_free_impl,
   .allocate_device_memory = terakan_bo_wddm_allocate_device_memory,
};
