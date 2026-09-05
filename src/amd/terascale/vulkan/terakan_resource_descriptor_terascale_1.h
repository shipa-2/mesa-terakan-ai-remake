/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TERAKAN_RESOURCE_DESCRIPTOR_TERASCALE_1_H
#define TERAKAN_RESOURCE_DESCRIPTOR_TERASCALE_1_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TERAKAN_RESOURCE_DESCRIPTOR_TERASCALE_1_DWORDS 7
#define TERAKAN_RESOURCE_DESCRIPTOR_TERASCALE_1_SET_PACKET_DWORDS                                  \
   (2 + TERAKAN_RESOURCE_DESCRIPTOR_TERASCALE_1_DWORDS)

struct terakan_resource_texture_descriptor_terascale_1_info {
   uint32_t dim;
   uint32_t tile_mode;
   bool tile_type;
   uint32_t pitch;
   uint32_t width;
   uint32_t height;
   uint32_t depth;
   uint32_t data_format;
   uint32_t base_address;
   uint32_t mip_address;
   uint32_t format_comp[4];
   uint32_t num_format;
   uint32_t srf_mode;
   bool force_degamma;
   uint32_t endian_swap;
   uint32_t dst_sel[4];
   uint32_t base_level;
   uint32_t last_level;
   uint32_t base_array;
   uint32_t last_array;
   uint32_t max_aniso;
};

void terakan_resource_texture_descriptor_terascale_1_encode(
   struct terakan_resource_texture_descriptor_terascale_1_info const * info,
   uint32_t descriptor_out[TERAKAN_RESOURCE_DESCRIPTOR_TERASCALE_1_DWORDS]);

void terakan_resource_buffer_descriptor_terascale_1_encode(
   uint32_t const source_words[4],
   uint32_t descriptor_out[TERAKAN_RESOURCE_DESCRIPTOR_TERASCALE_1_DWORDS]);

uint32_t terakan_resource_descriptor_terascale_1_hw_index(uint32_t evergreen_hw_index);

uint32_t * terakan_resource_descriptor_terascale_1_write_set_packet(
   uint32_t * packet, uint32_t resource_hw_index, uint32_t shader_type_flag,
   uint32_t const descriptor[TERAKAN_RESOURCE_DESCRIPTOR_TERASCALE_1_DWORDS]);

/* R600/R700 has no Evergreen SQ_TEX_RESOURCE_CLEAR control constant. Valid Vulkan draws don't
 * access unbound descriptors, so no replacement packet is needed until null descriptors or another
 * feature requiring an explicit invalid resource are implemented for TeraScale 1.
 */
uint32_t terakan_resource_descriptor_terascale_1_clear_packet_dwords(void);

#ifdef __cplusplus
}
#endif

#endif
