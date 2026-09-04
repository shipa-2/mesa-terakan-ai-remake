/*
 * Copyright © 2026 Vitaliy Triang3l Kuzmin
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

#include "terakan_hw_config_shared.h"

#include "terakan_barrier.h"
#include "terakan_command_buffer.h"
#include "terakan_device.h"
#include "terakan_hw_config_loop_constants.h"
#include "terakan_hw_config_shared_terascale_1.h"
#include "terakan_limits.h"
#include "terakan_physical_device.h"
#include "terakan_shader.h"

#include "util/bitscan.h"
#include "util/macros.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool
terakan_hw_config_shared_emit_config_register(
   struct terakan_gfx_command_writer * const command_writer, uint32_t const register_address_bytes,
   uint32_t const register_value)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 1);
   if (unlikely(packet == NULL)) {
      return false;
   }
   *packet++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
   *packet++ = TERAKAN_CONFIG_REG_OFFSET(register_address_bytes);
   *packet++ = register_value;
   terakan_gfx_command_writer_emit_done(command_writer, packet);
   return true;
}

static void
terakan_hw_config_shared_set_all_modified(struct terakan_hw_config_shared * const config)
{
   config->draw_.entries_modified =
      BITFIELD_MASK(TERAKAN_HW_CONFIG_SHARED_ENTRIES_COMMON_AND_DRAW_COUNT);

   config->draw_.sq_thread_stack_resource_mgmt.ps_modified = true;

   config->compute_.entries_modified =
      BITFIELD_MASK(TERAKAN_HW_CONFIG_SHARED_ENTRIES_COMMON_AND_COMPUTE_COUNT);
}

void
terakan_hw_config_shared_indirect_buffer_begun(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_shared_set_all_modified(&command_writer->hw_config_shared);

   struct terakan_physical_device_chip_info const * const chip_info =
      &terakan_gfx_command_writer_physical_device(command_writer)->chip_info;

   /* Flush all shader invocations before configuring the sequencer.
    * This is necessary, and on WDDM Radeon Software, not flushing before setting `SQ_CONFIG`
    * consistently results in a GPU hang (tested on Barts on 15.301.1901).
    *
    * EVENT_TYPE_CS_PARTIAL_FLUSH is explicitly Evergreen-and-newer in r600d.h. R7xx does not
    * have a CS stage; its classic r600_init_atom_start_cs() preamble emits only
    * PS_PARTIAL_FLUSH here. The Radeon command-stream validator accepts event 0x07, but an RV710
    * empty-IB experiment subsequently stalled ring 0, so leaving the Evergreen CS flush in this
    * shared preamble is not defensible. Keep the pre-existing two-event sequence byte-for-byte for
    * Evergreen and use the documented R600/R700 event instead.
    */
   terakan_barrier_emit_actions_unconditionally(
      command_writer, TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS |
                          (chip_info->is_terascale_1 ? 0 : TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CS));

   /* TeraScale 1's SQ_CONFIG/SQ_GPR_RESOURCE_MGMT/SQ_THREAD_RESOURCE_MGMT and the rest of the
    * per-command-buffer context defaults have a genuinely different register layout from R8xx/R9xx
    * below (see r600d.h vs evergreend.h, and the comment on
    * terakan_hw_config_shared_terascale_1_write_context_defaults()), so this is a real fork, not a
    * value plugged into shared code. `TERAKAN_CONTEXT_REG_OFFSET`/`TERAKAN_CONFIG_REG_OFFSET`
    * themselves are unaffected -- R600_CONTEXT_REG_OFFSET/R600_CONFIG_REG_OFFSET in r600d_common.h
    * are numerically identical to the EVERGREEN_* constants these macros are built from -- but the
    * register offsets and values written relative to them are not.
    */
   if (chip_info->is_terascale_1) {
      uint32_t * packet = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG,
         TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_SQ_CONFIG_DWORDS +
            TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_CONTEXT_DEFAULTS_MAX_DWORDS);
      if (unlikely(packet == NULL)) {
         return;
      }
      struct terakan_hw_config_shared_terascale_1_sq_config_info const sq_config_info = {
         .has_vertex_cache = chip_info->has_vertex_cache,
         .num_ps_gprs = chip_info->terascale_1.num_ps_gprs,
         .num_vs_gprs = chip_info->terascale_1.num_vs_gprs,
         .num_temp_gprs = chip_info->terascale_1.num_temp_gprs,
         .num_gs_gprs = chip_info->terascale_1.num_gs_gprs,
         .num_es_gprs = chip_info->terascale_1.num_es_gprs,
         .num_ps_threads = chip_info->terascale_1.num_ps_threads,
         .num_vs_threads = chip_info->terascale_1.num_vs_threads,
         .num_gs_threads = chip_info->terascale_1.num_gs_threads,
         .num_es_threads = chip_info->terascale_1.num_es_threads,
         .num_ps_stack_entries = chip_info->terascale_1.num_ps_stack_entries,
         .num_vs_stack_entries = chip_info->terascale_1.num_vs_stack_entries,
         .num_gs_stack_entries = chip_info->terascale_1.num_gs_stack_entries,
         .num_es_stack_entries = chip_info->terascale_1.num_es_stack_entries,
      };
      packet = terakan_hw_config_shared_terascale_1_write_sq_config(packet, &sq_config_info);
      packet = terakan_hw_config_shared_terascale_1_write_context_defaults(
         packet, terakan_physical_device_chip_family_is_r700(chip_info->chip_family));
      terakan_gfx_command_writer_emit_done(command_writer, packet);
      return;
   }

   {
      uint32_t * packet = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 2 + 2 + 2);
      if (unlikely(packet == NULL)) {
         return;
      }
      *packet++ = PKT3(PKT3_SET_CONFIG_REG, chip_info->is_r9xx ? 2 : 6, 0);
      *packet++ = TERAKAN_CONFIG_REG_OFFSET(R_008C00_SQ_CONFIG);
      /* `SQ_CONFIG` same as in DRM Radeon 2.50.0, and also on R8xx, same as in WDDM Radeon Software
       * 15.301.1901.
       */
      *packet++ =
         S_008C00_VC_ENABLE(chip_info->has_vertex_cache) | S_008C00_EXPORT_SRC_C(true) |
         (chip_info->is_r9xx ? S_008C00_CS2_PRIO(1)
                             : S_008C00_ES_PRIO(3) | S_008C00_GS_PRIO(2) | S_008C00_VS_PRIO(1));
      /* R_008C04_SQ_GPR_RESOURCE_MGMT_1 */
      *packet++ = S_008C04_NUM_CLAUSE_TEMP_GPRS(4);
      if (chip_info->is_r9xx) {
         *packet++ = PKT3(PKT3_SET_CONFIG_REG, 2, 0);
         *packet++ = TERAKAN_CONFIG_REG_OFFSET(R_008C10_SQ_GLOBAL_GPR_RESOURCE_MGMT_1);
      } else {
         /* R_008C08_SQ_GPR_RESOURCE_MGMT_2 */
         *packet++ = 0;
         /* R_008C0C_SQ_GPR_RESOURCE_MGMT_3 */
         *packet++ = 0;
      }
      /* R_008C10_SQ_GLOBAL_GPR_RESOURCE_MGMT_1 */
      *packet++ = 0;
      /* R_008C14_SQ_GLOBAL_GPR_RESOURCE_MGMT_2 */
      *packet++ = 0;
      terakan_gfx_command_writer_emit_done(command_writer, packet);
   }

   /* TODO(Triang3l): Dynamic GPR usage on R8xx - see `evergreen_emit_config_state`, and also
    * disable them for tessellation, see `evergreen_adjust_gprs`. Keep them always enabled for R9xx
    * though.
    */
   terakan_hw_config_shared_emit_config_register(
      command_writer, R_008D8C_SQ_DYN_GPR_CNTL_PS_FLUSH_REQ, S_008D8C_DYN_GPR_ENABLE(true));

   uint32_t const common_constant_config[] = {
      /* Remove LS and HS from one SIMD to work around a hardware bug according to the Gallium R600
       * driver.
       */
      PKT3(PKT3_SET_CONFIG_REG, 3, 0),
      TERAKAN_CONFIG_REG_OFFSET(R_008E20_SQ_STATIC_THREAD_MGMT1),
      /* R_008E20_SQ_STATIC_THREAD_MGMT1 */
      ~(uint32_t)0,
      /* R_008E24_SQ_STATIC_THREAD_MGMT2 */
      ~(uint32_t)0,
      /* R_008E28_SQ_STATIC_THREAD_MGMT3 */
      ~(uint32_t)1,

      PKT3(PKT3_SET_CONFIG_REG, 1, 0),
      TERAKAN_CONFIG_REG_OFFSET(R_009100_SPI_CONFIG_CNTL),
      0,

      PKT3(PKT3_SET_CONFIG_REG, 1, 0),
      TERAKAN_CONFIG_REG_OFFSET(R_00913C_SPI_CONFIG_CNTL_1),
      S_00913C_VTX_DONE_DELAY(4),

      PKT3(PKT3_SET_CONFIG_REG, 1, 0),
      TERAKAN_CONFIG_REG_OFFSET(R_008A14_PA_CL_ENHANCE),
      S_008A14_CLIP_VTX_REORDER_ENA(true) | S_008A14_NUM_CLIP_SEQ(3),
   };

   {
      uint32_t * packet = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG,
         ARRAY_SIZE(common_constant_config));
      if (unlikely(packet == NULL)) {
         return;
      }
      memcpy(packet, common_constant_config, sizeof(common_constant_config));
      packet += ARRAY_SIZE(common_constant_config);
      terakan_gfx_command_writer_emit_done(command_writer, packet);
   }

   {
      uint32_t * packet = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG,
         TERAKAN_HW_CONFIG_LOOP_CONSTANT_DWORDS);
      if (unlikely(packet == NULL)) {
         return;
      }
      packet = terakan_hw_config_loop_constants_write(packet, TERAKAN_PACKET3_COMPUTE);
      terakan_gfx_command_writer_emit_done(command_writer, packet);
   }

   /* TODO(Triang3l): Switch to three-state `is_compute_active_`. This will cause the register to be
    * set twice if the first action is a compute dispatch.
    */
   if (!chip_info->is_r9xx) {
      terakan_hw_config_shared_emit_config_register(
         command_writer, R_008E2C_SQ_LDS_RESOURCE_MGMT,
         command_writer->hw_config_shared.is_compute_active_
            ? S_008E2C_NUM_LS_LDS(TERAKAN_LIMITS_HW_LDS_SIMD_DWORD_COUNT)
            : S_008E2C_NUM_PS_LDS(TERAKAN_LIMITS_HW_LDS_SIMD_DWORD_COUNT / 2) |
                 S_008E2C_NUM_LS_LDS(TERAKAN_LIMITS_HW_LDS_SIMD_DWORD_COUNT / 2));
   }
}

