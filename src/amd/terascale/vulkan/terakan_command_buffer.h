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

#ifndef TERAKAN_COMMAND_BUFFER_H
#define TERAKAN_COMMAND_BUFFER_H

#include "terakan_barrier.h"
#include "terakan_bo.h"
#include "terakan_descriptor.h"
#include "terakan_device.h"
#include "terakan_hw_state.h"
#include "terakan_physical_device.h"
#include "terakan_push_constants.h"
#include "terakan_queue.h"
#include "terakan_shader.h"
#include "terakan_state.h"

#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/bitset.h"
#include "util/list.h"
#include "util/macros.h"
#include "vk_command_buffer.h"
#include "vk_command_pool.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TERAKAN_CONFIG_REG_OFFSET(address)  (((address) - EVERGREEN_CONFIG_REG_OFFSET) >> 2)
#define TERAKAN_CONTEXT_REG_OFFSET(address) (((address) - EVERGREEN_CONTEXT_REG_OFFSET) >> 2)
#define TERAKAN_CTL_CONST_OFFSET(address)   (((address) - EVERGREEN_CTL_CONST_OFFSET) >> 2)

/* Given that Terakan exposes more sampled image bindings than the Gallium R600 driver due to
 * separate images and samplers, the number of bindings may be much bigger, and thus, also taking
 * into account that each binding is set one by one, the sizes are larger than in the Gallium R600
 * driver.
 */

/* Must be large enough to hold all the necessary setup, including up to 1024 resources (up to 14
 * dwords per resource - 2 for the SET_RESOURCE header, 8 for the constant, and 4 dwords for 2
 * relocations for textures), for at least one draw / dispatch command.
 * Command buffers using virtual memory on the DRM Radeon kernel driver must not be larger than
 * RADEON_INFO_IB_VM_MAX_SIZE dwords reported by the kernel driver, however.
 * Twice the size in the Gallium R600 driver as of May 2023.
 */
#define TERAKAN_GFX_OPTIMAL_INDIRECT_BUFFER_SIZE_DWORDS ((uint32_t)1 << 15)

/* Must be large enough to hold all bindings for a single command even if they all point to
 * different BOs.
 * Assuming that new references may be needed every 8 dwords on average (resource constants are
 * 2 SET_RESOURCE dwords plus 10-12 dwords, DRAW_INDEX_2 is 6 dwords plus 2 dwords for the
 * relocation).
 */
#define TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT_LOG2 12
#define TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT                                                \
   ((uint32_t)1 << TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT_LOG2)

/* With the current rationale behind the optimal indirect buffer size and BO reference count (around
 * 1 address per 8 dwords), 1 address per reference is optimal, but 40-bit addresses require 2
 * relocations, and also texture resources have separate base and mip addresses, and RTVs may have
 * metadata, so expecting around 2 relocations per 8 dwords.
 */
#define TERAKAN_GFX_OPTIMAL_WDDM_RELOCATION_COUNT (2 * TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT)

/* Double as large as the reference count to reduce the likelihood of hash collisions, and also to
 * provide one additional entry per hash value for quick collision resolution.
 * Twice the size of the Gallium R600 driver relocation hash table as of May 2023.
 */
#define TERAKAN_BO_REFERENCE_HASH_BITS (TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT_LOG2 + 1)
#define TERAKAN_BO_REFERENCE_HASH_MASK (((uint32_t)1 << TERAKAN_BO_REFERENCE_HASH_BITS) - 1)
static_assert(
   TERAKAN_BO_REFERENCE_HASH_MASK + 1 >= TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT,
   "There need to be enough BO reference hash map entries for each BO reference with the default "
   "BO reference count, so it can be assumed externally that allocation can fail only due to an "
   "overflow of the total BO reference count and not because of the hash map.");

struct terakan_bo_reference_writer {
   struct terakan_device const * device;
   void * references;
   uint32_t max_reference_count;

   uint32_t reference_count;

