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

/* RV710's real 2D-tiled and 1D-tiled alignments for bpe=4, samples=1 (from test_2d_tiled_thin1()/
 * test_1d_tiled_thin1() above): 2D pitch/height alignment 128/16, base BO alignment 8192; 1D
 * pitch/height alignment 16/8, base BO alignment 512.
 */
static struct terakan_image_tiling_terascale_1_alignments const rv710_2d_alignments_bpe4 = {
   .pitch_surfels = 128, .height_surfels = 16, .base_level_bo_alignment_bytes = 8192,
};
static struct terakan_image_tiling_terascale_1_alignments const rv710_1d_alignments_bpe4 = {
   .pitch_surfels = 16, .height_surfels = 8, .base_level_bo_alignment_bytes = 512,
};

static void
test_mip_chain_layout_no_degrade(void)
{
   /* 300x50, 3 levels, single layer, no 3D depth. Every level stays large enough to remain
    * 2D-tiled (hand-derived from terakan_image_tiling_terascale_1_mip_extent()/_level_layout()'s
    * own already-tested formulas -- see the comment in the corresponding commit for the full
    * derivation): level 0 300x50 -> aligned 384x64, pitch_bytes=1536, slice=98304; level 1 minifies
    * to 150x25 -> POT-rounded 256x32 -> aligned 256x32 (already aligned), pitch_bytes=1024,
    * slice=32768; level 2 minifies to 75x12 -> POT-rounded 128x16 -> aligned 128x16 (exactly at the
    * alignment boundary, not below it, so still no degrade), pitch_bytes=512, slice=8192.
    */
   struct terakan_image_tiling_terascale_1_mip_chain_level levels[3];
   uint64_t const total = terakan_image_tiling_terascale_1_mip_chain_layout(
      300, 50, 1, false, 3, 1, 1, 1, 4, 1, &rv710_2d_alignments_bpe4,
      &rv710_1d_alignments_bpe4, levels);

   CHECK(!levels[0].is_1d_tiled_thin1_or_fixed);
   CHECK(levels[0].offset_bytes == 0);
   CHECK(levels[0].aligned_pitch_surfels == 384);
   CHECK(levels[0].aligned_height_surfels == 64);
   CHECK(levels[0].pitch_bytes == 1536);
   CHECK(levels[0].slice_bytes == 98304);
   CHECK(levels[0].depth_planes_or_array_layers == 1);
   /* Level 0's slice (98304) is already a multiple of the 2D base BO alignment (8192), so the
    * level-0 alignment step is a no-op here -- level 1 starts right after it.
    */
   CHECK(levels[1].offset_bytes == 98304);
   CHECK(!levels[1].is_1d_tiled_thin1_or_fixed);
   CHECK(levels[1].aligned_pitch_surfels == 256);
   CHECK(levels[1].aligned_height_surfels == 32);
   CHECK(levels[1].pitch_bytes == 1024);
   CHECK(levels[2].offset_bytes == 98304 + 32768);
   CHECK(!levels[2].is_1d_tiled_thin1_or_fixed);
   CHECK(levels[2].aligned_pitch_surfels == 128);
   CHECK(levels[2].aligned_height_surfels == 16);
   CHECK(levels[2].pitch_bytes == 512);
   CHECK(total == 98304 + 32768 + 8192);
}

static void
test_mip_chain_layout_degrades(void)
{
   /* 100x50: level 0 is already smaller than the 2D pitch alignment (128), so the whole chain
    * degrades to 1D-tiled starting at level 0, and stays 1D-tiled for level 1 too (a 2D-tiled chain
    * never reverts once degraded, matching r6_surface_init_2d() calling back into
    * r6_surface_init_1d() for the rest of the chain in the reference). Level 0 aligned (1D
    * alignment 16/8): 100x50 -> 112x56, pitch_bytes=448, slice=25088, already a multiple of the 1D
    * base BO alignment (512). Level 1 minifies to 50x25 -> POT-rounded 64x32 -> aligned 64x32
    * (already aligned to 16/8), pitch_bytes=256, slice=8192.
    */
   struct terakan_image_tiling_terascale_1_mip_chain_level levels[2];
   uint64_t const total = terakan_image_tiling_terascale_1_mip_chain_layout(
      100, 50, 1, false, 2, 1, 1, 1, 4, 1, &rv710_2d_alignments_bpe4,
      &rv710_1d_alignments_bpe4, levels);

   CHECK(levels[0].is_1d_tiled_thin1_or_fixed);
   CHECK(levels[0].offset_bytes == 0);
   CHECK(levels[0].aligned_pitch_surfels == 112);
   CHECK(levels[0].aligned_height_surfels == 56);
   CHECK(levels[0].pitch_bytes == 448);
   CHECK(levels[1].is_1d_tiled_thin1_or_fixed);
   CHECK(levels[1].offset_bytes == 25088);
   CHECK(levels[1].aligned_pitch_surfels == 64);
   CHECK(levels[1].aligned_height_surfels == 32);
   CHECK(levels[1].pitch_bytes == 256);
   CHECK(total == 25088 + 8192);
}

