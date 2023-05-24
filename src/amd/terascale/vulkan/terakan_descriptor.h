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

#ifndef TERAKAN_DESCRIPTOR_H
#define TERAKAN_DESCRIPTOR_H

#include "winsys/terakan_winsys.h"

#include "gallium/drivers/r600/evergreend.h"

#include <stdint.h>

/* Hardware CB_COLOR[0-11] registers.
 * Note that image views don't store color buffer or RAT descriptors directly, instead they contain
 * data for both, but color buffers and RATs each have fields they don't use, or require specific
 * values for each field.
 * Before setting CB_COLOR[0-11] to these descriptors, pass them through
 * terakan_color_descriptor_image_view_to_color_attachment or
 * terakan_color_descriptor_image_view_to_storage_image depending on the needed binding type.
 */
struct terakan_color_descriptor {
   uint32_t base;
   uint32_t pitch;
   uint32_t slice;
   uint32_t view;
   /* In image views, the INFO register is for a color attachment. */
   uint32_t info;
   uint32_t attrib;
   /* In image views, the DIM register is for a storage image. */
   uint32_t dim;
};

static inline void
terakan_color_descriptor_image_view_to_color_attachment(
   struct terakan_color_descriptor * const descriptor)
{
   /* The meaning of DIM depends on RESOURCE_TYPE, but it's used only for RATs.
    * DIM is ignored for color attachments, scissor must be used to prevent out-of-bounds access.
    */
   descriptor->dim = 0;
}

/* The resource type must be the one actually requested by the shader in the binding declaration. */
static inline void
terakan_color_descriptor_image_view_to_storage_image(
   struct terakan_color_descriptor * const descriptor, uint32_t const resource_type)
{
   descriptor->info =
      (descriptor->info & (C_028C70_FAST_CLEAR & C_028C70_SOURCE_FORMAT)) |
      S_028C70_SOURCE_FORMAT(V_028C70_EXPORT_4C_32BPC) | S_028C70_RAT(1) |
      S_028C70_RESOURCE_TYPE(resource_type);
   descriptor->attrib &= C_028C74_FORCE_DST_ALPHA_1;
}

/* Additional hardware CB_COLOR[0-7] registers. */
struct terakan_color_meta_descriptor {
   uint32_t cmask;
   uint32_t cmask_slice;
   uint32_t fmask;
   uint32_t fmask_slice;
};

struct terakan_mutable_descriptor {
   struct terakan_winsys_bo const * bo;

   /* Hardware SQ_VTX_CONSTANT / SQ_TEX_RESOURCE. */
   uint32_t resource[8];

   struct terakan_color_descriptor color;
};

#endif /* TERAKAN_DESCRIPTOR_H */
