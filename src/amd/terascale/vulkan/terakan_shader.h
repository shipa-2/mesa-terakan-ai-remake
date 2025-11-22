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

#ifndef TERAKAN_SHADER_H
#define TERAKAN_SHADER_H

#include "terakan_bo.h"
#include "terakan_descriptor.h"
#include "terakan_pipeline_layout.h"
#include "terakan_push_constants.h"
#include "terakan_vertex_input.h"

#include "gallium/drivers/r600/eg_sq.h"
#include "gallium/drivers/r600/r600_opcodes.h"
#include "gallium/drivers/r600/r600_shader_common.h"
#include "util/bitset.h"
#include "util/macros.h"
#include "nir.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TERAKAN_SHADER_GPR_LAST_NON_CLAUSE_TEMP (0x7F - 4)

/* For easier writing of meta shaders: initializer for two array elements containing two words of an
 * instruction with the most important fields.
 *
 * To set other fields, use OR on the left for the first word, or OR on the right for the second
 * word.
 */

#define TERAKAN_SHADER_CF_END_R9XX                                                                 \
   0, (S_SQ_CF_WORD1_BARRIER(true) | CM_V_SQ_CF_WORD1_SQ_CF_INST_END)

#define TERAKAN_SHADER_CF_VS_DUMMY_EXPORT_DONE_AND_END_R8XX                                        \
   (S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_POS) |                    \
    S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(60)),                                                    \
      (S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_0) |                                \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_0) |                                \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_0) |                                \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_0) |                                \
       S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) |                                                  \
       S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(true) |                                           \
       EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE)

/* No barrier, expecting no other exports. */
#define TERAKAN_SHADER_CF_VS_DUMMY_EXPORT_DONE_R9XX                                                \
   (S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_POS) |                    \
    S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(60)),                                                    \
      (S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_0) |                                \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_0) |                                \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_0) |                                \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_0) |                                \
       EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE)

#define TERAKAN_SHADER_CF_PS_DUMMY_EXPORT_DONE_AND_END_R8XX                                        \
   (S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |                  \
    S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0)),                                                     \
      (S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_MASK) |                             \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_MASK) |                             \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_MASK) |                             \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_MASK) |                             \
       S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(true) |                                                  \
       S_SQ_CF_ALLOC_EXPORT_WORD1_END_OF_PROGRAM(true) |                                           \
       EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE)

/* No barrier, expecting no other exports. */
#define TERAKAN_SHADER_CF_PS_DUMMY_EXPORT_DONE_R9XX                                                \
   (S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL) |                  \
    S_SQ_CF_ALLOC_EXPORT_WORD0_ARRAY_BASE(0)),                                                     \
      (S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_X(TERASCALE_SWIZZLE_MASK) |                             \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Y(TERASCALE_SWIZZLE_MASK) |                             \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_Z(TERASCALE_SWIZZLE_MASK) |                             \
       S_SQ_CF_ALLOC_EXPORT_WORD1_SWIZ_SEL_W(TERASCALE_SWIZZLE_MASK) |                             \
       EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE)

/* Note that the actual handling of ARRAY_SIZE for burst writes (that write multiple array elements
 * with the given stride from consecutive GPR indices) differs from what the Evergreen Family
 * Instruction Set Architecture reference says.
 * ARRAY_SIZE is not minus 1.
 * On R8xx, for STORE_RAW, it's in bytes.
 * On R9xx, for STORE_DWORD (and STORE_RAW, which still exists, but is changed to "reserved" in the
 * documentation), it's in dwords.
 */

#define TERAKAN_SHADER_CF_UAV(cacheless, uav_inst, uav_id, index_gpr, data_gpr, comp_mask,         \
                              barrier)                                                             \
   (S_SQ_CF_ALLOC_EXPORT_WORD0_RAT_RAT_ID(uav_id) |                                                \
    S_SQ_CF_ALLOC_EXPORT_WORD0_RAT_RAT_INST(V_RAT_INST_##uav_inst) |                               \
    S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_WRITE_IND) |              \
    S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(data_gpr) |                                                  \
    S_SQ_CF_ALLOC_EXPORT_WORD0_INDEX_GPR(index_gpr)),                                              \
      (S_SQ_CF_ALLOC_EXPORT_WORD1_BUF_COMP_MASK(comp_mask) |                                       \
       S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(barrier) |                                               \
       ((cacheless) ? EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_RAT_CACHELESS                   \
                    : EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_RAT))

