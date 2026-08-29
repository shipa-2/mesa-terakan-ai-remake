/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_resource_descriptor_terascale_1.h"

#include "gallium/drivers/r600/r600d.h"
#include "gallium/drivers/r600/r600d_common.h"

#include <string.h>

void
terakan_resource_texture_descriptor_terascale_1_encode(
   struct terakan_resource_texture_descriptor_terascale_1_info const * const info,
   uint32_t descriptor_out[TERAKAN_RESOURCE_DESCRIPTOR_TERASCALE_1_DWORDS])
{
   descriptor_out[0] = S_038000_DIM(info->dim) | S_038000_TILE_MODE(info->tile_mode) |
                       S_038000_TILE_TYPE(info->tile_type) | S_038000_PITCH(info->pitch) |
                       S_038000_TEX_WIDTH(info->width);
   descriptor_out[1] = S_038004_TEX_HEIGHT(info->height) | S_038004_TEX_DEPTH(info->depth) |
                       S_038004_DATA_FORMAT(info->data_format);
   descriptor_out[2] = info->base_address;
   descriptor_out[3] = info->mip_address;
   descriptor_out[4] =
      S_038010_FORMAT_COMP_X(info->format_comp[0]) | S_038010_FORMAT_COMP_Y(info->format_comp[1]) |
      S_038010_FORMAT_COMP_Z(info->format_comp[2]) | S_038010_FORMAT_COMP_W(info->format_comp[3]) |
      S_038010_NUM_FORMAT_ALL(info->num_format) | S_038010_SRF_MODE_ALL(info->srf_mode) |
      S_038010_FORCE_DEGAMMA(info->force_degamma) | S_038010_ENDIAN_SWAP(info->endian_swap) |
      S_038010_REQUEST_SIZE(1) | S_038010_DST_SEL_X(info->dst_sel[0]) |
      S_038010_DST_SEL_Y(info->dst_sel[1]) | S_038010_DST_SEL_Z(info->dst_sel[2]) |
      S_038010_DST_SEL_W(info->dst_sel[3]) | S_038010_BASE_LEVEL(info->base_level);
   descriptor_out[5] = S_038014_LAST_LEVEL(info->last_level) |
                       S_038014_BASE_ARRAY(info->base_array) |
                       S_038014_LAST_ARRAY(info->last_array);
   descriptor_out[6] =
      S_038018_MAX_ANISO(info->max_aniso) | S_038018_TYPE(V_038010_SQ_TEX_VTX_VALID_TEXTURE);
}

void
terakan_resource_buffer_descriptor_terascale_1_encode(
   uint32_t const source_words[4],
   uint32_t descriptor_out[TERAKAN_RESOURCE_DESCRIPTOR_TERASCALE_1_DWORDS])
{
   /* R600 texture-buffer descriptors use the same first four SQ_VTX_CONSTANT words. Words 4 and
    * 5 are zero in texture_buffer_sampler_view(); the classic driver explicitly notes that the
    * nominal element count in word 4 does not work. The validity type moves from Evergreen word 7
    * to R600/R700 word 6.
    */
   memcpy(descriptor_out, source_words, 4 * sizeof(uint32_t));
   descriptor_out[4] = 0;
   descriptor_out[5] = 0;
   descriptor_out[6] = S_038018_TYPE(V_038010_SQ_TEX_VTX_VALID_BUFFER);
}

uint32_t *
terakan_resource_descriptor_terascale_1_write_set_packet(
   uint32_t * packet, uint32_t const resource_hw_index, uint32_t const shader_type_flag,
   uint32_t const descriptor[TERAKAN_RESOURCE_DESCRIPTOR_TERASCALE_1_DWORDS])
{
   *packet++ =
      PKT3(PKT3_SET_RESOURCE, TERAKAN_RESOURCE_DESCRIPTOR_TERASCALE_1_DWORDS, 0) | shader_type_flag;
   *packet++ = TERAKAN_RESOURCE_DESCRIPTOR_TERASCALE_1_DWORDS * resource_hw_index;
   memcpy(packet, descriptor, TERAKAN_RESOURCE_DESCRIPTOR_TERASCALE_1_DWORDS * sizeof(uint32_t));
   return packet + TERAKAN_RESOURCE_DESCRIPTOR_TERASCALE_1_DWORDS;
}
