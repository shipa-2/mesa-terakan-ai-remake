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

#define TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_SPI_PS_INPUT_COUNT 32

/* R600/R700 selects perspective/linear and center/centroid/sample interpolation in every
 * SPI_PS_INPUT_CNTL_n entry. Evergreen removed those selections from the per-input words and
 * introduced SPI_BARYC_CNTL at 0x0286E0 instead. On R600/R700 that address is
 * SPI_FOG_FUNC_SCALE, so an Evergreen payload must never be emitted there.
 *
 * Keep this input generation-neutral. The shader compiler fills it from r600_shader_io, while
 * this translation unit (which includes only r600d.h) owns the exact R700 field packing.
 */
struct terakan_hw_config_draw_terascale_1_spi_ps_input {
   uint32_t semantic;
   uint32_t gpr;
   bool position;
   bool front_face_or_sample_mask;
   bool sample_id;
   bool flat;
   bool centroid;
   bool linear;
   bool point_sprite;
   bool sample;
};

struct terakan_hw_config_draw_terascale_1_spi_ps {
   uint32_t input_count;
   uint32_t input_control[TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_SPI_PS_INPUT_COUNT];
   uint32_t in_control_0;
   uint32_t in_control_1;
   uint32_t input_z;
};

bool terakan_hw_config_draw_terascale_1_spi_ps_encode(
   struct terakan_hw_config_draw_terascale_1_spi_ps_input const * inputs,
   uint32_t input_count, struct terakan_hw_config_draw_terascale_1_spi_ps * spi_out);

/* Write SPI_PS_INPUT_CNTL_n, SPI_PS_IN_CONTROL_0/1 and SPI_INPUT_Z. Deliberately does not write
 * 0x0286E0: it is SPI_FOG_FUNC_SCALE on R600/R700, not SPI_BARYC_CNTL.
 */
uint32_t * terakan_hw_config_draw_terascale_1_write_spi_ps(
   uint32_t * packet, struct terakan_hw_config_draw_terascale_1_spi_ps const * spi);

/* R600/R700 has one 8-bit sample mask per pixel in R_028C48_PA_SC_AA_MASK. Classic
 * r600_emit_sample_mask() repeats the API-visible low byte for all four pixels. Evergreen uses
 * R_028C3C for its one-dword form, but that address is R_028C3C_CB_CLRCMP_MSK on R600/R700, so the
 * shared emitter must not be used even though the payload itself has the same repeated shape.
 */
uint32_t terakan_hw_config_draw_terascale_1_pa_sc_aa_mask_encode(uint32_t sample_mask);

uint32_t * terakan_hw_config_draw_terascale_1_write_pa_sc_aa_mask(uint32_t * packet,
                                                                   uint32_t value);

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

/* Convert the currently all-zero Evergreen software state to the actual no-query, no-HTILE R700
 * baseline from r600_emit_db_misc_state(). Nonzero Evergreen state is rejected because its fields
 * are not register-compatible with R700 and the corresponding query/HTILE/copy paths have not been
 * ported. There is no DB_RENDER_OVERRIDE2 equivalent on R600/R700.
 */
bool terakan_hw_config_draw_terascale_1_db_render_control_override_encode(
   uint32_t evergreen_db_render_control, uint32_t evergreen_db_render_override,
   uint32_t * db_render_control_out, uint32_t * db_render_override_out);

/* PKT3_SET_CONTEXT_REG_SEQ(DB_RENDER_CONTROL, DB_RENDER_OVERRIDE), 2 dwords. Values must already
 * be R700-shaped, normally produced by the encoder above. This two-register sequence is confirmed
 * against r600_emit_db_misc_state(), not inferred from the adjacent register addresses.
 */
uint32_t * terakan_hw_config_draw_terascale_1_write_db_render_control_override(
   uint32_t * packet, uint32_t db_render_control, uint32_t db_render_override);

