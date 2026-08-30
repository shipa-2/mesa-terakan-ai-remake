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
   .num_ps_gprs = 192,
   .num_vs_gprs = 56,
   .num_temp_gprs = 4,
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
   CHECK(packets[2] == (S_008C00_ALU_INST_PREFER_VECTOR(1) | S_008C00_VS_PRIO(1) |
                        S_008C00_GS_PRIO(2) | S_008C00_ES_PRIO(3)));

   /* GPR_RESOURCE_MGMT_1/2, THREAD_RESOURCE_MGMT, STACK_RESOURCE_MGMT_1/2, one
    * PKT3_SET_CONFIG_REG covering all five consecutive R600/R700 registers. 0x8C0C is thread
    * management here, not Evergreen GPR_RESOURCE_MGMT_3.
    */
   CHECK(packets[3] == PKT3(PKT3_SET_CONFIG_REG, 5, 0));
   CHECK(packets[4] == (R_008C04_SQ_GPR_RESOURCE_MGMT_1 - R600_CONFIG_REG_OFFSET) >> 2);
   CHECK(packets[5] == (S_008C04_NUM_PS_GPRS(192) | S_008C04_NUM_VS_GPRS(56) |
                        S_008C04_NUM_CLAUSE_TEMP_GPRS(4)));
   CHECK(packets[6] == 0);
   CHECK(packets[7] == (S_008C0C_NUM_PS_THREADS(136) | S_008C0C_NUM_VS_THREADS(48) |
                        S_008C0C_NUM_GS_THREADS(4) | S_008C0C_NUM_ES_THREADS(4)));
   CHECK(packets[8] == (S_008C10_NUM_PS_STACK_ENTRIES(128) | S_008C10_NUM_VS_STACK_ENTRIES(128)));
   CHECK(packets[9] == 0);
}

static void
test_compute_lds_packet_is_empty(void)
{
   CHECK(terakan_hw_config_shared_terascale_1_compute_lds_packet_dwords() == 0);
}

/* R700 (RV710 is R700) writes 191 dwords: two blocks (PA_SC_EDGERULE and SX_MISC) exist only for
 * R700, at 3 dwords each -- see the comment on
 * terakan_hw_config_shared_terascale_1_write_context_defaults()'s declaration.
 */
