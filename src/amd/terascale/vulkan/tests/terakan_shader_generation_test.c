/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_shader_generation.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static bool
check(enum radeon_family const family, enum amd_gfx_level const expected_gfx_level,
      enum r600_chip_class const expected_isa_chip_class)
{
   enum amd_gfx_level const gfx_level = terakan_shader_gfx_level(family);
   enum r600_chip_class const isa_chip_class = terakan_shader_isa_chip_class(family);
   if (gfx_level == expected_gfx_level && isa_chip_class == expected_isa_chip_class)
      return true;

   fprintf(stderr, "family %u: gfx level %u (expected %u), ISA class %u (expected %u)\n", family,
           gfx_level, expected_gfx_level, isa_chip_class, expected_isa_chip_class);
   return false;
}

int
main(void)
{
   bool passed = true;

   passed &= check(CHIP_R600, R600, ISA_CC_R600);
   passed &= check(CHIP_RS880, R600, ISA_CC_R600);

   passed &= check(CHIP_RV770, R700, ISA_CC_R700);
   passed &= check(CHIP_RV730, R700, ISA_CC_R700);
   passed &= check(CHIP_RV710, R700, ISA_CC_R700);
   passed &= check(CHIP_RV740, R700, ISA_CC_R700);

   passed &= check(CHIP_CEDAR, EVERGREEN, ISA_CC_EVERGREEN);
   passed &= check(CHIP_CAICOS, EVERGREEN, ISA_CC_EVERGREEN);
   passed &= check(CHIP_CAYMAN, CAYMAN, ISA_CC_CAYMAN);
   passed &= check(CHIP_ARUBA, CAYMAN, ISA_CC_CAYMAN);

   return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