typedef void (*terakan_hw_config_shared_emit_function)(
   struct terakan_gfx_command_writer * command_writer);

static void
terakan_hw_config_shared_emit_pipelinestat_streamoutstats_enable(
   struct terakan_gfx_command_writer * const command_writer)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_EVENT_WRITE, 0, 0);
   *packet++ = EVENT_TYPE(command_writer->hw_config_shared.pipelinestat_streamoutstats_enable_
                             ? EVENT_TYPE_PIPELINESTAT_START
                             : EVENT_TYPE_PIPELINESTAT_STOP) |
               EVENT_INDEX(0);
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_shared_emit_sq_vtx_start_inst_loc(
   struct terakan_gfx_command_writer * const command_writer)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 1);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_CTL_CONST, 1, 0);
   *packet++ = TERAKAN_CTL_CONST_OFFSET(R_03CFF4_SQ_VTX_START_INST_LOC);
   *packet++ = command_writer->hw_config_shared.sq_vtx_start_inst_loc_;
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_shared_draw_emit_vgt_primitive_type(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_shared_emit_config_register(
      command_writer, R_008958_VGT_PRIMITIVE_TYPE,
      command_writer->hw_config_shared.draw_.vgt_primitive_type);
}

static void
terakan_hw_config_shared_compute_emit_vgt_primitive_type(
   struct terakan_gfx_command_writer * const command_writer)
{
   terakan_hw_config_shared_emit_config_register(
      command_writer, R_008958_VGT_PRIMITIVE_TYPE,
      TERAKAN_HW_CONFIG_SHARED_COMPUTE_VGT_PRIMITIVE_TYPE);
}

