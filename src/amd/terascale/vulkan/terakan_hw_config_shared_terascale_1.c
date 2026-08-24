/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Kept in its own translation unit including only r600d.h/r600d_common.h, deliberately never
 * evergreend.h: several registers at the same offset mean something entirely different on
 * TeraScale 1 than on Evergreen-and-later (0x8C0C is SQ_THREAD_RESOURCE_MGMT here and
 * SQ_GPR_RESOURCE_MGMT_3 there), and while the handful of field macros this file actually uses
 * happen to be bit-identical between the two headers where their names coincide, relying on that
 * for every name either header defines is not something to bet real hardware state on. The classic
 * Gallium R600 driver keeps the same separation, splitting r600_state.c (r600d.h) from
 * evergreen_state.c (evergreend.h) rather than mixing them in one file.
 */

#include "terakan_hw_config_shared_terascale_1.h"

#include "gallium/drivers/r600/r600d.h"
#include "gallium/drivers/r600/r600d_common.h"

#include <stddef.h>

uint32_t *
terakan_hw_config_shared_terascale_1_write_sq_config(
   uint32_t * packet, struct terakan_hw_config_shared_terascale_1_sq_config_info const * const info)
{
   /* Priorities match the fixed ps/vs/gs/es = 0/1/2/3 order r600_init_atom_start_cs() (
    * src/gallium/drivers/r600/r600_state.c) uses unconditionally for every TeraScale 1 chip family;
    * VC_ENABLE is per-family (see terakan_physical_device_chip_info::has_vertex_cache, which the
    * caller already computed the TeraScale 1 way).
    */
   *packet++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
   *packet++ = (R_008C00_SQ_CONFIG - R600_CONFIG_REG_OFFSET) >> 2;
   *packet++ = S_008C00_VC_ENABLE(info->has_vertex_cache) | S_008C00_EXPORT_SRC_C(1) |
              S_008C00_DX9_CONSTS(0) | S_008C00_ALU_INST_PREFER_VECTOR(1) | S_008C00_PS_PRIO(0) |
              S_008C00_VS_PRIO(1) | S_008C00_GS_PRIO(2) | S_008C00_ES_PRIO(3);

   /* One PKT3_SET_CONFIG_REG covering all four consecutive registers 0x8C08-0x8C14; SQ_GPR_RESOURCE_
    * MGMT_1 at 0x8C04 is deliberately not part of this run -- see the comment on
    * terakan_hw_config_shared_terascale_1_sq_config_info.
    */
   *packet++ = PKT3(PKT3_SET_CONFIG_REG, 4, 0);
   *packet++ = (R_008C08_SQ_GPR_RESOURCE_MGMT_2 - R600_CONFIG_REG_OFFSET) >> 2;
   *packet++ = S_008C08_NUM_GS_GPRS(info->num_gs_gprs) | S_008C08_NUM_ES_GPRS(info->num_es_gprs);
   *packet++ = S_008C0C_NUM_PS_THREADS(info->num_ps_threads) |
              S_008C0C_NUM_VS_THREADS(info->num_vs_threads) |
              S_008C0C_NUM_GS_THREADS(info->num_gs_threads) |
              S_008C0C_NUM_ES_THREADS(info->num_es_threads);
   *packet++ = S_008C10_NUM_PS_STACK_ENTRIES(info->num_ps_stack_entries) |
              S_008C10_NUM_VS_STACK_ENTRIES(info->num_vs_stack_entries);
   *packet++ = S_008C14_NUM_GS_STACK_ENTRIES(info->num_gs_stack_entries) |
              S_008C14_NUM_ES_STACK_ENTRIES(info->num_es_stack_entries);

   return packet;
}
