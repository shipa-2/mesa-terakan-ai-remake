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

/* Per-draw context register emission for TeraScale 1 (R600/R700), the counterpart of
 * terakan_hw_config_shared_terascale_1.{c,h} (the once-per-command-buffer atom) for state that
 * changes per draw. Split into its own file/naming rather than folded into that one, mirroring how
 * the R8xx/R9xx driver keeps terakan_hw_config_shared.c (once per command buffer) and
 * terakan_hw_config_draw.c (per draw) apart, so the same split is there to grow into as more
 * per-draw registers get ported.
 *
 * Both registers here take a caller-computed value rather than computing one from Vulkan pipeline
 * state themselves, because both are confirmed byte-identical to their R8xx/R9xx counterparts (see
 * each function's comment for how that was checked), so the value-computation logic Terakan already
 * has for R8xx/R9xx needs no TeraScale 1 equivalent at all -- only the register offset differs
 * between generations, which these two functions exist to isolate. This is not true of every
 * register: CB_COLOR_CONTROL at the neighboring offset 0x028808, for instance, has a 3-bit field at
 * the same bit position (4) that means SPECIAL_OP on R600/R700 and MODE on Evergreen-and-later, an
 * incompatible field with no shared meaning despite the shared position, so it needs real
 * TeraScale 1 logic of its own -- not written yet.
 */

/* DB_DEPTH_CONTROL (0x028800): every S_028800_* field (STENCIL_ENABLE, Z_ENABLE, Z_WRITE_ENABLE,
 * ZFUNC, BACKFACE_ENABLE, STENCILFUNC/FAIL/ZPASS/ZFAIL and their _BF back-face counterparts) has an
 * identical bit position and width in r600d.h and evergreend.h, checked directly against both
 * headers, not assumed. `value` is whatever Terakan's existing R8xx/R9xx S_028800_* computation
 * already produces for the same VkPipelineDepthStencilStateCreateInfo.
 */
uint32_t * terakan_hw_config_draw_terascale_1_write_db_depth_control(uint32_t * packet,
                                                                     uint32_t value);

/* CB_TARGET_MASK (0x028238): a plain 4-bits-per-render-target component write mask with no named
 * S_028238_* or G_028238_* fields defined in either header (checked directly), so there is no field
 * layout to diverge in the first place. `value` is whatever Terakan's existing R8xx/R9xx write-mask
 * packing already produces.
 */
uint32_t * terakan_hw_config_draw_terascale_1_write_cb_target_mask(uint32_t * packet,
                                                                    uint32_t value);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_H */
