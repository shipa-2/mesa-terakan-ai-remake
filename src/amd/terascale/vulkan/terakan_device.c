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

#include "meta/terakan_meta_nir.h"
#include "terakan_device.h"

#include "terakan_command_buffer.h"
#include "terakan_cp_dma.h"
#include "terakan_descriptor.h"
#include "terakan_entrypoints.h"
#include "terakan_format.h"
#include "terakan_physical_device.h"
#include "terakan_queue.h"

#include "gallium/drivers/r600/eg_sq.h"
#include "gallium/drivers/r600/r600_opcodes.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "vk_alloc.h"
#include "vk_cmd_enqueue_entrypoints.h"
#include "vk_common_entrypoints.h"
#include "vk_log.h"
#include "wsi_common.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void
terakan_device_finish(struct terakan_device * const device)
{
   if (device->queue_graphics != NULL) {
      terakan_queue_destroy(device->queue_graphics);
   }

   mtx_lock(&device->completion_mutex);
   device->completion_lost = true;
   mtx_unlock(&device->completion_mutex);
   cnd_broadcast(&device->completion_condition);

   cnd_destroy(&device->completion_condition);
   mtx_destroy(&device->completion_mutex);

   terakan_bo_free(device->meta_shaders_bo, NULL);

   terakan_bo_free(device->query_accumulator_bo, NULL);

   terakan_bo_free(device->uav_immediate_bo, NULL);

   terakan_bo_free(device->gfx_discard_bo, NULL);

   for (size_t reference_placeholder_bo_index = 0;
        reference_placeholder_bo_index < TERAKAN_QUEUE_BO_REFERENCE_PLACEHOLDER_INDEX_COUNT;
        ++reference_placeholder_bo_index) {
      terakan_bo_free(device->reference_placeholder_bos[reference_placeholder_bo_index], NULL);
   }

   vk_device_finish(&device->vk);
}

VKAPI_ATTR void VKAPI_CALL
terakan_DestroyDevice(VkDevice const deviceHandle, VkAllocationCallbacks const * const pAllocator)
{
   struct terakan_device * const device = terakan_device_from_handle(deviceHandle);

   if (device == NULL) {
      return;
   }

   device->winsys_fn->destroy(device);
}