#define TERAKAN_SHADER_CF_UAV_COMBINED_STORE(is_r9xx, uav_id, data_gpr, two_component, barrier)    \
   (S_SQ_CF_ALLOC_EXPORT_WORD0_RAT_RAT_ID(uav_id) |                                                \
    S_SQ_CF_ALLOC_EXPORT_WORD0_RAT_RAT_INST((is_r9xx) ? V_RAT_INST_STORE_DWORD                     \
                                                      : V_RAT_INST_STORE_RAW) |                    \
    S_SQ_CF_ALLOC_EXPORT_WORD0_TYPE(V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_WRITE) |                  \
    S_SQ_CF_ALLOC_EXPORT_WORD0_RW_GPR(data_gpr)),                                                  \
      (S_SQ_CF_ALLOC_EXPORT_WORD1_BUF_COMP_MASK((two_component) ? 0b1011 : 0b1001) |               \
       S_SQ_CF_ALLOC_EXPORT_WORD1_BARRIER(barrier) |                                               \
       EG_V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_MEM_RAT_COMBINED_CACHELESS)

/* If the sources need to be set up by some other means (such as using the
 * TERAKAN_KCACHE_DWORD_WORD#_SRC# macros), pass 0 as src_sel and src_chan.
 *
 * BANK_SWIZZLE is VEC_012 or SCL_210 most of the time, but it being explicitly required by these
 * macros serves as a reminder to be mindful of read port conflicts and the unit that will be
 * executing the last instruction (on R8xx, if the opcode can be executed by either the vector or
 * the scalar unit, and the destination component is not used by another instruction, the vector
 * unit will execute it, like with PREFER_VECTOR on R6xx/R7xx).
 */

static_assert(('X' & 3) == 0 && ('Y' & 3) == 1 && ('Z' & 3) == 2 && ('W' & 3) == 3,
              "XYZW character literals must be usable as component indices");
static_assert(('x' & 3) == 0 && ('y' & 3) == 1 && ('z' & 3) == 2 && ('w' & 3) == 3,
              "xyzw character literals must be usable as component indices");

/* The WM version (with an explicit write mask) is primarily for internal use by other SHADER_OP1/2
 * macros. The writing or non-writing (NW) macros should be used instead where possible.
 */
