/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Meta shaders built as NIR and compiled through the same backend as application shaders, rather
 * than written out as Evergreen bytecode by hand.
 *
 * Every meta shader in this driver is hand-assembled, per generation, in the file that uses it.
 * That was affordable while the set was small, and it has become the thing standing between the
 * driver and four measured gaps: 3x-expanded clearing and copying, integer colour resolve, and
 * multisample sub-region copying all need a shader that does not exist, and each one means writing
 * more of the same by hand.
 *
 * The backend needed for the alternative turned out to be entirely reusable.
 * terakan_shader_impl_compile takes NIR and an r600_shader_key and depends on no Vulkan pipeline
 * object at all -- its ShaderBindingLayout is two assignments -- and it fills exactly the
 * terakan_shader_static and terakan_shader_sqk_usage that the meta shader table already carries.
 * So a meta shader can be built with nir_builder, run through the driver's own post-link lowering,
 * and compiled by the same code path that compiles application shaders.
 *
 * The one thing that does not apply is descriptor binding lowering. Meta shaders address the
 * hardware's resource slots directly rather than through a VkPipelineLayout, so meta NIR is written
 * with the r600 intrinsics that already carry an explicit resource index, and
 * terakan_nir_lower_bindings finds nothing to lower. It is still run, because the rest of the
 * post-link pipeline is wanted and the pass is a no-op on instructions it does not recognize; the
 * NULL layout it is given is only ever dereferenced from instruction handlers that meta NIR does
 * not reach.
 *
 * This does not convert the existing hand-written shaders. They work, they are covered, and
 * rewriting them would risk a great deal to gain nothing. The point is to make the next meta shader
 * writable.
 */

#include "terakan_meta_nir.h"

#include "terakan_meta.h"
#include "terakan_descriptor.h"

#include "terakan_device.h"
#include "terakan_physical_device.h"
#include "terakan_shader.h"

#include "compiler/nir/nir_builder.h"
#include "util/macros.h"
#include "vk_log.h"

#include <string.h>

VkResult
terakan_meta_nir_compile(struct terakan_device * const device, nir_shader * const nir,
                         struct terakan_shader_impl * const shader_out)
{
   memset(shader_out, 0, sizeof(*shader_out));

   /* Meta NIR carries no VkPipelineLayout, so binding lowering has nothing to resolve; see the
    * file comment. Everything else in the post-link pipeline -- the tex lowerings, the scalar and
    * vectorization passes, the algebraic optimizations -- applies unchanged.
    */
   /* Input locations are assigned in the application path before linking; the output ones are
    * assigned by the post-link lowering below.
    */
   if (nir->info.stage == MESA_SHADER_FRAGMENT && nir->num_inputs == 0) {
      nir_assign_io_var_locations(nir, nir_var_shader_in, &nir->num_inputs, nir->info.stage);
   }

   terakan_shader_lower_and_optimize_post_link(
      nir, NULL, &shader_out->sqk_usage, shader_out->uavs_for_mutable_resources_needed,
      &shader_out->push_constants_usage.driver_constants,
      nir->info.stage == MESA_SHADER_FRAGMENT ? &shader_out->fs.rtv_dsb_uncompacted_exports : NULL);

   /* The binding pass is what normally records which hardware resource slots a shader touches, and
    * meta NIR skips it because it has no descriptors to resolve. Without this the slot is never
    * bound and every fetch returns zero -- which is exactly what the first attempt did.
    */
   nir_foreach_function_impl (impl, nir) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            switch (instr->type) {
            case nir_instr_type_tex: {
               nir_tex_instr const * const tex = nir_instr_as_tex(instr);
               BITSET_SET(shader_out->sqk_usage.resources, tex->texture_index);
               /* A fetch addresses the resource directly; only a filtered sample reads a sampler,
                * and binding one that the shader never uses would keep a slot alive for nothing.
                */
               if (tex->op != nir_texop_txf && tex->op != nir_texop_txf_ms) {
                  shader_out->sqk_usage.samplers |= BITFIELD_BIT(tex->sampler_index);
               }
               break;
            }
            case nir_instr_type_intrinsic: {
               nir_intrinsic_instr const * const intrin = nir_instr_as_intrinsic(instr);
               /* Meta NIR reads its constants straight out of the constant cache, naming the
                * hardware buffer itself, so the buffer it locks has to be recorded here -- the
                * application path records kcache use while lowering descriptor sets, which meta NIR
                * does not go through.
                */
               if (intrin->intrinsic == nir_intrinsic_load_ubo_vec4) {
                  shader_out->sqk_usage.kcache |=
                     (uint16_t)BITFIELD_BIT(nir_src_as_uint(intrin->src[0]));
               }
               break;
            }
            default:
               break;
            }
         }
      }
   }

   union r600_shader_key shader_key = {};
   VkResult const result = terakan_shader_impl_compile(shader_out, device, &shader_key, nir, NULL);
   ralloc_free(nir);
   return result;
}

