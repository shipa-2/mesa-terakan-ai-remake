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

#ifndef TERAKAN_NIR_H
#define TERAKAN_NIR_H

#include "terakan_pipeline_layout.h"
#include "terakan_shader.h"

#include "util/bitset.h"
#include "nir.h"
#include "nir_builder.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Emits a buffer resource load intrinsic for a buffer resource with a stride of 1 producing a NIR
 * vector.
 * Supports 1, 2 and 4 components for 8 and 16 bits, and 1, 2, 3 and 4 components for 32 bits.
 * Vectorization is expected to be done externally before lowering to this intrinsic if needed,
 * taking alignment and robustness requirements into account.
 */
nir_def * terakan_nir_load_raw_resource_buffer(nir_builder * b, unsigned num_components,
                                               unsigned bit_size, enum gl_access_qualifier access,
                                               unsigned resource_index_base,
                                               nir_def * resource_index, unsigned byte_address_base,
                                               nir_def * byte_address);

/* Compact fragment data output locations, so all RTVs and the dual-source blend factor precede all
 * UAVs as required and to be able to allocate arrays of UAVs without fragmentation, and so there
 * are no holes in `CB_SHADER_MASK`, which, according to RadeonSI (but likely applicable to earlier
 * generations), may cause hangs.
 * The mask of which pre-compaction fragment data output locations (where bit 1 indicates either the
 * color attachment 1 or the second source in dual-source blending for color attachment 0) are used
 * is written to `rtv_dsb_uncompacted_exports_out`.
 */
bool terakan_nir_compact_rtv_dsb_exports(nir_shader * shader,
                                         uint8_t * rtv_dsb_uncompacted_exports_out);

/* Translate Vulkan set and binding indices and push constant loads into hardware constant indices,
 * and lower various instructions involving bindings into TeraScale-specific intrinsics and logic.
 *
 * Works with instructions generated from nir_lower_explicit_io.
 *
 * It's recommended to run nir_opt_load_store_vectorize before the pass, as this transforms certain
 * loads and stores into hardware-specific intrinsics.
 *
 * The function adds new bits to `sqk_usage_accum`, but keeps the already set ones.
 *
 * `uavs_for_mutable_resources_needed_out_opt`, however, is overwritten completely (more precisely,
 * bitset words for the maximum count of mutable resources at the stage are touched), as UAV indices
 * are prefix sums, and thus new bits can't be set after this lowering, and it also may be omitted
 * by the caller if UAVs are not needed (such as if the stage doesn't support UAVs).
 */
bool terakan_nir_lower_bindings(nir_shader * shader, struct terakan_pipeline_layout const * layout,
                                struct terakan_shader_sqk_usage * sqk_usage_accum,
                                unsigned uav_base,
                                BITSET_WORD * uavs_for_mutable_resources_needed_out_opt,
                                uint32_t * driver_push_constants_used_accum);

/* R8xx samplers have no `FORCE_UNNORMALIZED` -- the bit exists only on Cayman -- so a sampler
 * created with `unnormalizedCoordinates` has to have its coordinates divided by the texture size
 * by the shader. Which of the stage's sampler slots need that is only known at draw time, so the
 * division is predicated on `terakan_push_constants_driver::sampler_unnormalized`. Must run after
 * `terakan_nir_lower_bindings`, which assigns the hardware sampler and texture slots this reads.
 */
bool terakan_nir_lower_unnormalized_coordinates(
   nir_shader * shader, struct terakan_shader_sqk_usage * sqk_usage_accum,
   uint32_t * driver_push_constants_used_accum);

/* `NUM_FORMAT = INT` switches seamless cube map filtering off, so an integer cube view is
 * described as `SCALED` instead and the hardware returns its integer value as a float. Convert it
 * back. Which views got that treatment depends on the channel width, which the shader cannot see,
 * so it is read from `terakan_push_constants_driver::texture_scaled_integer`. Must run after
 * `terakan_nir_lower_bindings`, which assigns the hardware texture slots this reads.
 */
bool terakan_nir_lower_integer_cube_fetch(nir_shader * shader,
                                          uint32_t * driver_push_constants_used_accum,
                                          struct terakan_shader_sqk_usage * sqk_usage_accum);

bool terakan_nir_lower_sin_cos(nir_shader * shader);

/* TeraScale doesn't have general cross-lane data exchange instructions usable by all Vulkan
 * shader stages. Expose a logical subgroup containing one invocation and lower the subgroup
 * operations supported by that model to scalar NIR.
 */
bool terakan_nir_lower_subgroups(nir_shader * shader);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_NIR_H */
