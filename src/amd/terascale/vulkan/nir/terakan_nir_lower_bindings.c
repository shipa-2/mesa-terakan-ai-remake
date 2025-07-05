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
#include "terakan_descriptor_set_layout.h"
#include "terakan_nir.h"
#include "terakan_pipeline_layout.h"
#include "terakan_push_constants.h"

#include "util/macros.h"
#include "nir_builder.h"
#include "vk_enum_to_str.h"
#include "vk_log.h"

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool
terakan_nir_lower_load_vulkan_descriptor_filter(nir_instr const * const instr,
                                                UNUSED void const * const cb_data)
{
   return instr->type == nir_instr_type_intrinsic &&
          nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_load_vulkan_descriptor;
}

static nir_def *
terakan_nir_lower_load_vulkan_descriptor_impl(nir_builder * const b, nir_instr * const instr,
                                              UNUSED void * const cb_data)
{
   nir_intrinsic_instr * const resource_intrin =
      nir_src_as_intrinsic(nir_instr_as_intrinsic(instr)->src[0]);
   if (resource_intrin == NULL ||
       (resource_intrin->intrinsic != nir_intrinsic_vulkan_resource_index &&
        resource_intrin->intrinsic != nir_intrinsic_vulkan_resource_reindex)) {
      assert(!"load_vulkan_descriptor must accept a Vulkan resource index instruction");
      return NULL;
   }
   return &resource_intrin->def;
}

static nir_def *
terakan_nir_get_vulkan_resource_array_index(nir_builder * const b,
                                            nir_intrinsic_instr * const intrin)
{
   if (intrin->intrinsic == nir_intrinsic_vulkan_resource_index) {
      return intrin->src[0].ssa;
   }

   if (intrin->intrinsic != nir_intrinsic_vulkan_resource_reindex) {
      assert(!"vulkan_resource_reindex chains must consist only of vulkan_resource_reindex and "
              "vulkan_resource_index intrinsics");
      return NULL;
   }

   /* Accumulate in a way friendly to common subexpression elimination:
    * (index + reindex1) + reindex2...
    */
   nir_intrinsic_instr * const previous_resource_intrin = nir_src_as_intrinsic(intrin->src[0]);
   if (previous_resource_intrin == NULL) {
      assert(!"vulkan_resource_reindex chains must consist only of vulkan_resource_reindex and "
              "vulkan_resource_index intrinsics");
      return NULL;
   }
   nir_def * const previous_array_index =
      terakan_nir_get_vulkan_resource_array_index(b, previous_resource_intrin);
   b->cursor = nir_before_instr(&intrin->instr);
   return nir_iadd(b, previous_array_index, intrin->src[1].ssa);
}

static bool
terakan_nir_lower_vulkan_resource_reindex_instr(nir_builder * const b, nir_instr * const instr,
                                                UNUSED void * const cb_data)
{
   if (instr->type != nir_instr_type_intrinsic) {
      return false;
   }
   nir_intrinsic_instr * const resource_reindex_intrin = nir_instr_as_intrinsic(instr);
   if (resource_reindex_intrin->intrinsic != nir_intrinsic_vulkan_resource_reindex) {
      return false;
   }

   /* Go to the initial vulkan_resource_index to obtain the set and the binding. */
   nir_intrinsic_instr const * initial_resource_intrin = resource_reindex_intrin;
   while (initial_resource_intrin != NULL &&
          initial_resource_intrin->intrinsic == nir_intrinsic_vulkan_resource_reindex) {
      initial_resource_intrin = nir_src_as_intrinsic(initial_resource_intrin->src[0]);
   }
   if (initial_resource_intrin == NULL ||
       initial_resource_intrin->intrinsic != nir_intrinsic_vulkan_resource_index) {
      assert(!"vulkan_resource_reindex chains must consist only of vulkan_resource_reindex and "
              "vulkan_resource_index intrinsics");
      return false;
   }

   nir_def * const array_index =
      terakan_nir_get_vulkan_resource_array_index(b, resource_reindex_intrin);
   assert(array_index != NULL);
   if (array_index == NULL) {
      return false;
   }

   b->cursor = nir_before_instr(&resource_reindex_intrin->instr);
   nir_def_rewrite_uses(
      &resource_reindex_intrin->def,
      nir_vulkan_resource_index(b, initial_resource_intrin->num_components,
                                initial_resource_intrin->def.bit_size, array_index,
                                .desc_set = nir_intrinsic_desc_set(initial_resource_intrin),
                                .binding = nir_intrinsic_binding(initial_resource_intrin),
                                .desc_type = nir_intrinsic_desc_type(initial_resource_intrin)));
   nir_instr_remove(&resource_reindex_intrin->instr);
   return true;
}

