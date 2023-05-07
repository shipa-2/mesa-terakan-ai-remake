/*
 * Copyright © 2023 Vitaliy Triang3l Kuzmin
 *
 * Based on Gallium Radeon DRM winsys which is:
 * Copyright © 2008 Jérôme Glisse
 * Copyright © 2009 Corbin Simpson <MostAwesomeDude@gmail.com>
 * Copyright © 2011 Marek Olšák <maraeo@gmail.com>
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

#include "terakan_winsys_drm_radeon.h"

#include "util/macros.h"
#include "util/u_memory.h"

#include <radeon_drm.h>
#include <stddef.h>
#include <stdio.h>

static void
terakan_winsys_drm_radeon_destroy(struct terakan_winsys * const winsys_base)
{
   struct terakan_winsys_drm_radeon * const winsys =
      container_of(winsys_base, struct terakan_winsys_drm_radeon, base);

   FREE(winsys);
}

static struct terakan_winsys_fn const terakan_winsys_drm_radeon_fn = {
   .destroy = terakan_winsys_drm_radeon_destroy,
};

struct terakan_winsys *
terakan_winsys_drm_radeon_create(int const fd)
{
   struct terakan_winsys_drm_radeon * winsys = MALLOC_STRUCT(terakan_winsys_drm_radeon);
   if (winsys == NULL) {
      fputs("terakan/drm_radeon: Failed to allocate memory for the winsys structure.\n", stderr);
      return NULL;
   }
   winsys->fd = fd;
   winsys->base.fn = &terakan_winsys_drm_radeon_fn;

   return &winsys->base;
}