nir_shader *
terakan_meta_nir_build_opaque_ps(struct terakan_device const * const device)
{
   nir_builder builder = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &terakan_device_physical_device(device)->nir_options_fs,
      "terakan_meta_opaque_ps");
   nir_builder * const b = &builder;

   /* Every pixel shader has to export at least once, and this one is the stand-in for a pipeline
    * with no fragment shader, so it exports the value alpha-to-coverage reads as fully opaque.
    */
   nir_variable * const colour = nir_variable_create(
      b->shader, nir_var_shader_out, glsl_vec4_type(), "colour");
   colour->data.location = FRAG_RESULT_DATA0;
   colour->data.driver_location = 0;
   nir_store_var(b, colour, nir_imm_vec4(b, 0.0f, 0.0f, 0.0f, 1.0f), 0xF);

   b->shader->info.outputs_written = BITFIELD64_BIT(FRAG_RESULT_DATA0);
   return b->shader;
}

/* The opaque pixel shader is the one conversion this table starts with, chosen because it is the
 * smallest possible shader and because the driver uses it as the fallback whenever a pipeline has
 * no fragment shader -- so every depth-only draw in the test suite exercises it. Its hand-written
 * counterpart in terakan_meta_dummy.c stays in the table and simply goes unused for it, which keeps
 * the two comparable: both compile to two dwords that encode (0, 0, 0, 1) into the export
 * instruction's swizzle selects.
 */
nir_shader *
terakan_meta_nir_build_resolve_sample_zero_ps(struct terakan_device const * const device)
{
   nir_builder builder = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &terakan_device_physical_device(device)->nir_options_fs,
      "terakan_meta_resolve_sample_zero_ps");
   nir_builder * const b = &builder;

   /* CB_RESOLVE reads and writes the same coordinate, and the shader path keeps that: the draw is
    * scissored to the destination rectangle and the source is bound at the same offset, so the
    * fragment's own position is the source texel.
    */
   /* The backend takes the fragment position as an input variable at VARYING_SLOT_POS, which is
    * what nir_lower_sysvals_to_varyings produces for gl_FragCoord in the application path. The
    * load_frag_coord intrinsic is not something it accepts, so the variable is made directly.
    */
   nir_variable * const position = nir_variable_create(
      b->shader, nir_var_shader_in, glsl_vec4_type(), "gl_FragCoord");
   position->data.location = VARYING_SLOT_POS;
   position->data.interpolation = INTERP_MODE_NOPERSPECTIVE;
   b->shader->info.inputs_read = BITFIELD64_BIT(VARYING_SLOT_POS);
   nir_def * const frag_coord = nir_load_var(b, position);
   nir_def * const coord =
      nir_vec3(b, nir_f2i32(b, nir_channel(b, frag_coord, 0)),
               nir_f2i32(b, nir_channel(b, frag_coord, 1)), nir_imm_int(b, 0));

   /* The source is bound as a 2D array multisample resource in the primary meta slot, with the
    * layer already selected by the descriptor, so the array coordinate is zero.
    *
    * Sample zero rather than an average: Vulkan resolves an integer format by selecting one
    * sample, and averaging integers is not a thing CB_RESOLVE can be asked to stop doing.
    */
   nir_tex_instr * const fetch = nir_tex_instr_create(b->shader, 2);
   fetch->op = nir_texop_txf_ms;
   fetch->sampler_dim = GLSL_SAMPLER_DIM_MS;
   fetch->is_array = true;
   fetch->dest_type = nir_type_uint32;
   fetch->coord_components = 3;
   fetch->texture_index = TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META;
   fetch->sampler_index = 0;
   fetch->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
   fetch->src[1] = nir_tex_src_for_ssa(nir_tex_src_ms_index, nir_imm_int(b, 0));
   nir_def_init(&fetch->instr, &fetch->def, 4, 32);
   nir_builder_instr_insert(b, &fetch->instr);

   /* Exported as raw bits. A sample-zero resolve copies the value rather than converting it, so
    * the same shader serves signed and unsigned sources.
    */
   nir_variable * const colour = nir_variable_create(
      b->shader, nir_var_shader_out, glsl_uvec4_type(), "colour");
   colour->data.location = FRAG_RESULT_DATA0;
   colour->data.driver_location = 0;
   nir_store_var(b, colour, &fetch->def, 0xF);

   b->shader->info.outputs_written = BITFIELD64_BIT(FRAG_RESULT_DATA0);
   b->shader->info.fs.uses_sample_shading = false;
   return b->shader;
}


