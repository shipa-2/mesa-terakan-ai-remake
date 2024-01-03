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

#include "terakan_descriptor_set_layout.h"
#include "terakan_nir.h"
#include "terakan_pipeline_layout.h"

#include "util/macros.h"
#include "nir_builder.h"

#include <assert.h>
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

static struct terakan_descriptor_set_layout_binding const *
terakan_nir_get_binding_for_index_chain(nir_src const src, VkDescriptorType const expected_type,
                                        struct terakan_pipeline_layout const * const layout,
                                        struct terakan_pipeline_layout_set const ** const set_out,
                                        nir_src * const array_index_out)
{
   nir_binding const binding = nir_chase_binding(src);
   assert(binding.success);
   if (unlikely(!binding.success)) {
      return NULL;
   }
   assert(binding.num_indices == 1);

   assert(binding.desc_set < layout->vk.set_count &&
          "Descriptor set index is out of pipeline layout bounds");
   if (unlikely(binding.desc_set >= layout->vk.set_count)) {
      return NULL;
   }
   struct terakan_descriptor_set_layout const * const set_layout = container_of(
      layout->vk.set_layouts[binding.desc_set], struct terakan_descriptor_set_layout const, vk);

   assert(binding.binding < set_layout->binding_count &&
          "Descriptor set binding index is out of descriptor set layout bounds");
   if (unlikely(binding.binding >= set_layout->binding_count)) {
      return NULL;
   }

   struct terakan_descriptor_set_layout_binding const * const set_binding =
      &set_layout->bindings[binding.binding];

   /* If a binding is empty, its fields are not initialized. */
   assert(set_binding->descriptor_count != 0 &&
          "Descriptor set binding index points to an empty or a non-existent binding");
   if (unlikely(set_binding->descriptor_count == 0)) {
      return NULL;
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
   assert(type_compatible &&
          "Binding type is incompatible with the descriptor type in the descriptor set layout");
   if (unlikely(!type_compatible)) {
      return NULL;
   }

   *set_out = &layout->sets[binding.desc_set];
   *array_index_out = binding.indices[0];
   return &set_layout->bindings[binding.binding];
}

static void
terakan_nir_get_binding_index_range(uint32_t const descriptor_count, nir_src const array_index,
                                    uint32_t * const first_out, uint32_t * const last_out)
{
   nir_const_value const * const array_index_const = nir_src_as_const_value(array_index);
   if (array_index_const != NULL) {
      uint32_t const array_index_const_value = array_index_const->u32;
      *first_out = array_index_const_value;
      *last_out = array_index_const_value;
      return;
   }

   *first_out = 0;
   *last_out = descriptor_count - 1;
}

struct terakan_nir_lower_bindings_state {
   struct terakan_pipeline_layout const * layout;

   BITSET_WORD * resources_needed;
};

static bool
terakan_nir_lower_bindings_instr(nir_builder * const b, nir_instr * const instr,
                                 void * const cb_data)
{
   struct terakan_nir_lower_bindings_state * const state =
      (struct terakan_nir_lower_bindings_state *)cb_data;

   gl_shader_stage const stage = b->shader->info.stage;

   struct terakan_descriptor_set_layout_binding const * binding;
   struct terakan_pipeline_layout_set const * set;
   nir_src array_index;
   uint32_t needed_first, needed_last;

   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr * const intrin = nir_instr_as_intrinsic(instr);
      switch (intrin->intrinsic) {
      case nir_intrinsic_load_ubo_vec4: {
         /* UBOs are referenced by a chain involving vulkan_resource_index's result. */

         binding = terakan_nir_get_binding_for_index_chain(
            intrin->src[0], VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, state->layout, &set, &array_index);
         if (unlikely(binding == NULL)) {
            return false;
         }

         uint8_t const resource_index_base =
            set->first_shader_resources[stage] + binding->first_shader_resources[stage];

         terakan_nir_get_binding_index_range(binding->descriptor_count, array_index, &needed_first,
                                             &needed_last);
         BITSET_SET_RANGE(state->resources_needed, resource_index_base + needed_first,
                          resource_index_base + needed_last);

         b->cursor = nir_before_instr(instr);
         /* TODO(Triang3l): Load from the constant cache, and better addressing such as making sure
          * the base doesn't go out of bounds.
          */
         nir_def_rewrite_uses(
            &intrin->def,
            nir_load_buffer_resource_r600(
               b, intrin->num_components, intrin->def.bit_size, array_index.ssa, intrin->src[1].ssa,
               .access = nir_intrinsic_access(intrin), .id_base = resource_index_base,
               .base = 16 * nir_intrinsic_base(intrin),
               .component = nir_intrinsic_component(intrin),
               .format = PIPE_FORMAT_R32G32B32A32_UINT, .mega_fetch_count_r600 = 16));
         nir_instr_remove(instr);
         return true;
      } break;

      default:
         break;
      }
   }

   return false;
}

void
terakan_nir_lower_bindings(nir_shader * const shader,
                           struct terakan_pipeline_layout const * const layout,
                           BITSET_WORD * const resources_needed)
{
   NIR_PASS_V(shader, nir_lower_ubo_vec4);

   /* TODO(Triang3l): Lower 64-bit buffer access. */

   /* Lower load_vulkan_descriptor and vulkan_resource_reindex chains to vulkan_resource_index. */
   nir_shader_lower_instructions(shader, terakan_nir_lower_load_vulkan_descriptor_filter,
                                 terakan_nir_lower_load_vulkan_descriptor_impl, NULL);
   nir_shader_instructions_pass(shader, terakan_nir_lower_vulkan_resource_reindex_instr,
                                nir_metadata_block_index | nir_metadata_dominance, NULL);

   /* Make data offsets in buffers relative to 0. */
   nir_shader_lower_instructions(shader, terakan_nir_zero_vulkan_resource_offset_filter,
                                 terakan_nir_zero_vulkan_resource_offset_impl, NULL);

   /* Make sure resource indices and data offsets are known to be constant if they are. */
   bool constant_folding_progress;
   do {
      constant_folding_progress = false;
      NIR_PASS(constant_folding_progress, shader, nir_copy_prop);
      NIR_PASS(constant_folding_progress, shader, nir_opt_constant_folding);
   } while (constant_folding_progress);

   /* Apply the pipeline layout and perform various lowerings based on it. */
   struct terakan_nir_lower_bindings_state state = {
      .layout = layout,
      .resources_needed = resources_needed,
   };
   nir_shader_instructions_pass(shader, terakan_nir_lower_bindings_instr, nir_metadata_none,
                                &state);
}
