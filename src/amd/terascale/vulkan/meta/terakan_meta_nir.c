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
   terakan_shader_lower_and_optimize_post_link(
      nir, NULL, &shader_out->sqk_usage, shader_out->uavs_for_mutable_resources_needed,
      &shader_out->push_constants_usage.driver_constants,
      nir->info.stage == MESA_SHADER_FRAGMENT ? &shader_out->fs.rtv_dsb_uncompacted_exports : NULL);

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