#define TERAKAN_SHADER_OP2_WM(last, write_mask, dst_gpr, dst_chan, inst, isa, src0_sel, src0_chan, \
                              src1_sel, src1_chan, bank_swizzle)                                   \
   (S_SQ_ALU_WORD0_LAST(last) | S_SQ_ALU_WORD0_SRC0_SEL(src0_sel) |                                \
    S_SQ_ALU_WORD0_SRC0_CHAN((src0_chan) & 3) | S_SQ_ALU_WORD0_SRC1_SEL(src1_sel) |                \
    S_SQ_ALU_WORD0_SRC1_CHAN((src1_chan) & 3)),                                                    \
      (S_SQ_ALU_WORD1_DST_GPR(dst_gpr) | S_SQ_ALU_WORD1_DST_CHAN((dst_chan) & 3) |                 \
       S_SQ_ALU_WORD1_OP2_WRITE_MASK(write_mask) |                                                 \
       S_SQ_ALU_WORD1_OP2_ALU_INST(isa##_V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_##inst) |                  \
       S_SQ_ALU_WORD1_BANK_SWIZZLE(SQ_ALU_##bank_swizzle))
#define TERAKAN_SHADER_OP2(last, dst_gpr, dst_chan, inst, isa, src0_sel, src0_chan, src1_sel,      \
                           src1_chan, bank_swizzle)                                                \
   TERAKAN_SHADER_OP2_WM(last, true, dst_gpr, dst_chan, inst, isa, src0_sel, src0_chan, src1_sel,  \
                         src1_chan, bank_swizzle)
#define TERAKAN_SHADER_OP2_NW(last, dst_chan, inst, isa, src0_sel, src0_chan, src1_sel, src1_chan, \
                              bank_swizzle)                                                        \
   TERAKAN_SHADER_OP2_WM(last, false, TERAKAN_SHADER_GPR_LAST_NON_CLAUSE_TEMP, dst_chan, inst,     \
                         isa, src0_sel, src0_chan, src1_sel, src1_chan, bank_swizzle)
/* For single-operand instructions, passing src0 to src1. If it's a GPR, the load from the GPR read
 * port of src0 is reused for src1 in the hardware as a special case, and if it's a constant used on
 * the transcendental unit, it doesn't matter whether one or two constants are loaded, because
 * there's no other operand to load anyway.
 * Note that if relative source addressing is needed, it needs to be enabled for both src0 and src1.
 */
#define TERAKAN_SHADER_OP1(last, dst_gpr, dst_chan, inst, isa, src0_sel, src0_chan, bank_swizzle)  \
   TERAKAN_SHADER_OP2_WM(last, true, dst_gpr, dst_chan, inst, isa, src0_sel, src0_chan, src0_sel,  \
                         src0_chan, bank_swizzle)
#define TERAKAN_SHADER_OP1_NW(last, dst_chan, inst, isa, src0_sel, src0_chan, bank_swizzle)        \
   TERAKAN_SHADER_OP2_WM(last, false, TERAKAN_SHADER_GPR_LAST_NON_CLAUSE_TEMP, dst_chan, inst,     \
                         isa, src0_sel, src0_chan, src0_sel, src0_chan, bank_swizzle)

#define TERAKAN_SHADER_OP3(last, dst_gpr, dst_chan, inst, isa, src0_sel, src0_chan, src1_sel,      \
                           src1_chan, src2_sel, src2_chan, bank_swizzle)                           \
   (S_SQ_ALU_WORD0_LAST(last) | S_SQ_ALU_WORD0_SRC0_SEL(src0_sel) |                                \
    S_SQ_ALU_WORD0_SRC0_CHAN((src0_chan) & 3) | S_SQ_ALU_WORD0_SRC1_SEL(src1_sel) |                \
    S_SQ_ALU_WORD0_SRC1_CHAN((src1_chan) & 3)),                                                    \
      (S_SQ_ALU_WORD1_DST_GPR(dst_gpr) | S_SQ_ALU_WORD1_DST_CHAN((dst_chan) & 3) |                 \
       S_SQ_ALU_WORD1_OP3_ALU_INST(isa##_V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_##inst) |                  \
       S_SQ_ALU_WORD1_OP3_SRC2_SEL(src2_sel) | S_SQ_ALU_WORD1_OP3_SRC2_CHAN((src2_chan) & 3) |     \
       S_SQ_ALU_WORD1_BANK_SWIZZLE(SQ_ALU_##bank_swizzle))

/* Writing to a GPR that's generally out of bounds (but not clause-temporary), for cases where the
 * result doesn't need to be written.
 */
#define TERAKAN_SHADER_OP3_NW(last, dst_chan, inst, isa, src0_sel, src0_chan, src1_sel, src1_chan, \
                              src2_sel, src2_chan, bank_swizzle)                                   \
   (S_SQ_ALU_WORD0_LAST(last) | S_SQ_ALU_WORD0_SRC0_SEL(src0_sel) |                                \
    S_SQ_ALU_WORD0_SRC0_CHAN((src0_chan) & 3) | S_SQ_ALU_WORD0_SRC1_SEL(src1_sel) |                \
    S_SQ_ALU_WORD0_SRC1_CHAN((src1_chan) & 3)),                                                    \
      (S_SQ_ALU_WORD1_DST_GPR(TERAKAN_SHADER_GPR_LAST_NON_CLAUSE_TEMP) |                           \
       S_SQ_ALU_WORD1_DST_CHAN((dst_chan) & 3) |                                                   \
       S_SQ_ALU_WORD1_OP3_ALU_INST(isa##_V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_##inst) |                  \
       S_SQ_ALU_WORD1_OP3_SRC2_SEL(src2_sel) | S_SQ_ALU_WORD1_OP3_SRC2_CHAN((src2_chan) & 3) |     \
       S_SQ_ALU_WORD1_BANK_SWIZZLE(SQ_ALU_##bank_swizzle))

inline unsigned
terakan_shader_hw_vertex_stage_count(bool const tessellation_shader, bool const geometry_shader)
{
   return (tessellation_shader ? 2 /* LS, HS */ : 0) +
          (geometry_shader ? 3 /* ES, GS, VS */ : 1 /* VS */);
}

#define TERAKAN_SHADER_PROGRAM_ALIGNMENT_LOG2 8
#define TERAKAN_SHADER_PROGRAM_ALIGNMENT      (1 << TERAKAN_SHADER_PROGRAM_ALIGNMENT_LOG2)

enum terakan_shader_ring_index {
   TERAKAN_SHADER_RING_INDEX_LSTMP,
   TERAKAN_SHADER_RING_INDEX_HSTMP,
   TERAKAN_SHADER_RING_INDEX_ESTMP,
   TERAKAN_SHADER_RING_INDEX_GSTMP,
   TERAKAN_SHADER_RING_INDEX_VSTMP,
   TERAKAN_SHADER_RING_INDEX_PSTMP,

   TERAKAN_SHADER_RING_INDEX_COUNT,
};

static_assert(TERAKAN_SHADER_RING_INDEX_COUNT <= 32,
              "Using shader ring buffer indices in a 32-bit bitfield.");

#define TERAKAN_SHADER_RINGS_PER_SHADER_ENGINE                                                     \
   (BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_LSTMP) |                                                \
    BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_HSTMP) |                                                \
    BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_ESTMP) |                                                \
    BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_GSTMP) |                                                \
    BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_VSTMP) | BITFIELD_BIT(TERAKAN_SHADER_RING_INDEX_PSTMP))

