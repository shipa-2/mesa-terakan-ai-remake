/*
 * Copyright © 2026 Vitaliy Triang3l Kuzmin
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

#ifndef TERAKAN_HW_CONFIG_SQK_H
#define TERAKAN_HW_CONFIG_SQK_H

#include "terakan_bo.h"
#include "terakan_descriptor.h"
#include "terakan_shader.h"

#include "compiler/shader_enums.h"
#include "gallium/drivers/r600/evergreend.h"
#include "util/bitset.h"
#include "util/macros.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Binding of buffer, texture and sampler descriptor constants to shaders.
 * SQK in the driver stands for "sequencer constants".
 * The primary purpose, aside from re-emission when switching to a new indirect buffer or allocating
 * the compute state context, is avoiding emission of unchanged constants, as binding the same
 * constant multiple times is expected to be common in Vulkan with its descriptor set model
 * involving grouping of large numbers of descriptors into one bind operation.
 */

/* On R9xx with `USE_LS_CONSTS`, software VS constants are always emitted to hardware LS registers,
 * and software TES constants are emitted to hardware VS/ES registers.
 *
 * However, when that's not the case, there can be two scenarios:
 * - Without tessellation:
 *   - Software VS -> hardware VS/ES.
 * - With tessellation:
 *   - Software VS -> hardware LS.
 *   - Software TES -> hardware VS/ES.
 *
 * This means that `terakan_hw_config_sqk` needs to:
 * - Track whether new software VS constants have been emitted to each of the hardware LS and VS/ES
 *   register spaces.
 * - Arbitrate hardware VS/ES registers between software VS and TES.
 *
 * The former is largely trivial, two "modified" flag sets are maintained (for LS and VS/ES) instead
 * of one, and when a binding is changed, it's marked as modified in both.
 *
 * VS/ES register space arbitration, however, needs to be done in a way that:
 * - CPU overhead is minimized within sequences of non-tessellated or tessellated draws, especially
 *   because many applications don't use tessellation at all.
 *   - Significant performance costs shouldn't be introduced to descriptor setting and emission.
 *     Most of the work should be performed only when tessellation is actually toggled.
 * - Excessive register setting emissions are avoided when toggling tessellation.
 *   - If the constant at a given index is the same for the VS and the TES, and has already been
 *     emitted for the other software stage, it's desirable not to re-emit it. This is likely to
 *     happen if the application follows the recommendations implied by Vulkan pipeline layout
 *     compatibility, placing less frequently changed (such as per-frame) descriptor sets at lower
 *     indices, so if the first bindings in the pipeline layout are accessible by both VS and TES,
 *     they will receive the same hardware register indices.
 *   - Constants not used by the currently bound shaders, which may contain irrelevant leftovers
 *     from other draws, shouldn't cause registers to be spuriously marked as modified and
 *     re-emitted.
 * The approach used is to mark whether each VS/ES register was last used for VS or TES, and when
 * tessellation is toggled, for the registers whose ownership by VS or TES is changed, to check if
 * the needed constant for the new stage is potentially different from the constant emitted for the
 * stage that used the register previously.
 */

struct terakan_hw_config_sqk_stage_emission_flags {
   uint16_t kcache;

   uint32_t samplers;

   BITSET_DECLARE(resources, TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE);
};

/* Assuming that both samplers are bound (all `SQ_TEX_SAMPLER` fields are relevant regardless of the
 * `TYPE`).
 */
static inline bool
terakan_hw_config_sqk_bound_sampler_equal(struct terakan_sampler_descriptor const * const a,
                                          struct terakan_sampler_descriptor const * const b)
{
   if (memcmp(a->sampler, b->sampler, sizeof(a->sampler)) != 0) {
      return false;
   }
   if (G_03C000_BORDER_COLOR_TYPE(a->sampler[0]) == V_03C000_SQ_TEX_BORDER_COLOR_REGISTER &&
       memcmp(a->register_border_color, b->register_border_color,
              sizeof(a->register_border_color)) != 0) {
      return false;
   }
   return true;
}

