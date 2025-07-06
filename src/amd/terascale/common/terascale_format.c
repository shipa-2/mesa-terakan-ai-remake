/*
 * Copyright © 2024 Vitaliy Triang3l Kuzmin
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

#include "terascale_format.h"

#include "util/format_r11g11b10f.h"
#include "util/format_rgb9e5.h"
#include "util/format_srgb.h"
#include "util/half_float.h"
#include "util/macros.h"
#include "util/u_math.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

const uint8_t terascale_format_block_texels_log2[1 << 6][2] = {
   [TERASCALE_FORMAT_INDEX_1] = {3, 0},     [TERASCALE_FORMAT_INDEX_1_REVERSED] = {3, 0},
   [TERASCALE_FORMAT_INDEX_GB_GR] = {1, 0}, [TERASCALE_FORMAT_INDEX_BG_RG] = {1, 0},
   [TERASCALE_FORMAT_INDEX_BC1] = {2, 2},   [TERASCALE_FORMAT_INDEX_BC2] = {2, 2},
   [TERASCALE_FORMAT_INDEX_BC3] = {2, 2},   [TERASCALE_FORMAT_INDEX_BC4] = {2, 2},
   [TERASCALE_FORMAT_INDEX_BC5] = {2, 2},   [TERASCALE_FORMAT_INDEX_BC6] = {2, 2},
   [TERASCALE_FORMAT_INDEX_BC7] = {2, 2},   [TERASCALE_FORMAT_INDEX_CTX1] = {2, 2},
};

const uint8_t terascale_format_bytes_per_block[1 << 6] = {
   [TERASCALE_FORMAT_INDEX_8] = 1,
   [TERASCALE_FORMAT_INDEX_4_4] = 1,
   [TERASCALE_FORMAT_INDEX_3_3_2] = 1,
   [TERASCALE_FORMAT_INDEX_16] = 2,
   [TERASCALE_FORMAT_INDEX_16_FLOAT] = 2,
   [TERASCALE_FORMAT_INDEX_8_8] = 2,
   [TERASCALE_FORMAT_INDEX_5_6_5] = 2,
   [TERASCALE_FORMAT_INDEX_6_5_5] = 2,
   [TERASCALE_FORMAT_INDEX_1_5_5_5] = 2,
   [TERASCALE_FORMAT_INDEX_4_4_4_4] = 2,
   [TERASCALE_FORMAT_INDEX_5_5_5_1] = 2,
   [TERASCALE_FORMAT_INDEX_32] = 4,
   [TERASCALE_FORMAT_INDEX_32_FLOAT] = 4,
   [TERASCALE_FORMAT_INDEX_16_16] = 4,
   [TERASCALE_FORMAT_INDEX_16_16_FLOAT] = 4,
   [TERASCALE_FORMAT_INDEX_8_24] = 4,
   [TERASCALE_FORMAT_INDEX_8_24_FLOAT] = 4,
   [TERASCALE_FORMAT_INDEX_24_8] = 4,
   [TERASCALE_FORMAT_INDEX_24_8_FLOAT] = 4,
   [TERASCALE_FORMAT_INDEX_10_11_11] = 4,
   [TERASCALE_FORMAT_INDEX_10_11_11_FLOAT] = 4,
   [TERASCALE_FORMAT_INDEX_11_11_10] = 4,
   [TERASCALE_FORMAT_INDEX_11_11_10_FLOAT] = 4,
   [TERASCALE_FORMAT_INDEX_2_10_10_10] = 4,
   [TERASCALE_FORMAT_INDEX_8_8_8_8] = 4,
   [TERASCALE_FORMAT_INDEX_10_10_10_2] = 4,
   [TERASCALE_FORMAT_INDEX_X24_8_32_FLOAT] = 8,
   [TERASCALE_FORMAT_INDEX_32_32] = 8,
   [TERASCALE_FORMAT_INDEX_32_32_FLOAT] = 8,
   [TERASCALE_FORMAT_INDEX_16_16_16_16] = 8,
   [TERASCALE_FORMAT_INDEX_16_16_16_16_FLOAT] = 8,
   [TERASCALE_FORMAT_INDEX_32_32_32_32] = 16,
   [TERASCALE_FORMAT_INDEX_32_32_32_32_FLOAT] = 16,
   [TERASCALE_FORMAT_INDEX_1] = 1,
   [TERASCALE_FORMAT_INDEX_1_REVERSED] = 1,
   [TERASCALE_FORMAT_INDEX_GB_GR] = 4,
   [TERASCALE_FORMAT_INDEX_BG_RG] = 4,
   [TERASCALE_FORMAT_INDEX_32_AS_8] = 4,
   [TERASCALE_FORMAT_INDEX_32_AS_8_8] = 4,
   [TERASCALE_FORMAT_INDEX_5_9_9_9_SHAREDEXP] = 4,
   [TERASCALE_FORMAT_INDEX_8_8_8] = 3,
   [TERASCALE_FORMAT_INDEX_16_16_16] = 6,
   [TERASCALE_FORMAT_INDEX_16_16_16_FLOAT] = 6,
   [TERASCALE_FORMAT_INDEX_32_32_32] = 12,
   [TERASCALE_FORMAT_INDEX_32_32_32_FLOAT] = 12,
   [TERASCALE_FORMAT_INDEX_BC1] = 8,
   [TERASCALE_FORMAT_INDEX_BC2] = 16,
   [TERASCALE_FORMAT_INDEX_BC3] = 16,
   [TERASCALE_FORMAT_INDEX_BC4] = 8,
   [TERASCALE_FORMAT_INDEX_BC5] = 16,
   [TERASCALE_FORMAT_INDEX_BC6] = 16,
   [TERASCALE_FORMAT_INDEX_BC7] = 16,
   [TERASCALE_FORMAT_INDEX_32_AS_32_32_32_32] = 4,
   [TERASCALE_FORMAT_INDEX_CTX1] = 8,
};

const uint8_t terascale_format_big_endian_swap[1 << 6] = {
   [TERASCALE_FORMAT_INDEX_16] = TERASCALE_ENDIAN_SWAP_8IN16,
   [TERASCALE_FORMAT_INDEX_16_FLOAT] = TERASCALE_ENDIAN_SWAP_8IN16,
   [TERASCALE_FORMAT_INDEX_5_6_5] = TERASCALE_ENDIAN_SWAP_8IN16,
   [TERASCALE_FORMAT_INDEX_6_5_5] = TERASCALE_ENDIAN_SWAP_8IN16,
   [TERASCALE_FORMAT_INDEX_1_5_5_5] = TERASCALE_ENDIAN_SWAP_8IN16,
   [TERASCALE_FORMAT_INDEX_4_4_4_4] = TERASCALE_ENDIAN_SWAP_8IN16,
   [TERASCALE_FORMAT_INDEX_5_5_5_1] = TERASCALE_ENDIAN_SWAP_8IN16,
   [TERASCALE_FORMAT_INDEX_32] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_32_FLOAT] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_16_16] = TERASCALE_ENDIAN_SWAP_8IN16,
   [TERASCALE_FORMAT_INDEX_16_16_FLOAT] = TERASCALE_ENDIAN_SWAP_8IN16,
   [TERASCALE_FORMAT_INDEX_8_24] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_8_24_FLOAT] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_24_8] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_24_8_FLOAT] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_10_11_11] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_10_11_11_FLOAT] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_11_11_10] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_11_11_10_FLOAT] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_2_10_10_10] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_10_10_10_2] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_X24_8_32_FLOAT] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_32_32] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_32_32_FLOAT] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_16_16_16_16] = TERASCALE_ENDIAN_SWAP_8IN16,
   [TERASCALE_FORMAT_INDEX_16_16_16_16_FLOAT] = TERASCALE_ENDIAN_SWAP_8IN16,
   [TERASCALE_FORMAT_INDEX_32_32_32_32] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_32_32_32_32_FLOAT] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_32_AS_8] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_32_AS_8_8] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_5_9_9_9_SHAREDEXP] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_16_16_16] = TERASCALE_ENDIAN_SWAP_8IN16,
   [TERASCALE_FORMAT_INDEX_16_16_16_FLOAT] = TERASCALE_ENDIAN_SWAP_8IN16,
   [TERASCALE_FORMAT_INDEX_32_32_32] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_32_32_32_FLOAT] = TERASCALE_ENDIAN_SWAP_8IN32,
   [TERASCALE_FORMAT_INDEX_32_AS_32_32_32_32] = TERASCALE_ENDIAN_SWAP_8IN32,
};

const uint8_t terascale_format_channel_count[1 << 6] = {
   [TERASCALE_FORMAT_INDEX_8] = 1,
   [TERASCALE_FORMAT_INDEX_4_4] = 2,
   [TERASCALE_FORMAT_INDEX_3_3_2] = 3,
   [TERASCALE_FORMAT_INDEX_16] = 1,
   [TERASCALE_FORMAT_INDEX_16_FLOAT] = 1,
   [TERASCALE_FORMAT_INDEX_8_8] = 2,
   [TERASCALE_FORMAT_INDEX_5_6_5] = 3,
   [TERASCALE_FORMAT_INDEX_6_5_5] = 3,
   [TERASCALE_FORMAT_INDEX_1_5_5_5] = 4,
   [TERASCALE_FORMAT_INDEX_4_4_4_4] = 4,
   [TERASCALE_FORMAT_INDEX_5_5_5_1] = 4,
   [TERASCALE_FORMAT_INDEX_32] = 1,
   [TERASCALE_FORMAT_INDEX_32_FLOAT] = 1,
   [TERASCALE_FORMAT_INDEX_16_16] = 2,
   [TERASCALE_FORMAT_INDEX_16_16_FLOAT] = 2,
   [TERASCALE_FORMAT_INDEX_8_24] = 2,
   [TERASCALE_FORMAT_INDEX_8_24_FLOAT] = 2,
   [TERASCALE_FORMAT_INDEX_24_8] = 2,
   [TERASCALE_FORMAT_INDEX_24_8_FLOAT] = 2,
   [TERASCALE_FORMAT_INDEX_10_11_11] = 3,
   [TERASCALE_FORMAT_INDEX_10_11_11_FLOAT] = 3,
   [TERASCALE_FORMAT_INDEX_11_11_10] = 3,
   [TERASCALE_FORMAT_INDEX_11_11_10_FLOAT] = 3,
   [TERASCALE_FORMAT_INDEX_2_10_10_10] = 4,
   [TERASCALE_FORMAT_INDEX_8_8_8_8] = 4,
   [TERASCALE_FORMAT_INDEX_10_10_10_2] = 4,
   /* Different from the number of channels (3) in the respective pipe_formats. */
   [TERASCALE_FORMAT_INDEX_X24_8_32_FLOAT] = 2,
   [TERASCALE_FORMAT_INDEX_32_32] = 2,
   [TERASCALE_FORMAT_INDEX_32_32_FLOAT] = 2,
   [TERASCALE_FORMAT_INDEX_16_16_16_16] = 4,
   [TERASCALE_FORMAT_INDEX_16_16_16_16_FLOAT] = 4,
   [TERASCALE_FORMAT_INDEX_32_32_32_32] = 4,
   [TERASCALE_FORMAT_INDEX_32_32_32_32_FLOAT] = 4,
   [TERASCALE_FORMAT_INDEX_1] = 1,
   [TERASCALE_FORMAT_INDEX_1_REVERSED] = 1,
   [TERASCALE_FORMAT_INDEX_GB_GR] = 3,
   [TERASCALE_FORMAT_INDEX_BG_RG] = 3,
   [TERASCALE_FORMAT_INDEX_5_9_9_9_SHAREDEXP] = 3,
   [TERASCALE_FORMAT_INDEX_8_8_8] = 3,
   [TERASCALE_FORMAT_INDEX_16_16_16] = 3,
   [TERASCALE_FORMAT_INDEX_16_16_16_FLOAT] = 3,
   [TERASCALE_FORMAT_INDEX_32_32_32] = 3,
   [TERASCALE_FORMAT_INDEX_32_32_32_FLOAT] = 3,
   [TERASCALE_FORMAT_INDEX_BC1] = 4,
   [TERASCALE_FORMAT_INDEX_BC2] = 4,
   [TERASCALE_FORMAT_INDEX_BC3] = 4,
   [TERASCALE_FORMAT_INDEX_BC4] = 1,
   [TERASCALE_FORMAT_INDEX_BC5] = 2,
   [TERASCALE_FORMAT_INDEX_BC6] = 3,
   [TERASCALE_FORMAT_INDEX_BC7] = 4,
   [TERASCALE_FORMAT_INDEX_CTX1] = 2,
};

