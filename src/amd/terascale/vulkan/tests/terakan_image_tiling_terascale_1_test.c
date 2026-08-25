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

int
main(void)
{
   test_linear_aligned();
   test_1d_tiled_thin1();
   test_2d_tiled_thin1();
   return 0;
}