static void
test_mip_chain_layout_fixed_1d(void)
{
   /* alignments_2d == NULL: every level uses the 1D alignment unconditionally, no degrade check at
    * all (matching a chain whose base array mode was already chosen as 1D-tiled or linear-aligned
    * by terakan_image_tiling_terascale_1_select_array_mode(), which never attempts 2D tiling in the
    * first place). 50x30 -> aligned 64x32, pitch_bytes=256, slice=8192, a multiple of the 512-byte
    * base BO alignment. Level 1 minifies to 25x15 -> POT-rounded 32x16 -> aligned 32x16 (already
    * aligned), pitch_bytes=128, slice=2048.
    */
   struct terakan_image_tiling_terascale_1_mip_chain_level levels[2];
   uint64_t const total = terakan_image_tiling_terascale_1_mip_chain_layout(
      50, 30, 1, false, 2, 1, 1, 1, 4, 1, NULL, &rv710_1d_alignments_bpe4, levels);

   CHECK(levels[0].is_1d_tiled_thin1_or_fixed);
   CHECK(levels[0].offset_bytes == 0);
   CHECK(levels[0].pitch_bytes == 256);
   CHECK(levels[1].offset_bytes == 8192);
   CHECK(levels[1].pitch_bytes == 128);
   CHECK(total == 8192 + 2048);
}

static void
test_mip_chain_layout_array_layers(void)
{
   /* Same base level as test_mip_chain_layout_no_degrade's level 0 (300x50, no degrade), but with 3
    * array layers instead of 1: the per-level size is multiplied by array_layers, matching
    * r6_surface_init_2d()'s own `surf->bo_size = offset + slice_size * nblk_z * surf->array_size`.
    */
   struct terakan_image_tiling_terascale_1_mip_chain_level levels[1];
   uint64_t const total = terakan_image_tiling_terascale_1_mip_chain_layout(
      300, 50, 3, false, 1, 1, 1, 1, 4, 1, &rv710_2d_alignments_bpe4,
      &rv710_1d_alignments_bpe4, levels);
   CHECK(total == 98304 * 3);
   CHECK(levels[0].slice_bytes == 98304);
   CHECK(levels[0].depth_planes_or_array_layers == 3);
}

static void
test_mip_chain_layout_3d_depth(void)
{
   /* depth_minifies_per_level = true: base_depth_or_array_layers (3) is mip-minified per level like
    * width/height, instead of staying constant like an array layer count. Level 0's depth stays 3
    * (u_minify(3, 0) = 3, no power-of-two rounding for level 0); level 1's depth minifies to
    * u_minify(3, 1) = 1, rounded up to the next power of two (still 1, already a power of two).
    */
   struct terakan_image_tiling_terascale_1_mip_chain_level levels[2];
   uint64_t const total = terakan_image_tiling_terascale_1_mip_chain_layout(
      300, 50, 3, true, 2, 1, 1, 1, 4, 1, &rv710_2d_alignments_bpe4,
      &rv710_1d_alignments_bpe4, levels);
   /* Level 0: slice 98304 * 3 depth planes = 294912, already a multiple of the 8192-byte base BO
    * alignment. Level 1: slice 32768 * 1 depth plane = 32768.
    */
   CHECK(levels[0].depth_planes_or_array_layers == 3);
   CHECK(levels[1].depth_planes_or_array_layers == 1);
   CHECK(levels[1].offset_bytes == 294912);
   CHECK(total == 294912 + 32768);
}