struct terakan_shader_ring {
   uint64_t base_wddm_patch_ids;
   /* The size register is the next after the base register. */
   uint32_t base_size_config_reg_offset;
   uint32_t item_size_context_reg_offset;
   /* Pipeline stages potentially accessing this ring. */
   VkPipelineStageFlags2 stages;
   uint32_t sx_surface_sync_mask;
};

extern struct terakan_shader_ring const terakan_shader_rings[TERAKAN_SHADER_RING_INDEX_COUNT];

/* Fields that don't depend on any other state. */
struct terakan_shader_static {
   struct terakan_bo * program_bo;
   uint32_t program_va_shr8;

   uint32_t sq_pgm_resources[2];

   union {
      struct {
         uint32_t spi_vs_out_id[10];
         uint32_t spi_vs_out_config;
         uint32_t pa_cl_vs_out_cntl;
      } vs;

      struct {
         uint32_t sq_pgm_exports_ps;
         uint32_t spi_ps_input_cntl[32];
         uint32_t spi_ps_in_control[2];
         uint32_t spi_input_z;
         uint32_t spi_baryc_cntl;
         /* This must include all color exports done by the shader, otherwise there will be hangs
          * (tested with dEQP-VK.pipeline.monolithic.blend.dual_source.* on Barts).
          * The CB_SHADER_MASK rules for dual-source blending described in Radeon Evergreen /
          * Northern Islands Acceleration are incorrect.
          */
         uint32_t cb_shader_mask;
      } ps;
   } stage;
};

/* Shader implementation common for both pipelines (to be used in a pipeline-cached wrapper) and
 * shader objects (for an uncached wrapper implementing VkShaderEXT).
 */
struct terakan_shader_impl {
   /* This object owns the BO in `static_state`. */
   /* TODO(Triang3l): Shader suballocation. */
   struct terakan_shader_static static_state;

   uint32_t scratch_item_size_dwords;

   struct terakan_push_constants_usage push_constants_usage;

   BITSET_DECLARE(resources_needed, TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE);
   uint32_t samplers_needed;

   /* TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL bits are valid in fragment shaders,
    * TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL bits are valid in compute shaders.
    * Padded to BITSET_WORD with zero bits, can be used in memcmp.
    */
   BITSET_DECLARE(uavs_for_mutable_resources_needed,
                  MAX2(TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL,
                       TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL));

   struct {
      BITSET_DECLARE(vertex_attributes_needed, TERAKAN_VERTEX_INPUT_MAX_ATTRIBUTES);
   } vs;

   struct {
      /* DUAL_EXPORT_ENABLE specifies only whether it can potentially be enabled for the shader. */
      uint32_t db_shader_control;

      uint8_t fragment_data_uncompacted_locations;
   } fs;

   struct r600_shader shader;
};

struct terakan_device;

/* Converts SPIR-V to NIR and performs pre-link lowerings. */
nir_shader * terakan_shader_spirv_to_nir(struct terakan_device * device, size_t spirv_size_bytes,
                                         uint32_t const * spirv, gl_shader_stage stage,
                                         char const * entrypoint,
                                         VkSpecializationInfo const * specialization_info);

void terakan_shader_lower_and_optimize_post_link(
   nir_shader * nir, struct terakan_pipeline_layout const * pipeline_layout,
   BITSET_WORD * resources_needed, uint32_t * samplers_needed,
   BITSET_WORD * uavs_for_mutable_resources_needed, uint32_t * driver_push_constants_used,
   uint8_t * fragment_data_uncompacted_locations_out);

void terakan_shader_impl_finish(struct terakan_shader_impl * shader,
                                VkAllocationCallbacks const * allocator);

/* Compiles the shader into the microcode written to a BO, and fills the info from the backend
 * compiler. Assumes that everything in the shader not intended to be filled directly from the NIR
 * has been zeroed prior to the call.
 */
VkResult terakan_shader_impl_compile(struct terakan_shader_impl * shader,
                                     struct terakan_device * device,
                                     union r600_shader_key const * key, nir_shader * nir,
                                     VkAllocationCallbacks const * allocator);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_SHADER_H */