VkResult
terakan_device_init(struct terakan_device * const device,
                    struct terakan_physical_device * const physical_device,
                    VkDeviceCreateInfo const * const create_info,
                    VkAllocationCallbacks const * const allocator,
                    struct terakan_device_winsys_fn const * const winsys_fn_static,
                    size_t const bo_reference_size, size_t const bo_reference_alignment)
{
   VkResult result;

   struct vk_device_dispatch_table dispatch_table;
   /* For secondary command buffer support, overwrite any command entrypoints in the main
    * device-level dispatch table with vk_cmd_enqueue_unless_primary_Cmd*.
    *
    * With no kernel support for chained indirect buffers (and thus having to re-emit all state
    * including up to 1024 resource bindings one by one), and with the need to merge address
    * relocations if inserting one indirect buffer into another, encoding secondary command buffers
    * into hardware packets wouldn't be a viable approach. Also, serializing vkCmdBindDescriptorSets
    * calls themselves may be a more compact way of representing them compared to expanding them
    * into updates of each individual binding register.
    */
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &vk_cmd_enqueue_unless_primary_device_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(&dispatch_table, &terakan_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(&dispatch_table, &wsi_device_entrypoints, false);

   result =
      vk_device_init(&device->vk, &physical_device->vk, &dispatch_table, create_info, allocator);
   if (result != VK_SUCCESS) {
      return result;
   }

   /* Populate the command dispatch table for secondary command buffer emulation. */
   vk_device_dispatch_table_from_entrypoints(&device->command_dispatch_table,
                                             &terakan_device_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(&device->command_dispatch_table,
                                             &vk_common_device_entrypoints, false);
   device->vk.command_dispatch_table = &device->command_dispatch_table;

   device->vk.command_buffer_ops = &terakan_command_buffer_ops;

   device->winsys_fn = winsys_fn_static;

   device->bo_reference_size = bo_reference_size;
   device->bo_reference_alignment = bo_reference_alignment;

   device->last_bo_creation_number = 0;

   size_t reference_placeholder_bos_created;
   for (reference_placeholder_bos_created = 0;
        reference_placeholder_bos_created < TERAKAN_QUEUE_BO_REFERENCE_PLACEHOLDER_INDEX_COUNT;
        ++reference_placeholder_bos_created) {
      /* 0x100 for base address alignment for 8-shifted addresses. */
      result = device->winsys_fn->bo->allocate_device_memory(
         device, 0x100, 0x100, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, NULL,
         VK_SYSTEM_ALLOCATION_SCOPE_DEVICE,
         &device->reference_placeholder_bos[reference_placeholder_bos_created]);
      if (result != VK_SUCCESS) {
         result = vk_errorf(physical_device->vk.instance, result,
                            "Failed to create allocation reference placeholder allocations");
         goto fail_reference_placeholder_bos;
      }
   }

   result = device->winsys_fn->bo->allocate_device_memory(
      device, MAX2(sizeof(uint32_t), TERAKAN_CP_DMA_COPY_OPTIMAL_ALIGNMENT * 2),
      MAX2(sizeof(uint32_t), TERAKAN_CP_DMA_COPY_OPTIMAL_ALIGNMENT),
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, NULL, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE,
      &device->gfx_discard_bo);
   if (result != VK_SUCCESS) {
      result =
         vk_errorf(physical_device->vk.instance, result,
                   "Failed to allocate memory for temporary data to discard on the graphics queue");
      goto fail_reference_placeholder_bos;
   }

   uint32_t uav_immediate_bo_bytes_shr8 = 0;
   for (unsigned element_bytes_log2 = 0; element_bytes_log2 <= 4; ++element_bytes_log2) {
      device->uav_immediate_va_shr8[element_bytes_log2] = uav_immediate_bo_bytes_shr8;
      uav_immediate_bo_bytes_shr8 +=
         DIV_ROUND_UP(physical_device->chip_info.uav_immediate_size_elements << element_bytes_log2,
                      (uint32_t)1 << 8);
   }
   /* R700 has no Evergreen RAT immediate-return storage. Keep one aligned page as an inert
    * bootstrap allocation so logical-device construction has valid BO bookkeeping while command
    * submission remains disabled for this bring-up stage. Do not invent a hardware element count.
    */
   VkDeviceSize const uav_immediate_bo_bytes =
      MAX2((VkDeviceSize)uav_immediate_bo_bytes_shr8 << 8, (VkDeviceSize)1 << 8);
   result = device->winsys_fn->bo->allocate_device_memory(
      device, uav_immediate_bo_bytes, (VkDeviceSize)1 << 8, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
      NULL, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE, &device->uav_immediate_bo);
   if (result != VK_SUCCESS) {
      result =
         vk_errorf(physical_device->vk.instance, result,
                   "Failed to allocate memory for unordered access view operation return values");
      goto fail_gfx_discard_bo;
   }
   uint32_t const uav_immediate_bo_va_shr8 = device->uav_immediate_bo->va >> 8;
   for (unsigned element_bytes_log2 = 0; element_bytes_log2 <= 4; ++element_bytes_log2) {
      device->uav_immediate_va_shr8[element_bytes_log2] += uav_immediate_bo_va_shr8;
   }

   /* Accessed via query sampling (never larger than a kcache line), kcache, and a UAV with 4 bytes
    * per element.
    */
   unsigned const query_accumulator_uav_bytes_per_element = sizeof(uint32_t);
   unsigned const query_accumulator_uav_base_granularity_log2 =
      terakan_color_descriptor_buffer_uav_base_granularity_log2(
         query_accumulator_uav_bytes_per_element, physical_device);
   result = device->winsys_fn->bo->allocate_device_memory(
      device,
      ALIGN_POT(TERAKAN_KCACHE_HW_LINE_BYTES,
                query_accumulator_uav_bytes_per_element *
                   terakan_format_pitch_alignment_linear_surfels(
                      query_accumulator_uav_bytes_per_element,
                      physical_device->tiling_info.pipe_interleave_bytes_log2)),
      1u << MAX2(TERAKAN_KCACHE_HW_LINE_BYTES_LOG2, query_accumulator_uav_base_granularity_log2),
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, NULL, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE,
      &device->query_accumulator_bo);
   if (result != VK_SUCCESS) {
      result = vk_errorf(physical_device->vk.instance, result,
                         "Failed to allocate memory for the query result accumulator");
      goto fail_uav_immediate_bo;
   }

   bool const is_r9xx = physical_device->chip_info.is_r9xx;

   /* Meta shaders built as NIR are compiled before the buffer is sized, because unlike the
    * hand-written ones their length is only known once the backend has run. See
    * meta/terakan_meta_nir.c.
    */
   struct terakan_shader_impl * const meta_nir_shaders =
      vk_zalloc(&device->vk.alloc, sizeof(*meta_nir_shaders) * TERAKAN_META_SHADER_COUNT,
                alignof(struct terakan_shader_impl), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (meta_nir_shaders == NULL) {
      result = vk_error(physical_device->vk.instance, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_query_accumulator_bo;
   }
   for (size_t meta_shader_index = 0; meta_shader_index < TERAKAN_META_SHADER_COUNT;
        ++meta_shader_index) {
      terakan_meta_nir_builder const builder = terakan_meta_nir_builders[meta_shader_index];
      if (builder == NULL) {
         continue;
      }
      result = terakan_meta_nir_compile(device, builder(device),
                                        &meta_nir_shaders[meta_shader_index]);
      if (result != VK_SUCCESS) {
         result = vk_errorf(physical_device->vk.instance, result,
                            "Failed to compile internal shader %zu from NIR", meta_shader_index);
         goto fail_meta_nir_shaders;
      }
   }

   /* The first shader is the empty fetch shader. */
   VkDeviceSize meta_shaders_bo_size =
      ALIGN_POT(sizeof(uint32_t) * 2, TERAKAN_SHADER_PROGRAM_ALIGNMENT_LOG2);
   for (size_t meta_shader_index = 0; meta_shader_index < TERAKAN_META_SHADER_COUNT;
        ++meta_shader_index) {
      struct terakan_shader_static * const device_meta_shader =
         &device->meta_shaders[meta_shader_index];
      bool const from_nir = terakan_meta_nir_builders[meta_shader_index] != NULL;
      struct terakan_meta_shader const * const meta_shader =
         terakan_meta_shaders[meta_shader_index];
      /* A shader built from NIR need not have a hand-written counterpart at all. */
      struct terakan_meta_shader_description const * const meta_shader_description =
         meta_shader == NULL ? NULL : (is_r9xx ? &meta_shader->r9xx : &meta_shader->r8xx);
      *device_meta_shader = from_nir ? meta_nir_shaders[meta_shader_index].static_state
                                     : meta_shader_description->static_registers;
      VkDeviceSize const meta_shader_program_size_bytes =
         from_nir ? (VkDeviceSize)meta_nir_shaders[meta_shader_index].shader.bc.ndw *
                       sizeof(uint32_t)
                  : meta_shader_description->program_size_bytes;
      VkDeviceSize const meta_shader_offset =
         ALIGN_POT(meta_shaders_bo_size, (VkDeviceSize)TERAKAN_SHADER_PROGRAM_ALIGNMENT);
      /* Relative for initialization purposes.
       * Will be translated into virtual addresses when the device shaders are bound to the BO.
       */
      device_meta_shader->program_va_shr8 =
         meta_shader_offset >> TERAKAN_SHADER_PROGRAM_ALIGNMENT_LOG2;
      meta_shaders_bo_size = meta_shader_offset + meta_shader_program_size_bytes;
   }
   result = device->winsys_fn->bo->allocate_device_memory(
      device, meta_shaders_bo_size, TERAKAN_SHADER_PROGRAM_ALIGNMENT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      0, NULL, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE, &device->meta_shaders_bo);
   if (result != VK_SUCCESS) {
      result = vk_errorf(physical_device->vk.instance, result,
                         "Failed to allocate memory for internal shaders");
      goto fail_query_accumulator_bo;
   }
   uint32_t const meta_shaders_va_shr8 = (uint32_t)(device->meta_shaders_bo->va >> 8);
   {
      char * const meta_shaders_bo_mapping = terakan_bo_map(device->meta_shaders_bo);
      if (meta_shaders_bo_mapping == NULL) {
         result = vk_errorf(physical_device->vk.instance, VK_ERROR_OUT_OF_HOST_MEMORY,
                            "Failed to map the internal shaders buffer object");
         goto fail_meta_shaders_bo;
      }
      /* Empty fetch shader. */
      {
         device->meta_shaders_empty_fetch_va_shr8 = meta_shaders_va_shr8;
         uint32_t * empty_fetch_shader_mapping_next = (uint32_t *)meta_shaders_bo_mapping;
         *(empty_fetch_shader_mapping_next++) = 0;
         *(empty_fetch_shader_mapping_next++) =
            util_cpu_to_le32(EG_V_SQ_CF_WORD1_SQ_CF_INST_RETURN);
      }
      /* Meta shaders. */
      memset(device->meta_shader_sqk_usage, 0, sizeof(device->meta_shader_sqk_usage));
      for (size_t meta_shader_index = 0; meta_shader_index < TERAKAN_META_SHADER_COUNT;
           ++meta_shader_index) {
         bool const from_nir = terakan_meta_nir_builders[meta_shader_index] != NULL;
         struct terakan_meta_shader const * const meta_shader =
            terakan_meta_shaders[meta_shader_index];
         struct terakan_meta_shader_description const * const meta_shader_description =
            meta_shader == NULL ? NULL : (is_r9xx ? &meta_shader->r9xx : &meta_shader->r8xx);
         /* Program memory. */
         struct terakan_shader_static * const device_meta_shader =
            &device->meta_shaders[meta_shader_index];
         device_meta_shader->program_bo = device->meta_shaders_bo;
         struct terakan_shader_impl * const meta_nir_shader =
            &meta_nir_shaders[meta_shader_index];
         util_memcpy_cpu_to_le32(
            meta_shaders_bo_mapping + ((VkDeviceSize)device_meta_shader->program_va_shr8
                                       << TERAKAN_SHADER_PROGRAM_ALIGNMENT_LOG2),
            from_nir ? meta_nir_shader->shader.bc.bytecode : meta_shader_description->program,
            from_nir ? (size_t)meta_nir_shader->shader.bc.ndw * sizeof(uint32_t)
                     : meta_shader_description->program_size_bytes);
         device_meta_shader->program_va_shr8 += meta_shaders_va_shr8;
         /* Constant usage. The backend works this out for a NIR shader, so only the hand-written
          * ones state it in their description.
          */
         struct terakan_shader_sqk_usage * const meta_shader_sqk_usage =
            &device->meta_shader_sqk_usage[meta_shader_index];
         if (from_nir) {
            *meta_shader_sqk_usage = meta_nir_shader->sqk_usage;
         } else {
            meta_shader_sqk_usage->kcache = meta_shader->kcache_used;
            meta_shader_sqk_usage->samplers = meta_shader->samplers_used;
            if (meta_shader->primary_meta_resource_used) {
               BITSET_SET(meta_shader_sqk_usage->resources,
                          TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META);
            }
            if (meta_shader->non_pixel_stage_specific_resource_used) {
               BITSET_SET(meta_shader_sqk_usage->resources,
                          TERAKAN_RESOURCE_RANGE_NON_PIXEL_STAGE_SPECIFIC);
            }
         }
      }
      terakan_bo_unmap(device->meta_shaders_bo);
   }
   for (size_t meta_shader_index = 0; meta_shader_index < TERAKAN_META_SHADER_COUNT;
        ++meta_shader_index) {
      if (terakan_meta_nir_builders[meta_shader_index] != NULL) {
         terakan_shader_impl_finish(&meta_nir_shaders[meta_shader_index]);
      }
   }
   vk_free(&device->vk.alloc, meta_nir_shaders);


   if (mtx_init(&device->completion_mutex, mtx_plain) != thrd_success) {
      result = vk_errorf(physical_device->vk.instance, VK_ERROR_OUT_OF_HOST_MEMORY,
                         "Failed to initialize the submission mutex");
      goto fail_meta_shaders_bo;
   }
   if (cnd_init(&device->completion_condition) != thrd_success) {
      result = vk_errorf(physical_device->vk.instance, VK_ERROR_OUT_OF_HOST_MEMORY,
                         "Failed to initialize the submission condition variable");
      goto fail_completion_mutex;
   }

   device->completion_lost = false;

   device->command_buffer_submission_size_gfx = terakan_command_buffer_optimal_submission_size_gfx(
      &physical_device->submission_info_gfx.base);
   /* If the optimal sizes are above hard limits, they can't be considered optimal as they'd
    * possibly result in allocating unused memory.
    */
   assert(device->command_buffer_submission_size_gfx.bo_references <=
          TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT);

   device->queue_graphics = NULL;
   for (uint32_t queue_create_info_index = 0;
        queue_create_info_index < create_info->queueCreateInfoCount; ++queue_create_info_index) {
      VkDeviceQueueCreateInfo const * const queue_create_info =
         &create_info->pQueueCreateInfos[queue_create_info_index];
      if (queue_create_info->queueFamilyIndex == 0) {
         if (queue_create_info->queueCount != 1 || device->queue_graphics != NULL) {
            result = vk_errorf(physical_device->vk.instance, VK_ERROR_INITIALIZATION_FAILED,
                               "Only one graphics queue can be created");
            goto fail_queues;
         }
         result = terakan_queue_create(device, queue_create_info, 0, &device->queue_graphics);
         if (result != VK_SUCCESS) {
            goto fail_queues;
         }
         device->command_buffer_submission_size_gfx = terakan_queue_submission_size_min(
            device->queue_graphics->submission_context->max_submission_size,
            device->command_buffer_submission_size_gfx);
      } else {
         result = vk_errorf(physical_device->vk.instance, VK_ERROR_INITIALIZATION_FAILED,
                            "Unknown queue family requested");
         goto fail_queues;
      }
   }

   device->command_buffer_submission_size_gfx.indirect_buffer_dwords &=
      ~(uint32_t)(TERAKAN_QUEUE_INDIRECT_BUFFER_SIZE_ALIGNMENT_DWORDS_GFX - 1);

   return VK_SUCCESS;

fail_queues:
   if (device->queue_graphics != NULL) {
      terakan_queue_destroy(device->queue_graphics);
   }
   cnd_destroy(&device->completion_condition);
fail_completion_mutex:
   mtx_destroy(&device->completion_mutex);
fail_meta_shaders_bo:
   terakan_bo_free(device->meta_shaders_bo, NULL);
fail_meta_nir_shaders:
   for (size_t meta_shader_index = 0; meta_shader_index < TERAKAN_META_SHADER_COUNT;
        ++meta_shader_index) {
      if (terakan_meta_nir_builders[meta_shader_index] != NULL) {
         terakan_shader_impl_finish(&meta_nir_shaders[meta_shader_index]);
      }
   }
   vk_free(&device->vk.alloc, meta_nir_shaders);
fail_query_accumulator_bo:
   terakan_bo_free(device->query_accumulator_bo, NULL);
fail_uav_immediate_bo:
   terakan_bo_free(device->uav_immediate_bo, NULL);
fail_gfx_discard_bo:
   terakan_bo_free(device->gfx_discard_bo, NULL);
fail_reference_placeholder_bos:
   while (reference_placeholder_bos_created > 0) {
      terakan_bo_free(device->reference_placeholder_bos[--reference_placeholder_bos_created], NULL);
   }
   vk_device_finish(&device->vk);
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreateDevice(VkPhysicalDevice const physicalDevice,
                     VkDeviceCreateInfo const * const pCreateInfo,
                     VkAllocationCallbacks const * const pAllocator, VkDevice * const pDevice)
{
   struct terakan_physical_device * const physical_device =
      terakan_physical_device_from_handle(physicalDevice);

   /* Minimal logical-device bring-up is hardware-validated on both R600 and R700. ISA selection,
    * BO alignment, the fixed SQ configuration and the R600-vs-R700 context defaults come from the
    * actual runtime chip family and DRM queries, not the build-generation label. Queue submission
    * remains blocked for every TeraScale 1 device until the R600 and R700 command streams have
    * separate hardware readback validation.
    */

   struct terakan_device * device;
   VkResult const result =
      physical_device->winsys_fn->create_device(physical_device, pCreateInfo, pAllocator, &device);
   if (result != VK_SUCCESS) {
      return result;
   }

   *pDevice = terakan_device_to_handle(device);
   return VK_SUCCESS;
}
