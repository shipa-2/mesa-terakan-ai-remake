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
#include "terakan_physical_device.h"
#include "terakan_pipeline_layout.h"
#include "terakan_push_constants.h"

#include "gallium/drivers/r600/eg_sq.h"
#include "util/bitscan.h"
#include "util/list.h"
#include "util/macros.h"
#include "nir_builder.h"
#include "vk_device.h"
#include "vk_enum_to_str.h"
#include "vk_log.h"

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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
   return nir_iadd_nuw(b, previous_array_index, intrin->src[1].ssa);
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

static VkDescriptorType
terakan_nir_image_descriptor_type(enum glsl_sampler_dim const dim)
{
   return dim == GLSL_SAMPLER_DIM_BUF ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
                                      : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
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

   unsigned uav_base;
   /* TERAKAN_RESOURCE_RANGE_MUTABLE_BASE-based, NULL if the stage doesn't support UAVs. */
   BITSET_WORD * uavs_for_mutable_resources_needed;

   uint32_t * driver_push_constants_used;
};

/* Section 8.2 "Dataflow in Memory Hierarchy" of Evergreen Family Instruction Set Architecture says:
 *
 *     "Buffer objects are generally read and written directly by the work-items. Data is accessed
 *     through the L2 and L1 data caches on the GPU, but immediately invalidated at the end of a
 *     clause. [...] Similarly, writes are executed through the "fast-path" (depth buffer or DB) or
 *     "complete-path" (color buffer or CB), which have write-only caches that are invalidated, and
 *     all update bits are sent to memory at the end of a clause."
 *
 *     "Image objects are limited to read-only or write-only (no concurrent r/w). Thus, on reads,
 *     the data is cached through the L2 and L1 data caches; on writes, the data is cached through
 *     the CB/DB buffers."
 *
 * Therefore, for storage buffer and storage texel buffer loads, vertex fetch can always be used,
 * but for storage images, if write-read coherence is needed, the load must be performed via a
 * NOP_RTN UAV operation.
 */

static bool
terakan_nir_gather_uavs_needed_instr(nir_builder * const b, nir_instr * const instr,
                                     void * const cb_data)
{
   struct terakan_nir_lower_bindings_state * const state =
      (struct terakan_nir_lower_bindings_state *)cb_data;

   nir_src src = NIR_SRC_INIT;
   VkDescriptorType expected_type = VK_DESCRIPTOR_TYPE_MAX_ENUM;

   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr * const intrin = nir_instr_as_intrinsic(instr);
      switch (intrin->intrinsic) {
      case nir_intrinsic_store_ssbo:
         src = intrin->src[1];
         expected_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
         break;
      case nir_intrinsic_ssbo_atomic:
      case nir_intrinsic_ssbo_atomic_swap:
         src = intrin->src[0];
         expected_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
         break;
      case nir_intrinsic_image_deref_load:
         if (nir_intrinsic_image_dim(intrin) != GLSL_SAMPLER_DIM_BUF) {
            /* TODO(Triang3l): Detect more precisely whether the image load actually needs a UAV. */
            src = intrin->src[0];
            expected_type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
         }
         break;
      case nir_intrinsic_image_deref_store:
      case nir_intrinsic_image_deref_atomic:
      case nir_intrinsic_image_deref_atomic_swap:
         src = intrin->src[0];
         expected_type = terakan_nir_image_descriptor_type(nir_intrinsic_image_dim(intrin));
         break;
      default:
         break;
      }
   }

   if (src.ssa == NULL) {
      return false;
   }
   assert(expected_type != VK_DESCRIPTOR_TYPE_MAX_ENUM);
   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(src, expected_type, state->layout, b->shader, &binding))) {
      return false;
   }
   uint8_t mutable_resource_first =
      binding.set->first_shader_resources[b->shader->info.stage] +
      binding.set_binding->first_shader_resources[b->shader->info.stage] -
      TERAKAN_RESOURCE_RANGE_MUTABLE_BASE;
   /* The array index logic must be consistent with the assumptions in terakan_nir_get_binding_uav.
    */
   if (binding.array_index == NULL ||
       binding.array_index->parent_instr->type == nir_instr_type_load_const) {
      /* Constant index - mark only one resource as needing a UAV, and expect this constant index to
       * be added before retrieving the UAV index for the resource index.
       */
      if (binding.array_index != NULL) {
         mutable_resource_first +=
            nir_instr_as_load_const(binding.array_index->parent_instr)->value[0].u32;
      }
      BITSET_SET(state->uavs_for_mutable_resources_needed, mutable_resource_first);
   } else {
      /* Non-constant index - demand the whole array, starting from index 0 (disregarding
       * array_index_range_first even if at some point more precise estimation is added to avoid
       * offsetting the index at runtime).
       */
      BITSET_SET_RANGE(state->uavs_for_mutable_resources_needed, mutable_resource_first,
                       mutable_resource_first + binding.array_index_range_last);
   }

   return false;
}

