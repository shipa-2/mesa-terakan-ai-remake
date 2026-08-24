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

static uint32_t *
write_config_reg(uint32_t * packet, uint32_t const reg, uint32_t const value)
{
   *packet++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
   *packet++ = (reg - R600_CONFIG_REG_OFFSET) >> 2;
   *packet++ = value;
   return packet;
}

static uint32_t *
write_context_reg(uint32_t * packet, uint32_t const reg, uint32_t const value)
{
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
   *packet++ = (reg - R600_CONTEXT_REG_OFFSET) >> 2;
   *packet++ = value;
   return packet;
}

static uint32_t *
write_context_reg_seq_header(uint32_t * packet, uint32_t const reg, uint32_t const count)
{
   *packet++ = PKT3(PKT3_SET_CONTEXT_REG, count, 0);
   *packet++ = (reg - R600_CONTEXT_REG_OFFSET) >> 2;
   return packet;
}

/* R600_CTL_CONST_OFFSET (src/gallium/drivers/r600/r600_pipe.h), not pulled in from there to avoid
 * the rest of that driver-context-sized header; the only ctl_const register this function writes
 * is the base of that space itself, so the offset arithmetic in write_ctl_const() below reduces to
 * zero for it, but the constant is kept symbolic rather than inlined as 0 so the intent reads the
 * same way the config/context helpers above do.
 */
#define TERASCALE_1_CTL_CONST_OFFSET 0x3CFF0u

static uint32_t *
write_ctl_const(uint32_t * packet, uint32_t const reg, uint32_t const value)
{
   *packet++ = PKT3(PKT3_SET_CTL_CONST, 1, 0);
   *packet++ = (reg - TERASCALE_1_CTL_CONST_OFFSET) >> 2;
   *packet++ = value;
   return packet;
}

static uint32_t *
write_loop_const(uint32_t * packet, uint32_t const reg, uint32_t const value)
{
   *packet++ = PKT3(PKT3_SET_LOOP_CONST, 1, 0);
   *packet++ = (reg - R600_LOOP_CONST_OFFSET) >> 2;
   *packet++ = value;
   return packet;
}

