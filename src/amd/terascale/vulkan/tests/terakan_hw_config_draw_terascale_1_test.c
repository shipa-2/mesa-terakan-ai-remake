/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_hw_config_draw_terascale_1.h"

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

static void
test_db_depth_control(void)
{
   /* An arbitrary but representative value: depth test and write enabled, LEQUAL, stencil enabled
    * with distinct front/back ops, exercising fields across the whole register rather than just its
    * low bits.
    */
   uint32_t const value = S_028800_Z_ENABLE(1) | S_028800_Z_WRITE_ENABLE(1) |
                          S_028800_ZFUNC(V_028800_STENCILFUNC_LEQUAL) |
                          S_028800_STENCIL_ENABLE(1) |
                          S_028800_STENCILFUNC(V_028800_STENCILFUNC_ALWAYS) |
                          S_028800_STENCILZPASS(V_028800_STENCIL_KEEP) |
                          S_028800_STENCILZPASS_BF(V_028800_STENCIL_REPLACE);

   uint32_t packets[3];
   uint32_t * const end = terakan_hw_config_draw_terascale_1_write_db_depth_control(packets, value);
   CHECK(end == packets + 3);
   CHECK(packets[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[1] == (R_028800_DB_DEPTH_CONTROL - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[2] == value);
}

static void
test_cb_target_mask(void)
{
   /* Render target 0 writes all four components, render target 1 writes none, matching the plain
    * 4-bits-per-target packing this register uses on every TeraScale generation.
    */
   uint32_t const value = 0xFu << 0;

   uint32_t packets[3];
   uint32_t * const end = terakan_hw_config_draw_terascale_1_write_cb_target_mask(packets, value);
   CHECK(end == packets + 3);
   CHECK(packets[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[1] == (R_028238_CB_TARGET_MASK - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[2] == value);
}

int
main(void)
{
   test_db_depth_control();
   test_cb_target_mask();
   return 0;
}