struct terakan_hw_config_sqk_kcache_buffer {
   struct terakan_bo const * bo;
   uint32_t va_lines;
   uint32_t size_lines;
};

static inline bool
terakan_hw_config_sqk_kcache_buffer_is_bound(
   struct terakan_hw_config_sqk_kcache_buffer const * const kcache_buffer)
{
   return kcache_buffer->size_lines != 0 && kcache_buffer->bo != NULL;
}

static inline bool
terakan_hw_config_sqk_kcache_buffer_equal(
   struct terakan_hw_config_sqk_kcache_buffer const * const a,
   struct terakan_hw_config_sqk_kcache_buffer const * const b)
{
   if (!terakan_hw_config_sqk_kcache_buffer_is_bound(a)) {
      return !terakan_hw_config_sqk_kcache_buffer_is_bound(b);
   }
   /* Never passes if `b` is unbound. */
   return a->bo == b->bo && a->va_lines == b->va_lines && a->size_lines == b->size_lines;
}

struct terakan_hw_config_sqk_resource {
   struct terakan_bo const * bo;
   struct terakan_resource_descriptor descriptor;
};

struct terakan_hw_config_sqk_stage {
   /* Section "Allocation of Descriptor Sets" of the Vulkan 1.4.337 specification says:
    *
    *     "Descriptor sets containing undefined descriptors can still be bound and used, subject to
    *     the following conditions:
    *
    *       * [...]
    *       * For descriptor set bindings created without the
    *         VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT bit set, all descriptors in that binding
    *         that are statically used must have been populated before the descriptor set is
    *         consumed.
    *       * [...]
    *       * Entries that are not used by a pipeline can have undefined descriptors."
    *
    * Because of that, it's important that dereferencing of the BO pointer (which otherwise may
    * point to a freed BO or be completely invalid), or any processing of the descriptor contents
    * that assumes that the descriptor must be valid (such as anything involving assertions about
    * its contents), may be done only when finally emitting the packets for setting the constants,
    * and only for the constants included in the `terakan_shader_sqk_usage` of the currently bound
    * shader.
    *
    * The usage is NULL if the shader is not bound. Particularly, a non-NULL TES constant usage
    * pointer indicates that the VS/ES hardware constants are going to be used for TES.
    */
   struct terakan_shader_sqk_usage const * usage;

   /* Whether each constant for each software stage needs to be emitted for its current hardware
    * stage for the next draw or dispatch if the corresponding software and hardware stage and
    * constant are used in it.
    */
   struct terakan_hw_config_sqk_stage_emission_flags modified;

   struct terakan_hw_config_sqk_kcache_buffer kcache[TERAKAN_KCACHE_HW_BUFFERS_PER_STAGE];

   struct terakan_sampler_descriptor samplers[TERAKAN_SAMPLER_HW_COUNT_PER_STAGE];

   BITSET_DECLARE(resources_bound, TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE);
};

struct terakan_hw_config_sqk {
   /* Whether hardware LS and VS/ES registers are always used exclusively for software VS and TES
    * constants respectively (R9xx `USE_LS_CONSTS`).
    */
   bool sw_vs_always_uses_hw_ls_sqk_;

   /* Optimization flag for only arbitrating hardware VS/ES registers between software VS and TES if
    * actually potentially needed.
    */
   bool hw_vses_arbitration_needed_;

   /* Whether each graphics shader stage has non-NULL constant usage. */
   uint8_t draw_stages_used_;

   /* Whether each graphics shader stage potentially needs anything to be emitted for the next draw.
    */
   uint8_t draw_stages_pending_;

   /* Vertex input (fetch shader) stage. */
   struct {
      uint32_t resources_used;
      uint32_t resources_modified;
      uint32_t resources_bound;
   } vi_;

   struct terakan_hw_config_sqk_stage stages_[MESA_SHADER_COMPUTE + 1];

   /* Whether each constant needs to be emitted for the software VS in the other hardware LS or
    * VS/ES register space than the current for the next draw that will have the software VS
    * constants in that stage.
    */
   struct terakan_hw_config_sqk_stage_emission_flags modified_in_sw_vs_as_other_hw_stage_;