const uint8_t terascale_format_cb_color_export_component_masks[4 + 1][4] = {
   [0] = {},
   [1] =
      {
         [TERASCALE_FORMAT_CB_COLOR_SWAP_STD] = 0b0001,
         [TERASCALE_FORMAT_CB_COLOR_SWAP_ALT] = 0b0010,
         [TERASCALE_FORMAT_CB_COLOR_SWAP_STD_REV] = 0b0100,
         [TERASCALE_FORMAT_CB_COLOR_SWAP_ALT_REV] = 0b1000,
      },
   [2] =
      {
         [TERASCALE_FORMAT_CB_COLOR_SWAP_STD] = 0b0011,
         [TERASCALE_FORMAT_CB_COLOR_SWAP_ALT] = 0b1001,
         [TERASCALE_FORMAT_CB_COLOR_SWAP_STD_REV] = 0b0011,
         [TERASCALE_FORMAT_CB_COLOR_SWAP_ALT_REV] = 0b1001,
      },
   [3] =
      {
         [TERASCALE_FORMAT_CB_COLOR_SWAP_STD] = 0b0111,
         [TERASCALE_FORMAT_CB_COLOR_SWAP_ALT] = 0b1011,
         [TERASCALE_FORMAT_CB_COLOR_SWAP_STD_REV] = 0b0111,
         [TERASCALE_FORMAT_CB_COLOR_SWAP_ALT_REV] = 0b1011,
      },
   [4] =
      {
         [TERASCALE_FORMAT_CB_COLOR_SWAP_STD] = 0b1111,
         [TERASCALE_FORMAT_CB_COLOR_SWAP_ALT] = 0b1111,
         [TERASCALE_FORMAT_CB_COLOR_SWAP_STD_REV] = 0b1111,
         [TERASCALE_FORMAT_CB_COLOR_SWAP_ALT_REV] = 0b1111,
      },
};

