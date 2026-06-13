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

#ifndef TERAKAN_HW_CONFIG_SHARED_H
#define TERAKAN_HW_CONFIG_SHARED_H

#include "terakan_physical_device.h"
#include "terakan_shader.h"

#include "gallium/drivers/r600/evergreend.h"
#include "util/macros.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bottom-level emission of packets setting non-context graphics and compute registers. */

/* Safe defaults for registers, based on the values expected when the respective state is
 * zero-initialized in Vulkan structures or commands, or when the corresponding Vulkan optional
 * features or pipeline stages are disabled.
 */

/* VkPipelineInputAssemblyStateCreateInfo topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST */
#define TERAKAN_HW_CONFIG_SHARED_DRAW_DEFAULT_VGT_PRIMITIVE_TYPE                                   \
   S_008958_PRIM_TYPE(V_008958_DI_PT_POINTLIST)

/* The hardware interprets 0 as 1. */
#define TERAKAN_HW_CONFIG_SHARED_DRAW_DEFAULT_VGT_NUM_INSTANCES 1

/* Register values for compute dispatches. */

#define TERAKAN_HW_CONFIG_SHARED_COMPUTE_VGT_PRIMITIVE_TYPE                                        \
   S_008958_PRIM_TYPE(V_008958_DI_PT_POINTLIST)

/* vkCmdDraw* firstInstance = 0 */
#define TERAKAN_HW_CONFIG_SHARED_DEFAULT_SQ_VTX_START_INST_LOC 0

enum terakan_hw_config_shared_entry {
   /* Generally ordered roughly by the location of the hardware unit in the pipeline, and within
    * each unit, by register address.
    */

   /* Entries for registers always needed for both graphics and compute. */

   TERAKAN_HW_CONFIG_SHARED_ENTRY_PIPELINESTAT_STREAMOUTSTATS_ENABLE,

   TERAKAN_HW_CONFIG_SHARED_ENTRY_VGT_PRIMITIVE_TYPE,

   /* Requires a partial flush of the stages the allocation is changed for (taking into account that
    * CS is LS).
    */
   TERAKAN_HW_CONFIG_SHARED_ENTRY_SQ_THREAD_STACK_RESOURCE_MGMT,

   TERAKAN_HW_CONFIG_SHARED_ENTRY_SQ_VTX_START_INST_LOC,

   TERAKAN_HW_CONFIG_SHARED_ENTRIES_COMMON_COUNT,

   /* Entries for registers needed only for graphics.
    * In some cases, collisions between graphics and compute values of some of these registers still
    * may happen, which must be resolved when switching between graphics and compute.
    */

   TERAKAN_HW_CONFIG_SHARED_ENTRIES_DRAW_ONLY_FIRST =
      TERAKAN_HW_CONFIG_SHARED_ENTRIES_COMMON_COUNT - 1,

   TERAKAN_HW_CONFIG_SHARED_ENTRY_DRAW_VGT_NUM_INSTANCES,

   /* Requires `PS_PARTIAL_FLUSH` before emitting the base and size setting packets for a ring in
    * the indirect buffer, and also `CS_PARTIAL_FLUSH` before emitting the base and size setting
    * packets for the `LSTMP` ring because the LS stage is shared.
    */
   /* `SQ_RING_USAGE` is placed in the draw-only and compute-only ranges, not in the common range,
    * because modification tracking is done very differently from the common entries.
    */
   TERAKAN_HW_CONFIG_SHARED_ENTRY_DRAW_SQ_RING_USAGE,

   TERAKAN_HW_CONFIG_SHARED_ENTRIES_COMMON_AND_DRAW_COUNT,

   /* Entries for registers needed only for compute.
    * In some cases, collisions between graphics and compute values of some of these registers still
    * may happen, which must be resolved when switching between graphics and compute.
    */

   TERAKAN_HW_CONFIG_SHARED_ENTRIES_COMPUTE_ONLY_FIRST =
      TERAKAN_HW_CONFIG_SHARED_ENTRIES_COMMON_COUNT - 1,

   /* Requires `CS_PARTIAL_FLUSH` and, because the LS stage is shared, `VS_PARTIAL_FLUSH` before
    * emitting the base and size setting packets in the indirect buffer.
    */
   TERAKAN_HW_CONFIG_SHARED_ENTRY_COMPUTE_SQ_RING_USAGE_LSTMP,

   TERAKAN_HW_CONFIG_SHARED_ENTRIES_COMMON_AND_COMPUTE_COUNT,
};

