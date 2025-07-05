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

#include "terakan_nir.h"

#include "util/bitscan.h"

#include <assert.h>
#include <stdint.h>

nir_def *
terakan_nir_load_raw_resource_buffer(nir_builder * const b, unsigned const num_components,
                                     unsigned const bit_size, enum gl_access_qualifier const access,
                                     unsigned const resource_index_base,
                                     nir_def * const resource_index, unsigned byte_address_base,
                                     nir_def * byte_address)
{
   assert(bit_size == 8 || bit_size == 16 || bit_size == 32);

   /* According to testing on Barts, 8_8_8 and 16_16_16 buffer fetches return completely invalid
    * values.
    */
   assert(!(num_components == 3 && bit_size != 32));

   static enum pipe_format const formats[][4] = {
      {
         PIPE_FORMAT_R8_UINT,
         PIPE_FORMAT_R8G8_UINT,
         PIPE_FORMAT_R8G8B8_UINT,
         PIPE_FORMAT_R8G8B8A8_UINT,
      },
      {
         PIPE_FORMAT_R16_UINT,
         PIPE_FORMAT_R16G16_UINT,
         PIPE_FORMAT_R16G16B16_UINT,
         PIPE_FORMAT_R16G16B16A16_UINT,
      },
      {
         PIPE_FORMAT_R32_UINT,
         PIPE_FORMAT_R32G32_UINT,
         PIPE_FORMAT_R32G32B32_UINT,
         PIPE_FORMAT_R32G32B32A32_UINT,
      },
   };

   /* Apply the base to the address source if it's too large to be specified in the fetch
    * instruction.
    */
   if (byte_address_base > UINT16_MAX) {
      byte_address = nir_iadd(b, byte_address, nir_imm_int(b, (int)byte_address_base));
      byte_address_base = 0;
   }

   return nir_u2uN(b,
                   nir_load_buffer_resource_r600(
                      b, num_components, 32, resource_index, byte_address, .access = access,
                      .id_base = resource_index_base, .base = byte_address_base, .component = 0,
                      .format = formats[ffs(bit_size / 8) - 1][num_components - 1]),
                   bit_size);
}
