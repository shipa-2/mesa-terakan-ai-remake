/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_shader_generation.h"

#include "gallium/drivers/r600/sfn/sfn_nir.h"

enum radeon_family
terakan_shader_family_from_pci_id(uint32_t const pci_device_id)
{
   enum radeon_family family = CHIP_UNKNOWN;
   switch (pci_device_id) {
#define CHIPSET(chipset_pci_id, chipset_name, chipset_family)                                      \
   case chipset_pci_id:                                                                            \
      family = CHIP_##chipset_family;                                                              \
      break;
#include "pci_ids/r600_pci_ids.h"
#undef CHIPSET
   }
   return family;
}

void
terakan_shader_nir_options_init(enum radeon_family const family,
                                nir_shader_compiler_options * const non_fs_out,
                                nir_shader_compiler_options * const fs_out)
{
   enum amd_gfx_level const gfx_level = terakan_shader_gfx_level(family);

   *non_fs_out = (nir_shader_compiler_options){
      .lower_fdiv = true,

      .fuse_ffma16 = true,
      /* Keep vertex-position arithmetic reproducible across depth-prepass and color-pass shader
       * variants. FMA fusion can otherwise change the rounded gl_Position.z value and make
       * VK_COMPARE_OP_EQUAL fail.
       */
      .fuse_ffma32 = false,
      .fuse_ffma64 = true,

      .lower_flrp16 = true,
      .lower_flrp32 = true,
      .lower_flrp64 = true,
      .lower_fpow = true,
      .lower_fmod = true,

      .lower_bitfield_extract = true,
      .lower_bitfield_extract16 = true,
      .lower_bitfield_extract8 = true,
      .lower_bitfield_insert = true,
      .lower_ifind_msb = true,
      .lower_ufind_msb = true,
      .lower_uadd_carry = true,
      .lower_usub_borrow = true,
      .lower_fisnormal = true,
      .lower_isign = true,
      .lower_fsign = true,
      .lower_iabs = true,
      .lower_ldexp = true,

      .lower_pack_unorm_2x16 = true,
      .lower_pack_snorm_2x16 = true,
      .lower_pack_unorm_4x8 = true,
      .lower_pack_snorm_4x8 = true,
      .lower_pack_64_4x16 = true,
      .lower_pack_32_2x16 = true,
      .lower_pack_32_2x16_split = true,
      .lower_unpack_unorm_2x16 = true,
      .lower_unpack_snorm_2x16 = true,
      .lower_unpack_unorm_4x8 = true,
      .lower_unpack_snorm_4x8 = true,
      .lower_unpack_32_2x16_split = true,

      /* SFN implements the split half pack/unpack operations directly. lower_pack_split would
       * expand them back to unsupported 16-bit f2f ALU.
       */
      .lower_extract_byte = true,
      .lower_extract_word = true,
      .lower_insert_byte = true,
      .lower_insert_word = true,
      .lower_cs_local_index_to_id = true,
      .lower_device_index_to_zero = true,
      .lower_hadd = true,
      .lower_uadd_sat = true,
      .lower_usub_sat = true,
      .lower_iadd_sat = true,
      .lower_mul_32x16 = true,
      .vectorize_tess_levels = true,
      .lower_to_scalar = true,
      .lower_to_scalar_filter = r600_lower_to_scalar_instr_filter,
      .lower_interpolate_at = true,
      .lower_mul_2x32_64 = true,

      .has_umul24 = true,
      .has_umad24 = true,
      .has_fused_comp_and_csel = true,
      .has_fsub = true,
      .has_isub = true,
      .has_fmulz = true,
      .has_find_msb_rev = true,

      /* Classic r600_screen_create() exposes these native ALU operations only on Evergreen and
       * newer. R600/R700 must lower bit count/reverse and must not let NIR generate BFE/BFM/BFI.
       */
      .lower_bit_count = gfx_level < EVERGREEN,
      .lower_bitfield_reverse = gfx_level < EVERGREEN,
      .has_bfe = gfx_level >= EVERGREEN,
      .has_bfm = gfx_level >= EVERGREEN,
      .has_bitfield_select = gfx_level >= EVERGREEN,
      .force_indirect_unrolling_sampler = gfx_level < EVERGREEN,
      .vertex_id_zero_based = gfx_level >= EVERGREEN,

      .max_unroll_iterations = 32,
      .max_unroll_iterations_aggressive = 128,
      .lower_int64_options = ~(nir_lower_int64_options)0,
      .lower_doubles_options = gfx_level >= CAYMAN
                                  ? nir_lower_ddiv | nir_lower_dfloor | nir_lower_dceil |
                                       nir_lower_dmod | nir_lower_dsub | nir_lower_dtrunc
                                  : nir_lower_fp64_full_software,
      .lower_image_offset_to_range_base = true,
      /* lower_atomic_offset_to_range_base (needed by Gallium on pre-Cayman) is not applicable to
       * Vulkan because Terakan's binding lowering already folds the range base.
       */
      .lower_fquantize2f16 = true,
      .has_ddx_intrinsics = true,
      .io_options = nir_io_mediump_is_32bit,
   };

   *fs_out = *non_fs_out;
   fs_out->lower_all_io_to_temps = true;
}