   struct terakan_bo const * reference_bos[TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT];

   /* Which elements of the map are used, faster to clear than the array itself. */
   BITSET_DECLARE(map_entries_used, TERAKAN_BO_REFERENCE_HASH_MASK + 1);
   uint32_t map[TERAKAN_BO_REFERENCE_HASH_MASK + 1];
};

/* bo_references must point to `terakan_device::bo_reference_size *
 * TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT` references that will be passed to the winsys during
 * submission.
 */
void terakan_bo_reference_writer_reset(struct terakan_bo_reference_writer * writer,
                                       void * bo_references, uint32_t max_bo_reference_count);

/* Returns the reference index, or UINT32_MAX if too many references. */
uint32_t terakan_bo_reference_writer_add_reference(struct terakan_bo_reference_writer * writer,
                                                   struct terakan_bo const * bo, bool is_reading,
                                                   bool is_writing,
                                                   enum terakan_bo_priority priority);

struct terakan_push_buffer {
   struct terakan_bo * bo;

   struct list_head link;
};

/* Rings are allocated at submission time via BO reference placeholders, so their base and size are
 * set once per indirect buffer, and the setting packets are patched at submission time.
 */

struct terakan_command_buffer_indirect_buffer_shader_ring {
   /* [0] is UINT32_MAX if the ring is not used.
    * [1] is for the second shader engine if needed.
    */
   uint32_t set_base_argument_offsets_dwords[2];
   uint32_t set_base_relocation_handles[2];
   uint32_t set_size_argument_offset_dwords;
};

struct terakan_command_buffer_indirect_buffer {
   /* If owned, within terakan_command_buffer::indirect_buffers.
    * If free, within terakan_command_pool::indirect_buffers_free.
    */
   struct list_head link;

   uint32_t bo_reference_count;
   void * bo_references;

   uint32_t indirect_buffer_size_dwords;
   uint32_t * indirect_buffer;

   uint32_t relocation_count;
   /* If the queue doesn't use relocations of a type that's represented by an array of relocations,
    * the pointer to the array of relocations is NULL.
    */
   void * relocations;

   /* UINT32_MAX if the BO reference hasn't been created yet. */
   uint32_t shader_rings_bo_placeholder_reference;
   struct terakan_command_buffer_indirect_buffer_shader_ring
      shader_rings[TERAKAN_SHADER_RING_INDEX_COUNT];
};

struct terakan_gfx_command_writer;

struct terakan_command_buffer {
   struct vk_command_buffer vk;

   struct list_head push_buffers;
   /* Bytes currently used in the head of push_buffers. */
   uint32_t current_push_buffer_used_bytes;

   /* RING_SIZE for one shader engine. */
   uint32_t shader_ring_bytes_needed_for_se_shr8[TERAKAN_SHADER_RING_INDEX_COUNT];

   struct list_head indirect_buffers;

   union {
      struct terakan_gfx_command_writer * gfx;
   } command_writer;
};

VK_DEFINE_HANDLE_CASTS(terakan_command_buffer, vk.base, VkCommandBuffer,
                       VK_OBJECT_TYPE_COMMAND_BUFFER)

TERAKAN_DEVICE_DEFINE_OBJECT_SHORTCUTS(command_buffer, container_of(command_buffer->vk.base.device,
                                                                    struct terakan_device, vk))

/* Note that `bo_references` in any optimal size must not exceed
 * `TERAKAN_BO_REFERENCE_WRITER_REFERENCE_COUNT` as `terakan_bo_reference_writer` has static limits
 * related to it.
 */
struct terakan_queue_submission_size terakan_command_buffer_optimal_submission_size_gfx(
   struct terakan_physical_device_submission_info const * submission_info_gfx);

/* Usable for:
 * - Push constants.
 * - vkCmdUpdateBuffer.
 * - Dynamic vertex fetch shaders.
 * - Shader direct draw parameters.
 * - Other things within those bounds, such as small index buffers.
 *
 * Returns the mapping, or NULL if failed.
 */
