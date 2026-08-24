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

#define CHECK(condition)                                                                          \
   do {                                                                                            \
      if (!(condition)) {                                                                          \
         fprintf(stderr, "CHECK failed at %s:%u: %s\n", __FILE__, __LINE__, #condition);           \
         abort();                                                                                  \
      }                                                                                            \
   } while (0)

static void
test_db_depth_view(void)
{
   /* An arbitrary but representative value exercising both fields, matching the field layout
    * confirmed identical to R8xx/R9xx's own DB_DEPTH_VIEW (SLICE_START/SLICE_MAX, both 11 bits).
    */
   uint32_t const value = S_028004_SLICE_START(3) | S_028004_SLICE_MAX(5);

   uint32_t packets[3];
   uint32_t * const end = terakan_hw_config_draw_terascale_1_write_db_depth_view(packets, value);
   CHECK(end == packets + 3);
   CHECK(packets[0] == PKT3(PKT3_SET_CONTEXT_REG, 1, 0));
   CHECK(packets[1] == (R_028004_DB_DEPTH_VIEW - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[2] == value);
}

static void
test_db_render_control_override(void)
{
   uint32_t const db_render_control = S_028D0C_ZPASS_INCREMENT_DISABLE(1);
   uint32_t const db_render_override =
      S_028D10_FORCE_HIS_ENABLE0(V_028D10_FORCE_DISABLE) |
      S_028D10_FORCE_HIS_ENABLE1(V_028D10_FORCE_DISABLE);

   uint32_t packets[4];
   uint32_t * const end = terakan_hw_config_draw_terascale_1_write_db_render_control_override(
      packets, db_render_control, db_render_override);
   CHECK(end == packets + 4);
   CHECK(packets[0] == PKT3(PKT3_SET_CONTEXT_REG, 2, 0));
   CHECK(packets[1] == (R_028D0C_DB_RENDER_CONTROL - R600_CONTEXT_REG_OFFSET) >> 2);
   CHECK(packets[2] == db_render_control);
   CHECK(packets[3] == db_render_override);
}

static void
test_db_render_control_override_default_matches_r8xx_baseline(void)
{
   /* Every current R8xx/R9xx caller only ever passes the all-zero default -- see
    * TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_RENDER_CONTROL/_DB_RENDER_OVERRIDE in
    * terakan_hw_config_draw.h -- since this driver has no dynamic per-draw DB_RENDER_CONTROL/
    * DB_RENDER_OVERRIDE logic for either generation yet. Checked explicitly here so a future
    * R8xx/R9xx default change doesn't silently stop matching what TeraScale 1 assumes.
    */
   uint32_t packets[4];
   uint32_t * const end =
      terakan_hw_config_draw_terascale_1_write_db_render_control_override(packets, 0, 0);
   CHECK(end == packets + 4);
   CHECK(packets[2] == 0);
   CHECK(packets[3] == 0);
}

int
main(void)
{
   test_db_depth_view();
   test_db_render_control_override();
   test_db_render_control_override_default_matches_r8xx_baseline();
   return 0;
}