   /* Whether each constant in the hardware VS/ES registers was last used by the software TES (1) or
    * VS (0).
    */
   struct terakan_hw_config_sqk_stage_emission_flags vses_last_used_by_tes_;

   /* Whether each resource register, for its current (or if currently unused, its last) usage,
    * needs to have the `UNCACHED` flag force-enabled during emission if a buffer is bound to it.
    */
   BITSET_DECLARE(resources_last_used_as_uncached_fs_, TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE);
   BITSET_DECLARE(resources_last_used_as_uncached_cs_, TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE);

   /* Resource arrays are large, so they're not initialized. Rather, if `resources_bound` for the
    * given stage doesn't have the corresponding bit set for a resource, the BO pointer and the
    * resource descriptor are undefined.
    */
   struct terakan_hw_config_sqk_resource resources_vi_[TERAKAN_RESOURCE_HW_COUNT_FETCH];
   struct terakan_hw_config_sqk_resource resources_vs_[TERAKAN_RESOURCE_HW_COUNT_VERTEX];
   struct terakan_hw_config_sqk_resource resources_tcs_[TERAKAN_RESOURCE_HW_COUNT_VERTEX];
   struct terakan_hw_config_sqk_resource resources_tes_[TERAKAN_RESOURCE_HW_COUNT_VERTEX];
   struct terakan_hw_config_sqk_resource resources_gs_[TERAKAN_RESOURCE_HW_COUNT_VERTEX];
   struct terakan_hw_config_sqk_resource resources_fs_[TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE];
   struct terakan_hw_config_sqk_resource resources_cs_[TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE];
};

/* #MemoryIntegrity is expected to be handled for the contents of the bound constants before the
 * call, such as during descriptor creation.
 *
 * In the constant setters, for optimization purposes, assuming that:
 * - The setters are going to be called many times for a single draw. Therefore, marking the stage
 *   constants as pending is desirable even if the current shader doesn't use the constant being
 *   modified. Additionally, the current shader may be changed after the constants are set anyway.
 * - There's no point in checking if the modification flag is already set to skip the comparison, as
 *   as comparison and writing costs are similar, and the situation where a descriptor is set twice
 *   to the same value, but not used by a shader in between, is expected to be less frequent than
 *   other cases, and not significant enough to increase the complexity of the setter code.
 */

/* Pass zero size or NULL BO to the kcache buffer setter to unbind the buffer.
 * Both the size and the BO pointer are ignored if any of them is zero.
 */
typedef void (*terakan_hw_config_sqk_set_kcache_function)(struct terakan_hw_config_sqk * config,
                                                          unsigned index, uint32_t size_lines,
                                                          struct terakan_bo const * bo,
                                                          uint32_t va_lines);
void terakan_hw_config_sqk_set_kcache_vs(struct terakan_hw_config_sqk * config, unsigned index,
                                         uint32_t size_lines, struct terakan_bo const * bo,
                                         uint32_t va_lines);
void terakan_hw_config_sqk_set_kcache_tcs(struct terakan_hw_config_sqk * config, unsigned index,
                                          uint32_t size_lines, struct terakan_bo const * bo,
                                          uint32_t va_lines);
void terakan_hw_config_sqk_set_kcache_tes(struct terakan_hw_config_sqk * config, unsigned index,
                                          uint32_t size_lines, struct terakan_bo const * bo,
                                          uint32_t va_lines);
void terakan_hw_config_sqk_set_kcache_gs(struct terakan_hw_config_sqk * config, unsigned index,
                                         uint32_t size_lines, struct terakan_bo const * bo,
                                         uint32_t va_lines);
void terakan_hw_config_sqk_set_kcache_fs(struct terakan_hw_config_sqk * config, unsigned index,
                                         uint32_t size_lines, struct terakan_bo const * bo,
                                         uint32_t va_lines);
void terakan_hw_config_sqk_set_kcache_cs(struct terakan_hw_config_sqk * config, unsigned index,
                                         uint32_t size_lines, struct terakan_bo const * bo,
                                         uint32_t va_lines);