static void
test_context_defaults_packets_r700(void)
{
   uint32_t
      guarded_packets[TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_CONTEXT_DEFAULTS_MAX_DWORDS + 2];
   guarded_packets[0] = CANARY;
   guarded_packets[TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_CONTEXT_DEFAULTS_MAX_DWORDS + 1] = CANARY;

   uint32_t * const packets = &guarded_packets[1];
   uint32_t * const end =
      terakan_hw_config_shared_terascale_1_write_context_defaults(packets, true);

   CHECK(end == packets + TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_CONTEXT_DEFAULTS_MAX_DWORDS);
   CHECK(guarded_packets[0] == CANARY);
   CHECK(guarded_packets[TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_CONTEXT_DEFAULTS_MAX_DWORDS + 1] ==
        CANARY);

   uint32_t i = 0;

   CHECK(packets[i++] == PKT3(PKT3_SET_CONFIG_REG, 1, 0));
   CHECK(packets[i++] == (R_009714_VC_ENHANCE - R600_CONFIG_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0);

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[i++] == (R_028A50_VGT_ENHANCE - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 4);
   CHECK(packets[i++] == PKT3(PKT3_SET_CONFIG_REG, 1, 0));
   CHECK(packets[i++] == (R_008D8C_SQ_DYN_GPR_CNTL_PS_FLUSH_REQ - R600_CONFIG_REG_OFFSET) >> 2);
   CHECK(packets[i++] == S_008D8C_VS_PC_LIMIT_ENABLE(1));
   CHECK(packets[i++] == PKT3(PKT3_SET_CONFIG_REG, 1, 0));
   CHECK(packets[i++] == (R_009830_DB_DEBUG - R600_CONFIG_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0);
   CHECK(packets[i++] == PKT3(PKT3_SET_CONFIG_REG, 1, 0));
   CHECK(packets[i++] == (R_009838_DB_WATERMARKS - R600_CONFIG_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0x00420204);
   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[i++] == (R_0286C8_SPI_THREAD_GROUPING - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0);

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 9, 0));
   CHECK(packets[i++] == (R_0288A8_SQ_ESGS_RING_ITEMSIZE - R600_CONTEXT_REG_OFFSET) >> 2);
   for (uint32_t j = 0; j < 9; ++j) {
      CHECK(packets[i++] == 0);
   }

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 16, 0));
   CHECK(packets[i++] == (R_028140_ALU_CONST_BUFFER_SIZE_PS_0 - R600_CONTEXT_REG_OFFSET) >> 2);
   for (uint32_t j = 0; j < 16; ++j) {
      CHECK(packets[i++] == 0);
   }
   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 16, 0));
   CHECK(packets[i++] == (R_028180_ALU_CONST_BUFFER_SIZE_VS_0 - R600_CONTEXT_REG_OFFSET) >> 2);
   for (uint32_t j = 0; j < 16; ++j) {
      CHECK(packets[i++] == 0);
   }
   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 16, 0));
   CHECK(packets[i++] == (R_0281C0_ALU_CONST_BUFFER_SIZE_GS_0 - R600_CONTEXT_REG_OFFSET) >> 2);
   for (uint32_t j = 0; j < 16; ++j) {
      CHECK(packets[i++] == 0);
   }

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 13, 0));
   CHECK(packets[i++] == (R_028A10_VGT_OUTPUT_PATH_CNTL - R600_CONTEXT_REG_OFFSET) >> 2);
   for (uint32_t j = 0; j < 13; ++j) {
      CHECK(packets[i++] == 0);
   }

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[i++] == (R_028A84_VGT_PRIMITIVEID_EN - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0);
   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[i++] == (R_028AA0_VGT_INSTANCE_STEP_RATE_0 - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0);
   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[i++] == (R_028AA4_VGT_INSTANCE_STEP_RATE_1 - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0);

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 2, 0));
   CHECK(packets[i++] == (R_028AB4_VGT_REUSE_OFF - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 1);
   CHECK(packets[i++] == 0);

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[i++] == (R_028B20_VGT_STRMOUT_BUFFER_EN - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0);

   CHECK(packets[i++] == PKT3(PKT3_SET_CTL_CONST, 1, 0));
   CHECK(packets[i++] == (R_03CFF0_SQ_VTX_BASE_VTX_LOC - 0x3CFF0u) >> 2);
   CHECK(packets[i++] == 0);

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[i++] == (R_028028_DB_STENCIL_CLEAR - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0);

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 3, 0));
   CHECK(packets[i++] == (R_0286DC_SPI_FOG_CNTL - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0);
   CHECK(packets[i++] == 0);
   CHECK(packets[i++] == 0);

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 3, 0));
   CHECK(packets[i++] == (R_028D28_DB_SRESULTS_COMPARE_STATE0 - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0);
   CHECK(packets[i++] == 0);
   CHECK(packets[i++] == 0);

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[i++] == (R_028820_PA_CL_NANINF_CNTL - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0);
   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[i++] == (R_028A48_PA_SC_MPASS_PS_CNTL - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0);

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[i++] == (R_028200_PA_SC_WINDOW_OFFSET - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0);
   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[i++] == (R_02820C_PA_SC_CLIPRECT_RULE - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0xFFFF);

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[i++] == (R_028230_PA_SC_EDGERULE - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0xAAAAAAAAu);

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 4, 0));
   CHECK(packets[i++] == (R_028C30_CB_CLRCMP_CONTROL - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0x1000000);
   CHECK(packets[i++] == 0);
   CHECK(packets[i++] == 0xFF);
   CHECK(packets[i++] == 0xFFFFFFFFu);

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 2, 0));
   CHECK(packets[i++] == (R_028030_PA_SC_SCREEN_SCISSOR_TL - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0);
   CHECK(packets[i++] == (S_028034_BR_X(8192) | S_028034_BR_Y(8192)));

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 2, 0));
   CHECK(packets[i++] == (R_028240_PA_SC_GENERIC_SCISSOR_TL - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0);
   CHECK(packets[i++] == (S_028244_BR_X(8192) | S_028244_BR_Y(8192)));

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 5, 0));
   CHECK(packets[i++] == (R_0288CC_SQ_PGM_CF_OFFSET_PS - R600_CONTEXT_REG_OFFSET) >> 2);
   for (uint32_t j = 0; j < 5; ++j) {
      CHECK(packets[i++] == 0);
   }

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[i++] == (R_0288E0_SQ_VTX_SEMANTIC_CLEAR - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == ~0u);

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 2, 0));
   CHECK(packets[i++] == (R_028400_VGT_MAX_VTX_INDX - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == ~0u);
   CHECK(packets[i++] == 0);

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[i++] == (R_0288A4_SQ_PGM_RESOURCES_FS - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0);

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[i++] == (R_028350_SX_MISC - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0);

   CHECK(packets[i++] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[i++] == (R_028800_DB_DEPTH_CONTROL - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[i++] == 0);

   CHECK(packets[i++] == PKT3(PKT3_SET_LOOP_CONST, 1, 0));
   CHECK(packets[i++] == (R_03E200_SQ_LOOP_CONST_0 - R600_LOOP_CONST_OFFSET) >> 2);
   CHECK(packets[i++] == 0x1000FFFu);
   CHECK(packets[i++] == PKT3(PKT3_SET_LOOP_CONST, 1, 0));
   CHECK(packets[i++] == (R_03E200_SQ_LOOP_CONST_0 + 32 * 4 - R600_LOOP_CONST_OFFSET) >> 2);
   CHECK(packets[i++] == 0x1000FFFu);
   CHECK(packets[i++] == PKT3(PKT3_SET_LOOP_CONST, 1, 0));
   CHECK(packets[i++] == (R_03E200_SQ_LOOP_CONST_0 + 64 * 4 - R600_LOOP_CONST_OFFSET) >> 2);
   CHECK(packets[i++] == 0x1000FFFu);

   CHECK(i == TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_CONTEXT_DEFAULTS_MAX_DWORDS);
}

/* R600 (not R700) omits VGT_ENHANCE, PA_SC_EDGERULE and SX_MISC, 3 dwords each, none of which R600
 * has an equivalent for at all: 182 instead of 191. The branch right after VC_ENHANCE takes R600's
 * values and, unlike R700's, has no VGT_ENHANCE store, so it starts 3 dwords earlier than in the
 * R700 test above.
 */
static void
test_context_defaults_packets_r600(void)
{
   uint32_t packets[TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_CONTEXT_DEFAULTS_MAX_DWORDS];
   uint32_t * const end =
      terakan_hw_config_shared_terascale_1_write_context_defaults(packets, false);
   uint32_t const dwords_written = (uint32_t)(end - packets);
   CHECK(dwords_written ==
        TERAKAN_HW_CONFIG_SHARED_TERASCALE_1_CONTEXT_DEFAULTS_MAX_DWORDS - 9);

   CHECK(packets[3] == PKT3(PKT3_SET_CONFIG_REG, 1, 0));
   CHECK(packets[4] == (R_008D8C_SQ_DYN_GPR_CNTL_PS_FLUSH_REQ - R600_CONFIG_REG_OFFSET) >> 2);
   CHECK(packets[5] == 0);
   CHECK(packets[6] == PKT3(PKT3_SET_CONFIG_REG, 1, 0));
   CHECK(packets[7] == (R_009830_DB_DEBUG - R600_CONFIG_REG_OFFSET) >> 2);
   CHECK(packets[8] == 0x82000000u);
   CHECK(packets[9] == PKT3(PKT3_SET_CONFIG_REG, 1, 0));
   CHECK(packets[10] == (R_009838_DB_WATERMARKS - R600_CONFIG_REG_OFFSET) >> 2);
   CHECK(packets[11] == 0x01020204u);
   CHECK(packets[12] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[13] == (R_0286C8_SPI_THREAD_GROUPING - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[14] == 1);
}

int
main(void)
{
   test_sq_config_packets();
   test_compute_lds_packet_is_empty();
   test_context_defaults_packets_r700();
   test_context_defaults_packets_r600();
   return 0;
}