/* DB_SHADER_CONTROL has a shared address and six shared field positions, but the remainder of the
 * Evergreen payload is not R700 state. Keep the input field-based so this translation unit can
 * encode the R700 register without including evergreend.h. `source_format` is validated but not
 * emitted: classic r600_update_db_shader_control() selects only DUAL_EXPORT_ENABLE on R700, while
 * DB_SOURCE_FORMAT exists only on Evergreen. The other trailing fields are rejected until their
 * semantics are ported; notably, conservative-Z belongs in R700 DB_RENDER_CONTROL.
 */
struct terakan_hw_config_draw_terascale_1_db_shader_control_input {
   bool z_export_enable;
   bool stencil_ref_export_enable;
   uint32_t z_order;
   bool kill_enable;
   bool mask_export_enable;
   bool dual_export_enable;
   uint32_t source_format;
   bool exec_on_hier_fail;
   bool exec_on_noop;
   bool alpha_to_mask_disable;
   bool depth_before_shader;
   uint32_t conservative_z_export;
   uint32_t unknown_bits;
};

bool terakan_hw_config_draw_terascale_1_db_shader_control_encode(
   struct terakan_hw_config_draw_terascale_1_db_shader_control_input const * input,
   uint32_t * db_shader_control_out);

uint32_t * terakan_hw_config_draw_terascale_1_write_db_shader_control(uint32_t * packet,
                                                                       uint32_t value);

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

enum terakan_hw_config_draw_terascale_1_db_depth_format {
   TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_DB_DEPTH_INVALID,
   TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_DB_DEPTH_16,
   TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_DB_DEPTH_24,
   TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_DB_DEPTH_32_FLOAT,
};

struct terakan_hw_config_draw_terascale_1_db_depth_input {
   uint32_t base;
   uint32_t pitch_tile_max;
   uint32_t height_tile_max;
   uint32_t slice_tile_max;
   uint32_t slice_start;
   uint32_t slice_max;
   enum terakan_hw_config_draw_terascale_1_db_depth_format format;
   uint32_t array_mode;
   bool zrange_precision;
   uint32_t samples_log2;
};

struct terakan_hw_config_draw_terascale_1_db_depth {
   uint32_t base;
   uint32_t size;
   uint32_t view;
   uint32_t info;
   uint32_t prefetch_limit;
};

/* Encode the depth-only, single-sampled subset of r600_init_depth_surface(). R700's packed
 * depth/stencil formats and multisample layout are deliberately rejected until their shared
 * allocation has been ported; the caller must unbind DB when this returns false.
 */
bool terakan_hw_config_draw_terascale_1_db_depth_encode(
   struct terakan_hw_config_draw_terascale_1_db_depth_input const * input,
   struct terakan_hw_config_draw_terascale_1_db_depth * depth_out);

/* Writes SIZE, VIEW, BASE/INFO and PREFETCH_LIMIT in the order used by
 * r600_emit_framebuffer_state(). The relocated BASE payload is dword 8 of the 13-dword stream.
 */
uint32_t * terakan_hw_config_draw_terascale_1_write_db_depth(
   uint32_t * packet, struct terakan_hw_config_draw_terascale_1_db_depth const * depth);

uint32_t * terakan_hw_config_draw_terascale_1_write_db_depth_prefetch_limit(uint32_t * packet,
                                                                             uint32_t value);

uint32_t * terakan_hw_config_draw_terascale_1_write_db_depth_unbound(uint32_t * packet);

/* R700 DB_ALPHA_TO_MASK has the same ENABLE/OFFSET field positions as Evergreen, but lives at
 * R_028D44 rather than R_028B70. Classic r600_create_blend_state_mode() always uses the regular
 * 2,2,2,2 offsets, so take only the API-visible enable here rather than carrying an
 * Evergreen-shaped register value across generations.
 */
uint32_t terakan_hw_config_draw_terascale_1_db_alpha_to_mask_encode(bool enable);

uint32_t * terakan_hw_config_draw_terascale_1_write_db_alpha_to_mask(uint32_t * packet,
                                                                      uint32_t value);

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
