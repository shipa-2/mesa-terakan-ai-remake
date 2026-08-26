/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TERAKAN_PHYSICAL_DEVICE_TILING_CONFIG_H
#define TERAKAN_PHYSICAL_DEVICE_TILING_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decodes the raw uint32_t the DRM_RADEON_INFO ioctl's RADEON_INFO_TILING_CONFIG request returns
 * into a terakan_physical_device_tiling_info. Kept as a pure function, separate from the ioctl call
 * itself (terakan_physical_device_drm_radeon.c), so it is unit-testable without a DRM device.
 *
 * The bit layout this decodes is generation-specific, not just the meaning of an otherwise-shared
 * layout: on TeraScale 1 (R600/R700, `is_terascale_1` true) the pipe count field starts one bit
 * later (bits [1:3] instead of [0:3]) and the pipe interleave ("group bytes") field lives at bits
 * [6:7] instead of [8:11], not merely a different value at the same position. There is also no row
 * size / TILE_SPLIT concept on this hardware at all -- r600d.h defines no TILE_SPLIT field for any
 * CB/DB register, unlike evergreend.h -- so `row_bytes_log2` is set to 0 and must not be treated as
 * a real value by any TeraScale 1 caller (there is currently only one caller,
 * terakan_physical_device_get_capabilities()'s max_memory_allocation_size computation, gated behind
 * is_terascale_1 not reaching device creation yet -- see terakan_CreateDevice).
 *
 * Checked directly against libdrm's radeon/radeon_surface.c (r6_init_hw_info() vs eg_init_hw_info()),
 * not assumed from the R8xx/R9xx layout already in use. Terakan's existing R8xx/R9xx decode
 * (previously inline in terakan_physical_device_drm_radeon.c, now
 * terakan_physical_device_decode_tiling_config() with is_terascale_1 false) was cross-checked
 * against the same reference in the same pass and confirmed correct: every one of its four
 * `constant + ((tiling_config >> shift) & 0xF)` formulas matches eg_init_hw_info()'s switch
 * statements for every case those switches handle.
 *
 * bank_interleave_log2 is 0 for both: libdrm's struct radeon_hw_info has no bank_interleave field
 * for either generation, so this is not an R8xx/R9xx-specific empirical fact that could differ on
 * TeraScale 1, it is the absence of a concept in the real tiling algorithm for the entire R6xx
 * through Cayman range this driver targets.
 */
/* A copy of terakan_physical_device_tiling_info's fields (terakan_physical_device.h), kept
 * self-contained here -- rather than including that header, which pulls in the rest of the
 * Vulkan/NIR machinery it depends on -- so this file and its test stay as decoupled as
 * terakan_hw_config_loop_constants.{c,h} and the TeraScale 1 register-emission files are. The
 * caller (terakan_physical_device_drm_radeon.c) copies the fields across; the two structs are kept
 * in sync by hand since terakan_physical_device_tiling_info_equal() there would fail to compile if
 * a field were ever added to one and not the other.
 */
struct terakan_physical_device_tiling_config_info {
   uint8_t pipes_log2;
   uint8_t banks_log2;
   uint8_t pipe_interleave_bytes_log2;
   uint8_t bank_interleave_log2;
   uint8_t row_bytes_log2;
};

struct terakan_physical_device_tiling_config_info
terakan_physical_device_decode_tiling_config(uint32_t tiling_config, bool is_terascale_1);

/* Maximum BO base alignment needed by any TeraScale 1 image Terakan exposes (up to 16 bytes per
 * element and 8 samples), derived from r6_surface_init_2d() rather than R8xx AddrLib ROW_SIZE.
 */
uint64_t terakan_physical_device_terascale_1_max_bo_alignment(uint8_t pipes_log2,
                                                              uint8_t banks_log2,
                                                              uint8_t group_bytes_log2);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_PHYSICAL_DEVICE_TILING_CONFIG_H */
