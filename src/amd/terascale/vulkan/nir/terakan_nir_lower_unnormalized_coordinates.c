/*
 * Copyright © 2024 Vitaliy Triang3l Kuzmin
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

#include "terakan_descriptor.h"
#include "terakan_nir.h"
#include "terakan_push_constants.h"

#include "util/macros.h"
#include "util/u_dynarray.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

/* Whether an unnormalized sampler may legally be used with this instruction.
 *
 * VUID-RuntimeSpirv-None-06479 and the "Unnormalized Texel Coordinate Operations" section restrict
 * a sampler created with `unnormalizedCoordinates` to a single-level, single-layer 1D or 2D image
 * sampled with an explicit level of detail, without a depth comparison, projection, offset or
 * gather. Every other texture instruction therefore cannot observe such a sampler, and is left
 * alone so the common implicit-derivative `texture()` path keeps costing nothing.
 */
static bool
terakan_nir_tex_may_be_unnormalized(nir_tex_instr const * const tex)
{
   switch (tex->op) {
   case nir_texop_txl:
   case nir_texop_txd:
      break;
   default:
      return false;
   }
   if (tex->is_array || tex->is_shadow) {
      return false;
   }
   switch (tex->sampler_dim) {
   case GLSL_SAMPLER_DIM_1D:
   case GLSL_SAMPLER_DIM_2D:
   case GLSL_SAMPLER_DIM_RECT:
      break;
   default:
      return false;
   }
   return nir_tex_instr_src_index(tex, nir_tex_src_offset) == -1 &&
          nir_tex_instr_src_index(tex, nir_tex_src_coord) != -1;
}

/* Builds `textureSize(tex, 0)` for the same texture binding as `tex`. */
static nir_def *
terakan_nir_tex_size(nir_builder * const b, nir_tex_instr const * const tex,
                     unsigned const num_components)
{
   int const texture_offset_src_index =
      nir_tex_instr_src_index(tex, nir_tex_src_texture_offset);
   unsigned const src_count = texture_offset_src_index != -1 ? 2 : 1;

   nir_tex_instr * const size = nir_tex_instr_create(b->shader, src_count);
   size->op = nir_texop_txs;
   size->sampler_dim = tex->sampler_dim;
   size->is_array = false;
   size->dest_type = nir_type_int32;
   size->texture_index = tex->texture_index;
   size->sampler_index = tex->sampler_index;
   size->src[0].src_type = nir_tex_src_lod;
   size->src[0].src = nir_src_for_ssa(nir_imm_int(b, 0));
   if (texture_offset_src_index != -1) {
      size->src[1].src_type = nir_tex_src_texture_offset;
      size->src[1].src = nir_src_for_ssa(tex->src[texture_offset_src_index].src.ssa);
   }
   nir_def_init(&size->instr, &size->def, num_components, 32);
   nir_builder_instr_insert(b, &size->instr);
   return &size->def;
}

struct terakan_nir_lower_unnormalized_coordinates_state {
   struct terakan_shader_sqk_usage * sqk_usage;
   uint32_t * driver_push_constants_used;
};

static void
terakan_nir_lower_unnormalized_coordinates_instr(
   nir_builder * const b, nir_tex_instr * const tex,
   struct terakan_nir_lower_unnormalized_coordinates_state * const state)
{
   int const coord_src_index = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   nir_def * const coord = tex->src[coord_src_index].src.ssa;

   b->cursor = nir_before_instr(&tex->instr);

   *state->driver_push_constants_used |=
      BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_SAMPLER_UNNORMALIZED);
   BITSET_SET(state->sqk_usage->resources, TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS);
   nir_def * const mask = terakan_nir_load_raw_resource_buffer(
      b, 1, 32, ACCESS_CAN_REORDER, TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS, nir_imm_int(b, 0),
      (unsigned)(offsetof(struct terakan_push_constants_driver, sampler_unnormalized) +
                 sizeof(uint32_t) * (unsigned)b->shader->info.stage),
      nir_imm_int(b, 0));

   int const sampler_offset_src_index =
      nir_tex_instr_src_index(tex, nir_tex_src_sampler_offset);
   nir_def * sampler_slot = nir_imm_int(b, tex->sampler_index);
   if (sampler_offset_src_index != -1) {
      sampler_slot = nir_iadd(b, sampler_slot, tex->src[sampler_offset_src_index].src.ssa);
   }

   /* The mask is a push constant, so the condition is uniform across the wave and the size query
    * is skipped entirely by the usual draw that binds no unnormalized sampler.
    */
   nir_def * const is_unnormalized =
      nir_i2b(b, nir_iand_imm(b, nir_ushr(b, mask, sampler_slot), 1));

   nir_push_if(b, is_unnormalized);
   nir_def * const size = terakan_nir_tex_size(b, tex, coord->num_components);
   nir_def * const normalized = nir_fmul(b, coord, nir_frcp(b, nir_i2f32(b, size)));
   nir_pop_if(b, NULL);

   nir_src_rewrite(&tex->src[coord_src_index].src, nir_if_phi(b, normalized, coord));
}

bool
terakan_nir_lower_unnormalized_coordinates(
   nir_shader * const shader, struct terakan_shader_sqk_usage * const sqk_usage_accum,
   uint32_t * const driver_push_constants_used_accum)
{
   struct terakan_nir_lower_unnormalized_coordinates_state state = {
      .sqk_usage = sqk_usage_accum,
      .driver_push_constants_used = driver_push_constants_used_accum,
   };

   bool progress = false;
   nir_foreach_function_impl (impl, shader) {
      /* Collecting first: the rewrite splits the block the instruction is in, which must not
       * happen while that block is being walked.
       */
      struct util_dynarray instructions;
      util_dynarray_init(&instructions, NULL);
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_tex) {
               continue;
            }
            nir_tex_instr * const tex = nir_instr_as_tex(instr);
            if (terakan_nir_tex_may_be_unnormalized(tex)) {
               util_dynarray_append(&instructions, nir_tex_instr *, tex);
            }
         }
      }

      if (instructions.size != 0) {
         nir_builder b = nir_builder_create(impl);
         util_dynarray_foreach (&instructions, nir_tex_instr *, tex) {
            terakan_nir_lower_unnormalized_coordinates_instr(&b, *tex, &state);
         }
         progress = true;
         nir_progress(true, impl, nir_metadata_none);
      } else {
         nir_progress(false, impl, nir_metadata_all);
      }

      util_dynarray_fini(&instructions);
   }
   return progress;
}