const uint8_t terascale_format_cb_color_read_swizzle[4 + 1][4][4] =
   {
      [0] =
         {
            [TERASCALE_FORMAT_CB_COLOR_SWAP_STD] = {TERASCALE_SWIZZLE_0, TERASCALE_SWIZZLE_0,
                                                    TERASCALE_SWIZZLE_0, TERASCALE_SWIZZLE_0},
            [TERASCALE_FORMAT_CB_COLOR_SWAP_ALT] = {TERASCALE_SWIZZLE_0, TERASCALE_SWIZZLE_0,
                                                    TERASCALE_SWIZZLE_0, TERASCALE_SWIZZLE_0},
            [TERASCALE_FORMAT_CB_COLOR_SWAP_STD_REV] = {TERASCALE_SWIZZLE_0, TERASCALE_SWIZZLE_0,
                                                        TERASCALE_SWIZZLE_0, TERASCALE_SWIZZLE_0},
            [TERASCALE_FORMAT_CB_COLOR_SWAP_ALT_REV] = {TERASCALE_SWIZZLE_0, TERASCALE_SWIZZLE_0,
                                                        TERASCALE_SWIZZLE_0, TERASCALE_SWIZZLE_0},
         },
      [1] =
         {
            [TERASCALE_FORMAT_CB_COLOR_SWAP_STD] = {TERASCALE_SWIZZLE_X, TERASCALE_SWIZZLE_0,
                                                    TERASCALE_SWIZZLE_0, TERASCALE_SWIZZLE_1},
            [TERASCALE_FORMAT_CB_COLOR_SWAP_ALT] = {TERASCALE_SWIZZLE_0, TERASCALE_SWIZZLE_X,
                                                    TERASCALE_SWIZZLE_0, TERASCALE_SWIZZLE_1},
            [TERASCALE_FORMAT_CB_COLOR_SWAP_STD_REV] = {TERASCALE_SWIZZLE_0, TERASCALE_SWIZZLE_0,
                                                        TERASCALE_SWIZZLE_X, TERASCALE_SWIZZLE_1},
            [TERASCALE_FORMAT_CB_COLOR_SWAP_ALT_REV] = {TERASCALE_SWIZZLE_0, TERASCALE_SWIZZLE_0,
                                                        TERASCALE_SWIZZLE_0, TERASCALE_SWIZZLE_X},
         },
      [2] =
         {
            [TERASCALE_FORMAT_CB_COLOR_SWAP_STD] = {TERASCALE_SWIZZLE_X, TERASCALE_SWIZZLE_Y,
                                                    TERASCALE_SWIZZLE_0, TERASCALE_SWIZZLE_1},
            [TERASCALE_FORMAT_CB_COLOR_SWAP_ALT] = {TERASCALE_SWIZZLE_X, TERASCALE_SWIZZLE_0,
                                                    TERASCALE_SWIZZLE_0, TERASCALE_SWIZZLE_Y},
            [TERASCALE_FORMAT_CB_COLOR_SWAP_STD_REV] = {TERASCALE_SWIZZLE_Y, TERASCALE_SWIZZLE_X,
                                                        TERASCALE_SWIZZLE_0, TERASCALE_SWIZZLE_1},
            [TERASCALE_FORMAT_CB_COLOR_SWAP_ALT_REV] = {TERASCALE_SWIZZLE_Y, TERASCALE_SWIZZLE_0,
                                                        TERASCALE_SWIZZLE_0, TERASCALE_SWIZZLE_X},
         },
      [3] =
         {
            [TERASCALE_FORMAT_CB_COLOR_SWAP_STD] = {TERASCALE_SWIZZLE_X, TERASCALE_SWIZZLE_Y,
                                                    TERASCALE_SWIZZLE_Z, TERASCALE_SWIZZLE_1},
            [TERASCALE_FORMAT_CB_COLOR_SWAP_ALT] = {TERASCALE_SWIZZLE_X, TERASCALE_SWIZZLE_Y,
                                                    TERASCALE_SWIZZLE_0, TERASCALE_SWIZZLE_Z},
            [TERASCALE_FORMAT_CB_COLOR_SWAP_STD_REV] = {TERASCALE_SWIZZLE_Z, TERASCALE_SWIZZLE_Y,
                                                        TERASCALE_SWIZZLE_X, TERASCALE_SWIZZLE_1},
            [TERASCALE_FORMAT_CB_COLOR_SWAP_ALT_REV] = {TERASCALE_SWIZZLE_Z, TERASCALE_SWIZZLE_Y,
                                                        TERASCALE_SWIZZLE_0, TERASCALE_SWIZZLE_X},
         },
      [4] =
         {
            [TERASCALE_FORMAT_CB_COLOR_SWAP_STD] = {TERASCALE_SWIZZLE_X, TERASCALE_SWIZZLE_Y,
                                                    TERASCALE_SWIZZLE_Z, TERASCALE_SWIZZLE_W},
            [TERASCALE_FORMAT_CB_COLOR_SWAP_ALT] = {TERASCALE_SWIZZLE_Z, TERASCALE_SWIZZLE_Y,
                                                    TERASCALE_SWIZZLE_X, TERASCALE_SWIZZLE_W},
            [TERASCALE_FORMAT_CB_COLOR_SWAP_STD_REV] = {TERASCALE_SWIZZLE_W, TERASCALE_SWIZZLE_Z,
                                                        TERASCALE_SWIZZLE_Y, TERASCALE_SWIZZLE_X},
            [TERASCALE_FORMAT_CB_COLOR_SWAP_ALT_REV] = {TERASCALE_SWIZZLE_Y, TERASCALE_SWIZZLE_Z,
                                                        TERASCALE_SWIZZLE_W, TERASCALE_SWIZZLE_X},
         },
};

