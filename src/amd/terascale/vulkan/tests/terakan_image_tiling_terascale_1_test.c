/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_image_tiling_terascale_1.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                                          \
   do {                                                                                            \
      if (!(condition)) {                                                                          \
         fprintf(stderr, "CHECK failed at %s:%u: %s\n", __FILE__, __LINE__, #condition);           \
         abort();                                                                                  \
      }                                                                                            \
   } while (0)

/* group_bytes=512, num_pipes=2, num_banks=8: the RV710 in the dual-GPU test machine's real
 * RADEON_INFO_TILING_CONFIG decode (terakan_physical_device_tiling_config_test), not arbitrary
 * numbers.
 */
#define RV710_GROUP_BYTES 512u
#define RV710_NUM_PIPES 2u
#define RV710_NUM_BANKS 8u

static void
test_linear_aligned(void)
{
   /* bpe=4 (e.g. a 32-bit format): xalign = MAX2(64, 512/4=128) = 128. */
   struct terakan_image_tiling_terascale_1_alignments const bpe4 =
      terakan_image_tiling_terascale_1_alignments_linear_aligned(RV710_GROUP_BYTES, 4);
   CHECK(bpe4.pitch_surfels == 128);
   CHECK(bpe4.height_surfels == 1);
   CHECK(bpe4.base_level_bo_alignment_bytes == 512);

   /* bpe=1 (e.g. stencil): xalign = MAX2(64, 512/1=512) = 512. */
   struct terakan_image_tiling_terascale_1_alignments const bpe1 =
      terakan_image_tiling_terascale_1_alignments_linear_aligned(RV710_GROUP_BYTES, 1);
   CHECK(bpe1.pitch_surfels == 512);
   CHECK(bpe1.height_surfels == 1);
   CHECK(bpe1.base_level_bo_alignment_bytes == 512);

   /* A small group_bytes/bpe ratio hits the 64-surfel floor instead of the division result:
    * group_bytes=256, bpe=16 -> 256/16=16, floored to 64.
    */
   struct terakan_image_tiling_terascale_1_alignments const floored =
      terakan_image_tiling_terascale_1_alignments_linear_aligned(256, 16);
   CHECK(floored.pitch_surfels == 64);
   CHECK(floored.base_level_bo_alignment_bytes == 256);
}

static void
test_1d_tiled_thin1(void)
{
   /* bpe=4, samples=1: xalign = MAX2(8, 512/(8*4*1)=16) = 16. yalign = 8. */
   struct terakan_image_tiling_terascale_1_alignments const bpe4 =
      terakan_image_tiling_terascale_1_alignments_1d_tiled_thin1(RV710_GROUP_BYTES, 4, 1, false);
   CHECK(bpe4.pitch_surfels == 16);
   CHECK(bpe4.height_surfels == 8);
   CHECK(bpe4.base_level_bo_alignment_bytes == 512);

   /* bpe=1, samples=1: xalign = MAX2(8, 512/(8*1*1)=64) = 64. */
   struct terakan_image_tiling_terascale_1_alignments const bpe1 =
      terakan_image_tiling_terascale_1_alignments_1d_tiled_thin1(RV710_GROUP_BYTES, 1, 1, false);
   CHECK(bpe1.pitch_surfels == 64);
   CHECK(bpe1.height_surfels == 8);

   /* The tile-width-height floor (8) applies when the division result is smaller: group_bytes=64,
    * bpe=4, samples=1 -> 64/(8*4*1)=2, floored to 8.
    */
   struct terakan_image_tiling_terascale_1_alignments const floored =
      terakan_image_tiling_terascale_1_alignments_1d_tiled_thin1(64, 4, 1, false);
   CHECK(floored.pitch_surfels == 8);

   /* Scanout applies a further floor: bpe=4 -> 32 surfels, overriding a smaller computed value. */
   struct terakan_image_tiling_terascale_1_alignments const scanout =
      terakan_image_tiling_terascale_1_alignments_1d_tiled_thin1(64, 4, 1, true);
   CHECK(scanout.pitch_surfels == 32);
   /* bpe=1 scanout floor is 64 instead of 32. */
   struct terakan_image_tiling_terascale_1_alignments const scanout_bpe1 =
      terakan_image_tiling_terascale_1_alignments_1d_tiled_thin1(64, 1, 1, true);
   CHECK(scanout_bpe1.pitch_surfels == 64);
}

static void
test_2d_tiled_thin1(void)
{
   /* bpe=4, samples=1: xalign = MAX2(8*8=64, (512*8)/(8*4*1)=128) = 128. yalign = 8*2 = 16.
    * bo_alignment = MAX2(2*8*1*4*64=4096, 128*16*1*4=8192) = 8192.
    */
   struct terakan_image_tiling_terascale_1_alignments const bpe4 =
      terakan_image_tiling_terascale_1_alignments_2d_tiled_thin1(
         RV710_GROUP_BYTES, RV710_NUM_PIPES, RV710_NUM_BANKS, 4, 1, false);
   CHECK(bpe4.pitch_surfels == 128);
   CHECK(bpe4.height_surfels == 16);
   CHECK(bpe4.base_level_bo_alignment_bytes == 8192);

   /* bpe=1, samples=1: xalign = MAX2(64, (512*8)/(8*1*1)=512) = 512. yalign = 16.
    * bo_alignment = MAX2(2*8*1*1*64=1024, 512*16*1*1=8192) = 8192.
    */
   struct terakan_image_tiling_terascale_1_alignments const bpe1 =
      terakan_image_tiling_terascale_1_alignments_2d_tiled_thin1(
         RV710_GROUP_BYTES, RV710_NUM_PIPES, RV710_NUM_BANKS, 1, 1, false);
   CHECK(bpe1.pitch_surfels == 512);
   CHECK(bpe1.height_surfels == 16);
   CHECK(bpe1.base_level_bo_alignment_bytes == 8192);

   /* The tile_width_height*num_banks floor (64) applies when the division result is smaller:
    * group_bytes=64, num_banks=8, bpe=4, samples=1 -> (64*8)/(8*4*1)=16, floored to 8*8=64.
    */
   struct terakan_image_tiling_terascale_1_alignments const floored =
      terakan_image_tiling_terascale_1_alignments_2d_tiled_thin1(64, RV710_NUM_PIPES, RV710_NUM_BANKS,
                                                                  4, 1, false);
   CHECK(floored.pitch_surfels == 64);
}

static void
test_mip_extent(void)
{
   /* Level 0 is plain u_minify, no power-of-two rounding. */
   CHECK(terakan_image_tiling_terascale_1_mip_extent(100, 0) == 100);
   /* Level > 0: u_minify(100, 1) = 50, rounded up to the next power of two, 64. */
   CHECK(terakan_image_tiling_terascale_1_mip_extent(100, 1) == 64);
   /* u_minify(100, 2) = 25, rounded up to 32. */
   CHECK(terakan_image_tiling_terascale_1_mip_extent(100, 2) == 32);
   /* u_minify floors at 1 (never 0), and next_power_of_two(1) = 1. */
   CHECK(terakan_image_tiling_terascale_1_mip_extent(1, 5) == 1);
   /* An already-power-of-two minified size stays unchanged. */
   CHECK(terakan_image_tiling_terascale_1_mip_extent(17, 1) == 8);
}

static void
test_level_layout(void)
{
   /* RV710's real 2D-tiled alignment for bpe=4, samples=1 (from test_2d_tiled_thin1 above):
    * pitch_alignment=128, height_alignment=16.
    */
   uint32_t const pitch_alignment = 128, height_alignment = 16;

   /* Large enough not to degrade: aligned_pitch = ALIGN_POT(300, 128) = 384,
    * aligned_height = ALIGN_POT(50, 16) = 64. pitch_bytes = 384*4 = 1536,
    * slice_bytes = 1536*64 = 98304.
    */
   struct terakan_image_tiling_terascale_1_level_layout const large =
      terakan_image_tiling_terascale_1_level_layout(300, 50, pitch_alignment, height_alignment, 4, 1,
                                                     true);
   CHECK(!large.degrades_to_1d_tiled_thin1);
   CHECK(large.aligned_pitch_surfels == 384);
   CHECK(large.aligned_height_surfels == 64);
   CHECK(large.pitch_bytes == 1536);
   CHECK(large.slice_bytes == 98304);

   /* Smaller than the pitch alignment: degrades to 1D, per surf_minify()'s own check. */
   struct terakan_image_tiling_terascale_1_level_layout const small_pitch =
      terakan_image_tiling_terascale_1_level_layout(100, 50, pitch_alignment, height_alignment, 4, 1,
                                                     true);
   CHECK(small_pitch.degrades_to_1d_tiled_thin1);

   /* Smaller than the height alignment: also degrades. */
   struct terakan_image_tiling_terascale_1_level_layout const small_height =
      terakan_image_tiling_terascale_1_level_layout(300, 8, pitch_alignment, height_alignment, 4, 1,
                                                     true);
   CHECK(small_height.degrades_to_1d_tiled_thin1);

   /* The degrade check is skipped when is_2d_tiled_single_sample is false (e.g. computing a 1D-tiled
    * or multisampled level, which never degrades further): the same small dimensions as above just
    * get aligned up instead.
    */
   struct terakan_image_tiling_terascale_1_level_layout const not_2d_single_sample =
      terakan_image_tiling_terascale_1_level_layout(100, 50, pitch_alignment, height_alignment, 4, 1,
                                                     false);
   CHECK(!not_2d_single_sample.degrades_to_1d_tiled_thin1);
   CHECK(not_2d_single_sample.aligned_pitch_surfels == 128);
   CHECK(not_2d_single_sample.aligned_height_surfels == 64);
}

static void
test_select_array_mode(void)
{
   /* Default: a normal 2D image with no linear-forcing condition tiles. */
   CHECK(terakan_image_tiling_terascale_1_select_array_mode(false, false, false, false, false,
                                                             false, false) ==
        TERAKAN_IMAGE_TILING_TERASCALE_1_ARRAY_2D_TILED_THIN1);

   /* VK_IMAGE_TILING_LINEAR forces linear regardless of anything else. */
   CHECK(terakan_image_tiling_terascale_1_select_array_mode(true, false, false, true, true, true,
                                                             false) ==
        TERAKAN_IMAGE_TILING_TERASCALE_1_ARRAY_LINEAR_ALIGNED);

   /* The debug override forces linear too. */
   CHECK(terakan_image_tiling_terascale_1_select_array_mode(false, true, false, false, false, false,
                                                             false) ==
        TERAKAN_IMAGE_TILING_TERASCALE_1_ARRAY_LINEAR_ALIGNED);

   /* A format that must stay linear (e.g. FMT_32_32_32) forces linear even for a normal 2D image. */
   CHECK(terakan_image_tiling_terascale_1_select_array_mode(false, false, true, false, false, false,
                                                             false) ==
        TERAKAN_IMAGE_TILING_TERASCALE_1_ARRAY_LINEAR_ALIGNED);

   /* A non-DB, non-multisampled, non-tiled-only-format 1D image type prefers linear (more compact,
    * matching the classic Gallium R600 driver's own preference for 1D storage images).
    */
   CHECK(terakan_image_tiling_terascale_1_select_array_mode(false, false, false, false, false, false,
                                                             true) ==
        TERAKAN_IMAGE_TILING_TERASCALE_1_ARRAY_LINEAR_ALIGNED);

   /* A depth/stencil (used_by_db) 1D-image-type target must stay tiled -- R600/R700 zbuffers only
    * support 1D-tiled or 2D-tiled surfaces, never plain linear, matching r6_surface_init()'s own
    * "zbuffer only support 1D or 2D tiled surface" handling in the reference.
    */
   CHECK(terakan_image_tiling_terascale_1_select_array_mode(false, false, false, true, false, false,
                                                             true) ==
        TERAKAN_IMAGE_TILING_TERASCALE_1_ARRAY_2D_TILED_THIN1);

   /* A multisampled 1D-image-type target also stays tiled -- MSAA surfaces must be 2D-tiled per
    * r6_surface_init()'s own handling.
    */
   CHECK(terakan_image_tiling_terascale_1_select_array_mode(false, false, false, false, true, false,
                                                             true) ==
        TERAKAN_IMAGE_TILING_TERASCALE_1_ARRAY_2D_TILED_THIN1);

   /* A tiled-only-format (e.g. a block-compressed format) 1D-image-type target also stays tiled. */
   CHECK(terakan_image_tiling_terascale_1_select_array_mode(false, false, false, false, false, true,
                                                             true) ==
        TERAKAN_IMAGE_TILING_TERASCALE_1_ARRAY_2D_TILED_THIN1);
}

int
main(void)
{
   test_linear_aligned();
   test_1d_tiled_thin1();
   test_2d_tiled_thin1();
   test_mip_extent();
   test_level_layout();
   test_select_array_mode();
   return 0;
}
