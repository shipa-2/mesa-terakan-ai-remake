/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TERAKAN_SHADER_GENERATION_H
#define TERAKAN_SHADER_GENERATION_H

#include "gallium/drivers/r600/r600_isa.h"
#include "amd_family.h"

#include <assert.h>

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

#endif /* TERAKAN_SHADER_GENERATION_H */
