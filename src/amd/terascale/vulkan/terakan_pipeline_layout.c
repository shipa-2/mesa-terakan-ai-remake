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

#include "terakan_pipeline_layout.h"

#include "terakan_command_buffer.h"
#include "terakan_descriptor.h"
#include "terakan_descriptor_set.h"
#include "terakan_descriptor_set_layout.h"
#include "terakan_device.h"
#include "terakan_entrypoints.h"

#include "amd/terascale/common/terascale_format.h"
#include "compiler/shader_enums.h"
#include "gallium/drivers/r600/evergreend.h"
#include "util/bitscan.h"
#include "util/bitset.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "vk_alloc.h"
#include "vk_log.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

VKAPI_ATTR void VKAPI_CALL
terakan_CmdBindDescriptorSets(VkCommandBuffer const commandBuffer,
                              VkPipelineBindPoint const pipelineBindPoint,
                              VkPipelineLayout const layoutHandle, uint32_t const firstSet,
                              uint32_t const descriptorSetCount,
                              VkDescriptorSet const * const pDescriptorSets,
                              UNUSED uint32_t const dynamicOffsetCount,
                              uint32_t const * const pDynamicOffsets)
{
   struct terakan_gfx_command_writer * const command_writer =
      terakan_command_buffer_from_handle(commandBuffer)->command_writer.gfx;

   struct terakan_pipeline_layout const * const layout =
      terakan_pipeline_layout_from_handle(layoutHandle);

   bool const is_compute = pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE;
   gl_shader_stage const shader_stage_first = is_compute ? MESA_SHADER_COMPUTE : MESA_SHADER_VERTEX;
   gl_shader_stage const shader_stage_last =
      is_compute ? MESA_SHADER_COMPUTE : MESA_SHADER_FRAGMENT;

   gl_shader_stage const uav_shader_stage = is_compute ? MESA_SHADER_COMPUTE : MESA_SHADER_FRAGMENT;
   unsigned const uav_bind_point = is_compute ? TERAKAN_APP_CONFIG_DRAW_UAV_BIND_POINT_COMPUTE
                                              : TERAKAN_APP_CONFIG_DRAW_UAV_BIND_POINT_GRAPHICS;

   uint32_t const * set_dynamic_offsets = pDynamicOffsets;

   for (uint32_t set_relative_index = 0; set_relative_index < descriptorSetCount;
        ++set_relative_index) {
      struct terakan_pipeline_layout_set const * const layout_set =
         &layout->sets[firstSet + set_relative_index];

      struct terakan_descriptor_set const * const set =
         terakan_descriptor_set_from_handle(pDescriptorSets[set_relative_index]);
      if (set == NULL) {
         continue;
      }
      struct terakan_descriptor_set_layout const * const set_layout = set->layout;

      struct terakan_descriptor_set_resource const * const set_resources =
         (struct terakan_descriptor_set_resource const *)set->descriptors;
      struct terakan_descriptor_set_sampler const * const set_samplers =
         (struct terakan_descriptor_set_sampler const *)(set->descriptors +
                                                         set_layout
                                                            ->pool_first_sampler_offset_bytes);

      for (gl_shader_stage shader_stage = shader_stage_first; shader_stage <= shader_stage_last;
           ++shader_stage) {
         struct terakan_descriptor_set_layout_shader const * const set_layout_shader =
            &set_layout->shaders[shader_stage];
         struct terakan_hw_config_sqk_set_functions const * const sqk_set_functions =
            &terakan_hw_config_sqk_stage_set_functions[shader_stage];

         /* Resources. */

         uint8_t const shader_resource_set_base = layout_set->first_shader_resources[shader_stage];
         struct terakan_descriptor_set_layout_shader_range const * const resource_ranges =
            set_layout->shader_ranges + set_layout_shader->first_resource_range;
         for (uint8_t range_index = 0; range_index < set_layout_shader->resource_range_count;
              ++range_index) {
            struct terakan_descriptor_set_layout_shader_range const * const range =
               &resource_ranges[range_index];
            struct terakan_descriptor_set_resource const * const range_set_resources =
               set_resources + range->first_set_descriptor;
            uint8_t const range_shader_base =
               shader_resource_set_base + range->first_shader_descriptor;
            if (range->first_dynamic_offset != UINT16_MAX) {
                uint32_t const * const range_dynamic_offsets =
                   set_dynamic_offsets + range->first_dynamic_offset;
                for (uint8_t resource_index = 0; resource_index < range->descriptor_count;
                     ++resource_index) {
                   struct terakan_descriptor_set_resource const * const set_resource =
                      &range_set_resources[resource_index];
                   struct terakan_resource_descriptor resource = set_resource->resource;
                   /* Because descriptors for bindings not statically referenced by the pipeline can
                    * be undefined, the BO pointer must not be dereferenced here as it may be
                    * outdated.
                    */
                   /* #MemoryIntegrity: the dynamic offset shifts the base address without the
                    * static `range` this descriptor was written with accounting for it, so
                    * `resource.resource[1]` (SIZE - 1) must be reclamped against the real
                    * remaining extent of the bound VkBuffer from its static offset. Without this,
                    * a dynamic offset near the end of the buffer would keep the original SIZE and
                    * let the shader read past the buffer's allocation.
                    */
                   if (set_resource->bo != NULL) {
                      assert(G_03001C_TYPE(resource.resource[7]) ==
                             V_03001C_SQ_TEX_VTX_VALID_BUFFER);
                      uint32_t const dynamic_offset = range_dynamic_offsets[resource_index];
                      uint64_t const resource_address =
                         (resource.resource[0] |
                          ((uint64_t)G_030008_BASE_ADDRESS_HI(resource.resource[2]) << 32)) +
                         dynamic_offset;
                      resource.resource[0] = (uint32_t)resource_address;
                      resource.resource[2] = (resource.resource[2] & C_030008_BASE_ADDRESS_HI) |
                                             S_030008_BASE_ADDRESS_HI(resource_address >> 32);
                      uint64_t const existing_size_bytes = (uint64_t)resource.resource[1] + 1;
                      uint64_t const remaining_after_offset =
                         dynamic_offset < set_resource->dynamic_offset_remaining_bytes
                            ? set_resource->dynamic_offset_remaining_bytes - dynamic_offset
                            : 0;
                      uint64_t const clamped_size_bytes =
                         MIN2(existing_size_bytes, remaining_after_offset);
                      /* SIZE is encoded as byte count minus one, so a fully out-of-bounds dynamic
                       * offset is floored to a 1-byte window rather than 0 to avoid representing it
                       * as UINT32_MAX via unsigned underflow.
                       */
                      resource.resource[1] =
                         clamped_size_bytes != 0 ? (uint32_t)(clamped_size_bytes - 1) : 0;
                   }
                   sqk_set_functions->resource(&command_writer->hw_config_sqk,
                                               range_shader_base + resource_index, set_resource->bo,
                                               &resource);
               }
            } else {
                for (uint8_t resource_index = 0; resource_index < range->descriptor_count;
                     ++resource_index) {
                   struct terakan_descriptor_set_resource const * const resource =
                      &range_set_resources[resource_index];
                   sqk_set_functions->resource(&command_writer->hw_config_sqk,
                                               range_shader_base + resource_index, resource->bo,
                                               &resource->resource);
               }
            }
         }

         /* Samplers. */

         uint8_t const shader_sampler_set_base = layout_set->first_shader_samplers[shader_stage];
         struct terakan_descriptor_set_layout_shader_range const * const sampler_ranges =
            set_layout->shader_ranges + set_layout_shader->first_sampler_range;
         for (uint8_t range_index = 0; range_index < set_layout_shader->sampler_range_count;
              ++range_index) {
            struct terakan_descriptor_set_layout_shader_range const * const range =
               &sampler_ranges[range_index];
            struct terakan_descriptor_set_sampler const * const range_set_samplers =
               set_samplers + range->first_set_descriptor;
            uint8_t const range_shader_base =
               shader_sampler_set_base + range->first_shader_descriptor;
            for (uint8_t sampler_index = 0; sampler_index < range->descriptor_count;
                 ++sampler_index) {
               struct terakan_descriptor_set_sampler const * const sampler =
                  &range_set_samplers[sampler_index];
               /* Skip uninitialized (zeroed in descriptor set allocation) samplers, as descriptors
                * may be left uninitialized if they're not statically referenced by the pipeline.
                */
               if (likely(G_03C008_TYPE(sampler->sampler.sampler[2]))) {
                  uint8_t const shader_sampler = range_shader_base + sampler_index;
                  sqk_set_functions->sampler(&command_writer->hw_config_sqk, shader_sampler,
                                             &sampler->sampler);
                  /* R8xx samplers have no `FORCE_UNNORMALIZED`, so the shader divides the
                   * coordinates by the texture size instead, and needs to be told which slots that
                   * applies to. Immutable samplers are decided at compilation and are not in the
                   * mask, but they are also written through this loop, so the bit is maintained for
                   * every slot regardless -- a shader simply does not read the ones it resolved
                   * statically.
                   */
                  assert(shader_sampler < 32);
                  uint32_t * const unnormalized_mask =
                     &command_writer->push_constants_state.driver_constants
                         .sampler_unnormalized[shader_stage];
                  uint32_t const unnormalized_bit = BITFIELD_BIT(shader_sampler);
                  uint32_t const unnormalized_mask_new =
                     sampler->unnormalized_coordinates ? (*unnormalized_mask | unnormalized_bit)
                                                       : (*unnormalized_mask & ~unnormalized_bit);
                  if (unnormalized_mask_new != *unnormalized_mask) {
                     *unnormalized_mask = unnormalized_mask_new;
                     command_writer->push_constants_state.driver_constants_modified |=
                        BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_SAMPLER_UNNORMALIZED);
                  }
               }
            }
         }
      }

      /* Unordered access views. */

      struct terakan_descriptor_set_layout_shader const * const set_layout_uav_shader =
         &set_layout->shaders[uav_shader_stage];
      if (set_layout_uav_shader->uav_range_count != 0) {
         struct terakan_descriptor_set_uav const * const set_uavs =
            (struct terakan_descriptor_set_uav const *)(set->descriptors +
                                                        set_layout->pool_first_uav_offset_bytes);
         assert(layout_set->first_shader_resources[uav_shader_stage] >=
                TERAKAN_RESOURCE_RANGE_MUTABLE_BASE);
         uint8_t const shader_uav_set_base = layout_set->first_shader_resources[uav_shader_stage] -
                                             TERAKAN_RESOURCE_RANGE_MUTABLE_BASE;
         struct terakan_descriptor_set_layout_shader_range const * const uav_ranges =
            set_layout->shader_ranges + set_layout_uav_shader->first_uav_range;
         for (uint8_t range_index = 0; range_index < set_layout_uav_shader->uav_range_count;
              ++range_index) {
            struct terakan_descriptor_set_layout_shader_range const * const range =
               &uav_ranges[range_index];
            struct terakan_descriptor_set_uav const * const range_set_uavs =
               set_uavs + range->first_set_descriptor;
            uint8_t const range_shader_base = shader_uav_set_base + range->first_shader_descriptor;
            for (uint8_t uav_index = 0; uav_index < range->descriptor_count; ++uav_index) {
               uint8_t const shader_uav_index = range_shader_base + uav_index;
               assert(shader_uav_index < (is_compute
                                             ? TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL
                                             : TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL));
               struct terakan_descriptor_set_uav const * const set_uav = &range_set_uavs[uav_index];
               /* Because descriptors for bindings not statically referenced by the pipeline can be
                * undefined, the BO pointer must not be dereferenced here as it may be outdated.
                */
               if (set_uav->bo != NULL) {
                  struct terakan_color_descriptor const * new_uav_color = &set_uav->color;
                  struct terakan_color_descriptor new_uav_color_with_dynamic_offset;
                  if (range->first_dynamic_offset != UINT16_MAX) {
                     assert(G_028C70_RESOURCE_TYPE(set_uav->color.info) == V_028C70_BUFFER);
                     unsigned const uav_bytes_per_element =
                        terascale_format_bytes_per_block[G_028C70_FORMAT(set_uav->color.info)];
                     /* A buffer UAV states its extent as BASE, VIEW and DIM together: BASE is the
                      * descriptor's own alignment granularity floor, VIEW the byte distance from
                      * there to the buffer range's start, and DIM the inclusive last element
                      * counted from BASE, not from VIEW. A dynamic offset moves the range, so all
                      * three have to be rebuilt -- moving BASE alone, as this used to do, left DIM
                      * describing the old, further end and handed the shader the dynamic offset's
                      * worth of memory past the range. Recover the element count from the
                      * descriptor, then rebuild the three fields with the same helper that wrote
                      * them.
                      */
                     uint64_t const uav_va =
                        ((uint64_t)set_uav->color.base << 8) + set_uav->color.view;
                     uint64_t const uav_elements = (uint64_t)set_uav->color.dim + 1 -
                                                   set_uav->color.view / uav_bytes_per_element;
                     /* #MemoryIntegrity: hold the window inside the bound VkBuffer whatever the
                      * dynamic offset is. Keeping one element's room means the rebuilt descriptor
                      * always has a valid nonzero extent without ever reaching past the end.
                      */
                     uint32_t const remaining_bytes = set_uav->dynamic_offset_remaining_bytes;
                     uint32_t const max_dynamic_offset =
                        remaining_bytes >= uav_bytes_per_element
                           ? remaining_bytes - uav_bytes_per_element
                           : 0;
                     uint32_t const dynamic_offset =
                        MIN2(set_dynamic_offsets[range->first_dynamic_offset + uav_index],
                             max_dynamic_offset);
                     uint64_t const new_uav_elements =
                        MAX2(MIN2(uav_elements,
                                  (uint64_t)(remaining_bytes - dynamic_offset) /
                                     uav_bytes_per_element),
                             UINT64_C(1));
                     new_uav_color_with_dynamic_offset = set_uav->color;
                     terakan_color_descriptor_calculate_buffer_base_pitch_slice_view_dim(
                        &new_uav_color_with_dynamic_offset, uav_va + dynamic_offset,
                        new_uav_elements, uav_bytes_per_element,
                        terakan_gfx_command_writer_physical_device(command_writer), false);
                     new_uav_color = &new_uav_color_with_dynamic_offset;
                  }
                  terakan_app_config_draw_set_cb_color_uav(&command_writer->app_config_draw,
                                                           uav_bind_point, shader_uav_index,
                                                           set_uav->bo, new_uav_color);
               } else {
                  terakan_app_config_draw_set_cb_color_uav(&command_writer->app_config_draw,
                                                           uav_bind_point, shader_uav_index, NULL,
                                                           NULL);
               }
            }
         }
      }

      set_dynamic_offsets += set_layout->dynamic_offset_count;
   }
}