static_assert(
   MAX2(TERAKAN_HW_CONFIG_SHARED_ENTRIES_COMMON_AND_DRAW_COUNT,
        TERAKAN_HW_CONFIG_SHARED_ENTRIES_COMMON_AND_COMPUTE_COUNT) <= 32,
   "Using shared graphics / compute hardware configuration entry indices in 32-bit bitfields.");

struct terakan_hw_config_shared {
   bool is_compute_active_;

   bool pipelinestat_streamoutstats_enable_;

   uint32_t sq_vtx_start_inst_loc_;

   /* Graphics-specific values. */
   struct {
      /* Whether each entry starting from `TERAKAN_HW_CONFIG_SHARED_ENTRIES_DRAW_ONLY_FIRST` has
       * been modified and needs to be emitted before the next draw.
       */
      uint32_t entries_modified;

      uint32_t vgt_primitive_type;

      struct {
         uint32_t thread[2];
         uint32_t stack[3];
         bool ps_modified;
      } sq_thread_stack_resource_mgmt;

      uint32_t vgt_num_instances;

      /* Contains bits at `terakan_shader_ring_index` shifts for rings needed for the draw.
       * Base and size setting packets are emitted for all existing graphics pipeline rings at once
       * even if not all rings are actually used, to avoid additional shader invocation flushes, for
       * instance, when merely introducing some shader stages that use scratch memory when some
       * other stages have already used scratch memory in the current indirect buffer. The bitfield
       * is stored to simplify the interface.
       */
      uint32_t sq_ring_usage;
   } draw_;

   /* Compute-specific values. */
   struct {
      /* Whether each entry starting from `TERAKAN_HW_CONFIG_SHARED_ENTRIES_COMPUTE_ONLY_FIRST` has
       * been modified and needs to be emitted before the next dispatch.
       */
      uint32_t entries_modified;

      bool sq_ring_usage_lstmp;
   } compute_;
};

static inline uint32_t *
terakan_hw_config_shared_get_common_entries_modified_ptr_(
   struct terakan_hw_config_shared * const config)
{
   return config->is_compute_active_ ? &config->compute_.entries_modified
                                     : &config->draw_.entries_modified;
}

static inline void
terakan_hw_config_shared_set_pipelinestat_streamoutstats_enable(
   struct terakan_hw_config_shared * const config, bool const value)
{
   if (config->pipelinestat_streamoutstats_enable_ == value) {
      return;
   }
   config->pipelinestat_streamoutstats_enable_ = value;
   *terakan_hw_config_shared_get_common_entries_modified_ptr_(config) |=
      BITFIELD_BIT(TERAKAN_HW_CONFIG_SHARED_ENTRY_PIPELINESTAT_STREAMOUTSTATS_ENABLE);
}

static inline void
terakan_hw_config_shared_sq_vtx_start_inst_loc(struct terakan_hw_config_shared * const config,
                                               uint32_t const value)
{
   if (config->sq_vtx_start_inst_loc_ == value) {
      return;
   }
   config->sq_vtx_start_inst_loc_ = value;
   *terakan_hw_config_shared_get_common_entries_modified_ptr_(config) |=
      BITFIELD_BIT(TERAKAN_HW_CONFIG_SHARED_ENTRY_SQ_VTX_START_INST_LOC);
}

static inline void
terakan_hw_config_shared_draw_set_vgt_primitive_type(struct terakan_hw_config_shared * const config,
                                                     uint32_t const value)
{
   if (config->draw_.vgt_primitive_type == value) {
      return;
   }
   config->draw_.vgt_primitive_type = value;
   config->draw_.entries_modified |=
      BITFIELD_BIT(TERAKAN_HW_CONFIG_SHARED_ENTRY_VGT_PRIMITIVE_TYPE);
}

static inline void
terakan_hw_config_shared_draw_set_vgt_num_instances(struct terakan_hw_config_shared * const config,
                                                    uint32_t const value)
{
   assert(
      value != 0 &&
      "The hardware interprets VGT_NUM_INSTANCES = 0 as 1, zero instances is likely unintended");
   if (config->draw_.vgt_num_instances == value) {
      return;
   }
   config->draw_.vgt_num_instances = value;
   config->draw_.entries_modified |=
      BITFIELD_BIT(TERAKAN_HW_CONFIG_SHARED_ENTRY_DRAW_VGT_NUM_INSTANCES);
}

