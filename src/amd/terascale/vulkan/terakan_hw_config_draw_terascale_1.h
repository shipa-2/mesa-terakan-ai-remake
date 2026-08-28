/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_H
#define TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_H

#include <stdbool.h>
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
 * DB_DEPTH_SIZE/DB_DEPTH_BASE/DB_DEPTH_INFO emission (see below) exist here, since the underlying
 * tiling math (ARRAY_MODE, pitch/height in tiles) is now ported and tested
 * (terakan_image_tiling_terascale_1.c, wired into terakan_image_surface_compute_terascale_1() --
 * see TODO.md), but the VALUE COMPUTATION side -- deriving a DB_DEPTH_INFO FORMAT/DB_DEPTH_BASE
 * pair from a Vulkan depth/stencil attachment -- is not written yet, and is a genuinely open
 * research question, not just unstarted work: R600/R700 binds depth and stencil through a single
 * combined surface and base address (R_02800C_DB_DEPTH_BASE, no separate stencil base at all,
 * unlike R8xx/R9xx's four independent read/write Z/stencil base registers), which doesn't map onto
 * Terakan's existing terakan_depth_stencil_descriptor (built around separate z_base/stencil_base/
 * z_info/stencil_info) without figuring out how the packed combined-surface model actually stores
 * stencil data alongside depth on this hardware -- guessing at DB_DEPTH_INFO's FORMAT encoding or
 * the combined surface layout without that research done first is exactly the kind of
 * silent-memory-corruption risk this driver's porting conventions exist to avoid. The two functions
 * below take fully caller-computed values for exactly this reason: they exist so the register SHAPE
 * is ready once that research is done, not because the value-computation problem is solved.
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

/* PKT3_SET_CONTEXT_REG(DB_DEPTH_SIZE), 1 dword. R_028000_DB_DEPTH_SIZE has no R8xx/R9xx equivalent
 * at this offset or in this shape: it packs PITCH_TILE_MAX (10 bits) and SLICE_TILE_MAX (20 bits,
 * the whole slice's tile count, not a separate height field) into one register, where R8xx/R9xx
 * splits pitch and height into DB_DEPTH_SIZE and a separate DB_DEPTH_SLICE register -- R600/R700 has
 * no DB_DEPTH_SLICE at all. pitch_tile_max and slice_tile_max are taken as fully caller-computed
 * values (already including the "-1" MAX convention both fields use), not derived here, since this
 * is emission only -- see the file comment above for why the value side isn't written yet.
 */
uint32_t * terakan_hw_config_draw_terascale_1_write_db_depth_size(uint32_t * packet,
                                                                   uint32_t pitch_tile_max,
                                                                   uint32_t slice_tile_max);

/* PKT3_SET_CONTEXT_REG_SEQ(DB_DEPTH_BASE, DB_DEPTH_INFO), 2 dwords, matching
 * r600_state.c's own `radeon_set_context_reg_seq(cs, R_02800C_DB_DEPTH_BASE, 2)` sequencing (see
 * r600_db_state in the reference). db_depth_base is a plain shifted GPU address with no named fields
 * (unlike R8xx/R9xx's four separate Z/stencil read/write base registers -- see the file comment
 * above), and db_depth_info is the full DB_DEPTH_INFO value (FORMAT/READ_SIZE/ARRAY_MODE/
 * TILE_SURFACE_ENABLE/TILE_COMPACT/ZRANGE_PRECISION), both fully caller-computed.
 */
uint32_t * terakan_hw_config_draw_terascale_1_write_db_depth_base_info(uint32_t * packet,
                                                                        uint32_t db_depth_base,
                                                                        uint32_t db_depth_info);