/* Returns UINT_MAX if there are no UAVs exceeding the limit. */
static unsigned
terakan_nir_get_first_out_of_bounds_uav_mutable_resource(
   struct terakan_nir_lower_bindings_state const * const state,
   unsigned const mutable_resource_count)
{
   unsigned const max_uav_count = TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT - state->uav_base;
   unsigned uav_count = 0;
   unsigned mutable_resource_index;
   BITSET_FOREACH_SET (mutable_resource_index, state->uavs_for_mutable_resources_needed,
                       mutable_resource_count) {
      if (uav_count >= max_uav_count) {
         return mutable_resource_index;
      }
      ++uav_count;
   }
   return UINT_MAX;
}

static void
terakan_nir_gather_uavs_needed(nir_shader * const shader,
                               struct terakan_nir_lower_bindings_state * const state)
{
   if (state->uavs_for_mutable_resources_needed == NULL) {
      return;
   }

   /* TODO(Triang3l): Research detection of which storage image bindings can skip the UAV path for
    * reads, based on things like ACCESS_RESTRICT, ACCESS_COHERENT (or its Vulkan memory model
    * equivalents).
    */

   nir_shader_instructions_pass(shader, terakan_nir_gather_uavs_needed_instr, nir_metadata_none,
                                state);

   /* Disable UAVs that would be beyond the limit for safety. */
   unsigned const mutable_resource_count = shader->info.stage == MESA_SHADER_FRAGMENT
                                              ? TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL
                                              : TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL;
   unsigned const first_out_of_bounds_uav_mutable_resource =
      terakan_nir_get_first_out_of_bounds_uav_mutable_resource(state, mutable_resource_count);
   if (unlikely(first_out_of_bounds_uav_mutable_resource != UINT_MAX)) {
      BITSET_CLEAR_RANGE(state->uavs_for_mutable_resources_needed,
                         first_out_of_bounds_uav_mutable_resource, mutable_resource_count - 1);
   }
}

/* 0-based index (UAV base not applied).
 *
 * If immed_needed is true, also marks that the IMMED resources corresponding to the UAVs are
 * needed.
 *
 * If apply_array_index_out is false as a result, the array index has already been applied to the
 * UAV index, and the caller must use 0 instead of the array index from the binding when accessing
 * the UAV.
 * If apply_array_index_out is true as a result, the array index in the binding is also sure not to
 * be NULL.
 *
 * Returns UINT_MAX if not supported, not found, or exceeding the limit.
 */
static unsigned
terakan_nir_get_binding_uav(struct terakan_nir_binding const * const binding,
                            bool const immed_needed,
                            struct terakan_nir_lower_bindings_state const * const state,
                            gl_shader_stage const stage, bool * const apply_array_index_out)
{
   if (state->uavs_for_mutable_resources_needed == NULL) {
      /* UAVs are not supported at this stage. */
      return UINT_MAX;
   }

   uint8_t mutable_resource_first = binding->set->first_shader_resources[stage] +
                                    binding->set_binding->first_shader_resources[stage] -
                                    TERAKAN_RESOURCE_RANGE_MUTABLE_BASE;
   uint8_t mutable_resource_last;
   /* The array index logic must be consistent with the assumptions in
    * terakan_nir_gather_uavs_needed_instr.
    */
   bool apply_array_index;
   if (binding->array_index == NULL ||
       binding->array_index->parent_instr->type == nir_instr_type_load_const) {
      /* Constant index - don't require the entire array to be bound, pre-apply the array index. */
      if (binding->array_index != NULL) {
         mutable_resource_first +=
            nir_instr_as_load_const(binding->array_index->parent_instr)->value[0].u32;
      }
      mutable_resource_last = mutable_resource_first;
      apply_array_index = false;
   } else {
      /* Non-constant index - demand the whole array, starting from index 0 (disregarding
       * array_index_range_first even if at some point more precise estimation is added to avoid
       * offsetting the index at runtime).
       */
      mutable_resource_last = mutable_resource_first + binding->array_index_range_last;
      apply_array_index = true;
   }

   for (uint8_t mutable_resource_index = mutable_resource_first;
        mutable_resource_index <= mutable_resource_last; ++mutable_resource_index) {
      if (!BITSET_TEST(state->uavs_for_mutable_resources_needed, mutable_resource_index)) {
         return UINT_MAX;
      }
   }

   unsigned uav_index = 0;
   unsigned const first_uav_bit_word_index = BITSET_BITWORD(mutable_resource_first);
   for (unsigned word_index = 0; word_index < first_uav_bit_word_index; ++word_index) {
      uav_index += util_bitcount(state->uavs_for_mutable_resources_needed[word_index]);
   }
   uav_index += util_bitcount(state->uavs_for_mutable_resources_needed[first_uav_bit_word_index] &
                              (BITSET_BIT(mutable_resource_first) - 1));

   if (immed_needed) {
      /* IMMED resource indices don't have the color attachment count offset. */
      uint8_t const immed_resource_index_base =
         (stage == MESA_SHADER_FRAGMENT ? TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_PIXEL
                                        : TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_COMPUTE) +
         uav_index;
      if (apply_array_index) {
         BITSET_SET_RANGE(state->resources_needed, immed_resource_index_base,
                          immed_resource_index_base + binding->array_index_range_last);
      } else {
         BITSET_SET(state->resources_needed, immed_resource_index_base);
      }
   }

   *apply_array_index_out = apply_array_index;
   return uav_index;
}