enum terascale_r6xx_depth_stencil_format
terascale_get_r6xx_depth_stencil_format(const enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_Z16_UNORM:
      return TERASCALE_R6XX_DEPTH_STENCIL_FORMAT_16;
   case PIPE_FORMAT_Z32_FLOAT:
      return TERASCALE_R6XX_DEPTH_STENCIL_FORMAT_32_FLOAT;
   case PIPE_FORMAT_Z24_UNORM_S8_UINT:
   case PIPE_FORMAT_X24S8_UINT:
      return TERASCALE_R6XX_DEPTH_STENCIL_FORMAT_8_24;
   case PIPE_FORMAT_Z24X8_UNORM:
      return TERASCALE_R6XX_DEPTH_STENCIL_FORMAT_X8_24;
   case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT:
   case PIPE_FORMAT_X32_S8X24_UINT:
      /* Handled explicitly rather than via util_format_description channels to avoid the endianness
       * difference there (DB only works with little-endian data).
       */
      return TERASCALE_R6XX_DEPTH_STENCIL_FORMAT_X24_8_32_FLOAT;
   default:
      return TERASCALE_R6XX_DEPTH_STENCIL_FORMAT_INVALID;
   }
}

bool
terascale_get_r8xx_depth_stencil_format(const enum pipe_format format,
                                        enum terascale_r8xx_depth_format *const depth_format_out_opt,
                                        bool *const has_stencil_out_opt)
{
   enum terascale_r8xx_depth_format depth_format = TERASCALE_R8XX_DEPTH_FORMAT_INVALID;
   bool has_stencil = false;

   switch (format) {
   case PIPE_FORMAT_S8_UINT:
   case PIPE_FORMAT_X24S8_UINT:
   case PIPE_FORMAT_S8X24_UINT:
   case PIPE_FORMAT_X32_S8X24_UINT:
      has_stencil = true;
      break;
   case PIPE_FORMAT_Z16_UNORM:
      depth_format = TERASCALE_R8XX_DEPTH_FORMAT_16;
      break;
   case PIPE_FORMAT_Z16_UNORM_S8_UINT:
      depth_format = TERASCALE_R8XX_DEPTH_FORMAT_16;
      has_stencil = true;
      break;
   case PIPE_FORMAT_Z32_FLOAT:
      depth_format = TERASCALE_R8XX_DEPTH_FORMAT_32_FLOAT;
      break;
   case PIPE_FORMAT_Z24_UNORM_S8_UINT:
      depth_format = TERASCALE_R8XX_DEPTH_FORMAT_24;
      has_stencil = true;
      break;
   /* PIPE_FORMAT_S8_UINT_Z24_UNORM and PIPE_FORMAT_X8Z24_UNORM are not supported because their
    * corresponding color formats assume that the depth is in bits [31:8] of each dword rather than
    * [23:0].
    */
   case PIPE_FORMAT_Z24X8_UNORM:
      depth_format = TERASCALE_R8XX_DEPTH_FORMAT_24;
      break;
   case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT:
      depth_format = TERASCALE_R8XX_DEPTH_FORMAT_32_FLOAT;
      has_stencil = true;
      break;
   default:
      return false;
   }

   assert(depth_format != TERASCALE_R8XX_DEPTH_FORMAT_INVALID || has_stencil);
   if (depth_format_out_opt != NULL) {
      *depth_format_out_opt = depth_format;
   }
   if (has_stencil_out_opt != NULL) {
      *has_stencil_out_opt = has_stencil;
   }
   return true;
}

