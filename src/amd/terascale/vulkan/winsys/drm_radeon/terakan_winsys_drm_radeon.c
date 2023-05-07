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
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <xf86drm.h>

static bool
terakan_winsys_drm_radeon_get_drm_value(
   int const fd, unsigned const request, char const * const error_name, uint32_t * const out)
{
   struct drm_radeon_info info = {
      .request = request,
      .value = (__u64)(void const *)out,
   };
   int const result = drmCommandWriteRead(fd, DRM_RADEON_INFO, &info, sizeof(info));
   if (result != 0) {
      if (error_name != NULL) {
         fprintf(
            stderr,
            "terakan/drm_radeon: Failed to get %s from the kernel driver, error number %d.\n",
            error_name, result);
      }
      return false;
   }
   return true;
}

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
   /* Check if the kernel driver version is supported before doing any DRM queries.
    * Same DRM version requirement as in the Gallium Radeon winsys as of May 2023.
    */
   {
      drmVersionPtr const drm_version = drmGetVersion(fd);
      if (drm_version == NULL) {
         fputs(
            "terakan/drm_radeon: Failed to get the kernel driver version for the DRM device.",
            stderr);
         return NULL;
      }
      if (drm_version->version_major != 2 || drm_version->version_minor < 50) {
         fprintf(
            stderr,
            "terakan/drm_radeon: DRM version is %d.%d.%d, but this driver is only compatible with "
            "2.50.0 (kernel 4.12) or later.\n",
            drm_version->version_major, drm_version->version_minor,
            drm_version->version_patchlevel);
         drmFreeVersion(drm_version);
         return NULL;
      }
      drmFreeVersion(drm_version);
   }

   /* Get the PCI device ID. */
   uint32_t pci_id;
   if (!terakan_winsys_drm_radeon_get_drm_value(
           fd, RADEON_INFO_DEVICE_ID, "PCI device ID", &pci_id)) {
      return NULL;
   }

   /* Check if the device is supported, and initialize the new winsys. */

   struct terakan_winsys_drm_radeon * winsys = MALLOC_STRUCT(terakan_winsys_drm_radeon);
   if (winsys == NULL) {
      fputs("terakan/drm_radeon: Failed to allocate memory for the winsys structure.\n", stderr);
      return NULL;
   }
   if (!terakan_gpu_info_init_chip_family(&winsys->base.gpu_info, pci_id)) {
      /* Some other ATI/AMD GPU, not supported by Terakan. */
      goto fail_alloc;
   }
   winsys->fd = fd;

   winsys->base.fn = &terakan_winsys_drm_radeon_fn;

   return &winsys->base;

fail_alloc:
   FREE(winsys);
   return NULL;
}