static inline void
terakan_hw_config_shared_draw_set_sq_thread_resource_mgmt(
   struct terakan_hw_config_shared * const config, uint32_t const value[2])
{
   if (memcmp(config->draw_.sq_thread_stack_resource_mgmt.thread, value, sizeof(uint32_t) * 2) ==
       0) {
      return;
   }
   if (G_008C18_NUM_PS_THREADS(config->draw_.sq_thread_stack_resource_mgmt.thread[0]) !=
       G_008C18_NUM_PS_THREADS(value[0])) {
      config->draw_.sq_thread_stack_resource_mgmt.ps_modified = true;
   }
   memcpy(config->draw_.sq_thread_stack_resource_mgmt.thread, value, sizeof(uint32_t) * 2);
   config->draw_.entries_modified |=
      BITFIELD_BIT(TERAKAN_HW_CONFIG_SHARED_ENTRY_SQ_THREAD_STACK_RESOURCE_MGMT);
}

static inline void
terakan_hw_config_shared_draw_set_sq_stack_resource_mgmt(
   struct terakan_hw_config_shared * const config, uint32_t const value[3])
{
   if (memcmp(config->draw_.sq_thread_stack_resource_mgmt.stack, value, sizeof(uint32_t) * 3) ==
       0) {
      return;
   }
   if (G_008C20_NUM_PS_STACK_ENTRIES(config->draw_.sq_thread_stack_resource_mgmt.stack[0]) !=
       G_008C20_NUM_PS_STACK_ENTRIES(value[0])) {
      config->draw_.sq_thread_stack_resource_mgmt.ps_modified = true;
   }
   memcpy(config->draw_.sq_thread_stack_resource_mgmt.stack, value, sizeof(uint32_t) * 3);
   config->draw_.entries_modified |=
      BITFIELD_BIT(TERAKAN_HW_CONFIG_SHARED_ENTRY_SQ_THREAD_STACK_RESOURCE_MGMT);
}

static inline void
terakan_hw_config_shared_draw_set_sq_ring_usage(struct terakan_hw_config_shared * const config,
                                                uint32_t const replace_mask, uint32_t const value)
{
   assert(!(replace_mask & ~BITFIELD_MASK(TERAKAN_SHADER_RING_INDEX_COUNT)));
   assert(!(value & ~replace_mask));
   /* Emitting packets for all rings at once when any ring is needed, so it's enough to check just
    * whether ring usage is being enabled at all.
    */
   bool const old_any_ring_used = config->draw_.sq_ring_usage != 0b0;
   config->draw_.sq_ring_usage = (config->draw_.sq_ring_usage & ~replace_mask) | value;
   bool const new_any_ring_used = config->draw_.sq_ring_usage != 0b0;
   if (old_any_ring_used != new_any_ring_used) {
      if (new_any_ring_used) {
         config->draw_.entries_modified |=
            BITFIELD_BIT(TERAKAN_HW_CONFIG_SHARED_ENTRY_DRAW_SQ_RING_USAGE);
      } else {
         /* Nothing to do when all rings are disabled. */
         config->draw_.entries_modified &=
            ~BITFIELD_BIT(TERAKAN_HW_CONFIG_SHARED_ENTRY_DRAW_SQ_RING_USAGE);
      }
   }
}

static inline void
terakan_hw_config_shared_compute_set_sq_ring_usage_lstmp(
   struct terakan_hw_config_shared * const config, bool const value)
{
   if (config->compute_.sq_ring_usage_lstmp == value) {
      return;
   }
   config->compute_.sq_ring_usage_lstmp = value;
   if (value) {
      config->compute_.entries_modified |=
         BITFIELD_BIT(TERAKAN_HW_CONFIG_SHARED_ENTRY_COMPUTE_SQ_RING_USAGE_LSTMP);
   } else {
      /* Nothing to do when the ring is disabled. */
      config->compute_.entries_modified &=
         ~BITFIELD_BIT(TERAKAN_HW_CONFIG_SHARED_ENTRY_COMPUTE_SQ_RING_USAGE_LSTMP);
   }
}

void
terakan_hw_config_shared_indirect_buffer_begun(struct terakan_gfx_command_writer * command_writer);

void
terakan_hw_config_shared_draw_emit_modified(struct terakan_gfx_command_writer * command_writer);
void
terakan_hw_config_shared_compute_emit_modified(struct terakan_gfx_command_writer * command_writer);

void terakan_hw_config_shared_reset(struct terakan_hw_config_shared * config,
                                    struct terakan_physical_device_chip_info const * chip_info);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_HW_CONFIG_SHARED_H */
