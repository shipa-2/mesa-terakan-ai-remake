/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TERAKAN_SHADER_GENERATION_H
#define TERAKAN_SHADER_GENERATION_H

#include "gallium/drivers/r600/r600_isa.h"
#include "amd_family.h"
#include "nir.h"

#include <assert.h>
#include <stdint.h>

/* Production PCI-ID lookup shared by physical-device enumeration and the generation tests. */
enum radeon_family terakan_shader_family_from_pci_id(uint32_t pci_device_id);

/* Keep the shader backend generation selection independent from the R8xx/R9xx state paths.
 * In particular, treating every non-Cayman chip as Evergreen silently produces the wrong CF,
 * ALU and fetch instruction encoding for R700.
 */
static inline enum amd_gfx_level
terakan_shader_gfx_level(enum radeon_family const family)
{
   assert(family >= CHIP_R600 && family <= CHIP_ARUBA);

   if (family <= CHIP_RS880)
      return R600;
   if (family <= CHIP_RV740)
      return R700;
   if (family <= CHIP_CAICOS)
      return EVERGREEN;
   return CAYMAN;
}

static inline enum r600_chip_class
terakan_shader_isa_chip_class(enum radeon_family const family)
{
   switch (terakan_shader_gfx_level(family)) {
   case R600:
      return ISA_CC_R600;
   case R700:
      return ISA_CC_R700;
   case EVERGREEN:
      return ISA_CC_EVERGREEN;
   case CAYMAN:
   default:
      return ISA_CC_CAYMAN;
   }
}

/* Initialize the SPIR-V-to-NIR options for the actual runtime family. Pre-Evergreen ALU and
 * sampler-indexing limitations are materially different from Evergreen's and must be lowered
 * before SFN translation rather than being selected by the build-generation label.
 */
void terakan_shader_nir_options_init(enum radeon_family family,
                                     nir_shader_compiler_options * non_fs_out,
                                     nir_shader_compiler_options * fs_out);

#endif /* TERAKAN_SHADER_GENERATION_H */
