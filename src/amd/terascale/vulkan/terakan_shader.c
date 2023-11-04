/*
 * Copyright © 2023 Vitaliy Triang3l Kuzmin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "terakan_shader.h"

#include "terakan_bo.h"
#include "terakan_descriptor.h"
#include "terakan_device.h"
#include "terakan_physical_device.h"

#include "gallium/drivers/r600/sfn/sfn_nir.h"
#include "spirv/nir_spirv.h"
#include "util/macros.h"
#include "vk_nir.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

nir_shader *
terakan_shader_spirv_to_nir(struct terakan_device * const device, size_t const spirv_size_bytes,
                            uint32_t const * const spirv, gl_shader_stage const stage,
                            char const * const entrypoint,
                            VkSpecializationInfo const * const specialization_info)
{
   /* TODO(Triang3l): Move the options to the physical device for reuse. */

   static struct spirv_to_nir_options const spirv_options = {
      .environment = NIR_SPIRV_VULKAN,

      /* TODO(Triang3l): Possibly SUBGROUP_SIZE_API_CONSTANT when the subgroup size is properly
       * exposed.
       */
      .subgroup_size = SUBGROUP_SIZE_UNIFORM,

      /* TODO(Triang3l): Capabilities when supported and tested. */

      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = nir_address_format_32bit_index_offset,
      .push_const_addr_format = nir_address_format_32bit_offset,
      .shared_addr_format = nir_address_format_32bit_offset,

      .min_ubo_alignment = TERAKAN_CONSTANT_CACHE_LINE_BYTES,
      .min_ssbo_alignment = sizeof(uint32_t),
   };

   nir_shader_compiler_options nir_options = {
      .fuse_ffma16 = true,
      .fuse_ffma32 = true,
      .fuse_ffma64 = true,
      .lower_flrp32 = true,
      .lower_flrp64 = true,
      .lower_fdiv = true,
      .lower_isign = true,
      .lower_fsign = true,
      .lower_fmod = true,
      .lower_uadd_carry = true,
      .lower_usub_borrow = true,
      .lower_bitfield_extract = true,
      .lower_bitfield_insert = true,
      .lower_extract_byte = true,
      .lower_extract_word = true,
      .lower_insert_byte = true,
      .lower_insert_word = true,
      .lower_ldexp = true,
      /* Due to a bug in the (old) shader compiler, some loops hang  if they are not unrolled, see:
       *    https://bugs.freedesktop.org/show_bug.cgi?id=86720
       */
      /* TODO(Triang3l): Revisit max_unroll_iterations with the newer SFN. */
      .max_unroll_iterations = 255,
      .lower_interpolate_at = true,
      .vectorize_io = true,
      .has_umad24 = true,
      .has_umul24 = true,
      .has_fmulz = true,
      .use_interpolated_input_intrinsics = true,
      .has_fsub = true,
      .has_isub = true,
      .has_find_msb_rev = true,
      .lower_iabs = true,
      .lower_uadd_sat = true,
      .lower_usub_sat = true,
      .has_fused_comp_and_csel = true,
      .lower_ifind_msb = true,
      .lower_ufind_msb = true,
      .lower_to_scalar = true,
      .lower_to_scalar_filter = r600_lower_to_scalar_instr_filter,
      .linker_ignore_precision = true,
      .lower_fpow = true,
      .lower_int64_options = ~0,
      .lower_cs_local_index_to_id = true,
      .lower_uniforms_to_ubo = true,
      .lower_image_offset_to_range_base = 1,
      .vectorize_tess_levels = 1,
      .has_bfe = true,
      .has_bfm = true,
      .has_bitfield_select = true,
   };
   if (container_of(device->vk.physical, struct terakan_physical_device const, vk)
          ->chip_family_info.is_r9xx) {
      nir_options.lower_doubles_options = nir_lower_ddiv | nir_lower_dfloor | nir_lower_dceil |
                                          nir_lower_dmod | nir_lower_dsub | nir_lower_dtrunc;
   } else {
      nir_options.lower_doubles_options = nir_lower_fp64_full_software;
      nir_options.lower_atomic_offset_to_range_base = true;
   }
   if (stage == MESA_SHADER_FRAGMENT) {
      nir_options.lower_all_io_to_temps = true;
   }

   nir_shader * nir = vk_spirv_to_nir(&device->vk, spirv, spirv_size_bytes, stage, entrypoint,
                                      spirv_options.subgroup_size, specialization_info,
                                      &spirv_options, &nir_options, false, NULL);

   NIR_PASS(_, nir, nir_lower_system_values);

   return nir;
}

void
terakan_shader_impl_finish(struct terakan_shader_impl * const shader,
                           VkAllocationCallbacks const * const allocator)
{
   if (shader->shader.arrays != NULL) {
      free(shader->shader.arrays);
   }

   terakan_bo_free(shader->static_state.program_bo, allocator);
}
