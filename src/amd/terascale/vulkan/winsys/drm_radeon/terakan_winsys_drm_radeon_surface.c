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

#include "terakan_image.h"
#include "terakan_winsys_drm_radeon.h"

#include "gallium/drivers/r600/evergreend.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "vk_format.h"

#include <assert.h>
#include <radeon_surface.h>
#include <stdint.h>
#include <string.h>

static void
terakan_winsys_drm_radeon_surface_level_drm_to_ac(
   struct radeon_surface_level const * const drm_level, struct legacy_surf_level * const level_out)
{
   level_out->offset_256B = drm_level->offset / 256;
   level_out->slice_size_dw = drm_level->slice_size / sizeof(uint32_t);
   level_out->nblk_x = drm_level->nblk_x;
   level_out->nblk_y = drm_level->nblk_y;
   level_out->mode = drm_level->mode;
}

static void
terakan_winsys_drm_radeon_surface_drm_to_ac(
   struct radeon_surface const * const drm_surface, struct radeon_surf * const surface_out)
{
   memset(surface_out, 0, sizeof(*surface_out));

   surface_out->blk_w = drm_surface->blk_w;
   surface_out->blk_h = drm_surface->blk_h;
   surface_out->bpe = drm_surface->bpe;
   surface_out->is_linear = drm_surface->level[0].mode <= RADEON_SURF_MODE_LINEAR_ALIGNED;
   surface_out->is_displayable = 1;

   surface_out->surf_alignment_log2 = util_logbase2_64(drm_surface->bo_alignment);
   surface_out->alignment_log2 = surface_out->surf_alignment_log2;

   surface_out->flags = drm_surface->flags;

   surface_out->surf_size = drm_surface->bo_size;
   surface_out->total_size = surface_out->surf_size;

   surface_out->u.legacy.bankw = drm_surface->bankw;
   surface_out->u.legacy.bankh = drm_surface->bankh;
   surface_out->u.legacy.mtilea = drm_surface->mtilea;
   surface_out->u.legacy.tile_split = drm_surface->tile_split;

   for (uint32_t level = 0; level <= drm_surface->last_level; ++level) {
      terakan_winsys_drm_radeon_surface_level_drm_to_ac(
         &drm_surface->level[level], &surface_out->u.legacy.level[level]);
      surface_out->u.legacy.tiling_index[level] = drm_surface->tiling_index[level];
   }

   if (drm_surface->flags & RADEON_SURF_SBUFFER) {
      surface_out->has_stencil = 1;

      surface_out->u.legacy.stencil_tile_split = drm_surface->stencil_tile_split;

      for (uint32_t level = 0; level <= drm_surface->last_level; ++level) {
         terakan_winsys_drm_radeon_surface_level_drm_to_ac(
            &drm_surface->stencil_level[level], &surface_out->u.legacy.zs.stencil_level[level]);
         surface_out->u.legacy.zs.stencil_tiling_index[level] =
            drm_surface->stencil_tiling_index[level];
      }
   }
}

