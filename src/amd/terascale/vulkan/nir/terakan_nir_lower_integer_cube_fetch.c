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

static nir_def *
terakan_nir_load_texture_slot_mask(nir_builder * const b, size_t const field_offset_bytes,
                                   unsigned const component)
{
   return terakan_nir_load_raw_resource_buffer(
      b, 1, 32, ACCESS_CAN_REORDER, TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS, nir_imm_int(b, 0),
      (unsigned)(field_offset_bytes +
                 sizeof(uint32_t) * (4u * (unsigned)b->shader->info.stage + component)),
      nir_imm_int(b, 0));
}

static bool
terakan_nir_lower_integer_cube_fetch_instr(nir_builder * const b, nir_instr * const instr,
                                           void * const data)
{
   if (instr->type != nir_instr_type_tex) {
      return false;
   }
   nir_tex_instr * const tex = nir_instr_as_tex(instr);
   if (tex->is_shadow) {
      return false;
   }
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
   bool const is_integer = is_signed || tex->dest_type == nir_type_uint32;
   /* `r600_nir_lower_cube_to_2darray` has already turned a cube fetch into a 2D array one by the
    * time the hardware slots this reads are assigned, and leaves `array_is_lowered_cube` to say
    * what it was.
    */
   bool const convert_scaled_integer = is_integer && tex->array_is_lowered_cube;
   /* W is the one component whose swizzle constant a gather produces correctly on its own. */
   bool const substitute_swizzle_constant = tex->op == nir_texop_tg4 && tex->component < 3;
   if (!convert_scaled_integer && !substitute_swizzle_constant) {
      return false;
   }

   struct terakan_nir_lower_integer_cube_fetch_state * const state = data;

   b->cursor = nir_before_instr(instr);
   BITSET_SET(state->sqk_usage->resources, TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS);

   int const texture_offset_src_index = nir_tex_instr_src_index(tex, nir_tex_src_texture_offset);
   nir_def * texture_slot = nir_imm_int(b, tex->texture_index);
   if (texture_offset_src_index != -1) {
      texture_slot = nir_iadd(b, texture_slot, tex->src[texture_offset_src_index].src.ssa);
   }

   nir_def * is_scaled = NULL;
   if (convert_scaled_integer) {
      *state->driver_push_constants_used |=
         BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_TEXTURE_SCALED_INTEGER);
      nir_def * const mask = terakan_nir_load_raw_resource_buffer(
         b, 1, 32, ACCESS_CAN_REORDER, TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS, nir_imm_int(b, 0),
         (unsigned)(offsetof(struct terakan_push_constants_driver, texture_scaled_integer) +
                    sizeof(uint32_t) * (unsigned)b->shader->info.stage),
         nir_imm_int(b, 0));
      is_scaled = nir_i2b(b, nir_iand_imm(b, nir_ushr(b, mask, texture_slot), 1));
      /* Whichever way the descriptor went, the bits the hardware returns are what they are; only
       * how they are read differs. Declaring the fetch as float makes the conversion expressible,
       * and the unconverted value is the same bits seen as an integer.
       */
      tex->dest_type = nir_type_float32;
   }

   nir_def * is_constant = NULL;
   nir_def * is_one = NULL;
   if (substitute_swizzle_constant) {
      *state->driver_push_constants_used |=
         BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_TEXTURE_GATHER_SWIZZLE_CONSTANT);
      nir_def * const constant_mask = terakan_nir_load_texture_slot_mask(
         b, offsetof(struct terakan_push_constants_driver, texture_gather_swizzle_constant),
         tex->component);
      nir_def * const one_mask = terakan_nir_load_texture_slot_mask(
         b, offsetof(struct terakan_push_constants_driver, texture_gather_swizzle_one),
         tex->component);
      is_constant = nir_i2b(b, nir_iand_imm(b, nir_ushr(b, constant_mask, texture_slot), 1));
      is_one = nir_i2b(b, nir_iand_imm(b, nir_ushr(b, one_mask, texture_slot), 1));
   }

   b->cursor = nir_after_instr(instr);
   nir_def * value = &tex->def;

   /* The conversion comes first: the constant, when there is one, replaces the converted value
    * rather than the raw bits.
    */
   if (convert_scaled_integer) {
      nir_def * const converted = is_signed ? nir_f2i32(b, value) : nir_f2u32(b, value);
      /* Both masks are push constants, so every selection here is uniform across the wave. */
      value = nir_bcsel(b, is_scaled, converted, value);
   }

   if (substitute_swizzle_constant) {
      nir_def * const one = is_integer ? nir_imm_int(b, 1) : nir_imm_float(b, 1.0f);
      nir_def * const zero = is_integer ? nir_imm_int(b, 0) : nir_imm_float(b, 0.0f);
      nir_def * const constant_value =
         nir_replicate(b, nir_bcsel(b, is_one, one, zero), value->num_components);
      value = nir_bcsel(b, is_constant, constant_value, value);
   }

   nir_def_rewrite_uses_after(&tex->def, value, value->parent_instr);
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