static bool
terakan_nir_zero_vulkan_resource_offset_filter(nir_instr const * const instr,
                                               UNUSED void const * const cb_data)
{
   if (instr->type != nir_instr_type_intrinsic) {
      return false;
   }
   nir_intrinsic_instr const * const intrin = nir_instr_as_intrinsic(instr);
   return intrin->intrinsic == nir_intrinsic_vulkan_resource_index && intrin->num_components == 2;
}

static nir_def *
terakan_nir_zero_vulkan_resource_offset_impl(nir_builder * const b, nir_instr * const instr,
                                             UNUSED void * const cb_data)
{
   nir_intrinsic_instr const * const intrin = nir_instr_as_intrinsic(instr);
   return nir_vec2(b,
                   nir_vulkan_resource_index(b, 1, intrin->def.bit_size, intrin->src[0].ssa,
                                             .desc_set = nir_intrinsic_desc_set(intrin),
                                             .binding = nir_intrinsic_binding(intrin),
                                             .desc_type = nir_intrinsic_desc_type(intrin)),
                   nir_imm_int(b, 0));
}

struct terakan_nir_binding {
   struct terakan_pipeline_layout_set const * set;
   struct terakan_descriptor_set_layout_binding const * set_binding;

   /* NULL if not provided or zero. */
   nir_def * array_index;

   unsigned array_index_range_first;
   unsigned array_index_range_last;
};

static bool
terakan_nir_get_binding(nir_src const src, VkDescriptorType const expected_type,
                        struct terakan_pipeline_layout const * const layout,
                        nir_shader * const shader, struct terakan_nir_binding * const binding_out)
{
   nir_binding const binding = nir_chase_binding(src);
   assert(binding.success);
   if (unlikely(!binding.success)) {
      return false;
   }

   if (unlikely(binding.desc_set >= layout->vk.set_count)) {
      vk_loge(VK_LOG_OBJS(&layout->vk.base),
              "Descriptor set %u doesn't exist in the pipeline layout (contains %" PRIu32 " "
              "descriptor sets)",
              binding.desc_set, layout->vk.set_count);
      return false;
   }
   struct terakan_descriptor_set_layout const * const set_layout = container_of(
      layout->vk.set_layouts[binding.desc_set], struct terakan_descriptor_set_layout const, vk);

   if (unlikely(binding.binding >= set_layout->binding_count)) {
      vk_loge(VK_LOG_OBJS(&layout->vk.base),
              "Descriptor set %u binding %u doesn't exist in the descriptor set layout (contains "
              "%zu bindings)",
              binding.desc_set, binding.binding, set_layout->binding_count);
      return false;
   }
   struct terakan_descriptor_set_layout_binding const * const set_binding =
      &set_layout->bindings[binding.binding];

   /* If a terakan_descriptor_set_layout_binding has 0 descriptors, its fields may be uninitialized.
    * The inclusive array index range would also be impossible to calculate.
    */
   if (unlikely(set_binding->descriptor_count == 0)) {
      vk_loge(VK_LOG_OBJS(&layout->vk.base),
              "Descriptor set %u binding %u doesn't contain any descriptors", binding.desc_set,
              binding.binding);
      return false;
   }

   bool type_compatible = set_binding->descriptor_type == expected_type;
   if (!type_compatible) {
      if (expected_type == VK_DESCRIPTOR_TYPE_SAMPLER) {
         type_compatible =
            set_binding->descriptor_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      } else if (expected_type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
         type_compatible =
            set_binding->descriptor_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
            set_binding->descriptor_type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
      } else if (expected_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
         type_compatible =
            set_binding->descriptor_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      } else if (expected_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
         type_compatible =
            set_binding->descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
      }
   }
   if (unlikely(!type_compatible)) {
      vk_loge(
         VK_LOG_OBJS(&layout->vk.base),
         "Descriptor set %u binding %u is expected to contain descriptors compatible with %s, but "
         "the descriptor set layout specifies them as %s instead",
         binding.desc_set, binding.binding, vk_DescriptorType_to_str(expected_type),
         vk_DescriptorType_to_str(set_binding->descriptor_type));
      return false;
   }

   assert(binding.num_indices <= 1);
   nir_def * array_index = binding.num_indices >= 1 ? binding.indices[0].ssa : NULL;
   unsigned array_index_range_first, array_index_range_last;
   if (array_index != NULL) {
      if (array_index->parent_instr->type == nir_instr_type_load_const) {
         nir_load_const_instr const * const array_index_load_const =
            nir_instr_as_load_const(array_index->parent_instr);
         array_index_range_first = array_index_load_const->value[0].u32;
         if (unlikely(array_index_range_first >= set_binding->descriptor_count)) {
            vk_loge(VK_LOG_OBJS(&layout->vk.base),
                    "Descriptor %u doesn't exist in the descriptor set %u binding %u (contains %u "
                    "descriptors)",
                    array_index_range_first, binding.desc_set, binding.binding,
                    set_binding->descriptor_count);
            return false;
         }
         array_index_range_last = array_index_range_first;
         if (array_index_range_first == 0) {
            /* For consistency between buffer and texture instructions (the latter may have no array
             * source), don't pass an array index of 0 to the caller.
             */
            array_index = NULL;
         }
      } else {
         array_index_range_first = 0;
         array_index_range_last = set_binding->descriptor_count - 1;
         /* Limit to the array size specified in the shader (if not unbounded) for a more precise
          * descriptor demand.
          */
         nir_variable const * const binding_variable = nir_get_binding_variable(shader, binding);
         if (binding_variable != NULL && glsl_type_is_array(binding_variable->type) &&
             binding_variable->type->length != 0) {
            array_index_range_last =
               MIN2(binding_variable->type->length - 1u, array_index_range_last);
         }
      }
   } else {
      array_index_range_first = 0;
      array_index_range_last = 0;
   }

   binding_out->set = &layout->sets[binding.desc_set];
   binding_out->set_binding = set_binding;
   binding_out->array_index = array_index;
   binding_out->array_index_range_first = array_index_range_first;
   binding_out->array_index_range_last = array_index_range_last;
   return true;
}