static bool
terakan_winsys_drm_radeon_surface_translate_image_create_info(
   struct terakan_winsys const * const winsys_base,
   VkImageCreateInfo const * const image_create_info, struct radeon_surf * const surface_out)
{
   struct radeon_surface drm_surface = {
      .npix_x = image_create_info->extent.width,
      .npix_y = image_create_info->extent.height,
      .npix_z = image_create_info->extent.depth,
      .blk_w = vk_format_get_blockwidth(image_create_info->format),
      .blk_h = vk_format_get_blockheight(image_create_info->format),
      .blk_d = 1,
      .array_size = image_create_info->arrayLayers,
      .last_level = image_create_info->mipLevels - 1,
      .bpe =
         image_create_info->format == VK_FORMAT_D32_SFLOAT_S8_UINT
            ? sizeof(float)
            : vk_format_get_blocksize(image_create_info->format),
      .nsamples = (uint32_t)image_create_info->samples,
   };

   switch (image_create_info->imageType) {
   case VK_IMAGE_TYPE_1D:
      if (image_create_info->arrayLayers > 1) {
         drm_surface.flags |= RADEON_SURF_SET(RADEON_SURF_TYPE_1D_ARRAY, TYPE);
      } else {
         drm_surface.flags |= RADEON_SURF_SET(RADEON_SURF_TYPE_1D, TYPE);
      }
      break;
   case VK_IMAGE_TYPE_2D:
      if (image_create_info->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) {
         if (image_create_info->arrayLayers > 6) {
            /* Cube array layout is like 2D array. */
            drm_surface.flags |= RADEON_SURF_SET(RADEON_SURF_TYPE_2D_ARRAY, TYPE);
         } else {
            drm_surface.array_size = 1;
            drm_surface.flags |= RADEON_SURF_SET(RADEON_SURF_TYPE_CUBEMAP, TYPE);
         }
      } else {
         if (image_create_info->arrayLayers > 1) {
            drm_surface.flags |= RADEON_SURF_SET(RADEON_SURF_TYPE_2D_ARRAY, TYPE);
         } else {
            drm_surface.flags |= RADEON_SURF_SET(RADEON_SURF_TYPE_2D, TYPE);
         }
      }
      break;
   case VK_IMAGE_TYPE_3D:
      drm_surface.flags |= RADEON_SURF_SET(RADEON_SURF_TYPE_3D, TYPE);
      break;
   default:
      assert(!"Unsupported image type");
      return false;
   }

   if (vk_format_has_depth(image_create_info->format)) {
      drm_surface.flags |= RADEON_SURF_ZBUFFER;
   }
   if (vk_format_has_stencil(image_create_info->format)) {
      drm_surface.flags |= RADEON_SURF_SBUFFER | RADEON_SURF_HAS_SBUFFER_MIPTREE;
   }

   if (image_create_info->tiling == VK_IMAGE_TILING_LINEAR) {
      drm_surface.flags |= RADEON_SURF_SET(RADEON_SURF_MODE_LINEAR_ALIGNED, MODE);
   } else {
      switch (terakan_image_get_optimal_tiling_array_mode(image_create_info)) {
      case V_028C70_ARRAY_LINEAR_GENERAL:
         drm_surface.flags |= RADEON_SURF_SET(RADEON_SURF_MODE_LINEAR, MODE);
         break;
      case V_028C70_ARRAY_LINEAR_ALIGNED:
         drm_surface.flags |= RADEON_SURF_SET(RADEON_SURF_MODE_LINEAR_ALIGNED, MODE);
         break;
      case V_028C70_ARRAY_1D_TILED_THIN1:
         drm_surface.flags |= RADEON_SURF_SET(RADEON_SURF_MODE_1D, MODE);
         break;
      case V_028C70_ARRAY_2D_TILED_THIN1:
         drm_surface.flags |= RADEON_SURF_SET(RADEON_SURF_MODE_2D, MODE);
         break;
      }
   }

   struct terakan_winsys_drm_radeon const * const winsys =
      container_of(winsys_base, struct terakan_winsys_drm_radeon const, base);
   {
      int const drm_surface_best_result =
         radeon_surface_best(winsys->surface_manager, &drm_surface);
      assert(drm_surface_best_result == 0);
      if (drm_surface_best_result != 0) {
         return false;
      }
   }
   {
      int const drm_surface_init_result =
         radeon_surface_init(winsys->surface_manager, &drm_surface);
      assert(drm_surface_init_result == 0);
      if (drm_surface_init_result != 0) {
         return false;
      }
   }

   terakan_winsys_drm_radeon_surface_drm_to_ac(&drm_surface, surface_out);

   return true;
}

struct terakan_winsys_surface_fn const terakan_winsys_drm_radeon_surface_fn = {
   .translate_image_create_info = terakan_winsys_drm_radeon_surface_translate_image_create_info,
};
