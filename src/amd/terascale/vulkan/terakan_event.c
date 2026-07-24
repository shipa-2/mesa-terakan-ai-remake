/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_event.h"

#include "terakan_command_buffer.h"
#include "terakan_device.h"
#include "terakan_entrypoints.h"

#include "amd/terascale/common/terascale_wddm.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/macros.h"
#include "util/u_atomic.h"
#include "vk_alloc.h"
#include "vk_log.h"

#include <stdint.h>

VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreateEvent(VkDevice const device_handle, VkEventCreateInfo const * const create_info,
                    VkAllocationCallbacks const * const allocator, VkEvent * const event_out)
{
   struct terakan_device * const device = terakan_device_from_handle(device_handle);
   struct terakan_event * const event =
      vk_object_zalloc(&device->vk, allocator, sizeof(*event), VK_OBJECT_TYPE_EVENT);
   if (event == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   VkResult const result = device->winsys_fn->bo->allocate_device_memory(
      device, sizeof(uint64_t), sizeof(uint64_t),
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
         VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
      0, allocator, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &event->bo);
   if (result != VK_SUCCESS) {
      vk_object_free(&device->vk, allocator, event);
      return vk_error(device, result);
   }

   event->status = terakan_bo_map(event->bo);
   if (event->status == NULL) {
      terakan_bo_free(event->bo, allocator);
      vk_object_free(&device->vk, allocator, event);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   p_atomic_set(event->status, 0);

   *event_out = terakan_event_to_handle(event);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
terakan_DestroyEvent(VkDevice const device_handle, VkEvent const event_handle,
                     VkAllocationCallbacks const * const allocator)
{
   struct terakan_device * const device = terakan_device_from_handle(device_handle);
   struct terakan_event * const event = terakan_event_from_handle(event_handle);
   if (event == NULL) {
      return;
   }

   terakan_bo_free(event->bo, allocator);
   vk_object_free(&device->vk, allocator, event);
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_GetEventStatus(UNUSED VkDevice const device, VkEvent const event_handle)
{
   struct terakan_event const * const event = terakan_event_from_handle(event_handle);
   return p_atomic_read(event->status) != 0 ? VK_EVENT_SET : VK_EVENT_RESET;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_SetEvent(UNUSED VkDevice const device, VkEvent const event_handle)
{
   struct terakan_event * const event = terakan_event_from_handle(event_handle);
   p_atomic_set(event->status, 1);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_ResetEvent(UNUSED VkDevice const device, VkEvent const event_handle)
{
   struct terakan_event * const event = terakan_event_from_handle(event_handle);
   p_atomic_set(event->status, 0);
   return VK_SUCCESS;
}

static void
terakan_cmd_write_event(VkCommandBuffer const command_buffer_handle, VkEvent const event_handle,
                        uint32_t const value)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(command_buffer_handle)->command_writer.gfx;
   struct terakan_event const * const event = terakan_event_from_handle(event_handle);

   uint32_t *packet = terakan_gfx_command_writer_emit_with_bo(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 6, 1, 0, 1);
   if (unlikely(packet == NULL)) {
      return;
   }

   *packet++ = PKT3(PKT3_EVENT_WRITE_EOP, 5 - 1, 0);
   *packet++ = EVENT_TYPE(EVENT_TYPE_BOTTOM_OF_PIPE_TS) | EVENT_INDEX(5);
   uint32_t const * const packet_address = packet;
   *packet++ = (uint32_t)event->bo->va;
   *packet++ = ((event->bo->va >> 32) & 0xFF) |
               EOP_INT_SEL(EOP_INT_SEL_SEND_DATA_AFTER_WR_CONFIRM) |
               EOP_DATA_SEL(EOP_DATA_SEL_VALUE_32BIT);
   *packet++ = value;
   *packet++ = 0;
   terakan_gfx_command_writer_add_relocation_for_40_bits(
      command_writer, &packet, packet_address, packet_address + 1,
      TERASCALE_WDDM_PATCH_IDS_EVENT_WRITE_EOP_LO, TERASCALE_WDDM_PATCH_IDS_EVENT_WRITE_EOP_HI,
      terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                event->bo, false, true,
                                                TERAKAN_BO_PRIORITY_SYNC));
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdSetEvent(VkCommandBuffer const command_buffer, VkEvent const event,
                    UNUSED VkPipelineStageFlags const stage_mask)
{
   terakan_cmd_write_event(command_buffer, event, 1);
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdResetEvent(VkCommandBuffer const command_buffer, VkEvent const event,
                      UNUSED VkPipelineStageFlags const stage_mask)
{
   terakan_cmd_write_event(command_buffer, event, 0);
}
