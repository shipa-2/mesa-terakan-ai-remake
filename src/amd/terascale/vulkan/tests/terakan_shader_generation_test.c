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

static bool
check_pci_id(uint32_t const pci_device_id, enum radeon_family const expected_family)
{
   enum radeon_family const family = terakan_shader_family_from_pci_id(pci_device_id);
   if (family == expected_family)
      return true;

   fprintf(stderr, "PCI ID 0x%04x: family %u (expected %u)\n", pci_device_id, family,
           expected_family);
   return false;
}

static bool
check_nir_options(enum radeon_family const family, bool const pre_evergreen)
{
   nir_shader_compiler_options non_fs, fs;
   terakan_shader_nir_options_init(family, &non_fs, &fs);

   bool const passed =
      non_fs.force_indirect_unrolling_sampler == pre_evergreen &&
      non_fs.lower_bit_count == pre_evergreen &&
      non_fs.lower_bitfield_reverse == pre_evergreen && non_fs.has_bfe == !pre_evergreen &&
      non_fs.has_bfm == !pre_evergreen && non_fs.has_bitfield_select == !pre_evergreen &&
      /* Never zero-based on either generation: the base is in `VGT_INDX_OFFSET`, so R0.X already
       * carries it, which is what Vulkan's `VertexIndex` is. Asking NIR to treat it as zero-based
       * made it add the base a second time, and every one of the 144 supported
       * dEQP-VK.draw.*.indexed_draw cases failed.
       */
      !non_fs.vertex_id_zero_based && !non_fs.lower_all_io_to_temps &&
      fs.lower_all_io_to_temps && fs.force_indirect_unrolling_sampler == pre_evergreen &&
      fs.lower_bit_count == pre_evergreen && fs.has_bfe == !pre_evergreen;
   if (!passed) {
      fprintf(stderr, "family %u: incorrect %s NIR options\n", family,
              pre_evergreen ? "pre-Evergreen" : "Evergreen+");
   }
   return passed;
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

   /* Exercise the same generated PCI-ID table used by physical-device enumeration, not merely
    * hand-picked family enum values. This includes all R600, R700, Evergreen and Cayman IDs in
    * r600_pci_ids.h and catches a missing/incorrect family token in that table.
    */
#define CHIPSET(chipset_pci_id, chipset_name, chipset_family)                                      \
   passed &= check_pci_id(chipset_pci_id, CHIP_##chipset_family);
#include "pci_ids/r600_pci_ids.h"
#undef CHIPSET
   passed &= terakan_shader_family_from_pci_id(UINT32_C(0xFFFFFFFF)) == CHIP_UNKNOWN;

   passed &= check_nir_options(CHIP_R600, true);
   passed &= check_nir_options(CHIP_RV710, true);
   passed &= check_nir_options(CHIP_CEDAR, false);
   passed &= check_nir_options(CHIP_CAYMAN, false);

   return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
