/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TERAKAN_PHYSICAL_DEVICE_BACKEND_COUNT_H
#define TERAKAN_PHYSICAL_DEVICE_BACKEND_COUNT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Converts a raw render backend count (as returned by the RADEON_INFO_NUM_BACKENDS DRM ioctl) to
 * its log2, rounding up rather than down so a real count is never underrepresented. Every publicly
 * documented R600/R700 backend count is a power of two (1, 2, or 4), so this is exact in practice
 * for TeraScale 1, the only caller of this today -- see
 * terakan_physical_device_chip_info_init's terascale_1_num_backends parameter -- but the rounding
 * keeps this correct even for an encoding this driver doesn't know to expect.
 *
 * Kept as a pure function, separate from both the ioctl call itself
 * (terakan_physical_device_drm_radeon.c) and from terakan_physical_device_chip_info_init (which
 * pulls in the rest of the Vulkan/NIR machinery the driver depends on), so it is unit-testable
 * without either a DRM device or that machinery -- the same reasoning as
 * terakan_physical_device_tiling_config.h.
 *
 * backend_count must not be 0: chip_info_init treats a 0 terascale_1_num_backends as "not queried"
 * and never calls this in that case, so a 0 input here would indicate a caller bug, not a
 * legitimate "unknown" state.
 */
unsigned
terakan_physical_device_backend_count_to_log2(uint32_t backend_count);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_PHYSICAL_DEVICE_BACKEND_COUNT_H */