static uint32_t
terascale_format_pack_fixed_point(const bool is_normalized, bool is_signed,
                                  const unsigned bit_count, const uint32_t value)
{
   assert(bit_count >= 1);
   if (bit_count < 2) {
      /* The 1-bit channel of 1_5_5_5 and 5_5_5_1 is always unsigned. */
      is_signed = false;
   }
   /* For simplicity, not handling edge cases like (uint32_t)(float)UINT32_MAX being out of
    * uint32_t's range, and (float)0xFFFFFF + 0.5 not being exactly representable.
    */
   assert(bit_count < FLT_MANT_DIG);
   const uint32_t bit_mask = BITFIELD_MASK(bit_count);

   if (!is_normalized) {
      /* Integer or scaled (U/SSCALED is TEXTURECHANNELCLASS_UNSIGNED/SIGNED_INTEGER in CTS
       * getTextureChannelClass).
       */
      if (is_signed) {
         /* Signed integer. */
         const int32_t max_value_int = (int32_t)(bit_mask >> 1);
         return (uint32_t)CLAMP((int32_t)value, -max_value_int - 1, max_value_int) & bit_mask;
      } else {
         /* Unsigned integer. */
         return MIN2(value, bit_mask);
      }
   }

   /* Normalized. */
   float value_clamped = uif(value);
   if (unlikely(isnan(value_clamped))) {
      return 0;
   }
   if (is_signed) {
      /* Signed normalized. */
      value_clamped = CLAMP(value_clamped, -1.0f, 1.0f);
      const int32_t value_int =
         (int32_t)(value_clamped * (float)(bit_mask >> 1) + (value_clamped >= 0.0f ? 0.5f : -0.5f));
      return (uint32_t)value_int & bit_mask;
   } else {
      /* Unsigned normalized. */
      value_clamped = CLAMP(value_clamped, 0.0f, 1.0f);
      const uint32_t value_uint = (uint32_t)(value_clamped * (float)bit_mask + 0.5f);
      assert(value_uint <= bit_mask);
      return value_uint;
   }
}

static uint32_t
terascale_format_pack_normalized_32(const bool is_signed, const uint32_t value)
{
   const float value_float = uif(value);
   if (unlikely(isnan(value_float))) {
      return 0;
   }
   /* Use double precision, like in PAL Util::Math::FloatToU/SFixed, so all 32-bit integers can be
    * represented exactly.
    */
   double value_clamped = (double)uif(value);
   if (is_signed) {
      /* Signed normalized. */
      value_clamped = CLAMP(value_clamped, -1.0, 1.0);
      return (uint32_t)(int32_t)(value_clamped * (double)INT32_MAX +
                                 (value_clamped >= 0.0 ? 0.5 : -0.5));
   } else {
      /* Unsigned normalized. */
      value_clamped = CLAMP(value_clamped, 0.0, 1.0);
      return (uint32_t)(value_clamped * (double)UINT32_MAX);
   }
}

static void
terascale_format_pack_array_8(const unsigned channel_count, const bool is_normalized,
                              const unsigned channels_signed, const uint32_t *const xyzw,
                              void *const packed_out)
{
   for (unsigned channel_index = 0; channel_index < channel_count; ++channel_index) {
      const uint8_t packed_channel = (uint8_t)terascale_format_pack_fixed_point(
         is_normalized, (channels_signed & BITFIELD_BIT(channel_index)) != 0, 8,
         xyzw[channel_index]);
      memcpy((char *)packed_out + sizeof(uint8_t) * channel_index, &packed_channel,
             sizeof(packed_channel));
   }
}