/* R600/R700 CB_COLOR is not the shorter form of the R8xx/R9xx descriptor. Even though the
 * FORMAT/ARRAY_MODE/NUMBER_TYPE values are numerically shared, INFO diverges after bit 14, and the
 * other fields live in seven independently-strided register arrays rather than one per-target
 * register block. Keep the input field-based so this translation unit never needs evergreend.h.
 *
 * This first port deliberately accepts only single-sampled RTVs with metadata disabled. Classic
 * r600_init_color_surface() gives multisampled surfaces real FMASK/CMASK state, while Terakan's
 * TeraScale 1 surface layout does not compute those allocations yet. UAV/compute use is also not
 * ready on TeraScale 1. Returning false preserves those boundaries instead of manufacturing packet
 * values that could address the wrong memory once queue submission is eventually enabled.
 */
struct terakan_hw_config_draw_terascale_1_cb_color_input {
   uint32_t base;
   uint32_t pitch_tile_max;
   uint32_t slice_tile_max;
   uint32_t slice_start;
   uint32_t slice_max;
   uint32_t endian;
   uint32_t format;
   uint32_t array_mode;
   uint32_t number_type;
   uint32_t comp_swap;
   bool blend_clamp;
   bool blend_bypass;
   bool simple_float;
   uint32_t source_format;
   bool is_uav;
   bool is_multisampled;
   bool metadata_enabled;
};

struct terakan_hw_config_draw_terascale_1_cb_color {
   uint32_t base;
   uint32_t size;
   uint32_t view;
   uint32_t info;
   uint32_t fmask;
   uint32_t cmask;
   uint32_t mask;
};

bool terakan_hw_config_draw_terascale_1_cb_color_encode(
   struct terakan_hw_config_draw_terascale_1_cb_color_input const * input,
   struct terakan_hw_config_draw_terascale_1_cb_color * color_out);

/* Writes INFO, BASE, FRAG (FMASK), TILE (CMASK), SIZE, VIEW and MASK, in that order, as seven
 * one-register SET_CONTEXT_REG packets. The base-bearing payload dwords are at indices 5, 8 and 11
 * in the returned 21-dword stream, which the caller uses for BO relocations.
 */
uint32_t * terakan_hw_config_draw_terascale_1_write_cb_color(
   uint32_t * packet, uint32_t color_index,
   struct terakan_hw_config_draw_terascale_1_cb_color const * color);

uint32_t * terakan_hw_config_draw_terascale_1_write_cb_color_unbound(uint32_t * packet,
                                                                      uint32_t color_index,
                                                                      uint32_t source_format);

enum terakan_hw_config_draw_terascale_1_cb_color_operation {
   TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_CB_COLOR_DISABLE,
   TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_CB_COLOR_NORMAL,
   TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_CB_COLOR_RESOLVE_BOX,
};

/* R700 moves blend enables out of CB_BLENDn_CONTROL bit 30 and into
 * CB_COLOR_CONTROL.TARGET_BLEND_ENABLE. Its bits 4-6 are SPECIAL_OP, not Evergreen MODE, despite
 * the shared register address. Keep the operation neutral at this interface so evergreend.h values
 * can never accidentally reach the R700 packet.
 */
uint32_t terakan_hw_config_draw_terascale_1_cb_color_control_encode(
   enum terakan_hw_config_draw_terascale_1_cb_color_operation operation, uint32_t rop3,
   bool degamma_enable, bool multiwrite_enable, uint32_t target_blend_enable);

uint32_t * terakan_hw_config_draw_terascale_1_write_cb_color_control(uint32_t * packet,
                                                                      uint32_t value);

/* The coefficient/equation fields are identical between R700's CB_BLENDn_CONTROL and Evergreen's
 * CB_BLENDn_CONTROL, but R700 has no BLEND_CONTROL_ENABLE bit 30. The caller supplies that bit
 * separately through CB_COLOR_CONTROL.TARGET_BLEND_ENABLE; this writer strips it from every
 * register payload.
 */
uint32_t * terakan_hw_config_draw_terascale_1_write_cb_blend_control(
   uint32_t * packet, uint32_t first_color, uint32_t color_count,
   uint32_t const * blend_control);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_H */
