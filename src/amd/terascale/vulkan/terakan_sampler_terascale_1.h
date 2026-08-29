/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TERAKAN_SAMPLER_TERASCALE_1_H
#define TERAKAN_SAMPLER_TERASCALE_1_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

void terakan_sampler_terascale_1_create_descriptor(VkSamplerCreateInfo const * create_info,
                                                   bool force_base_mip, uint32_t descriptor_out[3]);

#ifdef __cplusplus
}
#endif

#endif