static void
terascale_format_pack_array_16(const unsigned channel_count,
                               const enum terascale_format_number_type number_type,
                               const unsigned channels_signed, const uint32_t *const xyzw,
                               void *const packed_out)
{
   if (number_type == TERASCALE_FORMAT_NUMBER_TYPE_FLOAT) {
      for (unsigned channel_index = 0; channel_index < channel_count; ++channel_index) {
         const uint16_t packed_channel = _mesa_float_to_float16_rtz(uif(xyzw[channel_index]));
         memcpy((char *)packed_out + sizeof(uint16_t) * channel_index, &packed_channel,
                sizeof(packed_channel));
      }
   } else {
      const bool is_normalized =
         terascale_format_get_sq_num_format(number_type) == TERASCALE_FORMAT_SQ_NUM_FORMAT_NORM;
      for (unsigned channel_index = 0; channel_index < channel_count; ++channel_index) {
         const uint16_t packed_channel = (uint16_t)terascale_format_pack_fixed_point(
            is_normalized, (channels_signed & BITFIELD_BIT(channel_index)) != 0, 16,
            xyzw[channel_index]);
         memcpy((char *)packed_out + sizeof(uint16_t) * channel_index, &packed_channel,
                sizeof(packed_channel));
      }
   }
}

static void
terascale_format_pack_array_32(const unsigned channel_count,
                               const enum terascale_format_number_type number_type,
                               const unsigned channels_signed, const uint32_t *const xyzw,
                               void *const packed_out)
{
   if (number_type == TERASCALE_FORMAT_NUMBER_TYPE_FLOAT ||
       terascale_format_get_sq_num_format(number_type) != TERASCALE_FORMAT_SQ_NUM_FORMAT_NORM) {
      memcpy(packed_out, xyzw, sizeof(uint32_t) * channel_count);
   } else {
      for (unsigned channel_index = 0; channel_index < channel_count; ++channel_index) {
         const uint32_t packed_channel = terascale_format_pack_normalized_32(
            (channels_signed & BITFIELD_BIT(channel_index)) != 0, xyzw[channel_index]);
         memcpy((char *)packed_out + sizeof(uint32_t) * channel_index, &packed_channel,
                sizeof(packed_channel));
      }
   }
}

static uint32_t
terascale_format_pack_fixed_point_vec4(const bool is_normalized, const unsigned channels_signed,
                                       const unsigned bit_count_x, const unsigned bit_count_y,
                                       const unsigned bit_count_z, const unsigned bit_count_w,
                                       const uint32_t *const xyzw)
{
   uint32_t packed = 0;
   unsigned next_channel_shift = 0;
   const unsigned bit_counts[] = {bit_count_x, bit_count_y, bit_count_z, bit_count_w};
   for (unsigned channel_index = 0; channel_index < 4; ++channel_index) {
      const unsigned channel_bit_count = bit_counts[channel_index];
      if (channel_bit_count == 0) {
         continue;
      }
      packed |= terascale_format_pack_fixed_point(
                   is_normalized, (channels_signed & BITFIELD_BIT(channel_index)) != 0,
                   channel_bit_count, xyzw[channel_index])
                << next_channel_shift;
      next_channel_shift += channel_bit_count;
   }
   return packed;
}

