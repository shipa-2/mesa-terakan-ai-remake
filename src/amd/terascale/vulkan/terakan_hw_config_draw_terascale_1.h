/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_H
#define TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-draw (as opposed to once-per-command-buffer) TeraScale 1 register emission that genuinely
 * needs its own code, as opposed to the R8xx/R9xx code already working unchanged (see
 * DB_DEPTH_CONTROL/CB_TARGET_MASK in TODO.md) -- either because the register lives at a different
 * offset with the same field layout, or because R600/R700 has no equivalent of an R8xx/R9xx
 * register at all and the driver's own existing behavior for it is trivial enough to port directly.
 *
 * DB_DEPTH_SIZE/DB_DEPTH_BASE/DB_DEPTH_INFO (the actual depth/stencil surface binding: base
 * address, pitch/slice tile counts, format, and array/tiling mode) are deliberately NOT here yet.
 * R600/R700 binds depth and stencil through a single combined surface and base address
 * (R_02800C_DB_DEPTH_BASE, no separate stencil base at all, unlike R8xx/R9xx's four independent
 * read/write Z/stencil base registers), and DB_DEPTH_INFO's ARRAY_MODE field needs real tiling
 * information that terakan_physical_device_tiling_config.c's decode fix is only a prerequisite for
 * -- the actual surface/macro-tile address math for TeraScale 1 has not been ported (see
 * terakan_image.c in TODO.md). Guessing ARRAY_MODE or the combined depth+stencil FORMAT encoding
 * without that work done first is exactly the kind of silent-memory-corruption risk this driver's
 * porting conventions exist to avoid.
 */

/* PKT3_SET_CONTEXT_REG(DB_DEPTH_VIEW). Field-for-field identical to R8xx/R9xx's own DB_DEPTH_VIEW
 * (SLICE_START/SLICE_MAX, both 11 bits) -- confirmed against evergreend.h directly -- just at a
 * different register offset (R_028004_DB_DEPTH_VIEW here vs R_028008_DB_DEPTH_VIEW there), so the
 * caller's existing R8xx/R9xx value-computation logic for `descriptor->view` needs no TeraScale 1
 * equivalent, only this offset-isolating wrapper.
 */
uint32_t * terakan_hw_config_draw_terascale_1_write_db_depth_view(uint32_t * packet,
                                                                    uint32_t value);

/* PKT3_SET_CONTEXT_REG_SEQ(DB_RENDER_CONTROL, DB_RENDER_OVERRIDE), 2 dwords. Takes caller-computed
 * values rather than a fixed default, matching the DB_DEPTH_CONTROL/CB_TARGET_MASK convention, even
 * though every current R8xx/R9xx caller of the equivalent function only ever passes
 * TERAKAN_HW_CONFIG_DRAW_DEFAULT_DB_RENDER_CONTROL/_DB_RENDER_OVERRIDE (both 0): this driver has no
 * dynamic per-draw DB_RENDER_CONTROL/DB_RENDER_OVERRIDE logic at all yet, for either generation --
 * no occlusion query hazard handling, no HTILE, no depth/stencil-through-CB flush path -- so a
 * TeraScale 1 port has nothing beyond that same all-zero baseline to port either. There is no
 * DB_RENDER_OVERRIDE2 equivalent on R600/R700 at all (r600d.h defines no such register, and
 * r600_state.c's r600_emit_db_misc_state() never emits a third register here), so unlike R8xx/R9xx
 * this is only two registers, not three -- confirmed against r600_state.c's
 * r600_emit_db_misc_state(), not guessed from the header alone.
 */
uint32_t * terakan_hw_config_draw_terascale_1_write_db_render_control_override(
   uint32_t * packet, uint32_t db_render_control, uint32_t db_render_override);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_H */
