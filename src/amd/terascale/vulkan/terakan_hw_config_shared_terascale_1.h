/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_H
#define TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PKT3_SET_CONFIG_REG(SQ_CONFIG) + PKT3_SET_CONFIG_REG(GPR_RESOURCE_MGMT_2, THREAD_RESOURCE_MGMT,
 * STACK_RESOURCE_MGMT_1, STACK_RESOURCE_MGMT_2).
 */
#define TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_SQ_CONFIG_DWORDS (3 + 6)

/* The subset of chip_info->terascale_1 (see terakan_physical_device.h) this needs, kept as its own
 * plain struct rather than taking terakan_physical_device_chip_info directly so this file and its
 * test stay decoupled from the rest of the driver, matching terakan_hw_config_loop_constants.{c,h}.
 * Deliberately excludes num_ps_gprs/num_vs_gprs/num_temp_gprs: those feed SQ_GPR_RESOURCE_MGMT_1,
 * which the classic Gallium R600 driver (r600_update_gpr_alloc() in
 * src/gallium/drivers/r600/r600_state.c) re-emits per shader-binding change, not once per command
 * buffer alongside the registers here -- not yet ported, tracked in TODO.md.
 */
struct terakan_hw_config_shared_terascale_1_sq_config_info {
   bool has_vertex_cache;
   uint32_t num_gs_gprs, num_es_gprs;
   uint32_t num_ps_threads, num_vs_threads, num_gs_threads, num_es_threads;
   uint32_t num_ps_stack_entries, num_vs_stack_entries, num_gs_stack_entries,
      num_es_stack_entries;
};

/* Writes TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_SQ_CONFIG_DWORDS dwords to `packet` and returns the
 * advanced pointer, the same convention terakan_hw_config_loop_constants_write() uses.
 */
uint32_t * terakan_hw_config_shared_terascale_1_write_sq_config(
   uint32_t * packet,
   struct terakan_hw_config_shared_terascale_1_sq_config_info const * info);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_H */