struct terakan_nir_lower_bindings_state {
   struct terakan_pipeline_layout const * layout;

   /* 0-based. */
   BITSET_WORD * resources_needed;
   uint32_t * samplers_needed;
};

/* If there was an error while lowering the binding, such as if the binding was not obtained
 * successfully, to avoid leaving the shader in an indeterminate state as it's very easy for an app
 * to cause issues like those, treating invalid instructions largely as null descriptor accesses -
 * returning zero for loads, ignoring stores.
 */
static void
terakan_nir_lower_bindings_instr_to_null(nir_instr * const instr)
{
   nir_def * const old_def = nir_instr_def(instr);
   if (old_def != NULL) {
      nir_builder b = nir_builder_at(nir_before_instr(instr));
      nir_def_rewrite_uses(old_def, nir_imm_zero(&b, old_def->num_components, old_def->bit_size));
   }
   nir_instr_remove(instr);
}

static bool
terakan_nir_lower_bindings_instr_tex(nir_builder * const b, nir_tex_instr * const tex,
                                     struct terakan_nir_lower_bindings_state * const state)
{
   bool shader_nir_progress = false;

   gl_shader_stage const stage = b->shader->info.stage;

   /* If nir_tex_src_texture/sampler_deref isn't present, the lowering was possibly invoked multiple
    * times, just ignore the instruction if it has already been lowered.
    */

   struct terakan_nir_binding binding;

   int const texture_deref_src_index = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
   if (likely(texture_deref_src_index != -1)) {
      nir_tex_src * const texture_src = &tex->src[texture_deref_src_index];
      if (unlikely(!terakan_nir_get_binding(texture_src->src, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                            state->layout, b->shader, &binding))) {
         terakan_nir_lower_bindings_instr_to_null(&tex->instr);
         return true;
      }
      uint8_t const resource_index_base = binding.set->first_shader_resources[stage] +
                                          binding.set_binding->first_shader_resources[stage];
      BITSET_SET_RANGE(state->resources_needed,
                       resource_index_base + binding.array_index_range_first,
                       resource_index_base + binding.array_index_range_last);
      shader_nir_progress = true;
      tex->texture_index = resource_index_base;
      if (binding.array_index != NULL) {
         if (binding.array_index->parent_instr->type == nir_instr_type_load_const) {
            tex->texture_index +=
               nir_instr_as_load_const(binding.array_index->parent_instr)->value[0].u32;
            nir_tex_instr_remove_src(tex, texture_deref_src_index);
         } else {
            texture_src->src_type = nir_tex_src_texture_offset;
            nir_src_rewrite(&texture_src->src, binding.array_index);
         }
      } else {
         nir_tex_instr_remove_src(tex, texture_deref_src_index);
      }
   }