void
terascale_format_pack_color(const struct terascale_format_info *const format_info,
                            const uint32_t rgba[4], void *const packed_out)
{
   /* Remap from RGBA to channels. Note that in cases like luminance formats, one channel may
    * correspond to multiple color components, so the mapping will be ambiguous. Using the same
    * logic as in PAL Formats::SwizzleColor.
    */
   uint32_t xyzw[4] = {};
   const enum terascale_swizzle swizzle_rgba[] = {
      (enum terascale_swizzle)format_info->swizzle_r,
      (enum terascale_swizzle)format_info->swizzle_g,
      (enum terascale_swizzle)format_info->swizzle_b,
      (enum terascale_swizzle)format_info->swizzle_a,
   };
   for (unsigned rgba_index = 0; rgba_index < 4; ++rgba_index) {
      unsigned channel_index;
      const enum terascale_swizzle swizzle_component = swizzle_rgba[rgba_index];
      if (swizzle_component >= TERASCALE_SWIZZLE_X && swizzle_component <= TERASCALE_SWIZZLE_W) {
         channel_index = (unsigned)swizzle_component - TERASCALE_SWIZZLE_X;
      } else if (format_info->format == TERASCALE_FORMAT_INDEX_5_9_9_9_SHAREDEXP) {
         channel_index = rgba_index;
      } else {
         continue;
      }
      xyzw[channel_index] = rgba[rgba_index];
   }

   /* First, handle formats to which NUMBER_TYPE and FORMAT_COMP are not applicable at all. */

   switch (format_info->format) {
   case TERASCALE_FORMAT_INDEX_8_24: {
      /* Unsigned normalized depth, unsigned integer stencil.
       * Convert for TC usage. Note that DB's behavior differs from TC at one value according to
       * AddrLib's ADDR_UNORM_R6XXDB handling: uif(0x33000000) is converted by DB to 1 even though
       * multiplying it by (float)BITFIELD_MASK(24) produces a value slightly smaller than 0.5.
       */
      const float depth_non_negative = fmaxf(uif(xyzw[0]), 0.0f);
      /* Clamp to 1 as floating-point first to make sure the packed value is within the uint32
       * range.
       */
      const uint32_t depth24_unclamped =
         (uint32_t)(MIN2(depth_non_negative, 1.0f) * (float)BITFIELD_MASK(24) + 0.5f);
      /* Clamp to 0xFFFFFF as integer because there isn't enough precision to add 0.5 to
       * (float)0xFFFFFF exactly, and adding it with rounding to the nearest even produces
       * (float)0x1000000 instead.
       */
      const uint32_t packed =
         MIN2(depth24_unclamped, BITFIELD_MASK(24)) | (MIN2(xyzw[1], BITFIELD_MASK(8)) << 24);
      memcpy(packed_out, &packed, sizeof(packed));
      return;
   } break;

   case TERASCALE_FORMAT_INDEX_24_8: {
      /* Unsigned integer stencil, unsigned normalized depth.
       * Convert for TC usage. Note that DB's behavior differs from TC at one value according to
       * AddrLib's ADDR_UNORM_R6XXDB handling: uif(0x33000000) is converted by DB to 1 even though
       * multiplying it by (float)BITFIELD_MASK(24) produces a value slightly smaller than 0.5.
       */
      const float depth_non_negative = fmaxf(uif(xyzw[1]), 0.0f);
      /* Clamp to 1 as floating-point first to make sure the packed value is within the uint32
       * range.
       */
      const uint32_t depth24_unclamped =
         (uint32_t)(MIN2(depth_non_negative, 1.0f) * (float)BITFIELD_MASK(24) + 0.5f);
      /* Clamp to 0xFFFFFF as integer because there isn't enough precision to add 0.5 to
       * (float)0xFFFFFF exactly, and adding it with rounding to the nearest even produces
       * (float)0x1000000 instead.
       */
      const uint32_t packed =
         (MIN2(depth24_unclamped, BITFIELD_MASK(24)) << 8) | MIN2(xyzw[0], BITFIELD_MASK(8));
      memcpy(packed_out, &packed, sizeof(packed));
      return;
   } break;

      /* TODO(Triang3l): Handle [0, 2) float24 depth on R6xx/R7xx if ever used by any client API
       * (such as Direct3D 9). See hardware behavior, AddrLib ADDR_U4FLOATC (though it has TODOs for
       * rounding and the denormal case), Direct3D 9 reference device's D3DFMT_D24FS8 handling.
       */

   case TERASCALE_FORMAT_INDEX_X24_8_32_FLOAT: {
      /* Floating-point depth, unsigned integer stencil. */
      memcpy(packed_out, &xyzw[0], sizeof(float));
      assert(terascale_format_big_endian_swap[TERASCALE_FORMAT_INDEX_X24_8_32_FLOAT] ==
             TERASCALE_ENDIAN_SWAP_8IN32);
      const uint32_t x24_8 = MIN2(xyzw[0], BITFIELD_MASK(8));
      memcpy((char *)packed_out + sizeof(float), &x24_8, sizeof(x24_8));
      return;
   } break;

   case TERASCALE_FORMAT_INDEX_5_9_9_9_SHAREDEXP: {
      float xyz_float[3];
      memcpy(xyz_float, xyzw, sizeof(xyz_float));
      const uint32_t packed = float3_to_rgb9e5(xyz_float);
      memcpy(packed_out, &packed, sizeof(packed));
      return;
   } break;

   default:
      break;
   }

   /* Handle generic formats. */

   enum terascale_format_number_type number_type =
      (enum terascale_format_number_type)format_info->number_type;
   enum terascale_format_index data_format = (enum terascale_format_index)format_info->format;
   if (TERASCALE_FORMATS_FLOAT_COUNTERPART & BITFIELD64_BIT((uint32_t)data_format)) {
      number_type = TERASCALE_FORMAT_NUMBER_TYPE_FLOAT;
      data_format = (enum terascale_format_index)((uint32_t)data_format - 1);
   }
   const bool is_normalized =
      terascale_format_get_sq_num_format(number_type) == TERASCALE_FORMAT_SQ_NUM_FORMAT_NORM;

   switch (data_format) {
   case TERASCALE_FORMAT_INDEX_8: {
      terascale_format_pack_array_8(1, is_normalized, format_info->channels_signed, xyzw,
                                    packed_out);
   } break;

   case TERASCALE_FORMAT_INDEX_4_4: {
      const uint8_t packed = (uint8_t)terascale_format_pack_fixed_point_vec4(
         is_normalized, format_info->channels_signed, 4, 4, 0, 0, xyzw);
      memcpy(packed_out, &packed, sizeof(packed));
   } break;

   case TERASCALE_FORMAT_INDEX_3_3_2: {
      const uint8_t packed = (uint8_t)terascale_format_pack_fixed_point_vec4(
         is_normalized, format_info->channels_signed, 2, 3, 3, 0, xyzw);
      memcpy(packed_out, &packed, sizeof(packed));
   } break;

   case TERASCALE_FORMAT_INDEX_16: {
      terascale_format_pack_array_16(1, number_type, format_info->channels_signed, xyzw,
                                     packed_out);
   } break;

   case TERASCALE_FORMAT_INDEX_8_8: {
      terascale_format_pack_array_8(2, is_normalized, format_info->channels_signed, xyzw,
                                    packed_out);
   } break;

   case TERASCALE_FORMAT_INDEX_5_6_5: {
      const uint16_t packed = (uint16_t)terascale_format_pack_fixed_point_vec4(
         is_normalized, format_info->channels_signed, 5, 6, 5, 0, xyzw);
      memcpy(packed_out, &packed, sizeof(packed));
   } break;

   case TERASCALE_FORMAT_INDEX_6_5_5: {
      const uint16_t packed = (uint16_t)terascale_format_pack_fixed_point_vec4(
         is_normalized, format_info->channels_signed, 5, 5, 6, 0, xyzw);
      memcpy(packed_out, &packed, sizeof(packed));
   } break;

   case TERASCALE_FORMAT_INDEX_1_5_5_5: {
      const uint16_t packed = (uint16_t)terascale_format_pack_fixed_point_vec4(
         is_normalized, format_info->channels_signed, 5, 5, 5, 1, xyzw);
      memcpy(packed_out, &packed, sizeof(packed));
   } break;

   case TERASCALE_FORMAT_INDEX_4_4_4_4: {
      const uint16_t packed = (uint16_t)terascale_format_pack_fixed_point_vec4(
         is_normalized, format_info->channels_signed, 4, 4, 4, 4, xyzw);
      memcpy(packed_out, &packed, sizeof(packed));
   } break;

   case TERASCALE_FORMAT_INDEX_5_5_5_1: {
      const uint16_t packed = (uint16_t)terascale_format_pack_fixed_point_vec4(
         is_normalized, format_info->channels_signed, 1, 5, 5, 5, xyzw);
      memcpy(packed_out, &packed, sizeof(packed));
   } break;

   case TERASCALE_FORMAT_INDEX_32: {
      terascale_format_pack_array_32(1, number_type, format_info->channels_signed, xyzw,
                                     packed_out);
   } break;

   case TERASCALE_FORMAT_INDEX_16_16: {
      terascale_format_pack_array_16(2, number_type, format_info->channels_signed, xyzw,
                                     packed_out);
   } break;

   case TERASCALE_FORMAT_INDEX_10_11_11: {
      uint32_t packed;
      if (number_type == TERASCALE_FORMAT_NUMBER_TYPE_FLOAT) {
         packed = f32_to_uf11(uif(xyzw[0])) | ((uint32_t)f32_to_uf11(uif(xyzw[1])) << 11) |
                  ((uint32_t)f32_to_uf10(uif(xyzw[2])) << 22);
      } else {
         packed = terascale_format_pack_fixed_point_vec4(
            is_normalized, format_info->channels_signed, 11, 11, 10, 0, xyzw);
      }
      memcpy(packed_out, &packed, sizeof(packed));
   } break;

   case TERASCALE_FORMAT_INDEX_11_11_10: {
      uint32_t packed;
      if (number_type == TERASCALE_FORMAT_NUMBER_TYPE_FLOAT) {
         packed = f32_to_uf10(uif(xyzw[0])) | ((uint32_t)f32_to_uf11(uif(xyzw[1])) << 10) |
                  ((uint32_t)f32_to_uf11(uif(xyzw[2])) << 21);
      } else {
         packed = terascale_format_pack_fixed_point_vec4(
            is_normalized, format_info->channels_signed, 10, 11, 11, 0, xyzw);
      }
      memcpy(packed_out, &packed, sizeof(packed));
   } break;

   case TERASCALE_FORMAT_INDEX_2_10_10_10: {
      const uint32_t packed = terascale_format_pack_fixed_point_vec4(
         is_normalized, format_info->channels_signed, 10, 10, 10, 2, xyzw);
      memcpy(packed_out, &packed, sizeof(packed));
   } break;

   case TERASCALE_FORMAT_INDEX_8_8_8_8: {
      if (number_type == TERASCALE_FORMAT_NUMBER_TYPE_SRGB) {
         uint8_t packed[4];
         for (unsigned channel_index = 0; channel_index < 3; ++channel_index) {
            packed[channel_index] =
               util_format_linear_float_to_srgb_8unorm(uif(xyzw[channel_index]));
         }
         packed[3] = (uint8_t)terascale_format_pack_fixed_point(
            is_normalized, (format_info->channels_signed & BITFIELD_BIT(3)) != 0, 8, xyzw[3]);
         memcpy(packed_out, packed, sizeof(packed));
      } else {
         terascale_format_pack_array_8(4, is_normalized, format_info->channels_signed, xyzw,
                                       packed_out);
      }
   } break;

   case TERASCALE_FORMAT_INDEX_10_10_10_2: {
      const uint32_t packed = terascale_format_pack_fixed_point_vec4(
         is_normalized, format_info->channels_signed, 2, 10, 10, 10, xyzw);
      memcpy(packed_out, &packed, sizeof(packed));
   } break;

   case TERASCALE_FORMAT_INDEX_32_32: {
      terascale_format_pack_array_32(2, number_type, format_info->channels_signed, xyzw,
                                     packed_out);
   } break;

   case TERASCALE_FORMAT_INDEX_16_16_16_16: {
      terascale_format_pack_array_16(4, number_type, format_info->channels_signed, xyzw,
                                     packed_out);
   } break;

   case TERASCALE_FORMAT_INDEX_32_32_32_32: {
      terascale_format_pack_array_32(4, number_type, format_info->channels_signed, xyzw,
                                     packed_out);
   } break;

   case TERASCALE_FORMAT_INDEX_8_8_8: {
      terascale_format_pack_array_8(3, is_normalized, format_info->channels_signed, xyzw,
                                    packed_out);
   } break;

   case TERASCALE_FORMAT_INDEX_16_16_16: {
      terascale_format_pack_array_16(3, number_type, format_info->channels_signed, xyzw,
                                     packed_out);
   } break;

   case TERASCALE_FORMAT_INDEX_32_32_32: {
      terascale_format_pack_array_32(3, number_type, format_info->channels_signed, xyzw,
                                     packed_out);
   } break;

   default:
      assert(!"Texel packing is not implemented for the format");
   }
}