void * terakan_push_buffer_allocate(struct terakan_command_buffer * command_buffer,
                                    uint32_t size_bytes, uint32_t alignment_bytes,
                                    struct terakan_bo const ** bo_out, uint64_t * va_out);

static inline void *
terakan_push_buffer_allocate_kcache(struct terakan_command_buffer * const command_buffer,
                                    uint32_t const size_bytes,
                                    struct terakan_bo const ** const bo_out,
                                    uint32_t * const va_kcache_lines_out)
{
   uint64_t va;
   void * const mapping = terakan_push_buffer_allocate(command_buffer, size_bytes,
                                                       TERAKAN_KCACHE_HW_LINE_BYTES, bo_out, &va);
   if (likely(mapping != NULL)) {
      *va_kcache_lines_out = (uint32_t)(va >> TERAKAN_KCACHE_HW_LINE_BYTES_LOG2);
   }
   return mapping;
}

extern struct vk_command_buffer_ops const terakan_command_buffer_ops;

struct terakan_command_writer {
   /* Within terakan_command_pool::command_writers_free. */
   struct list_head free_link;

   struct terakan_command_buffer * command_buffer;

   struct terakan_bo_reference_writer bo_reference_writer;
};

TERAKAN_DEVICE_DEFINE_OBJECT_SHORTCUTS(
   command_writer, terakan_command_buffer_device(command_writer->command_buffer))

enum terakan_gfx_command_writer_emit_contents {
   /* Commands generated by `terakan_hw_state` emission callbacks for tracked state, or state
    * emitted in the preamble.
    * Inner only.
    */
   TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_STATE,

   /* Application or meta draw commands. Configuration registers and graphics context state will be
    * actualized in the indirect buffer before this packet sequence.
    * Outer only.
    */
   TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW,

   /* Packet types not requiring configuration or context state.
    * Outer or inner.
    */
   TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER,

   TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_INVALID,
};

struct terakan_gfx_command_writer {
   struct terakan_command_writer base;

   struct terakan_command_buffer_indirect_buffer * indirect_buffer;

#ifndef NDEBUG
   /* For testing multiple indirect buffers in one Vulkan command buffer.
    * If >= 0, this many more application's or internal draws or dispatches may be emitted in the
    * current indirect buffer.
    */
   int64_t actions_before_next_indirect_buffer_split;
#endif

   bool is_in_outer_emit_call;

#ifndef NDEBUG
   bool is_emission_unclosed;
   uint32_t current_emission_relocations_remaining;
#endif

   enum terakan_barrier_action_flags pending_barrier_actions;

   /* Actions the next barrier with srcAccessMask & TRANSFER_WRITE, srcStageMask & COPY should
    * perform, depending on how these transfers were actually performed.
    */
   enum terakan_barrier_action_flags post_buffer_copy_write_barrier_actions;
   enum terakan_barrier_action_flags post_color_image_copy_write_barrier_actions;
   enum terakan_barrier_action_flags post_depth_stencil_image_copy_write_barrier_actions;

   struct terakan_hw_state_draw hw_state_draw;
   struct terakan_hw_state_sqc hw_state_sqc;

   /* Modifies hw_state_sqc. */
   struct terakan_push_constants_state push_constants_state;

   /* Modifies hw_state_draw, hw_state_sqc, and push_constants_state. */
   struct terakan_state_draw state_draw;
};

TERAKAN_DEVICE_DEFINE_OBJECT_SHORTCUTS(gfx_command_writer,
                                       terakan_command_writer_device(&gfx_command_writer->base))

static inline void
terakan_gfx_command_writer_emit_done(ASSERTED struct terakan_gfx_command_writer * command_writer,
                                     ASSERTED uint32_t const * const final_append_ptr)
{
#ifndef NDEBUG
   assert(command_writer->is_emission_unclosed);
   assert(final_append_ptr == command_writer->indirect_buffer->indirect_buffer +
                                 command_writer->indirect_buffer->indirect_buffer_size_dwords);
   assert(command_writer->current_emission_relocations_remaining == 0);
   command_writer->is_emission_unclosed = false;
#endif
}

