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

#ifndef TERAKAN_META_H
#define TERAKAN_META_H

#include "terakan_shader.h"

#include "gallium/drivers/r600/evergreend.h"

#include <stddef.h>
#include <stdint.h>

#define TERAKAN_META_SQ_PGM_RESOURCES_COMMON S_028844_DX10_CLAMP(1)
#define TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON                                                     \
   (S_028848_SINGLE_ROUND(V_SQ_ROUND_NEAREST_EVEN) | S_028848_DOUBLE_ROUND(V_SQ_ROUND_NEAREST_EVEN))

struct terakan_meta_shader_description {
   uint32_t const * program;
   size_t program_size_bytes;
   struct terakan_shader_static static_registers;
};

struct terakan_meta_shader {
   struct terakan_meta_shader_description r8xx;
   struct terakan_meta_shader_description r9xx;
};

enum terakan_meta_shader_index {
   /* Vertex index unpacked as X16Y16 into the position. */
   TERAKAN_META_SHADER_POSITION_FROM_INDEX_VS,

   TERAKAN_META_SHADER_COUNT,
};

extern struct terakan_meta_shader const * const terakan_meta_shaders[TERAKAN_META_SHADER_COUNT];

#endif /* TERAKAN_META_H */