/* The blit's scale and offset constants, matching the layout terakan_meta_blit_image.c writes into
 * the constant cache. The first four are shared with the hand-written 2D shader and keep their
 * positions; the depth coordinate is a fifth, constant for a draw because each draw covers one
 * destination slice.
 */
enum {
   TERAKAN_META_NIR_BLIT_CONST_SCALE_X,
   TERAKAN_META_NIR_BLIT_CONST_OFF_X,
   TERAKAN_META_NIR_BLIT_CONST_SCALE_Y,
   TERAKAN_META_NIR_BLIT_CONST_OFF_Y,
   TERAKAN_META_NIR_BLIT_CONST_COORD_Z,
};

nir_shader *
terakan_meta_nir_build_blit_image_3d_ps(struct terakan_device const * const device)
{
   nir_builder builder = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &terakan_device_physical_device(device)->nir_options_fs,
      "terakan_meta_blit_image_3d_ps");
   nir_builder * const b = &builder;

   nir_variable * const position = nir_variable_create(
      b->shader, nir_var_shader_in, glsl_vec4_type(), "gl_FragCoord");
   position->data.location = VARYING_SLOT_POS;
   position->data.interpolation = INTERP_MODE_NOPERSPECTIVE;
   b->shader->info.inputs_read = BITFIELD64_BIT(VARYING_SLOT_POS);
   nir_def * const frag_coord = nir_load_var(b, position);

   /* Both vec4s of the constants are read: the first holds the four scale and offset values, the
    * second the depth coordinate in its x.
    */
   nir_def * const scale_off = nir_load_ubo_vec4(
      b, 4, 32, nir_imm_int(b, TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS), nir_imm_int(b, 0));
   nir_def * const depth = nir_load_ubo_vec4(
      b, 4, 32, nir_imm_int(b, TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS), nir_imm_int(b, 1));

   /* Same mapping the 2D shader applies, with the fragment's own position as the destination
    * coordinate: the constants are already divided by the source mip extent, so this lands in
    * normalized texture space.
    */
   nir_def * const u =
      nir_ffma(b, nir_channel(b, frag_coord, 0),
               nir_channel(b, scale_off, TERAKAN_META_NIR_BLIT_CONST_SCALE_X),
               nir_channel(b, scale_off, TERAKAN_META_NIR_BLIT_CONST_OFF_X));
   nir_def * const v =
      nir_ffma(b, nir_channel(b, frag_coord, 1),
               nir_channel(b, scale_off, TERAKAN_META_NIR_BLIT_CONST_SCALE_Y),
               nir_channel(b, scale_off, TERAKAN_META_NIR_BLIT_CONST_OFF_Y));
   nir_def * const w =
      nir_channel(b, depth, TERAKAN_META_NIR_BLIT_CONST_COORD_Z - 4);

   /* A 3D resource with a linear Z filter, which is the whole point: the source of a 3D blit is
    * otherwise sampled as a 2D array, and an array has no depth to filter along, so a linear blit
    * got the nearest slice.
    */
   nir_tex_instr * const sample = nir_tex_instr_create(b->shader, 1);
   sample->op = nir_texop_tex;
   sample->sampler_dim = GLSL_SAMPLER_DIM_3D;
   sample->is_array = false;
   sample->dest_type = nir_type_float32;
   sample->coord_components = 3;
   sample->texture_index = TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META;
   sample->sampler_index = TERAKAN_RESOURCE_RANGE_SHADER_CONSTANT_ARRAYS_OR_META;
   sample->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, nir_vec3(b, u, v, w));
   nir_def_init(&sample->instr, &sample->def, 4, 32);
   nir_builder_instr_insert(b, &sample->instr);

   nir_variable * const colour = nir_variable_create(
      b->shader, nir_var_shader_out, glsl_vec4_type(), "colour");
   colour->data.location = FRAG_RESULT_DATA0;
   colour->data.driver_location = 0;
   nir_store_var(b, colour, &sample->def, 0xF);

   b->shader->info.outputs_written = BITFIELD64_BIT(FRAG_RESULT_DATA0);
   return b->shader;
}

terakan_meta_nir_builder const terakan_meta_nir_builders[TERAKAN_META_SHADER_COUNT] = {
   [TERAKAN_META_SHADER_DUMMY_OPAQUE_PS] = terakan_meta_nir_build_opaque_ps,
   [TERAKAN_META_SHADER_RESOLVE_SAMPLE_ZERO_PS] = terakan_meta_nir_build_resolve_sample_zero_ps,
   [TERAKAN_META_SHADER_BLIT_IMAGE_3D_PS] = terakan_meta_nir_build_blit_image_3d_ps,
};