/* Entry point for emitting packets.
 *
 * Allocates space for `packet_dwords` and `relocation_*_count` relocations, and assumes that the
 * application will write them all.
 * `packet_dwords` must not be 0.
 *
 * Also ensures that `bo_count` calls to `terakan_bo_reference_writer_add_reference` for
 * `terakan_gfx_command_writer::bo_reference_writer` will succeed (regardless of which BOs are
 * specified). Unlike for `packet_dwords` and relocations, it's also okay to allocate more BO
 * references than the number of `terakan_bo_reference_writer_add_reference` calls that will
 * actually be done subsequently (`terakan_bo_reference_writer_add_reference` calls may thus be done
 * conditionally based on whether this emission has started a new indirect buffer and thus a new
 * list of BO references, for instance).
 *
 * A Vulkan command buffer may need to be split into multiple kernel driver submissions, and the
 * kernel may not preserve the GPU state between those submissions. Therefore, there are two types
 * of emissions that are handled differently:
 *
 * - Outer emissions.
 *
 *   These are emissions done directly from Vulkan commands: draws, dispatches, or other operations
 *   that don't affect persistent state settings in the GPU that may be assumed by subsequent
 *   Terakan commands.
 *
 *   When an outer emission is demanded, the emit call will make sure that there's an indirect
 *   buffer to write the packets to, and that the `terakan_hw_state` needed by the packets for the
 *   specified emission contents is applied in that indirect buffer. Therefore, an outer emit call
 *   may internally do inner emissions to ensure that the indirect buffer contains all the needed
 *   setup and state setting packets.
 *
 * - Inner emissions.
 *
 *   These are invoked by an outer `terakan_gfx_command_writer_emit_with_bo` and contain preamble
 *   packets or `terakan_hw_state` applying packets.
 *
 *   If the current indirect buffer was overflown, inner emit calls return NULL, because the outer
 *   emit call will reapply all the tracked state when it switches to a new indirect buffer.
 *
 * Because of the possibility of an emission ending up in a new indirect buffer:
 *
 * - Emissions must contain complete PM4 packets: the header and all the arguments must be in a
 *   single emission.
 *
 * - The persistent state of the GPU's command processor (such as configuration and context
 *   registers) must not be changed using free-standing emit calls, because state is not preserved
 *   between submissions of individual indirect buffers, and an emission may switch the indirect
 *   buffer at any moment.
 *
 *   Instead, all hardware state must use the `terakan_hw_state` tracking infrastructure, so that
 *   it's properly re-emitted in a new indirect buffer.
 *
 *   Registers that are set once and never changed may be set in the indirect buffer preamble
 *   instead of `terakan_hw_state`.
 *
 *   There may be some exceptions, but the fact that multiple emissions may end up in different
 *   indirect buffers with state becoming undefined in between must always be taken into account.
 *   For example, if some registers need to be set for different shader engines separately, then the
 *   `GRBM_GFX_INDEX` write switching to SE 0 configuration, the register writes for SE 0, the
 *   `GRBM_GFX_INDEX` write switching to SE 1, the SE 1 register writes, and the `GRBM_GFX_INDEX`
 *    write re-enabling broadcasting, must all be done with a single emit call.
 *
 * If failed to allocate, or if this is an inner emission, and the current indirect buffer has been
 * exhausted, returns NULL. Otherwise, returns a pointer to the packet dwords.
 *
 * The returned BO reference allocation is valid within the current command buffer recording until
 * the next `terakan_gfx_command_writer_emit_with_bo` call for it.
 *
 * After writing, `terakan_gfx_command_writer_emit_done` must be called with the actual append
 * pointer for the end of the emission to verify that `packet_dwords` have been written via it.
 */