/* Returns 0 in case of an error. */
static unsigned
terakan_nir_atomic_uav_op(nir_atomic_op const atomic_op, bool const result_used)
{
   unsigned uav_op;
   switch (atomic_op) {
   case nir_atomic_op_iadd:
      uav_op = V_RAT_INST_ADD;
      break;
   case nir_atomic_op_imin:
      uav_op = V_RAT_INST_MIN_INT;
      break;
   case nir_atomic_op_umin:
      uav_op = V_RAT_INST_MIN_UINT;
      break;
   case nir_atomic_op_imax:
      uav_op = V_RAT_INST_MAX_INT;
      break;
   case nir_atomic_op_umax:
      uav_op = V_RAT_INST_MAX_UINT;
      break;
   case nir_atomic_op_iand:
      uav_op = V_RAT_INST_AND;
      break;
   case nir_atomic_op_ior:
      uav_op = V_RAT_INST_OR;
      break;
   case nir_atomic_op_ixor:
      uav_op = V_RAT_INST_XOR;
      break;
   case nir_atomic_op_xchg:
      /* XCHG_RTN & 0x1F is STORE_RAW, but it's not available on R9xx. */
      return result_used ? V_RAT_INST_XCHG_RTN : V_RAT_INST_STORE_TYPED;
   case nir_atomic_op_cmpxchg:
      uav_op = V_RAT_INST_CMPXCHG_INT;
      break;
   case nir_atomic_op_inc_wrap:
      /* The source is the maximum value. */
      uav_op = V_RAT_INST_INC_UINT;
      break;
   case nir_atomic_op_dec_wrap:
      /* The source is the maximum value. */
      uav_op = V_RAT_INST_DEC_UINT;
      break;
   default:
      assert(!"Unsupported atomic operation");
      return 0;
   }
   if (result_used) {
      uav_op |= 0x20;
   }
   return uav_op;
}

static nir_def *
terakan_nir_uav_immed_index(nir_builder * const b,
                            struct terakan_physical_device_chip_info const * const chip_info)
{
   nir_def * wave_id = nir_load_hw_wave_id_r600(b);
   if (chip_info->two_shader_engines_max) {
      wave_id =
         nir_umad24_relaxed(b, nir_imm_int(b, 2), wave_id, nir_load_shader_engine_id_r600(b));
   }
   /* TODO(Triang3l): See how MBCNT behaves on wave32 chips and possibly scale the wave ID by 32
    * there.
    */
   return nir_umad24_relaxed(b, nir_imm_int(b, 64), wave_id,
                             nir_mbcnt_amd(b, nir_imm_int(b, ~0), nir_imm_zero(b, 1, 32)));
}

static nir_def *
terakan_nir_image_uav_coord(nir_builder * const b, nir_def * const image_coord,
                            enum glsl_sampler_dim const dim, bool const is_array)
{
   /* Buffers need separate handling due to the UAV base granularity offset. */
   assert(dim != GLSL_SAMPLER_DIM_BUF);

