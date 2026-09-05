/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Seamless cube map filtering is switched off by the resource descriptor's `NUM_FORMAT`.
 *
 * `terakan_cube_gather_probe` establishes this directly. Two cube images holding the same numbers,
 * one `R8G8B8A8_UNORM` and one `R8G8B8A8_UINT`, differ in exactly one descriptor bit --
 * `NUM_FORMAT_ALL`, `NORM` against `INT` -- and gathering a direction near a face edge returns
 * texels from both faces for the unorm image and texels clamped inside the one face for the
 * integer image. Forcing the integer image to `NORM` makes it cross the edge, which pins the field
 * as the cause.
 *
 * `SCALED` crosses the edge as well and, unlike `NORM`, delivers the integer value itself: the
 * probe reads back exactly 75.0, 8.0, 4.0 and 71.0 where the unorm image reads 75, 8, 4 and 71. So
 * `terakan_image.c` describes an integer cube view that way, and this pass converts the float back.
 *
 * It can only do that where the conversion is exact, which is why the two halves cannot simply
 * agree on "an integer cube": a 32-bit float holds integers exactly to 2^24, so a 32-bit integer
 * format keeps `INT` and keeps the seam. That decision belongs to the view's format, which a
 * shader cannot see, so it arrives at draw time in
 * `terakan_push_constants_driver::texture_scaled_integer` -- one bit per texture slot, the same
 * arrangement `sampler_unnormalized` already uses for the same reason.
 */

#include "terakan_nir.h"

#include "terakan_descriptor.h"
#include "terakan_push_constants.h"

#include "nir_builder.h"

struct terakan_nir_lower_integer_cube_fetch_state {
   uint32_t * driver_push_constants_used;
   struct terakan_shader_sqk_usage * sqk_usage;
};

static bool
terakan_nir_lower_integer_cube_fetch_instr(nir_builder * const b, nir_instr * const instr,
                                           void * const data)
{
   if (instr->type != nir_instr_type_tex) {
      return false;
   }
   nir_tex_instr * const tex = nir_instr_as_tex(instr);
   /* `r600_nir_lower_cube_to_2darray` has already turned the fetch into a 2D array one by the
    * time the hardware slots this reads are assigned, and it leaves `array_is_lowered_cube` behind
    * to say what it was.
    */
   if (!tex->array_is_lowered_cube || tex->is_shadow) {
      return false;
   }
   /* A size or level query returns integers of its own, unrelated to the format. */
   switch (tex->op) {
   case nir_texop_tex:
   case nir_texop_txb:
   case nir_texop_txl:
   case nir_texop_txd:
   case nir_texop_tg4:
      break;
   default:
      return false;
   }
   bool const is_signed = tex->dest_type == nir_type_int32;
   if (!is_signed && tex->dest_type != nir_type_uint32) {
      return false;
   }

   struct terakan_nir_lower_integer_cube_fetch_state * const state = data;

   b->cursor = nir_before_instr(instr);
   *state->driver_push_constants_used |=
      BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_TEXTURE_SCALED_INTEGER);
   BITSET_SET(state->sqk_usage->resources, TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS);
   nir_def * const mask = terakan_nir_load_raw_resource_buffer(
      b, 1, 32, ACCESS_CAN_REORDER, TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS, nir_imm_int(b, 0),
      (unsigned)(offsetof(struct terakan_push_constants_driver, texture_scaled_integer) +
                 sizeof(uint32_t) * (unsigned)b->shader->info.stage),
      nir_imm_int(b, 0));

   int const texture_offset_src_index = nir_tex_instr_src_index(tex, nir_tex_src_texture_offset);
   nir_def * texture_slot = nir_imm_int(b, tex->texture_index);
   if (texture_offset_src_index != -1) {
      texture_slot = nir_iadd(b, texture_slot, tex->src[texture_offset_src_index].src.ssa);
   }
   nir_def * const is_scaled = nir_i2b(b, nir_iand_imm(b, nir_ushr(b, mask, texture_slot), 1));

   /* Whichever way the descriptor went, the bits the hardware returns are what they are; only how
    * they are to be read differs. Declaring the fetch as float makes the conversion expressible,
    * and the unconverted value is the same bits seen as an integer.
    */
   tex->dest_type = nir_type_float32;

   b->cursor = nir_after_instr(instr);
   nir_def * const raw = &tex->def;
   nir_def * const converted = is_signed ? nir_f2i32(b, raw) : nir_f2u32(b, raw);
   /* The mask is a push constant, so this is uniform across the wave. */
   nir_def * const selected = nir_bcsel(b, is_scaled, converted, raw);
   nir_def_rewrite_uses_after(raw, selected, selected->parent_instr);
   return true;
}

bool
terakan_nir_lower_integer_cube_fetch(nir_shader * const shader,
                                     uint32_t * const driver_push_constants_used,
                                     struct terakan_shader_sqk_usage * const sqk_usage)
{
   struct terakan_nir_lower_integer_cube_fetch_state state = {
      .driver_push_constants_used = driver_push_constants_used,
      .sqk_usage = sqk_usage,
   };
   return nir_shader_instructions_pass(shader, terakan_nir_lower_integer_cube_fetch_instr,
                                       nir_metadata_none, &state);
}