static void
terakan_hw_config_shared_draw_emit_vgt_num_instances(
   struct terakan_gfx_command_writer * const command_writer)
{
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_NUM_INSTANCES, 0, 0);
   *packet++ = command_writer->hw_config_shared.draw_.vgt_num_instances;
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_shared_draw_emit_sq_thread_stack_resource_mgmt(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_physical_device_chip_info const * const chip_info =
      &terakan_gfx_command_writer_physical_device(command_writer)->chip_info;
   /* R9xx has no SQ_THREAD_RESOURCE_MGMT_1 at all (fixed thread/stack allocation instead). TeraScale
    * 1 has no register at this offset (0x008C18) either -- r600d.h defines no
    * R_008C18_SQ_THREAD_RESOURCE_MGMT_1, unlike evergreend.h -- and doesn't switch thread/stack
    * allocation per draw call in the first place, since it has no tessellator: the fixed set written
    * once in terakan_hw_config_shared_indirect_buffer_begun() (via
    * terakan_hw_config_shared_terascale_1_write_sq_config()) is all it needs. Writing this register
    * for TeraScale 1 would hit whatever unrelated register (if any) actually lives at this offset on
    * that hardware, not a differently-laid-out version of the same one.
    */
   if (chip_info->is_r9xx || chip_info->is_terascale_1) {
      return;
   }
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 2 + 3);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_CONFIG_REG, 2 + 3, 0);
   *packet++ = TERAKAN_CONFIG_REG_OFFSET(R_008C18_SQ_THREAD_RESOURCE_MGMT_1);
   memcpy(packet, command_writer->hw_config_shared.draw_.sq_thread_stack_resource_mgmt.thread,
          sizeof(uint32_t) * 2);
   packet += 2;
   memcpy(packet, command_writer->hw_config_shared.draw_.sq_thread_stack_resource_mgmt.stack,
          sizeof(uint32_t) * 3);
   packet += 3;
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_shared_compute_emit_sq_thread_stack_resource_mgmt(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_physical_device_chip_info const * const chip_info =
      &terakan_gfx_command_writer_physical_device(command_writer)->chip_info;
   /* See the comment on terakan_hw_config_shared_draw_emit_sq_thread_stack_resource_mgmt() for why
    * R9xx is excluded. TeraScale 1 is excluded for the same reason (no register at this offset in
    * r600d.h), but unlike the draw-time function, this isn't simply unneeded for TeraScale 1: its
    * compute/"LS" thread and stack allocation hasn't been researched yet at all (chip_info->terascale_1
    * has no LS-stage field the way it has num_ps_gprs/num_vs_gprs/etc. for the graphics stages -- see
    * terakan_physical_device.h), so this is a real gap, not a no-op, tracked in TODO.md. Guarding it
    * out here means a TeraScale 1 compute dispatch would run with no compute thread/stack
    * configuration at all rather than one built from a register that doesn't mean this on this
    * hardware.
    */
   if (chip_info->is_r9xx || chip_info->is_terascale_1) {
      return;
   }
   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, 2 + 2 + 3);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_SET_CONFIG_REG, 2 + 3, 0);
   *packet++ = TERAKAN_CONFIG_REG_OFFSET(R_008C18_SQ_THREAD_RESOURCE_MGMT_1);
   /* Thread resource management. */
   *packet++ = 0;
   /* The register stores the actual thread count. sq_max_threads_shr3 is kept divided by eight
    * only for allocation arithmetic, matching the graphics-path calculations above. Passing it
    * directly limited Caicos compute to 24 rather than the intended 192 thread slots. */
   *packet++ = S_008C1C_NUM_LS_THREADS(chip_info->sq_max_threads_shr3 << 3);
   /* Stack resource management. */
   *packet++ = 0;
   *packet++ = 0;
   *packet++ = S_008C28_NUM_LS_STACK_ENTRIES(chip_info->sq_max_stack_entries);
   terakan_gfx_command_writer_emit_done(command_writer, packet);
   command_writer->hw_config_shared.draw_.sq_thread_stack_resource_mgmt.ps_modified = false;
}