   unsigned uav_coord_num_components = 2;
   nir_def * uav_coord_components[3] = {
      nir_channel(b, image_coord, 0),
      /* 1D images may be promoted to 2D if they're tiled (such as if they're used by DB), so always
       * specify Y = 0 for them.
       */
      dim == GLSL_SAMPLER_DIM_1D ? nir_imm_zero(b, 1, 32) : nir_channel(b, image_coord, 1),
   };

   /* The hardware accepts the array layer in Z for both 1D and 2D/3D. It's relevant only if
    * RESOURCE_TYPE is TEXTURE#DARRAY or TEXTURE3D. UAV instructions don't accept a coordinate
    * swizzle, so don't initialize the array layer if it's not needed to avoid emitting an ALU
    * instruction for it.
    */
   if (dim == GLSL_SAMPLER_DIM_3D || is_array) {
      uav_coord_num_components = 3;
      uav_coord_components[2] = nir_channel(b, image_coord, dim == GLSL_SAMPLER_DIM_1D ? 1 : 2);
   }

   return nir_vec(b, uav_coord_components, uav_coord_num_components);
}

static nir_def *
terakan_nir_buffer_uav_coord(nir_builder * const b, nir_def * coord,
                             uint32_t const uav_index_zero_based, nir_def * const uav_array_index,
                             bool const robust_access, bool const include_helpers,
                             struct terakan_nir_lower_bindings_state * const state)
{
   /* Add the UAV base granularity offset. */

   if (robust_access) {
      /* If the coordinate provided by the application is already near UINT32_MAX, adding the UAV
       * base granularity offset may result in wrapping and turn an out-of-bounds address into a
       * in-bounds address near zero.
       * Clamp the coordinate so that no possible base granularity offset value will result in
       * wrapping.
       * For storage buffers, the offset is provided in bytes, but the UAV uses a format with 4
       * bytes per element, for which terakan_color_descriptor_buffer_uav_base_granularity_log2 is
       * the pipe interleave.
       * For texel buffers, the offset is in elements, but for all possible element sizes,
       * terakan_color_descriptor_buffer_uav_base_granularity_log2 divided by the element size never
       * exceeds the pipe interleave. Moreover, texel buffers are fetched at a signed coordinate,
       * not unsigned, so any address > INT32_MAX is out-of-bounds.
       */
      struct terakan_physical_device const * const physical_device = container_of(
         state->layout->vk.base.device->physical, struct terakan_physical_device const, vk);
      uint32_t const max_uav_range =
         ~(((uint32_t)1 << physical_device->tiling_info.pipe_interleave_bytes_log2) - 1);
      assert(physical_device->vk.properties.maxStorageBufferRange <= max_uav_range);
      coord = nir_umin_imm(b, coord, max_uav_range);
   }

   *state->driver_push_constants_used |=
      BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_BUFFER_UAV_BASE_GRANULARITY_OFFSET);
   /* TODO(Triang3l): If the array index is constant, load via kcache rather than vertex fetch. */
   BITSET_SET(state->resources_needed, TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS);
   nir_def * const base_granularity_offset = nir_load_buffer_resource_r600(
      b, 1, 32, nir_imm_zero(b, 1, 32), nir_ishl_imm(b, uav_array_index, 2),
      .access = ACCESS_CAN_REORDER | (include_helpers ? ACCESS_INCLUDE_HELPERS : 0),
      .id_base = TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS,
      .base = offsetof(struct terakan_push_constants_driver, buffer_uav_base_granularity_offset) +
              sizeof(uint32_t) * uav_index_zero_based,
      .format = PIPE_FORMAT_R32_UINT);
   return nir_iadd_nuw(b, coord, base_granularity_offset);
}

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

