/*
 * Copyright © 2023 Vitaliy Triang3l Kuzmin
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

#ifndef TERAKAN_LIMITS_H
#define TERAKAN_LIMITS_H

#include "util/u_math.h"

#include <assert.h>

/* HW values are hardware limits, VK values are allocation for use by Vulkan applications. */

#define TERAKAN_LIMITS_HW_VIEWPORTS 16

#define TERAKAN_LIMITS_HW_COLOR_MRT_COUNT 8
#define TERAKAN_LIMITS_HW_COLOR_RAT_COUNT 12

#define TERAKAN_LIMITS_HW_CONSTANT_BUFFER_CACHE_LINE_BYTES_LOG2 8
#define TERAKAN_LIMITS_HW_CONSTANT_BUFFER_CACHE_LINE_BYTES                                         \
   (1 << TERAKAN_LIMITS_HW_CONSTANT_BUFFER_CACHE_LINE_BYTES_LOG2)
#define TERAKAN_LIMITS_HW_CONSTANT_BUFFER_CACHE_LINE_COUNT_LOG2 8
#define TERAKAN_LIMITS_HW_CONSTANT_BUFFER_CACHE_LINE_COUNT                                         \
   (1 << TERAKAN_LIMITS_HW_CONSTANT_BUFFER_CACHE_LINE_COUNT_LOG2)
#define TERAKAN_LIMITS_HW_CONSTANT_BUFFER_SIZE_BYTES_LOG2                                          \
   (TERAKAN_LIMITS_HW_CONSTANT_BUFFER_CACHE_LINE_BYTES_LOG2 +                                      \
    TERAKAN_LIMITS_HW_CONSTANT_BUFFER_CACHE_LINE_COUNT_LOG2)
#define TERAKAN_LIMITS_HW_CONSTANT_BUFFER_SIZE_BYTES                                               \
   (1 << TERAKAN_LIMITS_HW_CONSTANT_BUFFER_SIZE_BYTES_LOG2)
#define TERAKAN_LIMITS_HW_CONSTANT_BUFFER_COUNT 16

#define TERAKAN_LIMITS_HW_SAMPLER_COUNT 18

#define TERAKAN_LIMITS_HW_PARAMETER_CACHE_VECTOR_COUNT 32

#define TERAKAN_LIMITS_HW_LDS_SIMD_BANK_COUNT       32
#define TERAKAN_LIMITS_HW_LDS_SIMD_BANK_DWORD_COUNT 256
#define TERAKAN_LIMITS_HW_LDS_SIMD_DWORD_COUNT                                                     \
   (TERAKAN_LIMITS_HW_LDS_SIMD_BANK_DWORD_COUNT * TERAKAN_LIMITS_HW_LDS_SIMD_BANK_COUNT)

#define TERAKAN_LIMITS_HW_COMPUTE_GROUP_SIZE           1024
#define TERAKAN_LIMITS_HW_COMPUTE_GROUPS_PER_DIMENSION UINT16_MAX

/* The goal behind binding allocation on Vulkan is to provide enough bindings for Direct3D 11.0 to
 * be possible to implement on top of Terakan.
 */

/* Storage buffers/images are allocated top-down: for instance, the first storage binding in the
 * pipeline layout goes to RAT `HW_COLOR_RAT_COUNT - 1`, the second to RAT
 * `HW_COLOR_RAT_COUNT - 2`, and so on. This makes RAT indices depend only on the pipeline
 * layout and not on the render pass.
 */

/* Push constants and frequently-updated internal constants. */
#define TERAKAN_LIMITS_VK_CONSTANT_BUFFER_HIGH_FREQUENCY                                           \
   (TERAKAN_LIMITS_HW_CONSTANT_BUFFER_COUNT - 1)
/* Uniform buffers. */
/* Starting at 0 for shader disassembly readability. */
#define TERAKAN_LIMITS_VK_CONSTANT_BUFFER_UNIFORM_BUFFER_BASE 0
#define TERAKAN_LIMITS_VK_CONSTANT_BUFFER_UNIFORM_BUFFER_MAX_COUNT                                 \
   (TERAKAN_LIMITS_VK_CONSTANT_BUFFER_HIGH_FREQUENCY -                                             \
    TERAKAN_LIMITS_VK_CONSTANT_BUFFER_UNIFORM_BUFFER_BASE)
static_assert(
   TERAKAN_LIMITS_VK_CONSTANT_BUFFER_UNIFORM_BUFFER_MAX_COUNT >= 12,
   "There must be enough constant buffer bindings for the minimum Vulkan uniform buffer count.");
static_assert(
   TERAKAN_LIMITS_VK_CONSTANT_BUFFER_UNIFORM_BUFFER_MAX_COUNT >= 15,
   "There should be enough constant buffer bindings for the Direct3D 11 constant buffer count.");

#endif /* TERAKAN_LIMITS_H */