   int const sampler_deref_src_index = nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref);
   if (likely(sampler_deref_src_index != -1)) {
      nir_tex_src * const sampler_src = &tex->src[sampler_deref_src_index];
      if (unlikely(!terakan_nir_get_binding(sampler_src->src, VK_DESCRIPTOR_TYPE_SAMPLER,
                                            state->layout, b->shader, &binding))) {
         terakan_nir_lower_bindings_instr_to_null(&tex->instr);
         return true;
      }
      uint8_t const sampler_index_base = binding.set->first_shader_samplers[stage] +
                                         binding.set_binding->first_shader_samplers[stage];
      *state->samplers_needed |=
         BITFIELD_RANGE(sampler_index_base + binding.array_index_range_first,
                        binding.array_index_range_last - binding.array_index_range_first + 1);
      shader_nir_progress = true;
      tex->sampler_index = sampler_index_base;
      if (binding.array_index != NULL) {
         if (binding.array_index->parent_instr->type == nir_instr_type_load_const) {
            tex->sampler_index +=
               nir_instr_as_load_const(binding.array_index->parent_instr)->value[0].u32;
            nir_tex_instr_remove_src(tex, sampler_deref_src_index);
         } else {
            sampler_src->src_type = nir_tex_src_sampler_offset;
            nir_src_rewrite(&sampler_src->src, binding.array_index);
         }
      } else {
         nir_tex_instr_remove_src(tex, sampler_deref_src_index);
      }
   }

   return shader_nir_progress;
}

static void
terakan_nir_lower_bindings_instr_load_ubo(nir_builder * const b, nir_intrinsic_instr * const intrin,
                                          struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_load_ubo);

   b->cursor = nir_before_instr(&intrin->instr);

   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(intrin->src[0], VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                         state->layout, b->shader, &binding))) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }
   if (binding.array_index == NULL) {
      binding.array_index = nir_imm_zero(b, 1, 32);
   }

   gl_shader_stage const stage = b->shader->info.stage;

   /* TODO(Triang3l): Load from the constant cache. */

   uint8_t const resource_index_base = binding.set->first_shader_resources[stage] +
                                       binding.set_binding->first_shader_resources[stage];
   BITSET_SET_RANGE(state->resources_needed, resource_index_base + binding.array_index_range_first,
                    resource_index_base + binding.array_index_range_last);
   nir_def_rewrite_uses(&intrin->def,
                        terakan_nir_load_raw_resource_buffer(
                           b, intrin->num_components, intrin->def.bit_size,
                           nir_intrinsic_access(intrin) | ACCESS_CAN_REORDER, resource_index_base,
                           binding.array_index, 0, intrin->src[1].ssa));
   nir_instr_remove(&intrin->instr);
}

static void
terakan_nir_lower_bindings_instr_load_push_constant(
   nir_builder * const b, nir_intrinsic_instr * const intrin,
   struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_load_push_constant);

   b->cursor = nir_before_instr(&intrin->instr);

   /* Push constants don't have access robustness, simply add the base without an integer overflow
    * check.
    */

   /* TODO(Triang3l): Load from the constant cache. */

   BITSET_SET(state->resources_needed, TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS);
   nir_def_rewrite_uses(
      &intrin->def,
      terakan_nir_load_raw_resource_buffer(
         b, intrin->num_components, intrin->def.bit_size, ACCESS_CAN_REORDER,
         TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS, nir_imm_int(b, 0),
         TERAKAN_PUSH_CONSTANTS_APP_BASE_BYTES + nir_intrinsic_base(intrin), intrin->src[0].ssa));
   nir_instr_remove(&intrin->instr);
}