static void
terakan_nir_lower_bindings_instr_store_ssbo(nir_builder * const b,
                                            nir_intrinsic_instr * const intrin,
                                            struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_store_ssbo);

   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(intrin->src[1], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         state->layout, b->shader, &binding))) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   b->cursor = nir_before_instr(&intrin->instr);

   bool apply_uav_array_index;
   unsigned const uav_index_zero_based = terakan_nir_get_binding_uav(
      &binding, false, state, b->shader->info.stage, &apply_uav_array_index);
   if (unlikely(uav_index_zero_based == UINT_MAX)) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   unsigned const bytes_per_component = nir_src_bit_size(intrin->src[0]) / 8;
   unsigned uav_op;
   if (bytes_per_component == 4) {
      uav_op = V_RAT_INST_STORE_TYPED;
   } else {
      assert(!"Unsupported storage buffer component size");
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   nir_def * const uav_array_index =
      apply_uav_array_index ? binding.array_index : nir_imm_zero(b, 1, 32);

   enum gl_access_qualifier const access = nir_intrinsic_access(intrin);

   /* TODO(Triang3l): VK_EXT_pipeline_robustness. */
   nir_def * coord = terakan_nir_buffer_uav_coord(
      b, intrin->src[2].ssa, uav_index_zero_based, uav_array_index,
      state->layout->vk.base.device->enabled_features.robustBufferAccess,
      (access & ACCESS_INCLUDE_HELPERS) != 0, state);
   if (bytes_per_component > 1) {
      coord = nir_udiv_imm(b, coord, bytes_per_component);
   }

   /* No point in vectorizing, the hardware instruction stores only one channel. */
   assert(nir_intrinsic_write_mask(intrin) == 0b1);
   nir_uav_instr_r600(b, uav_array_index, coord,
                      nir_u2u32(b, nir_channel(b, intrin->src[0].ssa, 0)), nir_undef(b, 1, 32),
                      .uav_op_r600 = uav_op, .access = access,
                      .id_base = state->uav_base + uav_index_zero_based);
   nir_instr_remove(&intrin->instr);
}

static void
terakan_nir_lower_bindings_instr_ssbo_atomic(nir_builder * const b,
                                             nir_intrinsic_instr * const intrin,
                                             struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_ssbo_atomic ||
          intrin->intrinsic == nir_intrinsic_ssbo_atomic_swap);

   if (unlikely(intrin->def.bit_size != 32)) {
      assert(!"Only 32-bit storage buffer atomic operations are supported");
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(intrin->src[0], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         state->layout, b->shader, &binding))) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   b->cursor = nir_before_instr(&intrin->instr);

   bool const result_used = !list_is_empty(&intrin->def.uses);

   bool apply_uav_array_index;
   unsigned const uav_index_zero_based = terakan_nir_get_binding_uav(
      &binding, result_used, state, b->shader->info.stage, &apply_uav_array_index);
   if (unlikely(uav_index_zero_based == UINT_MAX)) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }
   nir_def * const uav_array_index =
      apply_uav_array_index ? binding.array_index : nir_imm_zero(b, 1, 32);

   unsigned const uav_op = terakan_nir_atomic_uav_op(nir_intrinsic_atomic_op(intrin), result_used);
   if (unlikely(uav_op == 0)) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   enum gl_access_qualifier const access = nir_intrinsic_access(intrin) & ~ACCESS_CAN_REORDER;

   /* TODO(Triang3l): VK_EXT_pipeline_robustness. */
   nir_def * coord =
      nir_udiv_imm(b,
                   terakan_nir_buffer_uav_coord(
                      b, intrin->src[1].ssa, uav_index_zero_based, uav_array_index,
                      state->layout->vk.base.device->enabled_features.robustBufferAccess,
                      (access & ACCESS_INCLUDE_HELPERS) != 0, state),
                   4);

   /* For INC/DEC, the hardware instruction accepts the maximum possible value. */
   unsigned const uav_op_non_rtn = uav_op & 0x1F;
   nir_def * const value =
      uav_op_non_rtn == V_RAT_INST_INC_UINT || uav_op_non_rtn == V_RAT_INST_DEC_UINT
         ? nir_imm_int(b, (int)UINT32_MAX)
         : intrin->src[2].ssa;

   nir_def * const compare_value = intrin->intrinsic == nir_intrinsic_ssbo_atomic_swap
                                      ? intrin->src[3].ssa
                                      : nir_undef(b, 1, 32);

   unsigned const uav_id_base = state->uav_base + uav_index_zero_based;

   if (result_used) {
      /* TODO(Triang3l): Proper bit size conversion depending on the destination type? */
      nir_def_rewrite_uses(
         &intrin->def,
         nir_u2uN(
            b,
            nir_uav_returning_instr_r600(
               b, intrin->def.num_components, 32, uav_array_index, coord, value, compare_value,
               terakan_nir_uav_immed_index(b, &container_of(state->layout->vk.base.device->physical,
                                                            struct terakan_physical_device const, vk)
                                                  ->chip_info),
               .uav_op_r600 = uav_op, .access = access, .id_base = uav_id_base,
               .uav_return_id_base_r600 = (b->shader->info.stage == MESA_SHADER_FRAGMENT
                                              ? TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_PIXEL
                                              : TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_COMPUTE) +
                                          uav_index_zero_based),
            intrin->def.bit_size));
   } else {
      nir_uav_instr_r600(b, uav_array_index, coord, value, compare_value, .uav_op_r600 = uav_op,
                         .access = access, .id_base = uav_id_base);
      nir_def_rewrite_uses(&intrin->def,
                           nir_undef(b, intrin->def.num_components, intrin->def.bit_size));
   }
   nir_instr_remove(&intrin->instr);
}

