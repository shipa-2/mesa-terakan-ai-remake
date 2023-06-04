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
#include "util/os_mman.h"
#include "util/u_memory.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <radeon_drm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <xf86drm.h>

static void *
terakan_winsys_drm_radeon_bo_map(struct terakan_winsys_bo * const bo_base)
{
   struct terakan_winsys_drm_radeon_bo * const bo =
      container_of(bo_base, struct terakan_winsys_drm_radeon_bo, base);

   if (bo->mapping != NULL) {
      return bo->mapping;
   }

   struct terakan_winsys_drm_radeon const * const winsys =
      container_of(bo->base.winsys, struct terakan_winsys_drm_radeon const, base);

   /* The offset and size fields are ignored by the Radeon kernel driver as of Radeon 2.50.0, but
    * for correctness, still passing the size.
    */
   struct drm_radeon_gem_mmap gem_mmap_arguments = {
      .handle = bo->handle,
      .size = bo->size,
   };
   int const gem_mmap_result = drmCommandWriteRead(
      winsys->fd, DRM_RADEON_GEM_MMAP, &gem_mmap_arguments, sizeof(gem_mmap_arguments));
   if (gem_mmap_result != 0) {
      fprintf(
         stderr,
         "terakan/drm_radeon: Failed to map the buffer 0x%" PRIX32 " in GEM, error number %d.\n",
         (uint32_t)bo->handle, gem_mmap_result);
      return NULL;
   }

   void * const mapping = os_mmap(
      NULL, (size_t)bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, winsys->fd,
      (off_t)gem_mmap_arguments.addr_ptr);
   if (mapping == MAP_FAILED) {
      fprintf(
         stderr,
         "terakan/drm_radeon: Failed to map the buffer 0x%" PRIX32 " in the OS, error number %d.\n",
         (uint32_t)bo->handle, errno);
      return NULL;
   }

   bo->mapping = mapping;

   return mapping;
}

static void
terakan_winsys_drm_radeon_bo_unmap(struct terakan_winsys_bo * const bo_base)
{
   struct terakan_winsys_drm_radeon_bo * const bo =
      container_of(bo_base, struct terakan_winsys_drm_radeon_bo, base);

   if (bo->mapping == NULL) {
      return;
   }

   os_munmap(bo->mapping, (size_t)bo->size);
   bo->mapping = NULL;
}

static bool
terakan_winsys_drm_radeon_bo_wait_idle(struct terakan_winsys_bo * const bo_base)
{
   struct terakan_winsys_drm_radeon_bo const * const bo =
      container_of(bo_base, struct terakan_winsys_drm_radeon_bo const, base);
   struct terakan_winsys_drm_radeon const * const winsys =
      container_of(bo->base.winsys, struct terakan_winsys_drm_radeon const, base);

   struct drm_radeon_gem_wait_idle gem_wait_idle_arguments = {
      .handle = bo->handle,
   };
   /* Returns -EBUSY in finite time in case of a hang (30-second timeout in Linux Radeon 2.50.0). */
   return drmCommandWrite(
      winsys->fd, DRM_RADEON_GEM_WAIT_IDLE, &gem_wait_idle_arguments,
      sizeof(gem_wait_idle_arguments)) == 0;
}

static void
terakan_winsys_drm_radeon_bo_free(struct terakan_winsys_bo * const bo_base)
{
   struct terakan_winsys_drm_radeon_bo * const bo =
      container_of(bo_base, struct terakan_winsys_drm_radeon_bo, base);
   struct terakan_winsys_drm_radeon const * const winsys =
      container_of(bo->base.winsys, struct terakan_winsys_drm_radeon const, base);

   terakan_winsys_drm_radeon_bo_unmap(&bo->base);

   struct drm_gem_close gem_close_arguments = {
      .handle = bo->handle,
   };
   drmIoctl(winsys->fd, DRM_IOCTL_GEM_CLOSE, &gem_close_arguments);

   FREE(bo);
}

static struct terakan_winsys_bo *
terakan_winsys_drm_radeon_bo_allocate_device_memory(
   struct terakan_winsys * const winsys_base, VkDeviceSize const size, VkDeviceSize const alignment,
   VkMemoryPropertyFlags const flags)
{
   struct terakan_winsys_drm_radeon * const winsys =
      container_of(winsys_base, struct terakan_winsys_drm_radeon, base);

   struct drm_radeon_gem_create gem_create_arguments = {};

   gem_create_arguments.size = size;
   gem_create_arguments.alignment = alignment;

   __u32 initial_domains = 0;
   if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
      initial_domains |= RADEON_GEM_DOMAIN_VRAM;
      /* If VRAM is just stolen system memory, allow both VRAM and GTT, whichever has free space.
       * If a buffer is evicted from VRAM to GTT, it will stay there.
       */
      if (!winsys->base.gpu_info.has_dedicated_vram) {
         initial_domains |= RADEON_GEM_DOMAIN_GTT;
      }
   } else {
      initial_domains |= RADEON_GEM_DOMAIN_GTT;
   }
   gem_create_arguments.initial_domain = initial_domains;

   if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      gem_create_arguments.flags |= RADEON_GEM_CPU_ACCESS;
      if (!(flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
         gem_create_arguments.flags |= RADEON_GEM_GTT_WC;
      }
   } else {
      gem_create_arguments.flags |= RADEON_GEM_NO_CPU_ACCESS;
   }

   int const gem_create_result = drmCommandWriteRead(
      winsys->fd, DRM_RADEON_GEM_CREATE, &gem_create_arguments, sizeof(gem_create_arguments));
   if (gem_create_result != 0) {
      fprintf(
         stderr, "terakan/drm_radeon: Failed to allocate a buffer, error number %d:\n",
         gem_create_result);
      fprintf(stderr, "terakan/drm_radeon:    Size: %" PRIu64 " bytes\n", size);
      fprintf(
         stderr, "terakan/drm_radeon:    Alignment: %" PRIu64 " bytes\n", alignment);
      fprintf(
         stderr, "terakan/drm_radeon:    Domains: 0x%" PRIX32 "\n", (uint32_t)initial_domains);
      fprintf(
         stderr, "terakan/drm_radeon:    Flags: 0x%" PRIX32 "\n",
         (uint32_t)gem_create_arguments.flags);
      return NULL;
   }

   struct terakan_winsys_drm_radeon_bo * const bo = MALLOC_STRUCT(terakan_winsys_drm_radeon_bo);
   if (bo == NULL) {
      fputs(
         "terakan/drm_radeon: Failed to allocate memory for the winsys buffer structure.\n",
         stderr);
      struct drm_gem_close gem_close_arguments = {
         .handle = bo->handle,
      };
      drmIoctl(winsys->fd, DRM_IOCTL_GEM_CLOSE, &gem_close_arguments);
      return NULL;
   }

   terakan_winsys_bo_base_init(&bo->base, &winsys->base);

   bo->size = size;

   bo->domains = initial_domains;

   bo->handle = gem_create_arguments.handle;

   bo->mapping = NULL;

   return &bo->base;
}

struct terakan_winsys_bo_fn const terakan_winsys_drm_radeon_bo_fn = {
   .map = terakan_winsys_drm_radeon_bo_map,
   .unmap = terakan_winsys_drm_radeon_bo_unmap,
   .free = terakan_winsys_drm_radeon_bo_free,
   .wait_idle = terakan_winsys_drm_radeon_bo_wait_idle,
   .allocate_device_memory = terakan_winsys_drm_radeon_bo_allocate_device_memory,
};