static void
terakan_nir_lower_bindings_instr_load_ssbo(nir_builder * const b,
                                           nir_intrinsic_instr * const intrin,
                                           struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_load_ssbo);

   b->cursor = nir_before_instr(&intrin->instr);

   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(intrin->src[0], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         state->layout, b->shader, &binding))) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }
   if (binding.array_index == NULL) {
      binding.array_index = nir_imm_zero(b, 1, 32);
   }

   /* Vertex fetches are coherent with UAVs, do a vertex fetch unconditionally. */
   uint8_t const resource_index_base =
      binding.set->first_shader_resources[b->shader->info.stage] +
      binding.set_binding->first_shader_resources[b->shader->info.stage];
   BITSET_SET_RANGE(state->resources_needed, resource_index_base + binding.array_index_range_first,
                    resource_index_base + binding.array_index_range_last);
   nir_def_rewrite_uses(&intrin->def, terakan_nir_load_raw_resource_buffer(
                                         b, intrin->num_components, intrin->def.bit_size,
                                         nir_intrinsic_access(intrin), resource_index_base,
                                         binding.array_index, 0, intrin->src[1].ssa));
   nir_instr_remove(&intrin->instr);
}

static bool
terakan_nir_lower_bindings_instr(nir_builder * const b, nir_instr * const instr,
                                 void * const cb_data)
{
   struct terakan_nir_lower_bindings_state * const state =
      (struct terakan_nir_lower_bindings_state *)cb_data;

   if (instr->type == nir_instr_type_tex) {
      return terakan_nir_lower_bindings_instr_tex(b, nir_instr_as_tex(instr), state);
   }

   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr * const intrin = nir_instr_as_intrinsic(instr);
      switch (intrin->intrinsic) {
      case nir_intrinsic_load_ubo:
         terakan_nir_lower_bindings_instr_load_ubo(b, intrin, state);
         return true;
      case nir_intrinsic_load_push_constant:
         terakan_nir_lower_bindings_instr_load_push_constant(b, intrin, state);
         return true;
      case nir_intrinsic_load_ssbo:
         terakan_nir_lower_bindings_instr_load_ssbo(b, intrin, state);
         return true;
      default:
         break;
      }
   }

   return false;
}

bool
terakan_nir_lower_bindings(nir_shader * const shader,
                           struct terakan_pipeline_layout const * const layout,
                           BITSET_WORD * const resources_needed_accum,
                           uint32_t * const samplers_needed_accum)
{
   bool progress = false;

   /* TODO(Triang3l): Lower 64-bit buffer access. */

   /* Lower load_vulkan_descriptor and vulkan_resource_reindex chains to vulkan_resource_index. */
   progress |=
      nir_shader_lower_instructions(shader, terakan_nir_lower_load_vulkan_descriptor_filter,
                                    terakan_nir_lower_load_vulkan_descriptor_impl, NULL);
   progress |=
      nir_shader_instructions_pass(shader, terakan_nir_lower_vulkan_resource_reindex_instr,
                                   nir_metadata_block_index | nir_metadata_dominance, NULL);

   /* Make data offsets in buffers relative to 0. */
   progress |= nir_shader_lower_instructions(shader, terakan_nir_zero_vulkan_resource_offset_filter,
                                             terakan_nir_zero_vulkan_resource_offset_impl, NULL);

   /* Make sure resource indices and data offsets are known to be constant if they are. */
   bool constant_folding_progress;
   do {
      constant_folding_progress = false;
      bool copy_prop_progress = false;
      NIR_PASS(copy_prop_progress, shader, nir_copy_prop);
      if (copy_prop_progress) {
         constant_folding_progress = true;
         /* Cleanup to prevent the same propagations from happening infinitely. */
         NIR_PASS(constant_folding_progress, shader, nir_opt_dce);
      }
      NIR_PASS(constant_folding_progress, shader, nir_opt_constant_folding);
   } while (constant_folding_progress);

   /* Apply the pipeline layout and perform various lowerings based on it. */
   struct terakan_nir_lower_bindings_state state = {
      .layout = layout,
      .resources_needed = resources_needed_accum,
      .samplers_needed = samplers_needed_accum,
   };
   progress |= nir_shader_instructions_pass(shader, terakan_nir_lower_bindings_instr,
                                            nir_metadata_none, &state);

   nir_shader_preserve_all_metadata(shader);
   return progress;
}