static void
test_mip_chain_layout_block_compressed(void)
{
   /* BC1 uses 4x4 texel blocks with 8 bytes per block. On the real RV710 topology, its 2D
    * alignment is 64x16 blocks. Minification must happen in texels before conversion to blocks:
    * 1023x511 becomes 256x128 blocks at level 0, then 512x256 texels -> 128x64 blocks at level 1.
    */
   struct terakan_image_tiling_terascale_1_alignments const alignments_2d =
      terakan_image_tiling_terascale_1_alignments_2d_tiled_thin1(
         RV710_GROUP_BYTES, RV710_NUM_PIPES, RV710_NUM_BANKS, 8, 1, false);
   struct terakan_image_tiling_terascale_1_alignments const alignments_1d =
      terakan_image_tiling_terascale_1_alignments_1d_tiled_thin1(
         RV710_GROUP_BYTES, 8, 1, false);
   struct terakan_image_tiling_terascale_1_mip_chain_level levels[4];
   uint64_t const total = terakan_image_tiling_terascale_1_mip_chain_layout(
      1023, 511, 1, false, 4, 4, 4, 1, 8, 1, &alignments_2d, &alignments_1d, levels);

   CHECK(levels[0].aligned_pitch_surfels == 256);
   CHECK(levels[0].aligned_height_surfels == 128);
   CHECK(levels[0].slice_bytes == 262144);
   CHECK(levels[1].offset_bytes == 262144);
   CHECK(levels[1].aligned_pitch_surfels == 128);
   CHECK(levels[1].aligned_height_surfels == 64);
   CHECK(levels[1].slice_bytes == 65536);
   CHECK(!levels[2].is_1d_tiled_thin1_or_fixed);
   CHECK(levels[2].aligned_pitch_surfels == 64);
   CHECK(levels[2].aligned_height_surfels == 32);
   CHECK(levels[3].is_1d_tiled_thin1_or_fixed);
   CHECK(levels[3].aligned_pitch_surfels == 32);
   CHECK(levels[3].aligned_height_surfels == 16);
   CHECK(total == 262144 + 65536 + 16384 + 4096);
}

static void
test_mip_chain_layout_expand_3x(void)
{
   /* Linear R32G32B32 on the real RV710 topology stores three 4-byte surfels per texel. Width is
    * expanded only after texel minification. At level 0, 81 texels become 243 surfels, then the
    * descriptor-facing base-pitch rule aligns to 3 * 128 = 384 surfels. The 5-row slice is 7680
    * bytes and is already 512-byte aligned. At level 1, 40 texels become 120 surfels and are then
    * power-of-two padded to 128; the base-only 3x pitch rule no longer applies. Height 2 is already
    * a power of two, so the second slice is 1024 bytes.
    *
    * This distinguishes the fix from treating a 12-byte RGB texel as one element (which gives a
    * 64-surfel pitch alignment and a 192-surfel base pitch), applying mip power-of-two padding
    * before expanding (which gives 256 surfels at level 1), and omitting the base 3x pitch rule
    * (which gives a 256-surfel base pitch).
    */
   struct terakan_image_tiling_terascale_1_alignments const linear_bpe4 =
      terakan_image_tiling_terascale_1_alignments_linear_aligned(RV710_GROUP_BYTES, 4);
   struct terakan_image_tiling_terascale_1_mip_chain_level levels[2];
   uint64_t const total = terakan_image_tiling_terascale_1_mip_chain_layout(
      81, 5, 1, false, 2, 1, 1, 3, 4, 1, NULL, &linear_bpe4, levels);

   CHECK(levels[0].offset_bytes == 0);
   CHECK(levels[0].aligned_pitch_surfels == 384);
   CHECK(levels[0].aligned_height_surfels == 5);
   CHECK(levels[0].pitch_bytes == 1536);
   CHECK(levels[0].slice_bytes == 7680);
   CHECK(levels[1].offset_bytes == 7680);
   CHECK(levels[1].aligned_pitch_surfels == 128);
   CHECK(levels[1].aligned_height_surfels == 2);
   CHECK(levels[1].pitch_bytes == 512);
   CHECK(levels[1].slice_bytes == 1024);
   CHECK(total == 8704);
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
   test_mip_chain_layout_no_degrade();
   test_mip_chain_layout_degrades();
   test_mip_chain_layout_fixed_1d();
   test_mip_chain_layout_array_layers();
   test_mip_chain_layout_3d_depth();
   test_mip_chain_layout_block_compressed();
   test_mip_chain_layout_expand_3x();
   return 0;
}