VkResult
terakan_pipeline_layout_create(struct terakan_device * const device,
                               VkPipelineLayoutCreateInfo const * const create_info,
                               VkShaderStageFlags const stage_mask,
                               struct terakan_pipeline_layout ** const pipeline_layout_out)
{
   assert(!(stage_mask & ~(VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT)));

   VK_MULTIALLOC(multialloc);
   VK_MULTIALLOC_DECL(&multialloc, struct terakan_pipeline_layout, layout, 1);
   VK_MULTIALLOC_DECL(&multialloc, struct terakan_pipeline_layout_set, sets,
                      create_info->setLayoutCount);
   /* Mesa pipeline layout has a different lifetime than the corresponding VkPipelineLayout since
    * other objects hold additional references to them, allocation must be done in the device scope.
    */
   if (vk_pipeline_layout_multizalloc(&device->vk, &multialloc, create_info) == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   layout->sets = sets;

   uint8_t next_first_mutable_shader_resources[MESA_SHADER_STAGES] = {};
   uint8_t next_first_shader_uniform_buffers[MESA_SHADER_STAGES] = {};
   uint8_t next_first_shader_samplers[MESA_SHADER_STAGES] = {};

   for (uint32_t set_index = 0; set_index < layout->vk.set_count; ++set_index) {
      struct vk_descriptor_set_layout const * const set_layout_base =
         layout->vk.set_layouts[set_index];
      if (set_layout_base == NULL) {
         continue;
      }
      struct terakan_descriptor_set_layout const * const set_layout =
         container_of(set_layout_base, struct terakan_descriptor_set_layout const, vk);
      struct terakan_pipeline_layout_set * const set = &layout->sets[set_index];

      unsigned remaining_stages = (unsigned)stage_mask;
      while (remaining_stages) {
         unsigned const stage_index = u_bit_scan(&remaining_stages);
         struct terakan_descriptor_set_layout_shader const * const set_layout_shader =
            &set_layout->shaders[stage_index];

         uint8_t const first_mutable_shader_resource =
            next_first_mutable_shader_resources[stage_index];
         if (((VkShaderStageFlags)1 << stage_index == VK_SHADER_STAGE_FRAGMENT_BIT
                 ? TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL
                 : TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL) -
                first_mutable_shader_resource <
             set_layout_shader->resource_count) {
            goto too_many_descriptors;
         }
         set->first_shader_resources[stage_index] =
            TERAKAN_RESOURCE_RANGE_MUTABLE_BASE + first_mutable_shader_resource;
         next_first_mutable_shader_resources[stage_index] += set_layout_shader->resource_count;

         set->first_shader_uniform_buffers[stage_index] =
            next_first_shader_uniform_buffers[stage_index];
         next_first_shader_uniform_buffers[stage_index] += set_layout_shader->uniform_buffer_count;

         uint8_t const first_shader_sampler = next_first_shader_samplers[stage_index];
         if (TERAKAN_SAMPLER_HW_COUNT_PER_STAGE - first_shader_sampler <
             set_layout_shader->sampler_count) {
            goto too_many_descriptors;
         }
         set->first_shader_samplers[stage_index] = first_shader_sampler;
         layout->shader_non_immutable_samplers[stage_index] |=
            set_layout_shader->non_immutable_samplers << first_shader_sampler;
         layout->shader_immutable_samplers_unnormalized_coordinates[stage_index] |=
            set_layout_shader->immutable_samplers_unnormalized_coordinates << first_shader_sampler;
         next_first_shader_samplers[stage_index] += set_layout_shader->sampler_count;
      }
   }

   for (uint32_t push_constant_range_index = 0;
        push_constant_range_index < create_info->pushConstantRangeCount;
        ++push_constant_range_index) {
      VkPushConstantRange const * const push_constant_range =
         &create_info->pPushConstantRanges[push_constant_range_index];
      uint32_t const push_constant_range_extent =
         push_constant_range->offset + push_constant_range->size;
      unsigned remaining_stages = (unsigned)(push_constant_range->stageFlags & stage_mask);
      while (remaining_stages) {
         uint32_t * const shader_app_push_constants_extent =
            &layout->shader_app_push_constants_extents_bytes[u_bit_scan(&remaining_stages)];
         *shader_app_push_constants_extent =
            MAX2(push_constant_range_extent, *shader_app_push_constants_extent);
      }
   }

   *pipeline_layout_out = layout;
   return VK_SUCCESS;

   /* While Vulkan implementations generally shouldn't perform validation, TeraScale has very low
    * binding count limits, while modern games demand many more. If they're launched on Terakan,
    * catch that early and report that instead of proceeding with invalid state. Doing the same for
    * push constants isn't needed as Terakan provides a much larger amount than most other drivers
    * (accurate validation of their limit also would be more complex due to cube array layer counts
    * being passed alongside push constants).
    */
too_many_descriptors:
   vk_pipeline_layout_unref(&device->vk, &layout->vk);
   return vk_errorf(
      device, VK_ERROR_VALIDATION_FAILED_EXT,
      "The application creates a pipeline layout that is too large to fit into the hardware "
      "binding register spaces");
}

VKAPI_ATTR VkResult VKAPI_CALL
terakan_CreatePipelineLayout(VkDevice const deviceHandle,
                             VkPipelineLayoutCreateInfo const * const pCreateInfo,
                             UNUSED VkAllocationCallbacks const * const pAllocator,
                             VkPipelineLayout * const pPipelineLayout)
{
   struct terakan_pipeline_layout * layout;
   VkResult const result = terakan_pipeline_layout_create(
      terakan_device_from_handle(deviceHandle), pCreateInfo,
      VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT, &layout);
   if (result != VK_SUCCESS) {
      return result;
   }
   *pPipelineLayout = terakan_pipeline_layout_to_handle(layout);
   return VK_SUCCESS;
}

/* Whether `terakan_CreateDescriptorSetLayout` would accept this layout.
 *
 * The support query used to answer from a total-descriptor bound of its own, which the layout
 * creation path knows nothing about: creation validates each binding against the hardware's
 * register spaces, so the two could and did disagree.
 * `dEQP-VK.api.maintenance3_check.support_count_*_create_layout` builds the layout the query said
 * was supported and creates it, and got `VK_ERROR_VALIDATION_FAILED`. Asking creation itself
 * cannot disagree with creation.
 */
static bool
terakan_descriptor_set_layout_creatable(VkDevice const device,
                                        VkDescriptorSetLayoutCreateInfo const * const create_info)
{
   VkDescriptorSetLayout layout_handle = VK_NULL_HANDLE;
   if (terakan_CreateDescriptorSetLayout(device, create_info, NULL, &layout_handle) != VK_SUCCESS) {
      return false;
   }
   struct terakan_descriptor_set_layout * const layout =
      terakan_descriptor_set_layout_from_handle(layout_handle);
   vk_descriptor_set_layout_unref(&terakan_device_from_handle(device)->vk, &layout->vk);
   return true;
}

VKAPI_ATTR void VKAPI_CALL
terakan_GetDescriptorSetLayoutSupport(VkDevice device,
                                      VkDescriptorSetLayoutCreateInfo const * const pCreateInfo,
                                      VkDescriptorSetLayoutSupport * const pSupport)
{
   pSupport->supported = terakan_descriptor_set_layout_creatable(device, pCreateInfo);

   /* The `VkDescriptorSetVariableDescriptorCountLayoutSupport` reference says
    * `maxVariableDescriptorCount` "is set to zero" when the layout has no variable-sized binding,
    * and otherwise holds the largest count that binding may be given. The structure was left
    * untouched, so it kept whatever the caller had in it -- which is what
    * `dEQP-VK.api.maintenance3_check.support_count_*_no_variable_size_*` reports as "Nonzero
    * maxVariableDescriptorCount when using no variable descriptor counts".
    */
   VkDescriptorSetVariableDescriptorCountLayoutSupport * const variable_support =
      vk_find_struct(pSupport->pNext, DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT);
   if (variable_support == NULL) {
      return;
   }
   variable_support->maxVariableDescriptorCount = 0;

   VkDescriptorSetLayoutBindingFlagsCreateInfo const * const binding_flags =
      vk_find_struct_const(pCreateInfo->pNext, DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
   if (binding_flags == NULL || pCreateInfo->bindingCount == 0) {
      return;
   }
   /* Only the last binding may be variable-sized, so there is at most one. */
   uint32_t variable_binding_index = pCreateInfo->bindingCount;
   for (uint32_t i = 0; i < pCreateInfo->bindingCount; ++i) {
      if (i < binding_flags->bindingCount &&
          (binding_flags->pBindingFlags[i] &
           VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) != 0) {
         variable_binding_index = i;
         break;
      }
   }
   if (variable_binding_index >= pCreateInfo->bindingCount) {
      return;
   }

   /* The reported count has to be one creation accepts, and the test builds exactly that layout,
    * so the largest accepted count is found by asking. The bound is the device's own
    * `maxPerSetDescriptors`, which no binding can exceed.
    */
   struct terakan_device const * const terakan_device_ = terakan_device_from_handle(device);
   uint32_t const probe_limit = MIN2(
      terakan_device_physical_device(terakan_device_)->vk.properties.maxPerSetDescriptors, 1u << 16);
   VkDescriptorSetLayoutBinding * const probe_bindings =
      vk_alloc(&terakan_device_->vk.alloc, sizeof(*probe_bindings) * pCreateInfo->bindingCount, 8,
               VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (probe_bindings == NULL) {
      return;
   }
   memcpy(probe_bindings, pCreateInfo->pBindings,
          sizeof(*probe_bindings) * pCreateInfo->bindingCount);
   VkDescriptorSetLayoutCreateInfo probe_create_info = *pCreateInfo;
   probe_create_info.pBindings = probe_bindings;

   uint32_t accepted = 0, low = 0, high = probe_limit;
   while (low <= high) {
      uint32_t const middle = low + (high - low) / 2;
      probe_bindings[variable_binding_index].descriptorCount = middle;
      if (terakan_descriptor_set_layout_creatable(device, &probe_create_info)) {
         accepted = middle;
         low = middle + 1;
      } else {
         if (middle == 0) {
            break;
         }
         high = middle - 1;
      }
   }
   vk_free(&terakan_device_->vk.alloc, probe_bindings);
   variable_support->maxVariableDescriptorCount = accepted;
}

VKAPI_ATTR void VKAPI_CALL
terakan_GetDescriptorSetLayoutSupportKHR(VkDevice device,
                                         VkDescriptorSetLayoutCreateInfo const * const pCreateInfo,
                                         VkDescriptorSetLayoutSupport * const pSupport)
{
   terakan_GetDescriptorSetLayoutSupport(device, pCreateInfo, pSupport);
}