static void
terakan_hw_config_shared_emit_sq_ring_usage(
   struct terakan_gfx_command_writer * const command_writer, uint32_t const ring_usage)
{
   assert(!(ring_usage & ~BITFIELD_MASK(TERAKAN_SHADER_RING_INDEX_COUNT)));

   struct terakan_command_buffer_indirect_buffer * const indirect_buffer =
      command_writer->indirect_buffer;
   /* In the emission architecture, expecting the configuration emission functions to always be
    * invoked from within outer emissions (for draw or dispatch packets) when the indirect buffer
    * exists, never to trigger outer emissions potentially starting a new indirect buffer.
    */
   assert(indirect_buffer != NULL);

   uint32_t rings_to_emit = ring_usage;
   /* Don't set up rings already configured in the current indirect buffer. */
   u_foreach_bit (ring_index, ring_usage) {
      if (indirect_buffer->shader_rings[ring_index].set_base_argument_offsets_dwords[0] !=
          UINT32_MAX) {
         rings_to_emit &= ~BITFIELD_BIT(ring_index);
      }
   }
   if (!rings_to_emit) {
      return;
   }

   /* With multiple shader engines, configuration setting broadcasting must be both disabled and
    * re-enabled in the same emission, so the indirect buffer can't be ended with broadcasting
    * disabled, potentially affecting other clients using the GPU.
    */

   struct terakan_device const * const device = terakan_gfx_command_writer_device(command_writer);

   uint32_t const rings_to_emit_per_se =
      terakan_device_physical_device(device)->chip_info.two_shader_engines_max
         ? rings_to_emit & TERAKAN_SHADER_RINGS_PER_SHADER_ENGINE
         : 0b0;
   uint32_t const rings_to_emit_broadcasting = rings_to_emit & ~rings_to_emit_per_se;