static void
terakan_nir_lower_bindings_instr_image_deref_load(
   nir_builder * const b, nir_intrinsic_instr * const intrin,
   struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_image_deref_load);

   enum glsl_sampler_dim const image_dim = nir_intrinsic_image_dim(intrin);

   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(intrin->src[0],
                                         terakan_nir_image_descriptor_type(image_dim),
                                         state->layout, b->shader, &binding))) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   b->cursor = nir_before_instr(&intrin->instr);

   gl_shader_stage const stage = b->shader->info.stage;

   enum gl_access_qualifier access = nir_intrinsic_access(intrin);
   if (state->uavs_for_mutable_resources_needed != NULL) {
      /* Need write-read coherence within an invocation. */
      /* TODO(Triang3l): Detect whether write-read coherence is needed more precisely. */
      access &= ~ACCESS_CAN_REORDER;
   } else {
      access |= ACCESS_CAN_REORDER;
   }

   uint8_t const resource_index_base = binding.set->first_shader_resources[stage] +
                                       binding.set_binding->first_shader_resources[stage];

   if (image_dim == GLSL_SAMPLER_DIM_BUF) {
      /* Vertex fetches are coherent with UAVs, do a vertex fetch unconditionally. */
      BITSET_SET_RANGE(state->resources_needed,
                       resource_index_base + binding.array_index_range_first,
                       resource_index_base + binding.array_index_range_last);
      nir_def_rewrite_uses(
         &intrin->def,
         nir_u2uN(b,
                  nir_load_buffer_resource_r600(
                     b, intrin->def.num_components, 32,
                     binding.array_index != NULL ? binding.array_index : nir_imm_zero(b, 1, 32),
                     nir_channel(b, intrin->src[1].ssa, 0), .access = access,
                     .id_base = resource_index_base),
                  intrin->def.bit_size));
      nir_instr_remove(&intrin->instr);
      return;
   }

   /* Texture fetches are not coherent with UAVs, if write-read coherence is needed, load using a
    * NOP_RTN UAV operation.
    */

   bool const image_is_array = nir_intrinsic_image_array(intrin);

   bool apply_uav_array_index;
   unsigned const uav_index_zero_based =
      terakan_nir_get_binding_uav(&binding, true, state, stage, &apply_uav_array_index);

   if (uav_index_zero_based == UINT_MAX) {
      /* UAV not needed or not available at this stage, load via the texture cache. */
      BITSET_SET_RANGE(state->resources_needed,
                       resource_index_base + binding.array_index_range_first,
                       resource_index_base + binding.array_index_range_last);
      nir_def_rewrite_uses(
         &intrin->def,
         nir_u2uN(
            b,
            nir_load_texture_resource_r600(
               b, intrin->def.num_components, 32,
               binding.array_index != NULL ? binding.array_index : nir_imm_zero(b, 1, 32),
               nir_vec4(
                  b, nir_channel(b, intrin->src[1].ssa, 0),
                  image_dim != GLSL_SAMPLER_DIM_1D ? nir_channel(b, intrin->src[1].ssa, 1)
                                                   : nir_imm_zero(b, 1, 32),
                  image_dim == GLSL_SAMPLER_DIM_3D || image_is_array
                     ? nir_channel(b, intrin->src[1].ssa, image_dim == GLSL_SAMPLER_DIM_1D ? 1 : 2)
                     : nir_imm_zero(b, 1, 32),
                  nir_imm_zero(b, 1, 32)),
               .access = access, .id_base = resource_index_base),
            intrin->def.bit_size));
      nir_instr_remove(&intrin->instr);
      return;
   }

   nir_def * const uav_array_index =
      apply_uav_array_index ? binding.array_index : nir_imm_zero(b, 1, 32);
   nir_def * const immed_index =
      terakan_nir_uav_immed_index(b, &container_of(state->layout->vk.base.device->physical,
                                                   struct terakan_physical_device const, vk)
                                         ->chip_info);
   nir_def * const undef = nir_undef(b, 1, 32);
   /* TODO(Triang3l): Proper bit size conversion depending on the destination type? */
   nir_def_rewrite_uses(
      &intrin->def,
      nir_u2uN(
         b,
         nir_uav_returning_instr_r600(
            b, intrin->def.num_components, 32, uav_array_index,
            terakan_nir_image_uav_coord(b, intrin->src[1].ssa, image_dim, image_is_array), undef,
            undef, immed_index, .uav_op_r600 = V_RAT_INST_NOP_RTN, .access = access,
            .id_base = state->uav_base + uav_index_zero_based,
            .uav_return_id_base_r600 = (b->shader->info.stage == MESA_SHADER_FRAGMENT
                                           ? TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_PIXEL
                                           : TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_COMPUTE) +
                                       uav_index_zero_based),
         intrin->def.bit_size));
   nir_instr_remove(&intrin->instr);
}