typedef void (*terakan_hw_config_sqk_set_sampler_function)(
   struct terakan_hw_config_sqk * config, unsigned index,
   struct terakan_sampler_descriptor const * descriptor);
void terakan_hw_config_sqk_set_sampler_vs(struct terakan_hw_config_sqk * config, unsigned index,
                                          struct terakan_sampler_descriptor const * descriptor);
void terakan_hw_config_sqk_set_sampler_tcs(struct terakan_hw_config_sqk * config, unsigned index,
                                           struct terakan_sampler_descriptor const * descriptor);
void terakan_hw_config_sqk_set_sampler_tes(struct terakan_hw_config_sqk * config, unsigned index,
                                           struct terakan_sampler_descriptor const * descriptor);
void terakan_hw_config_sqk_set_sampler_gs(struct terakan_hw_config_sqk * config, unsigned index,
                                          struct terakan_sampler_descriptor const * descriptor);
void terakan_hw_config_sqk_set_sampler_fs(struct terakan_hw_config_sqk * config, unsigned index,
                                          struct terakan_sampler_descriptor const * descriptor);
void terakan_hw_config_sqk_set_sampler_cs(struct terakan_hw_config_sqk * config, unsigned index,
                                          struct terakan_sampler_descriptor const * descriptor);

/* Pass NULL BO or descriptor pointer to the resource setter to unbind the resource.
 * Both the BO and the descriptor pointers are ignored if any of them is NULL.
 */
typedef void (*terakan_hw_config_sqk_set_resource_function)(
   struct terakan_hw_config_sqk * config, unsigned index, struct terakan_bo const * bo,
   struct terakan_resource_descriptor const * descriptor);
void terakan_hw_config_sqk_set_resource_vi(struct terakan_hw_config_sqk * config, unsigned index,
                                           struct terakan_bo const * bo,
                                           struct terakan_resource_descriptor const * descriptor);
void terakan_hw_config_sqk_set_resource_vs(struct terakan_hw_config_sqk * config, unsigned index,
                                           struct terakan_bo const * bo,
                                           struct terakan_resource_descriptor const * descriptor);
void terakan_hw_config_sqk_set_resource_tcs(struct terakan_hw_config_sqk * config, unsigned index,
                                            struct terakan_bo const * bo,
                                            struct terakan_resource_descriptor const * descriptor);
void terakan_hw_config_sqk_set_resource_tes(struct terakan_hw_config_sqk * config, unsigned index,
                                            struct terakan_bo const * bo,
                                            struct terakan_resource_descriptor const * descriptor);
void terakan_hw_config_sqk_set_resource_gs(struct terakan_hw_config_sqk * config, unsigned index,
                                           struct terakan_bo const * bo,
                                           struct terakan_resource_descriptor const * descriptor);
void terakan_hw_config_sqk_set_resource_fs(struct terakan_hw_config_sqk * config, unsigned index,
                                           struct terakan_bo const * bo,
                                           struct terakan_resource_descriptor const * descriptor);
void terakan_hw_config_sqk_set_resource_cs(struct terakan_hw_config_sqk * config, unsigned index,
                                           struct terakan_bo const * bo,
                                           struct terakan_resource_descriptor const * descriptor);

struct terakan_hw_config_sqk_set_functions {
   terakan_hw_config_sqk_set_kcache_function kcache;
   terakan_hw_config_sqk_set_sampler_function sampler;
   terakan_hw_config_sqk_set_resource_function resource;
};

extern struct terakan_hw_config_sqk_set_functions const
   terakan_hw_config_sqk_stage_set_functions[MESA_SHADER_COMPUTE + 1];

/* Sets the constant usage for a graphics stage (compute constant usage is managed separately).
 * Returns whether the usage was changed.
 */