   /* For per-shader-engine rings:
    * - 3 dwords x 3 configuration destination setters (SE 0, SE 1, broadcast).
    * - For each ring, 3 dwords x 2 base setters (SE 0, SE 1).
    * - For each ring, 3 dwords x 1 size setter.
    * For rings configured by broadcasting:
    * - For each ring, 4 dwords x 1 base and size setter.
    */
   unsigned const rings_to_emit_per_se_count = util_bitcount(rings_to_emit_per_se);
   unsigned const rings_to_emit_broadcasting_count = util_bitcount(rings_to_emit_broadcasting);
   uint32_t packet_dwords = 4 * rings_to_emit_broadcasting_count;
   if (rings_to_emit_per_se_count != 0) {
      packet_dwords += 3 * 3 + 3 * (2 + 1) * rings_to_emit_per_se_count;
   }
   uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_CONFIG, packet_dwords,
      (uint32_t)(indirect_buffer->shader_rings_bo_placeholder_reference == UINT32_MAX),
      2 * rings_to_emit_per_se_count + rings_to_emit_broadcasting_count, 0);
   if (unlikely(packet == NULL)) {
      return;
   }
   assert(command_writer->indirect_buffer == indirect_buffer);

   struct terakan_bo const * const placeholder_bo =
      device->reference_placeholder_bos[TERAKAN_QUEUE_BO_REFERENCE_PLACEHOLDER_INDEX_SHADER_RINGS];
   if (indirect_buffer->shader_rings_bo_placeholder_reference == UINT32_MAX) {
      indirect_buffer->shader_rings_bo_placeholder_reference =
         terakan_bo_reference_writer_add_reference(&command_writer->base.bo_reference_writer,
                                                   placeholder_bo, true, true,
                                                   TERAKAN_BO_PRIORITY_SHADER_RINGS);
   }
   uint32_t const placeholder_bo_va_shr8 = (uint32_t)(placeholder_bo->va >> 8);

   /* Writing 0 as the size, and the origin of the placeholder BO as the base addresses, because
    * these are just placeholders, the actual bases and sizes will be written at submission time.
    * However, if these packets end up being emitted without the total required ring size for the
    * command buffer made nonzero (since the actual needed sizes, rather than merely the fact that
    * the rings are used, are updated outside `terakan_hw_config_shared`), the placeholder BO itself
    * may actually be bound, which is why the base address and the size must be valid for it.
    */

   if (rings_to_emit_per_se) {
      /* Base placeholders for the per-shader-engine rings, for each engine. */
      for (unsigned shader_engine = 0; shader_engine < 2; ++shader_engine) {
         *packet++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
         *packet++ = TERAKAN_CONFIG_REG_OFFSET(EG_0802C_GRBM_GFX_INDEX);
         *packet++ = S_0802C_SE_INDEX(shader_engine) | S_0802C_INSTANCE_BROADCAST_WRITES(true);
         u_foreach_bit (ring_index, rings_to_emit_per_se) {
            struct terakan_shader_ring const * const ring_info = &terakan_shader_rings[ring_index];
            struct terakan_command_buffer_indirect_buffer_shader_ring * const indirect_buffer_ring =
               &indirect_buffer->shader_rings[ring_index];
            *packet++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
            *packet++ = ring_info->base_size_config_reg_offset;
            uint32_t const * const packet_base = packet;
            indirect_buffer_ring->set_base_argument_offsets_dwords[shader_engine] =
               (uint32_t)(packet_base - indirect_buffer->indirect_buffer);
            *packet++ = placeholder_bo_va_shr8;
            indirect_buffer_ring->set_base_relocation_handles[shader_engine] =
               terakan_gfx_command_writer_add_relocation(
                  command_writer, &packet, packet_base, *packet_base,
                  ring_info->base_wddm_patch_ids,
                  indirect_buffer->shader_rings_bo_placeholder_reference);
         }
      }
      *packet++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
      *packet++ = TERAKAN_CONFIG_REG_OFFSET(EG_0802C_GRBM_GFX_INDEX);
      *packet++ = S_0802C_SE_BROADCAST_WRITES(true) | S_0802C_INSTANCE_BROADCAST_WRITES(true);

      /* Size placeholders for the per-shader-engine rings, common for both engines. */
      u_foreach_bit (ring_index, rings_to_emit_per_se) {
         *packet++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
         *packet++ = terakan_shader_rings[ring_index].base_size_config_reg_offset + 1;
         indirect_buffer->shader_rings[ring_index].set_size_argument_offset_dwords =
            (uint32_t)(packet - indirect_buffer->indirect_buffer);
         *packet++ = 0;
      }
   }

   /* Broadcast ring base and size placeholders. */
   u_foreach_bit (ring_index, rings_to_emit_broadcasting) {
      struct terakan_shader_ring const * const ring_info = &terakan_shader_rings[ring_index];
      struct terakan_command_buffer_indirect_buffer_shader_ring * const indirect_buffer_ring =
         &indirect_buffer->shader_rings[ring_index];
      *packet++ = PKT3(PKT3_SET_CONFIG_REG, 2, 0);
      *packet++ = ring_info->base_size_config_reg_offset;
      uint32_t const * const packet_base = packet;
      indirect_buffer_ring->set_base_argument_offsets_dwords[0] =
         (uint32_t)(packet_base - indirect_buffer->indirect_buffer);
      *packet++ = placeholder_bo_va_shr8;
      indirect_buffer_ring->set_size_argument_offset_dwords =
         (uint32_t)(packet - indirect_buffer->indirect_buffer);
      *packet++ = 0;
      indirect_buffer_ring->set_base_relocation_handles[0] =
         terakan_gfx_command_writer_add_relocation(
            command_writer, &packet, packet_base, *packet_base, ring_info->base_wddm_patch_ids,
            indirect_buffer->shader_rings_bo_placeholder_reference);
   }

   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_hw_config_shared_draw_emit_sq_ring_usage(
   struct terakan_gfx_command_writer * const command_writer)
{
   /* Emit all graphics rings at once if only some are needed to avoid redundant shader invocation
    * flushes.
    */
   if (!command_writer->hw_config_shared.draw_.sq_ring_usage) {
      return;
   }
   terakan_hw_config_shared_emit_sq_ring_usage(command_writer,
                                               BITFIELD_MASK(TERAKAN_SHADER_RING_INDEX_COUNT));
}