static void
terakan_nir_lower_bindings_instr_image_deref_store(
   nir_builder * const b, nir_intrinsic_instr * const intrin,
   struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_image_deref_store);

   enum glsl_sampler_dim const image_dim = nir_intrinsic_image_dim(intrin);

   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(intrin->src[0],
                                         terakan_nir_image_descriptor_type(image_dim),
                                         state->layout, b->shader, &binding))) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   b->cursor = nir_before_instr(&intrin->instr);

   bool apply_uav_array_index;
   unsigned const uav_index_zero_based = terakan_nir_get_binding_uav(
      &binding, false, state, b->shader->info.stage, &apply_uav_array_index);
   if (unlikely(uav_index_zero_based == UINT_MAX)) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }
   nir_def * const uav_array_index =
      apply_uav_array_index ? binding.array_index : nir_imm_zero(b, 1, 32);

   enum gl_access_qualifier const access = nir_intrinsic_access(intrin);

   nir_def * coord;
   if (image_dim == GLSL_SAMPLER_DIM_BUF) {
      coord = terakan_nir_buffer_uav_coord(b, nir_channel(b, intrin->src[1].ssa, 0),
                                           uav_index_zero_based, uav_array_index, true,
                                           (access & ACCESS_INCLUDE_HELPERS) != 0, state);
   } else {
      coord = terakan_nir_image_uav_coord(b, intrin->src[1].ssa, image_dim,
                                          nir_intrinsic_image_array(intrin));
   }

   nir_def * const undef = nir_undef(b, 1, 32);

   /* TODO(Triang3l): Proper bit size conversion depending on the source type? */
   nir_uav_instr_r600(b, uav_array_index, coord, nir_u2u32(b, intrin->src[3].ssa), undef,
                      .uav_op_r600 = V_RAT_INST_STORE_TYPED, .access = access,
                      .id_base = state->uav_base + uav_index_zero_based);
   nir_instr_remove(&intrin->instr);
}