static inline bool
terakan_hw_config_sqk_set_draw_usage_(struct terakan_hw_config_sqk * const config,
                                      gl_shader_stage const stage_index,
                                      struct terakan_shader_sqk_usage const * const usage)
{
   assert(stage_index != MESA_SHADER_COMPUTE);
   struct terakan_hw_config_sqk_stage * const stage = &config->stages_[stage_index];
   if (stage->usage == usage) {
      return false;
   }
   stage->usage = usage;
   uint8_t const stage_flag = BITFIELD_BIT(stage_index);
   if (usage != NULL) {
      config->draw_stages_used_ |= stage_flag;
      config->draw_stages_pending_ |= stage_flag;
   } else {
      config->draw_stages_used_ &= ~stage_flag;
   }
   return true;
}

static inline void
terakan_hw_config_sqk_set_usage_vi(struct terakan_hw_config_sqk * const config,
                                   uint32_t const resources_used)
{
   config->vi_.resources_used = resources_used;
}

/* Note that this doesn't mark the stage resources as pending, as it's expected that it's done
 * anyway when changing the shader resource usage.
 */
void terakan_hw_config_sqk_update_uncached_resources_(
   struct terakan_hw_config_sqk_stage * stage, BITSET_WORD * resources_last_used_as_uncached,
   struct terakan_hw_config_sqk_resource const * resources);

/* Pass NULL usage to specify that the shader is unbound.
 * This is especially important for the TES stage, because without `USE_LS_CONSTS`, its usage being
 * not NULL, determines that software VS and TES constants must be emitted to LS registers rather
 * than ES/VS.
 */

/* VS and TES usage setting is combined into one function because for optimal arbitration of the
 * hardware VS/ES registers, it needs to know the actual usage for either the software VS or TES
 * depending on which stage will use the hardware VS/ES registers.
 */
void terakan_hw_config_sqk_set_usage_vs_tes(struct terakan_hw_config_sqk * config,
                                            struct terakan_shader_sqk_usage const * usage_vs,
                                            struct terakan_shader_sqk_usage const * usage_tes);

static inline void
terakan_hw_config_sqk_set_usage_tcs(struct terakan_hw_config_sqk * const config,
                                    struct terakan_shader_sqk_usage const * const usage)
{
   terakan_hw_config_sqk_set_draw_usage_(config, MESA_SHADER_TESS_CTRL, usage);
}

static inline void
terakan_hw_config_sqk_set_usage_gs(struct terakan_hw_config_sqk * const config,
                                   struct terakan_shader_sqk_usage const * const usage)
{
   terakan_hw_config_sqk_set_draw_usage_(config, MESA_SHADER_GEOMETRY, usage);
}

static inline void
terakan_hw_config_sqk_set_usage_fs(struct terakan_hw_config_sqk * const config,
                                   struct terakan_shader_sqk_usage const * const usage)
{
   if (!terakan_hw_config_sqk_set_draw_usage_(config, MESA_SHADER_FRAGMENT, usage)) {
      return;
   }
   terakan_hw_config_sqk_update_uncached_resources_(&config->stages_[MESA_SHADER_FRAGMENT],
                                                    config->resources_last_used_as_uncached_fs_,
                                                    config->resources_fs_);
}

static inline void
terakan_hw_config_sqk_set_usage_cs(struct terakan_hw_config_sqk * const config,
                                   struct terakan_shader_sqk_usage const * const usage)
{
   struct terakan_hw_config_sqk_stage * const stage = &config->stages_[MESA_SHADER_COMPUTE];
   if (stage->usage == usage) {
      return;
   }
   stage->usage = usage;
   terakan_hw_config_sqk_update_uncached_resources_(
      stage, config->resources_last_used_as_uncached_cs_, config->resources_cs_);
}

struct terakan_gfx_command_writer;

void terakan_hw_config_sqk_begin_emitting_first_draw_dispatch_in_indirect_buffer(
   struct terakan_gfx_command_writer * command_writer);

void
terakan_hw_config_sqk_emit_modified_for_draw(struct terakan_gfx_command_writer * command_writer);

void
terakan_hw_config_sqk_emit_modified_for_compute(struct terakan_gfx_command_writer * command_writer);

void terakan_hw_config_sqk_reset(struct terakan_hw_config_sqk * config,
                                 bool sw_vs_always_uses_hw_ls_sqk);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_HW_CONFIG_SQK_H */