static void
terakan_hw_config_shared_compute_emit_sq_ring_usage_lstmp(
   struct terakan_gfx_command_writer * const command_writer)
{
   if (!command_writer->hw_config_shared.compute_.sq_ring_usage_lstmp) {
      return;
   }
   terakan_hw_config_shared_emit_sq_ring_usage(command_writer,
                                               BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_LSTMP));
}

static terakan_hw_config_shared_emit_function const terakan_hw_config_shared_draw_emit_functions
   [TERAKAN_HW_CONFIG_SHARED_ENTRIES_COMMON_AND_DRAW_COUNT] = {
      [TERAKAN_HW_CONFIG_SHARED_ENTRY_PIPELINESTAT_STREAMOUTSTATS_ENABLE] =
         terakan_hw_config_shared_emit_pipelinestat_streamoutstats_enable,
      [TERAKAN_HW_CONFIG_SHARED_ENTRY_SQ_VTX_START_INST_LOC] =
         terakan_hw_config_shared_emit_sq_vtx_start_inst_loc,
      [TERAKAN_HW_CONFIG_SHARED_ENTRY_VGT_PRIMITIVE_TYPE] =
         terakan_hw_config_shared_draw_emit_vgt_primitive_type,
      [TERAKAN_HW_CONFIG_SHARED_ENTRY_SQ_THREAD_STACK_RESOURCE_MGMT] =
         terakan_hw_config_shared_draw_emit_sq_thread_stack_resource_mgmt,
      [TERAKAN_HW_CONFIG_SHARED_ENTRY_DRAW_VGT_NUM_INSTANCES] =
         terakan_hw_config_shared_draw_emit_vgt_num_instances,
      [TERAKAN_HW_CONFIG_SHARED_ENTRY_DRAW_SQ_RING_USAGE] =
         terakan_hw_config_shared_draw_emit_sq_ring_usage,
};

static terakan_hw_config_shared_emit_function const terakan_hw_config_shared_compute_emit_functions
   [TERAKAN_HW_CONFIG_SHARED_ENTRIES_COMMON_AND_COMPUTE_COUNT] = {
      [TERAKAN_HW_CONFIG_SHARED_ENTRY_PIPELINESTAT_STREAMOUTSTATS_ENABLE] =
         terakan_hw_config_shared_emit_pipelinestat_streamoutstats_enable,
      [TERAKAN_HW_CONFIG_SHARED_ENTRY_SQ_VTX_START_INST_LOC] =
         terakan_hw_config_shared_emit_sq_vtx_start_inst_loc,
      [TERAKAN_HW_CONFIG_SHARED_ENTRY_VGT_PRIMITIVE_TYPE] =
         terakan_hw_config_shared_compute_emit_vgt_primitive_type,
      [TERAKAN_HW_CONFIG_SHARED_ENTRY_SQ_THREAD_STACK_RESOURCE_MGMT] =
         terakan_hw_config_shared_compute_emit_sq_thread_stack_resource_mgmt,
      [TERAKAN_HW_CONFIG_SHARED_ENTRY_COMPUTE_SQ_RING_USAGE_LSTMP] =
         terakan_hw_config_shared_compute_emit_sq_ring_usage_lstmp,
};

static void
terakan_hw_config_shared_set_common_modified_for_draw_compute_switch(
   struct terakan_hw_config_shared * const config)
{
   uint32_t * const entries_modified =
      terakan_hw_config_shared_get_common_entries_modified_ptr_(config);

   /* Copy modified common entries from the previous usage, because their current values in the
    * hardware are unknown, so they need to be emitted.
    */
   *entries_modified =
      (*entries_modified & ~BITFIELD_MASK(TERAKAN_HW_CONFIG_SHARED_ENTRIES_COMMON_COUNT)) |
      ((config->is_compute_active_ ? config->draw_.entries_modified
                                   : config->compute_.entries_modified) &
       BITFIELD_MASK(TERAKAN_HW_CONFIG_SHARED_ENTRIES_COMMON_COUNT));

   /* Always different for graphics and compute. */
   *entries_modified |= BITFIELD_BIT(TERAKAN_HW_CONFIG_SHARED_ENTRY_SQ_THREAD_STACK_RESOURCE_MGMT);
   config->draw_.sq_thread_stack_resource_mgmt.ps_modified = true;

   if (config->draw_.vgt_primitive_type != TERAKAN_HW_CONFIG_SHARED_COMPUTE_VGT_PRIMITIVE_TYPE) {
      *entries_modified |= BITFIELD_BIT(TERAKAN_HW_CONFIG_SHARED_ENTRY_VGT_PRIMITIVE_TYPE);
   }
}

