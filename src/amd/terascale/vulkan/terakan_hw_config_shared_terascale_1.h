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

/* PKT3_SET_CONFIG_REG(SQ_CONFIG) + PKT3_SET_CONFIG_REG(GPR_RESOURCE_MGMT_1/2,
 * THREAD_RESOURCE_MGMT, STACK_RESOURCE_MGMT_1/2).
 */
#define TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_SQ_CONFIG_DWORDS (3 + 7)

/* The subset of chip_info->terascale_1 (see terakan_physical_device.h) this needs, kept as its own
 * plain struct rather than taking terakan_physical_device_chip_info directly so this file and its
 * test stay decoupled from the rest of the driver, matching terakan_hw_config_loop_constants.{c,h}.
 * The values are the per-family baseline from r600_init_atom_start_cs(). The classic driver may
 * later redistribute GPRs in r600_adjust_gprs() when a bound shader exceeds that baseline; Terakan
 * does not enable submission yet, and that dynamic redistribution remains a separate draw-state
 * task rather than leaving SQ_GPR_RESOURCE_MGMT_1 entirely uninitialized here.
 */
struct terakan_hw_config_shared_terascale_1_sq_config_info {
   bool has_vertex_cache;
   uint32_t num_ps_gprs, num_vs_gprs, num_temp_gprs;
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

/* R600/R700 has no Evergreen SQ_LDS_RESOURCE_MGMT register. Compute LDS allocation has not been
 * identified yet, so the only safe graphics-to-compute transition packet is empty.
 */
uint32_t terakan_hw_config_shared_terascale_1_compute_lds_packet_dwords(void);

/* Upper bound on the dword count terakan_hw_config_shared_terascale_1_write_context_defaults()
 * writes (the R700 case; R600 writes 9 dwords fewer, 3 registers -- VGT_ENHANCE,
 * PA_SC_EDGERULE and SX_MISC -- that only exist for R700 in the reference function). Reserve this
 * many, use the function's returned end pointer for how many were actually written -- the same
 * pattern terakan_gfx_command_writer_emit() callers already use.
 */
#define TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_CONTEXT_DEFAULTS_MAX_DWORDS 191

/* The remainder of r600_init_atom_start_cs() (src/gallium/drivers/r600/r600_state.c) after the
 * SQ_CONFIG block above: resets the rest of the 3D context state this driver's command buffers
 * assume is in a known state, plus the initial SQ_LOOP_CONST values. Streaming output is not
 * implemented for TeraScale 1 (or, so far, at all beyond what P1 in TODO.md tracks for R8xx/R9xx),
 * so the reference function's has_streamout-conditional stores (VGT_STRMOUT_DRAW_OPAQUE_OFFSET,
 * and SX_SURFACE_SYNC's has_streamout half of its condition) are never written here; nothing
 * currently needs them, and writing them for a feature with no consumer would be exactly the kind
 * of speculative code this project's conventions ask not to add. `is_r700` selects between the
 * reference function's `gfx_level == R700`/`gfx_level >= R700` branches and its `else` (R600)
 * ones; R700 writes 3 extra registers (VGT_ENHANCE, PA_SC_EDGERULE, SX_MISC) that R600 has no
 * equivalent for at all, see TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_CONTEXT_DEFAULTS_MAX_DWORDS.
 */
uint32_t * terakan_hw_config_shared_terascale_1_write_context_defaults(uint32_t * packet,
                                                                       bool is_r700);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_H */