uint32_t *
terakan_hw_config_shared_terascale_1_write_context_defaults(uint32_t * packet, bool const is_r700)
{
   packet = write_config_reg(packet, R_009714_VC_ENHANCE, 0);

   if (is_r700) {
      packet = write_context_reg(packet, R_028A50_VGT_ENHANCE, 4);
      packet = write_config_reg(packet, R_008D8C_SQ_DYN_GPR_CNTL_PS_FLUSH_REQ,
                                S_008D8C_VS_PC_LIMIT_ENABLE(1));
      packet = write_config_reg(packet, R_009830_DB_DEBUG, 0);
      packet = write_config_reg(packet, R_009838_DB_WATERMARKS, 0x00420204);
      packet = write_context_reg(packet, R_0286C8_SPI_THREAD_GROUPING, 0);
   } else {
      packet = write_config_reg(packet, R_008D8C_SQ_DYN_GPR_CNTL_PS_FLUSH_REQ, 0);
      packet = write_config_reg(packet, R_009830_DB_DEBUG, 0x82000000);
      packet = write_config_reg(packet, R_009838_DB_WATERMARKS, 0x01020204);
      packet = write_context_reg(packet, R_0286C8_SPI_THREAD_GROUPING, 1);
   }

   packet = write_context_reg_seq_header(packet, R_0288A8_SQ_ESGS_RING_ITEMSIZE, 9);
   for (uint32_t i = 0; i < 9; ++i) {
      *packet++ = 0;
   }

   /* To avoid the GPU preloading constants from a random address. */
   packet = write_context_reg_seq_header(packet, R_028140_ALU_CONST_BUFFER_SIZE_PS_0, 16);
   for (uint32_t i = 0; i < 16; ++i) {
      *packet++ = 0;
   }
   packet = write_context_reg_seq_header(packet, R_028180_ALU_CONST_BUFFER_SIZE_VS_0, 16);
   for (uint32_t i = 0; i < 16; ++i) {
      *packet++ = 0;
   }
   packet = write_context_reg_seq_header(packet, R_0281C0_ALU_CONST_BUFFER_SIZE_GS_0, 16);
   for (uint32_t i = 0; i < 16; ++i) {
      *packet++ = 0;
   }

   packet = write_context_reg_seq_header(packet, R_028A10_VGT_OUTPUT_PATH_CNTL, 13);
   for (uint32_t i = 0; i < 13; ++i) {
      *packet++ = 0;
   }

   packet = write_context_reg(packet, R_028A84_VGT_PRIMITIVEID_EN, 0);
   packet = write_context_reg(packet, R_028AA0_VGT_INSTANCE_STEP_RATE_0, 0);
   packet = write_context_reg(packet, R_028AA4_VGT_INSTANCE_STEP_RATE_1, 0);

   packet = write_context_reg_seq_header(packet, R_028AB4_VGT_REUSE_OFF, 2);
   *packet++ = 1;
   *packet++ = 0;

   packet = write_context_reg(packet, R_028B20_VGT_STRMOUT_BUFFER_EN, 0);

   packet = write_ctl_const(packet, R_03CFF0_SQ_VTX_BASE_VTX_LOC, 0);

   packet = write_context_reg(packet, R_028028_DB_STENCIL_CLEAR, 0);

   packet = write_context_reg_seq_header(packet, R_0286DC_SPI_FOG_CNTL, 3);
   *packet++ = 0;
   *packet++ = 0;
   *packet++ = 0;

   packet = write_context_reg_seq_header(packet, R_028D28_DB_SRESULTS_COMPARE_STATE0, 3);
   *packet++ = 0;
   *packet++ = 0;
   *packet++ = 0;

   packet = write_context_reg(packet, R_028820_PA_CL_NANINF_CNTL, 0);
   packet = write_context_reg(packet, R_028A48_PA_SC_MPASS_PS_CNTL, 0);

   packet = write_context_reg(packet, R_028200_PA_SC_WINDOW_OFFSET, 0);
   packet = write_context_reg(packet, R_02820C_PA_SC_CLIPRECT_RULE, 0xFFFF);

   if (is_r700) {
      packet = write_context_reg(packet, R_028230_PA_SC_EDGERULE, 0xAAAAAAAAu);
   }

   packet = write_context_reg_seq_header(packet, R_028C30_CB_CLRCMP_CONTROL, 4);
   *packet++ = 0x1000000;
   *packet++ = 0;
   *packet++ = 0xFF;
   *packet++ = 0xFFFFFFFFu;

   packet = write_context_reg_seq_header(packet, R_028030_PA_SC_SCREEN_SCISSOR_TL, 2);
   *packet++ = 0;
   *packet++ = S_028034_BR_X(8192) | S_028034_BR_Y(8192);

   packet = write_context_reg_seq_header(packet, R_028240_PA_SC_GENERIC_SCISSOR_TL, 2);
   *packet++ = 0;
   *packet++ = S_028244_BR_X(8192) | S_028244_BR_Y(8192);

   packet = write_context_reg_seq_header(packet, R_0288CC_SQ_PGM_CF_OFFSET_PS, 5);
   for (uint32_t i = 0; i < 5; ++i) {
      *packet++ = 0;
   }

   packet = write_context_reg(packet, R_0288E0_SQ_VTX_SEMANTIC_CLEAR, ~0u);

   packet = write_context_reg_seq_header(packet, R_028400_VGT_MAX_VTX_INDX, 2);
   *packet++ = ~0u;
   *packet++ = 0;

   packet = write_context_reg(packet, R_0288A4_SQ_PGM_RESOURCES_FS, 0);

   if (is_r700) {
      packet = write_context_reg(packet, R_028350_SX_MISC, 0);
   }
   /* R_028354_SX_SURFACE_SYNC is also R700-and-streaming-output-only in the reference function;
    * streaming output is never on here, see the comment on this function's declaration.
    */

   packet = write_context_reg(packet, R_028800_DB_DEPTH_CONTROL, 0);
   /* R_028B28_VGT_STRMOUT_DRAW_OPAQUE_OFFSET is streaming-output-only in the reference function. */

   packet = write_loop_const(packet, R_03E200_SQ_LOOP_CONST_0, 0x1000FFFu);
   packet = write_loop_const(packet, R_03E200_SQ_LOOP_CONST_0 + 32 * 4, 0x1000FFFu);
   packet = write_loop_const(packet, R_03E200_SQ_LOOP_CONST_0 + 64 * 4, 0x1000FFFu);

   return packet;
}

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