void
terakan_hw_config_shared_draw_emit_modified(struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_shared * const config = &command_writer->hw_config_shared;
   unsigned max_entry = TERAKAN_HW_CONFIG_SHARED_ENTRIES_COMMON_AND_DRAW_COUNT - 1;
   unsigned skip_entry = TERAKAN_HW_CONFIG_SHARED_ENTRIES_COMMON_AND_DRAW_COUNT;
   if (terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_terascale_1 &&
       getenv("TERAKAN_DEBUG_TERASCALE_1_META_STATE_ONLY") != NULL) {
      char const * const max_value =
         getenv("TERAKAN_DEBUG_TERASCALE_1_META_SHARED_MAX_ENTRY");
      if (max_value != NULL && max_value[0] != '\0') {
         char * end = NULL;
         unsigned long const parsed = strtoul(max_value, &end, 10);
         if (end != max_value && *end == '\0' &&
             parsed < TERAKAN_HW_CONFIG_SHARED_ENTRIES_COMMON_AND_DRAW_COUNT) {
            max_entry = (unsigned)parsed;
         }
      }
      char const * const skip_value =
         getenv("TERAKAN_DEBUG_TERASCALE_1_META_SHARED_SKIP_ENTRY");
      if (skip_value != NULL && skip_value[0] != '\0') {
         char * end = NULL;
         unsigned long const parsed = strtoul(skip_value, &end, 10);
         if (end != skip_value && *end == '\0' &&
             parsed < TERAKAN_HW_CONFIG_SHARED_ENTRIES_COMMON_AND_DRAW_COUNT) {
            skip_entry = (unsigned)parsed;
         }
      }
   }

   if (config->is_compute_active_) {
      /* Flush before changing hardware resource allocation registers, as well as LS-related ones.
       * This also makes it possible not to do compute partial flushes before switching to compute
       * again later.
       */
      terakan_barrier_emit_actions_unconditionally(command_writer,
                                                   TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CS);

      config->is_compute_active_ = false;
      terakan_hw_config_shared_set_common_modified_for_draw_compute_switch(config);

      /* R_008E2C_SQ_LDS_RESOURCE_MGMT does not exist in r600d.h -- see the comment on
       * terakan_hw_config_shared_compute_emit_sq_thread_stack_resource_mgmt() for the same finding
       * about R_008C18. TeraScale 1 LDS/compute-shared-memory configuration is unresearched, so this
       * stays unguarded-but-skipped rather than writing to an offset that means something else there.
       */
      struct terakan_physical_device_chip_info const * const chip_info =
         &terakan_gfx_command_writer_physical_device(command_writer)->chip_info;
      if (!chip_info->is_r9xx && !chip_info->is_terascale_1) {
         terakan_hw_config_shared_emit_config_register(
            command_writer, R_008E2C_SQ_LDS_RESOURCE_MGMT,
            S_008E2C_NUM_PS_LDS(TERAKAN_LIMITS_HW_LDS_SIMD_DWORD_COUNT / 2) |
               S_008E2C_NUM_LS_LDS(TERAKAN_LIMITS_HW_LDS_SIMD_DWORD_COUNT / 2));
      }
   } else {
      bool ps_and_vs_partial_flush = false, vs_partial_flush = false;

      /* Flush before changing shader hardware resource allocation. */
      struct terakan_physical_device_chip_info const * const chip_info =
         &terakan_gfx_command_writer_physical_device(command_writer)->chip_info;
      if (!chip_info->is_r9xx && !chip_info->is_terascale_1) {
         if (config->draw_.entries_modified &
             BITFIELD_BIT(TERAKAN_HW_CONFIG_SHARED_ENTRY_SQ_THREAD_STACK_RESOURCE_MGMT)) {
            if (config->draw_.sq_thread_stack_resource_mgmt.ps_modified) {
               ps_and_vs_partial_flush = true;
            } else {
               vs_partial_flush = true;
            }
         }
      }

      /* Flush before introducing new rings (`CONFIG_REG`s for each ring are set only once in an
       * indirect buffer).
       */
      if (config->draw_.entries_modified &
          BITFIELD_BIT(TERAKAN_HW_CONFIG_SHARED_ENTRY_DRAW_SQ_RING_USAGE)) {
         if ((config->draw_.sq_ring_usage & BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_PSTMP)) &&
             command_writer->indirect_buffer->shader_rings[TERAKAN_SHADER_RING_INDEX_PSTMP]
                   .set_base_argument_offsets_dwords[0] == UINT32_MAX) {
            ps_and_vs_partial_flush = true;
         } else {
            u_foreach_bit (ring_index, config->draw_.sq_ring_usage &
                                          ~BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_PSTMP)) {
               if (command_writer->indirect_buffer->shader_rings[ring_index]
                      .set_base_argument_offsets_dwords[0] == UINT32_MAX) {
                  vs_partial_flush = true;
                  break;
               }
            }
         }
      }

      if (ps_and_vs_partial_flush) {
         terakan_barrier_emit_actions_unconditionally(
            command_writer, TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS);
      } else if (vs_partial_flush) {
         terakan_barrier_emit_actions_unconditionally(
            command_writer, TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_VS);
      }
   }

   u_foreach_bit (entry_index, config->draw_.entries_modified) {
      if (entry_index > max_entry || entry_index == skip_entry) {
         continue;
      }
      terakan_hw_config_shared_draw_emit_functions[entry_index](command_writer);
   }
   config->draw_.entries_modified = 0b0;
}

