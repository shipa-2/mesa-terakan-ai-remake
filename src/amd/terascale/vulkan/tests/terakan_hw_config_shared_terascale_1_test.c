/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_hw_config_shared_terascale_1.h"

#include "gallium/drivers/r600/r600d.h"
#include "gallium/drivers/r600/r600d_common.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                                           \
   do {                                                                                            \
      if (!(condition)) {                                                                          \
         fprintf(stderr, "CHECK failed at %s:%u: %s\n", __FILE__, __LINE__, #condition);           \
         abort();                                                                                  \
      }                                                                                            \
   } while (0)

#define CANARY 0xA5A5A5A5u

/* RV710's real values (src/gallium/drivers/r600/r600_state.c), chosen because it is the TeraScale 1
 * card this driver has actually been run against.
 */
static struct terakan_hw_config_shared_terascale_1_sq_config_info const rv710_info = {
   .has_vertex_cache = false,
   .num_gs_gprs = 0,
   .num_es_gprs = 0,
   .num_ps_threads = 136,
   .num_vs_threads = 48,
   .num_gs_threads = 4,
   .num_es_threads = 4,
   .num_ps_stack_entries = 128,
   .num_vs_stack_entries = 128,
   .num_gs_stack_entries = 0,
   .num_es_stack_entries = 0,
};

static void
test_sq_config_packets(void)
{
   uint32_t guarded_packets[TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_SQ_CONFIG_DWORDS + 2];
   guarded_packets[0] = CANARY;
   guarded_packets[TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_SQ_CONFIG_DWORDS + 1] = CANARY;

   uint32_t * const packets = &guarded_packets[1];
   uint32_t * const end =
      terakan_hw_config_shared_terascale_1_write_sq_config(packets, &rv710_info);

   CHECK(end == packets + TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_SQ_CONFIG_DWORDS);
   CHECK(guarded_packets[0] == CANARY);
   CHECK(guarded_packets[TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_SQ_CONFIG_DWORDS + 1] == CANARY);

   /* SQ_CONFIG, its own PKT3_SET_CONFIG_REG. VC_ENABLE is 0 for RV710 (see
    * terakan_physical_device_chip_info_init's has_vertex_cache switch); priorities are the fixed
    * ps/vs/gs/es = 0/1/2/3 every TeraScale 1 chip family uses.
    */
   CHECK(packets[0] == PKT3(PKT3_SET_CONFIG_REG, 1, 0));
   CHECK(packets[1] == (R_008C00_SQ_CONFIG - R600_CONFIG_REG_OFFSET) >> 2);
   CHECK(packets[2] == (S_008C00_EXPORT_SRC_C(1) | S_008C00_ALU_INST_PREFER_VECTOR(1) |
                        S_008C00_VS_PRIO(1) | S_008C00_GS_PRIO(2) | S_008C00_ES_PRIO(3)));

   /* GPR_RESOURCE_MGMT_2, THREAD_RESOURCE_MGMT, STACK_RESOURCE_MGMT_1/2, one PKT3_SET_CONFIG_REG
    * covering all four consecutive registers.
    */
   CHECK(packets[3] == PKT3(PKT3_SET_CONFIG_REG, 4, 0));
   CHECK(packets[4] == (R_008C08_SQ_GPR_RESOURCE_MGMT_2 - R600_CONFIG_REG_OFFSET) >> 2);
   CHECK(packets[5] == 0);
   CHECK(packets[6] == (S_008C0C_NUM_PS_THREADS(136) | S_008C0C_NUM_VS_THREADS(48) |
                        S_008C0C_NUM_GS_THREADS(4) | S_008C0C_NUM_ES_THREADS(4)));
   CHECK(packets[7] == (S_008C10_NUM_PS_STACK_ENTRIES(128) | S_008C10_NUM_VS_STACK_ENTRIES(128)));
   CHECK(packets[8] == 0);
}

int
main(void)
{
   test_sq_config_packets();
   return 0;
}
