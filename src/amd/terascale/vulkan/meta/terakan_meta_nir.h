/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TERAKAN_META_NIR_H
#define TERAKAN_META_NIR_H

#include "terakan_shader.h"

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

struct terakan_device;
struct nir_shader;

/* Runs the driver's post-link lowering over a meta shader built as NIR and compiles it with the
 * same backend as application shaders. Consumes `nir`. On success the caller owns
 * `shader_out->shader` and must release it with terakan_shader_impl_finish once the bytecode has
 * been copied where it belongs. See the file comment in terakan_meta_nir.c.
 */
VkResult terakan_meta_nir_compile(struct terakan_device * device, struct nir_shader * nir,
                                  struct terakan_shader_impl * shader_out);

/* Exports (0, 0, 0, 1) to render target 0: the stand-in for a pipeline with no fragment shader. */
struct nir_shader * terakan_meta_nir_build_opaque_ps(struct terakan_device const * device);

/* Fetches sample zero of a multisample colour source and exports it: an integer resolve. */
struct nir_shader * terakan_meta_nir_build_resolve_sample_zero_ps(
   struct terakan_device const * device);

/* Samples a 3D source with a per-draw depth coordinate so the hardware filters between slices. */
struct nir_shader * terakan_meta_nir_build_blit_image_3d_ps(
   struct terakan_device const * device);

/* Writes a constant colour into a 3x-expanded image, one surfel per component, through the UAV. */
struct nir_shader * terakan_meta_nir_build_clear_expand_3x_ps(
   struct terakan_device const * device);

/* Copies a 3x-expanded image one component at a time, one variant per surfel size. */
struct nir_shader * terakan_meta_nir_build_copy_expand_3x_8_ps(
   struct terakan_device const * device);
struct nir_shader * terakan_meta_nir_build_copy_expand_3x_16_ps(
   struct terakan_device const * device);
struct nir_shader * terakan_meta_nir_build_copy_expand_3x_32_ps(
   struct terakan_device const * device);

/* Builds the NIR for one meta shader. */
typedef struct nir_shader * (*terakan_meta_nir_builder)(struct terakan_device const * device);

/* Indexed by terakan_meta_shader_index. A non-NULL entry means the shader is built as NIR and
 * compiled at device creation, and the hand-written bytecode in terakan_meta_shaders is not used
 * for it. Declared here rather than in terakan_meta.h so that only this file needs nir_shader.
 */
extern terakan_meta_nir_builder const terakan_meta_nir_builders[];

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_META_NIR_H */