void
terakan_hw_config_shared_compute_emit_modified(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_hw_config_shared * const config = &command_writer->hw_config_shared;

   if (!config->is_compute_active_) {
      /* Flush before changing hardware resource allocation registers, as well as LS-related ones.
       * This also makes it possible not to do graphics partial flushes before switching to graphics
       * again later.
       */
      terakan_barrier_emit_actions_unconditionally(
         command_writer, TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CP_THROUGH_PS);

      config->is_compute_active_ = true;
      terakan_hw_config_shared_set_common_modified_for_draw_compute_switch(config);

      struct terakan_physical_device_chip_info const * const chip_info =
         &terakan_gfx_command_writer_physical_device(command_writer)->chip_info;
      if (chip_info->is_terascale_1) {
         /* 0x008E2C is Evergreen SQ_LDS_RESOURCE_MGMT, but r600d.h defines no register there.
          * TeraScale 1 compute allocation remains unsupported rather than targeting an unrelated
          * register merely because is_r9xx is false for both R700 and Evergreen.
          */
         assert(terakan_hw_config_shared_terascale_1_compute_lds_packet_dwords() == 0);
      } else if (!chip_info->is_r9xx) {
         terakan_hw_config_shared_emit_config_register(
            command_writer, R_008E2C_SQ_LDS_RESOURCE_MGMT,
            S_008E2C_NUM_LS_LDS(TERAKAN_LIMITS_HW_LDS_SIMD_DWORD_COUNT));
      }
   } else {
      bool cs_partial_flush = false;

      /* Flush before changing shader hardware resource allocation. */
      if (!terakan_gfx_command_writer_physical_device(command_writer)->chip_info.is_r9xx) {
         if (config->compute_.entries_modified &
             BITFIELD_BIT(TERAKAN_HW_CONFIG_SHARED_ENTRY_SQ_THREAD_STACK_RESOURCE_MGMT)) {
            cs_partial_flush = true;
         }
      }

      /* Flush before introducing new rings (`CONFIG_REG`s for each ring are set only once in an
       * indirect buffer).
       */
      if ((config->compute_.entries_modified &
           BITFIELD_BIT(TERAKAN_HW_CONFIG_SHARED_ENTRY_COMPUTE_SQ_RING_USAGE_LSTMP)) &&
          config->compute_.sq_ring_usage_lstmp &&
          command_writer->indirect_buffer->shader_rings[TERAKAN_SHADER_RING_INDEX_LSTMP]
                .set_base_argument_offsets_dwords[0] == UINT32_MAX) {
         cs_partial_flush = true;
      }

      if (cs_partial_flush) {
         terakan_barrier_emit_actions_unconditionally(command_writer,
                                                      TERAKAN_BARRIER_ACTION_PARTIAL_FLUSH_CS);
      }
   }

   u_foreach_bit (entry_index, config->compute_.entries_modified) {
      terakan_hw_config_shared_compute_emit_functions[entry_index](command_writer);
   }
   config->compute_.entries_modified = 0b0;
}

void
terakan_hw_config_shared_reset(struct terakan_hw_config_shared * const config,
                               struct terakan_physical_device_chip_info const * const chip_info)
{
   /* A freshly reset command writer starts in graphics mode. This must not be
    * inherited from recycled command-buffer memory: the first compute
    * dispatch relies on the graphics-to-compute transition to flush prior
    * work and configure the LS/LDS resource allocation.
    */
   config->is_compute_active_ = false;

   /* No active VK_QUERY_TYPE_PIPELINE_STATISTICS or VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT */
   config->pipelinestat_streamoutstats_enable_ = false;

   config->sq_vtx_start_inst_loc_ = TERAKAN_HW_CONFIG_SHARED_DEFAULT_SQ_VTX_START_INST_LOC;

   config->draw_.vgt_primitive_type = TERAKAN_HW_CONFIG_SHARED_DRAW_DEFAULT_VGT_PRIMITIVE_TYPE;

   memcpy(config->draw_.sq_thread_stack_resource_mgmt.thread,
          chip_info->sq_thread_resource_mgmt_ts_gs_r8xx[0][0], sizeof(uint32_t) * 2);

   config->draw_.sq_thread_stack_resource_mgmt.stack[0] =
      S_008C20_NUM_PS_STACK_ENTRIES(chip_info->sq_max_stack_entries -
                                    chip_info->sq_max_stack_entries / 2) |
      S_008C20_NUM_VS_STACK_ENTRIES(chip_info->sq_max_stack_entries / 2);
   config->draw_.sq_thread_stack_resource_mgmt.stack[1] = 0;
   config->draw_.sq_thread_stack_resource_mgmt.stack[2] = 0;

   config->draw_.vgt_num_instances = TERAKAN_HW_CONFIG_SHARED_DRAW_DEFAULT_VGT_NUM_INSTANCES;

   config->draw_.sq_ring_usage = 0b0;

   config->compute_.sq_ring_usage_lstmp = false;

   terakan_hw_config_shared_set_all_modified(config);
}
