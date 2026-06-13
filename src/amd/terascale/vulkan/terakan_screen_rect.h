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

#ifndef TERAKAN_SCREEN_RECT_H
#define TERAKAN_SCREEN_RECT_H

#include "util/macros.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Note that in registers like scissor and clip rectangles, the left and top coordinates can't be
 * `TERAKAN_IMAGE_MAX_WIDTH_HEIGHT` in the hardware, so in the edge case where the rectangle is
 * empty, but its left or top coordinate is `TERAKAN_IMAGE_MAX_WIDTH_HEIGHT`, it must be replaced
 * with a rectangle within the allowed value range.
 */

struct terakan_screen_rect {
   /* [TL, BR][X, Y]. */
   uint16_t bounds[2][2];
};

static inline bool
terakan_screen_rect_equal(struct terakan_screen_rect const a, struct terakan_screen_rect const b)
{
   return memcmp(a.bounds, b.bounds, sizeof(a.bounds)) == 0;
}

static inline bool
terakan_screen_rect_is_empty(struct terakan_screen_rect const rect)
{
   return rect.bounds[0][0] >= rect.bounds[1][0] || rect.bounds[0][1] >= rect.bounds[1][1];
}

static inline struct terakan_screen_rect
terakan_screen_rect_intersect(struct terakan_screen_rect const a,
                              struct terakan_screen_rect const b)
{
   return (struct terakan_screen_rect){
      .bounds =
         {
            {MAX2(a.bounds[0][0], b.bounds[0][0]), MAX2(a.bounds[0][1], b.bounds[0][1])},
            {MIN2(a.bounds[1][0], b.bounds[1][0]), MIN2(a.bounds[1][1], b.bounds[1][1])},
         },
   };
}

static inline struct terakan_screen_rect
terakan_screen_rect_square(uint16_t extent)
{
   return (struct terakan_screen_rect){.bounds = {[1] = {extent, extent}}};
}

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_SCREEN_RECT_H */
