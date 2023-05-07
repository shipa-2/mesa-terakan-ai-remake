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

#include "terakan_entrypoints.h"
#include "terakan_physical_device.h"
#include "winsys/drm_radeon/terakan_winsys_drm_radeon.h"

#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_log.h"
#include "wsi_common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <xf86drm.h>
#endif

void
terakan_physical_device_destroy(struct vk_physical_device * const device_base)
{
   struct terakan_physical_device * const device =
      container_of(device_base, struct terakan_physical_device, vk);

   device->winsys->fn->destroy(device->winsys);

#if !defined(_WIN32)
   close(device->local_fd);
#endif

   vk_physical_device_finish(&device->vk);

   vk_free(&device->vk.instance->alloc, device);
}

VkResult
terakan_physical_device_try_create_for_drm(
   struct vk_instance * const instance_base, struct _drmDevice * const drm_device,
   struct vk_physical_device * * const physical_device_out)
{
#if defined(_WIN32)
   return vk_errorf(
      instance, VK_ERROR_INCOMPATIBLE_DRIVER,
      "Radeon Software D3DKMT winsys is not supported currently");

#else
   if (!(drm_device->available_nodes & (1 << DRM_NODE_RENDER)) ||
       drm_device->bustype != DRM_BUS_PCI ||
       drm_device->deviceinfo.pci->vendor_id != TERAKAN_ATI_VENDOR_ID) {
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }

   VkResult result;

   struct terakan_instance * const instance =
      container_of(instance_base, struct terakan_instance, vk);

   char const * const render_node_path = drm_device->nodes[DRM_NODE_RENDER];
   int const fd = open(render_node_path, O_RDWR | O_CLOEXEC);
   if (fd < 0) {
      return vk_errorf(
         instance, errno == ENOMEM ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_INCOMPATIBLE_DRIVER,
         "Failed to open the DRM device '%s': %m", render_node_path);
   }
   {
      drmVersionPtr const drm_version = drmGetVersion(fd);
      if (drm_version == NULL) {
         close(fd);
         return vk_errorf(
            instance, VK_ERROR_INCOMPATIBLE_DRIVER,
            "Failed to get the kernel driver version for the DRM device '%s': %m",
            render_node_path);
      }
      if (strcmp(drm_version->name, "radeon") != 0) {
         drmFreeVersion(drm_version);
         close(fd);
         return VK_ERROR_INCOMPATIBLE_DRIVER;
      }
      drmFreeVersion(drm_version);
   }
   if (instance->debug_flags & TERAKAN_DEBUG_STARTUP) {
      fprintf(stderr, "terakan: info: Found a compatible DRM device '%s'.\n", render_node_path);
   }

   struct terakan_physical_device * const device =
      vk_alloc2(&instance->vk.alloc, NULL, sizeof(*device), alignof(*device),
      VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (device == NULL) {
      result = vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_fd;
   }

   struct vk_physical_device_dispatch_table dispatch_table;
   vk_physical_device_dispatch_table_from_entrypoints(
      &dispatch_table, &terakan_physical_device_entrypoints, true);
   vk_physical_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_physical_device_entrypoints, false);

   result = vk_physical_device_init(&device->vk, &instance->vk, NULL, NULL, NULL, &dispatch_table);
   if (result != VK_SUCCESS) {
      goto fail_alloc;
   }

   device->local_fd = fd;

   device->winsys = terakan_winsys_drm_radeon_create(fd);
   if (device->winsys == NULL) {
      result = VK_ERROR_INCOMPATIBLE_DRIVER;
      goto fail_object;
   }

   result = VK_ERROR_INCOMPATIBLE_DRIVER;
   goto fail_winsys;

fail_winsys:
   device->winsys->fn->destroy(device->winsys);
fail_object:
   vk_physical_device_finish(&device->vk);
fail_alloc:
   vk_free(&instance->vk.alloc, device);
fail_fd:
   close(fd);
   return result;
#endif
}