uint32_t * terakan_gfx_command_writer_emit_with_bo(
   struct terakan_gfx_command_writer * command_writer,
   enum terakan_gfx_command_writer_emit_contents contents, uint32_t packet_dwords,
   uint32_t bo_count, uint32_t relocation_for_32_bits_count, uint32_t relocation_for_40_bits_count);

static inline uint32_t *
terakan_gfx_command_writer_emit(struct terakan_gfx_command_writer * const command_writer,
                                enum terakan_gfx_command_writer_emit_contents const contents,
                                uint32_t const packet_dwords)
{
   return terakan_gfx_command_writer_emit_with_bo(command_writer, contents, packet_dwords, 0, 0, 0);
}

/* DRM Radeon relocation NOP packets are inserted after the packet containing the relative address,
 * so `indirect_buffer_append_ptr` must be after the corresponding packet (and previous relocations
 * for it if present) at the time of the call, and it will be updated to point after the newly
 * written relocation.
 *
 * `address_in_packet` must point to the location inside the packet where the relative address
 * itself is placed, for Radeon Software WDDM relocations. The meaning of `wddm_allocation_offset`
 * depends on the slot, but in most cases it equals to the entire 32 bits at `*address_in_packet`,
 * including everything stored in the register or packet argument dword other than the address bits
 * themselves.
 *
 * `wddm_patch_ids` is the WDDM patch location slot ID in the lower 32 bits, driver ID in the upper
 * 32 bits.
 *
 * Returns the handle of the relocation specific to the relocation type that may be used for
 * patching the relocation at submission time.
 */
uint32_t terakan_gfx_command_writer_add_relocation(
   struct terakan_gfx_command_writer * command_writer, uint32_t ** indirect_buffer_append_ptr,
   uint32_t const * address_in_packet, uint32_t wddm_allocation_offset, uint64_t wddm_patch_ids,
   uint32_t bo_reference);

/* If the relocation type requires two relocations for 40-bit addresses, returns the handle of the
 * first relocation, assuming that the handle of the second one can be calculated trivially from it.
 */
static inline uint32_t
terakan_gfx_command_writer_add_relocation_for_40_bits(
   struct terakan_gfx_command_writer * const command_writer,
   uint32_t ** const indirect_buffer_append_ptr, uint32_t const * const address_in_packet_lo,
   uint32_t const * const address_in_packet_hi, uint64_t const wddm_patch_ids_lo,
   uint64_t const wddm_patch_ids_hi, uint32_t const bo_reference)
{
   /* DRM Radeon uses one relocation for all 40 bits, WDDM Radeon Software uses separate ones for
    * each dword.
    */
   uint32_t const first_relocation_handle = terakan_gfx_command_writer_add_relocation(
      command_writer, indirect_buffer_append_ptr, address_in_packet_lo, *address_in_packet_lo,
      wddm_patch_ids_lo, bo_reference);
   if (terakan_gfx_command_writer_physical_device(command_writer)
          ->submission_info_gfx.base.relocation_type == TERAKAN_QUEUE_RELOCATION_TYPE_WDDM_PATCH) {
      terakan_gfx_command_writer_add_relocation(command_writer, indirect_buffer_append_ptr,
                                                address_in_packet_hi, *address_in_packet_hi,
                                                wddm_patch_ids_hi, bo_reference);
   }
   return first_relocation_handle;
}

void terakan_gfx_command_writer_emit_event_write_eop_discarding_data(
   struct terakan_gfx_command_writer * command_writer, uint32_t event);

struct terakan_command_pool {
   struct vk_command_pool vk;

   struct list_head push_buffers_free;

   struct list_head indirect_buffers_free;

   struct list_head command_writers_free;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(terakan_command_pool, vk.base, VkCommandPool,
                               VK_OBJECT_TYPE_COMMAND_POOL)

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_COMMAND_BUFFER_H */