static void
terakan_nir_lower_bindings_instr_image_deref_atomic(
   nir_builder * const b, nir_intrinsic_instr * const intrin,
   struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_image_deref_atomic ||
          intrin->intrinsic == nir_intrinsic_image_deref_atomic_swap);

   if (unlikely(intrin->def.bit_size != 32)) {
      assert(!"Only 32-bit storage image and storage texel buffer atomic operations are supported");
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   enum glsl_sampler_dim const image_dim = nir_intrinsic_image_dim(intrin);

   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(intrin->src[0],
                                         terakan_nir_image_descriptor_type(image_dim),
                                         state->layout, b->shader, &binding))) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   b->cursor = nir_before_instr(&intrin->instr);

   bool const result_used = !list_is_empty(&intrin->def.uses);

   bool apply_uav_array_index;
   unsigned const uav_index_zero_based = terakan_nir_get_binding_uav(
      &binding, result_used, state, b->shader->info.stage, &apply_uav_array_index);
   if (unlikely(uav_index_zero_based == UINT_MAX)) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }
   nir_def * const uav_array_index =
      apply_uav_array_index ? binding.array_index : nir_imm_zero(b, 1, 32);

   unsigned const uav_op = terakan_nir_atomic_uav_op(nir_intrinsic_atomic_op(intrin), result_used);
   if (unlikely(uav_op == 0)) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   enum gl_access_qualifier const access = nir_intrinsic_access(intrin) & ~ACCESS_CAN_REORDER;

   nir_def * coord;
   if (image_dim == GLSL_SAMPLER_DIM_BUF) {
      coord = terakan_nir_buffer_uav_coord(b, nir_channel(b, intrin->src[1].ssa, 0),
                                           uav_index_zero_based, uav_array_index, true,
                                           (access & ACCESS_INCLUDE_HELPERS) != 0, state);
   } else {
      coord = terakan_nir_image_uav_coord(b, intrin->src[1].ssa, image_dim,
                                          nir_intrinsic_image_array(intrin));
   }

   /* For INC/DEC, the hardware instruction accepts the maximum possible value. */
   unsigned const uav_op_non_rtn = uav_op & 0x1F;
   nir_def * const value =
      uav_op_non_rtn == V_RAT_INST_INC_UINT || uav_op_non_rtn == V_RAT_INST_DEC_UINT
         ? nir_imm_int(b, (int)UINT32_MAX)
         : intrin->src[3].ssa;

   nir_def * const compare_value = intrin->intrinsic == nir_intrinsic_image_deref_atomic_swap
                                      ? intrin->src[4].ssa
                                      : nir_undef(b, 1, 32);

   unsigned const uav_id_base = state->uav_base + uav_index_zero_based;

   if (result_used) {
      /* TODO(Triang3l): Proper bit size conversion depending on the destination type? */
      nir_def_rewrite_uses(
         &intrin->def,
         nir_u2uN(
            b,
            nir_uav_returning_instr_r600(
               b, intrin->def.num_components, 32, uav_array_index, coord, value, compare_value,
               terakan_nir_uav_immed_index(b, &container_of(state->layout->vk.base.device->physical,
                                                            struct terakan_physical_device const, vk)
                                                  ->chip_info),
               .uav_op_r600 = uav_op, .access = access, .id_base = uav_id_base,
               .uav_return_id_base_r600 = (b->shader->info.stage == MESA_SHADER_FRAGMENT
                                              ? TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_PIXEL
                                              : TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_COMPUTE) +
                                          uav_index_zero_based),
            intrin->def.bit_size));
   } else {
      nir_uav_instr_r600(b, uav_array_index, coord, value, compare_value, .uav_op_r600 = uav_op,
                         .access = access, .id_base = uav_id_base);
      nir_def_rewrite_uses(&intrin->def,
                           nir_undef(b, intrin->def.num_components, intrin->def.bit_size));
   }
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
      case nir_intrinsic_store_ssbo:
         terakan_nir_lower_bindings_instr_store_ssbo(b, intrin, state);
         return true;
      case nir_intrinsic_ssbo_atomic:
      case nir_intrinsic_ssbo_atomic_swap:
         terakan_nir_lower_bindings_instr_ssbo_atomic(b, intrin, state);
         return true;
      case nir_intrinsic_image_deref_load:
         terakan_nir_lower_bindings_instr_image_deref_load(b, intrin, state);
         return true;
      case nir_intrinsic_image_deref_store:
         terakan_nir_lower_bindings_instr_image_deref_store(b, intrin, state);
         return true;
      case nir_intrinsic_image_deref_atomic:
      case nir_intrinsic_image_deref_atomic_swap:
         terakan_nir_lower_bindings_instr_image_deref_atomic(b, intrin, state);
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
                           uint32_t * const samplers_needed_accum, unsigned const uav_base,
                           BITSET_WORD * const uavs_for_mutable_resources_needed_out_opt,
                           uint32_t * const driver_push_constants_used_accum)
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

   assert(uav_base <= TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT);

   struct terakan_nir_lower_bindings_state state = {
      .layout = layout,
      .resources_needed = resources_needed_accum,
      .samplers_needed = samplers_needed_accum,
      .uav_base = uav_base,
      .driver_push_constants_used = driver_push_constants_used_accum,
   };

   if (uavs_for_mutable_resources_needed_out_opt != NULL) {
      if (shader->info.stage == MESA_SHADER_FRAGMENT) {
         memset(uavs_for_mutable_resources_needed_out_opt, 0,
                sizeof(BITSET_WORD) * BITSET_WORDS(TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL));
         if (layout->vk.base.device->enabled_features.fragmentStoresAndAtomics) {
            state.uavs_for_mutable_resources_needed = uavs_for_mutable_resources_needed_out_opt;
         }
      } else {
         memset(
            uavs_for_mutable_resources_needed_out_opt, 0,
            sizeof(BITSET_WORD) * BITSET_WORDS(TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL));
         if (shader->info.stage == MESA_SHADER_COMPUTE) {
            state.uavs_for_mutable_resources_needed = uavs_for_mutable_resources_needed_out_opt;
         }
      }
   }

   terakan_nir_gather_uavs_needed(shader, &state);

   progress |= nir_shader_instructions_pass(shader, terakan_nir_lower_bindings_instr,
                                            nir_metadata_none, &state);

   nir_shader_preserve_all_metadata(shader);
   return progress;
}
