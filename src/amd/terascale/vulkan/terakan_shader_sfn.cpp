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

#include "terakan_shader.h"

#include "nir/terakan_nir.h"
#include "terakan_descriptor.h"
#include "terakan_device.h"
#include "terakan_hw_config_draw_terascale_1.h"
#include "terakan_physical_device.h"
#include "terakan_shader_generation.h"

#include "compiler/shader_enums.h"
#include "gallium/drivers/r600/evergreend.h"
#include "gallium/drivers/r600/r600_asm.h"
#include "gallium/drivers/r600/r600_isa.h"
#include "gallium/drivers/r600/sfn/sfn_assembler.h"
#include "gallium/drivers/r600/sfn/sfn_memorypool.h"
#include "gallium/drivers/r600/sfn/sfn_instr_mem.h"
#include "gallium/drivers/r600/sfn/sfn_nir.h"
#include "gallium/drivers/r600/sfn/sfn_nir_lower_alu.h"
#include "gallium/include/pipe/p_shader_tokens.h"
#include "gallium/include/pipe/p_state.h"
#include "util/macros.h"
#include "util/mesa-blake3.h"
#include "amd_family.h"
#include "nir.h"
#include "vk_log.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

/* Whether the shader writes memory through the r600 UAV intrinsics its storage buffer and image
 * accesses were lowered to. Every UAV operation writes except the two no-ops: `NOP`, and `NOP_RTN`,
 * which is how a read is spelled -- the other returning ones are atomics.
 */
static bool
terakan_shader_nir_writes_memory_through_uav(nir_shader * const nir)
{
   nir_foreach_function_impl (impl, nir) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_intrinsic) {
               continue;
            }
            nir_intrinsic_instr * const intrinsic = nir_instr_as_intrinsic(instr);
            switch (intrinsic->intrinsic) {
            case nir_intrinsic_uav_instr_r600:
            case nir_intrinsic_uav_returning_instr_r600:
               break;
            default:
               continue;
            }
            unsigned const uav_op = nir_intrinsic_uav_op_r600(intrinsic);
            if (uav_op != r600::RatInstr::NOP && uav_op != r600::RatInstr::NOP_RTN) {
               return true;
            }
         }
      }
   }
   return false;
}

VkResult
terakan_shader_impl_compile(terakan_shader_impl * const shader, terakan_device * const device,
                            r600_shader_key const * const key, nir_shader * const nir,
                            VkAllocationCallbacks const * const allocator)
{
   terakan_physical_device const & physical_device = *terakan_device_physical_device(device);
   terakan_physical_device_chip_info const & chip_info = physical_device.chip_info;
   amd_gfx_level const gfx_level = terakan_shader_gfx_level(chip_info.chip_family);
   r600_chip_class const isa_chip_class = terakan_shader_isa_chip_class(chip_info.chip_family);

   /* TODO(Triang3l): Fill stream output info from NIR. */
   pipe_stream_output_info so_info = {};

   r600::init_pool();

   /* Terakan doesn't run Gallium's r600_finalize_nir_common. Apply the
    * backend-mandatory lowerings that SFN relies on explicitly.
    */
   static nir_lower_subgroups_options const subgroup_options = {
      .subgroup_size = 1,
      .ballot_bit_size = 32,
      .ballot_components = 1,
   };
   NIR_PASS(_, nir, terakan_nir_lower_subgroups);
   NIR_PASS(_, nir, nir_opt_uniform_subgroup, &subgroup_options);
   NIR_PASS(_, nir, r600_nir_lower_pack_unpack_2x16);
   NIR_PASS(_, nir, r600_lower_shared_io);

   /* The backend gives every store_output to a position slot its own export, so the components of
    * one slot have to arrive in one store. In the Gallium path nir_opt_combine_stores runs on
    * nir_var_shader_out before nir_lower_io and that is what merges them; under Vulkan the outputs
    * are already store_output intrinsics by the time the shader gets here, so there is nothing left
    * for it to combine and the components stay scalar.
    *
    * gl_ClipDistance is what noticed: two distances of one array became two position exports, POS1
    * and POS2, so the second landed in CCDIST1 -- the slot for distances four to seven, whose
    * enable is not even set -- and had no effect. dEQP-VK.clipping.user_defined failed 59 of its 64
    * cases, and the only ones that passed were the ones using a single distance.
    */
   /* For r600_lower_and_optimize_nir, for fields like number bit sizes, and also for
    * DB_SHADER_CONTROL in fragment shaders.
    */
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   /* `nir_shader_gather_info` decides `writes_memory` from the portable intrinsics, and by the
    * time a shader arrives here its storage buffer and image writes have already been lowered to
    * the r600 UAV ones, which it has never heard of. The fragment stage needs the answer to be
    * right: a shader with side effects has to run even where the depth or stencil test rejects
    * every fragment, and leaving it on early Z means it never does.
    */
   nir->info.writes_memory |= terakan_shader_nir_writes_memory_through_uav(nir);

   r600_lower_and_optimize_nir(nir, key, gfx_level, &so_info);

   if (unlikely(getenv("TERAKAN_DUMP_HANGOVER_VERTEX_NIR") != nullptr) && nir->info.name != nullptr &&
       (!strcmp(nir->info.name, "084bedbeef7924b534d4157927e0e4a8fc8e2749") ||
        !strcmp(nir->info.name, "57931c606823732885ad1cd6f6fbbcce8f02c0ce") ||
        !strcmp(nir->info.name, "afc8b6c53206c3e7ab3a56cde73155696d6244e7"))) {
      static std::mutex dump_mutex;
      std::lock_guard<std::mutex> const dump_lock(dump_mutex);
      fprintf(stderr, "\n===== TERAKAN HANGOVER NIR %s =====\n", nir->info.name);
      nir_print_shader(nir, stderr);
      fprintf(stderr, "===== END TERAKAN HANGOVER NIR %s =====\n", nir->info.name);
   }

   r600::ShaderBindingLayout binding_layout;
   binding_layout.texture_resource_offset = 0;
   binding_layout.keep_all_vertex_inputs = key->vs.keep_all_vertex_inputs;

   r600::Shader * const unscheduled_sfn_shader = r600::Shader::translate_from_nir(
      nir, &so_info, nullptr, *key, isa_chip_class, chip_info.chip_family, binding_layout);
   if (unscheduled_sfn_shader == nullptr) {
      r600::release_pool();
      return vk_errorf(device, VK_ERROR_UNKNOWN, "Failed to translate the shader from NIR");
   }

   r600_finalize_and_optimize_shader(unscheduled_sfn_shader);
   r600::Shader * const sfn_shader = r600_schedule_shader(unscheduled_sfn_shader);
   if (sfn_shader != unscheduled_sfn_shader) {
      delete unscheduled_sfn_shader;
   }
   if (sfn_shader == nullptr) {
      r600::release_pool();
      return vk_errorf(device, VK_ERROR_UNKNOWN, "Failed to schedule the shader");
   }

   /* With the current size calculation, nir->scratch_size is in vec4 units. */
   /* TODO(Triang3l): Reject shaders with an overflowing scratch size. */
   shader->scratch_item_size_dwords = 4 * nir->scratch_size;

   sfn_shader->get_shader_info(&shader->shader);
   /* Pre-applied during binding lowering. */
   shader->shader.rat_base = 0;

   /* TODO(Triang3l): has_compressed_msaa_texturing. */
   r600_bytecode_init(&shader->shader.bc, gfx_level, chip_info.chip_family, false);

   /* We already schedule the code with this in mind, no need to handle this in the backend
    * assembler.
    */
   shader->shader.bc.ar_handling = AR_HANDLE_NORMAL;
   shader->shader.bc.r6xx_nop_after_rel_dst = 0;

   shader->shader.bc.type = shader->shader.processor_type;
   shader->shader.bc.isa = physical_device.isa;
   shader->shader.bc.ngpr = sfn_shader->required_registers();

   r600::Assembler assembler(&shader->shader, *key);
   if (!assembler.lower(sfn_shader)) {
      delete sfn_shader;

      r600::release_pool();

      r600_bytecode_clear(&shader->shader.bc);

      return vk_errorf(device, VK_ERROR_UNKNOWN, "Failed to lower the shader to assembly");
   }

   delete sfn_shader;

   r600::release_pool();

   if (r600_bytecode_build(&shader->shader.bc) != 0) {
      r600_bytecode_clear(&shader->shader.bc);
      return vk_errorf(device, VK_ERROR_UNKNOWN, "Failed to build the shader bytecode");
   }
   if (unlikely(getenv("TERAKAN_DUMP_HANGOVER_BYTECODE") != nullptr) &&
       nir->info.name != nullptr &&
       (!strcmp(nir->info.name, "084bedbeef7924b534d4157927e0e4a8fc8e2749") ||
        !strcmp(nir->info.name, "57931c606823732885ad1cd6f6fbbcce8f02c0ce") ||
        !strcmp(nir->info.name, "afc8b6c53206c3e7ab3a56cde73155696d6244e7"))) {
      static std::mutex disasm_mutex;
      std::lock_guard<std::mutex> const disasm_lock(disasm_mutex);
      fprintf(stderr, "\n===== TERAKAN HANGOVER BYTECODE %s =====\n", nir->info.name);
      r600_bytecode_disasm(&shader->shader.bc);
      fprintf(stderr, "===== END TERAKAN HANGOVER BYTECODE %s =====\n", nir->info.name);
   }
   if (unlikely(getenv("TERAKAN_DEBUG_DUMP_COMPUTE_BYTECODE") != nullptr) &&
       nir->info.stage == MESA_SHADER_COMPUTE) {
      static std::mutex compute_disasm_mutex;
      std::lock_guard<std::mutex> const disasm_lock(compute_disasm_mutex);
      fprintf(stderr, "\n===== TERAKAN COMPUTE BYTECODE %s =====\n",
              nir->info.name != nullptr ? nir->info.name : "unnamed");
      r600_bytecode_disasm(&shader->shader.bc);
      fprintf(stderr, "===== END TERAKAN COMPUTE BYTECODE =====\n");
   }
   if (unlikely(getenv("TERAKAN_DEBUG_DUMP_FRAGMENT_BYTECODE") != nullptr) &&
       nir->info.stage == MESA_SHADER_FRAGMENT) {
      static std::mutex fragment_disasm_mutex;
      std::lock_guard<std::mutex> const disasm_lock(fragment_disasm_mutex);
      fprintf(stderr, "\n===== TERAKAN FRAGMENT BYTECODE %s =====\n",
              nir->info.name != nullptr ? nir->info.name : "unnamed");
      r600_bytecode_disasm(&shader->shader.bc);
      fprintf(stderr, "===== END TERAKAN FRAGMENT BYTECODE =====\n");
   }
   char const * const graphics_machine_hash =
      getenv("TERAKAN_DEBUG_DUMP_GRAPHICS_MACHINE_BLAKE3");
   if (unlikely(graphics_machine_hash != nullptr &&
                strlen(graphics_machine_hash) == BLAKE3_HEX_LEN - 1 &&
                nir->info.stage == MESA_SHADER_FRAGMENT)) {
      blake3_hash bytecode_hash;
      char bytecode_hash_hex[BLAKE3_HEX_LEN];
      _mesa_blake3_compute(shader->shader.bc.bytecode,
                           shader->shader.bc.ndw * sizeof(uint32_t), bytecode_hash);
      _mesa_blake3_format(bytecode_hash_hex, bytecode_hash);
      if (!strcmp(graphics_machine_hash, bytecode_hash_hex)) {
         static std::mutex graphics_disasm_mutex;
         static bool graphics_disassembled = false;
         std::lock_guard<std::mutex> const disasm_lock(graphics_disasm_mutex);
         if (!graphics_disassembled) {
            graphics_disassembled = true;
            fprintf(stderr, "\n===== TERAKAN GRAPHICS BYTECODE %s =====\n",
                    bytecode_hash_hex);
            r600_bytecode_disasm(&shader->shader.bc);
            fprintf(stderr, "===== END TERAKAN GRAPHICS BYTECODE =====\n");
         }
      }
   }
   /* Fill shader registers and other info. */

   shader->static_state.sq_pgm_resources[0] = S_028844_NUM_GPRS(shader->shader.bc.ngpr) |
                                              S_028844_STACK_SIZE(shader->shader.bc.nstack) |
                                              S_028844_DX10_CLAMP(1);
   /* TODO(Triang3l): Rounding modes from shader float controls. */
   shader->static_state.sq_pgm_resources[1] = S_028848_SINGLE_ROUND(V_SQ_ROUND_NEAREST_EVEN) |
                                              S_028848_DOUBLE_ROUND(V_SQ_ROUND_NEAREST_EVEN);

   /* TODO(Triang3l): Correct vertex pipeline stages. */
   switch (nir->info.stage) {
   case MESA_SHADER_VERTEX: {
      std::memset(shader->static_state.stage.vs.spi_vs_out_id, 0,
                  sizeof(shader->static_state.stage.vs.spi_vs_out_id));
      for (unsigned output_index = 0; output_index < shader->shader.noutput; ++output_index) {
         r600_shader_io const & output = shader->shader.output[output_index];
         if (output.export_param >= 0) {
            unsigned & parameter_spi_vs_out_id =
               shader->static_state.stage.vs.spi_vs_out_id[output.export_param / 4];
            unsigned const parameter_shift = (output.export_param & 3) * 8;
            assert(!(parameter_spi_vs_out_id & ((uint32_t)0xFF << parameter_shift)));
            parameter_spi_vs_out_id |= (uint32_t)output.spi_sid << parameter_shift;
         }
      }

      shader->static_state.stage.vs.spi_vs_out_config =
         S_0286C4_VS_EXPORT_COUNT(shader->shader.highest_export_param);

      uint32_t const clip_distances_enabled =
         (((uint32_t)1 << nir->info.clip_distance_array_size) - 1);
      uint32_t const cull_distances_enabled =
         (((uint32_t)1 << nir->info.cull_distance_array_size) - 1)
         << nir->info.clip_distance_array_size;
      uint32_t const clip_cull_distances_enabled = clip_distances_enabled | cull_distances_enabled;
      shader->static_state.stage.vs.pa_cl_vs_out_cntl =
         clip_distances_enabled |
         ((cull_distances_enabled | (getenv("TKDBG_CULL") != NULL ? clip_distances_enabled : 0u))
          << 8) |
         S_02881C_USE_VTX_POINT_SIZE(shader->shader.vs_out_point_size) |
         S_02881C_USE_VTX_RENDER_TARGET_INDX(shader->shader.vs_out_layer) |
         S_02881C_USE_VTX_VIEWPORT_INDX(shader->shader.vs_out_viewport) |
         S_02881C_VS_OUT_MISC_VEC_ENA(shader->shader.vs_out_misc_write) |
         S_02881C_VS_OUT_CCDIST0_VEC_ENA((clip_cull_distances_enabled & 0b00001111) != 0) |
         S_02881C_VS_OUT_CCDIST1_VEC_ENA((clip_cull_distances_enabled & 0b11110000) != 0);
      if (getenv("TKDBG_CLIP") != NULL) { /* TKDBG */
         fprintf(stderr,
                 "TKDBG clip nir_clip=%u nir_cull=%u sfn_clip_write=0x%x sfn_cc_mask=0x%x"
                 " cntl=0x%08x highest_param=%u\n",
                 nir->info.clip_distance_array_size, nir->info.cull_distance_array_size,
                 shader->shader.clip_dist_write, shader->shader.cc_dist_mask,
                 shader->static_state.stage.vs.pa_cl_vs_out_cntl,
                 shader->shader.highest_export_param);
      }
   } break;

   case MESA_SHADER_FRAGMENT: {
      uint32_t db_shader_control =
         S_02880C_KILL_ENABLE(shader->shader.uses_kill) |
         S_02880C_CONSERVATIVE_Z_EXPORT(nir->info.fs.depth_layout == FRAG_DEPTH_LAYOUT_GREATER
                                           ? V_02880C_EXPORT_GREATER_THAN_Z
                                           : (nir->info.fs.depth_layout == FRAG_DEPTH_LAYOUT_LESS
                                                 ? V_02880C_EXPORT_LESS_THAN_Z
                                                 : V_02880C_EXPORT_ANY_Z));
      for (unsigned output_index = 0; output_index < shader->shader.noutput; ++output_index) {
         switch (shader->shader.output[output_index].frag_result) {
         case FRAG_RESULT_DEPTH:
            db_shader_control |= S_02880C_Z_EXPORT_ENABLE(1);
            break;
         case FRAG_RESULT_STENCIL:
            db_shader_control |= S_02880C_STENCIL_EXPORT_ENABLE(1);
            break;
         case FRAG_RESULT_SAMPLE_MASK:
            db_shader_control |= S_02880C_MASK_EXPORT_ENABLE(1);
            break;
         default:
            break;
         }
      }
      db_shader_control |= S_02880C_DB_SOURCE_FORMAT(
         db_shader_control & S_02880C_MASK_EXPORT_ENABLE(1)
            ? (db_shader_control & S_02880C_Z_EXPORT_ENABLE(1) ? V_02880C_EXPORT_DB_FULL
                                                               : V_02880C_EXPORT_DB_FOUR16)
            : V_02880C_EXPORT_DB_TWO);
      db_shader_control |= S_02880C_DUAL_EXPORT_ENABLE(
         G_02880C_DB_SOURCE_FORMAT(db_shader_control) != V_02880C_EXPORT_DB_FULL);
      /* See RadeonSI DB_SHADER_CONTROL setup for more details about the possible Z order and
       * EXEC_ON_* cases.
       * Not using ReZ currently due to unknown performance impact.
       */
      if (nir->info.fs.early_fragment_tests) {
         db_shader_control |= S_02880C_DEPTH_BEFORE_SHADER(1) |
                              S_02880C_Z_ORDER(V_02880C_EARLY_Z_THEN_LATE_Z) |
                              S_02880C_EXEC_ON_NOOP(nir->info.writes_memory);
      } else if (nir->info.writes_memory) {
         db_shader_control |= S_02880C_Z_ORDER(V_02880C_LATE_Z) | S_02880C_EXEC_ON_HIER_FAIL(1);
      } else {
         db_shader_control |= S_02880C_Z_ORDER(V_02880C_EARLY_Z_THEN_LATE_Z);
      }
      shader->fs.db_shader_control = db_shader_control;

      shader->fs.per_sample_invocation =
         BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_SAMPLE_ID) ||
         BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_SAMPLE_POS);

      bool export_z = (db_shader_control &
                       ~(uint32_t)(C_02880C_Z_EXPORT_ENABLE & C_02880C_STENCIL_EXPORT_ENABLE &
                                   C_02880C_MASK_EXPORT_ENABLE)) != 0;
      /* Something must be exported, either at least one color or at least the DB export.
       * Explicitly ensuring that is not necessary in this code due to the + 1.
       */
      shader->static_state.stage.ps.sq_pgm_exports_ps =
         S_02884C_EXPORT_COLORS(shader->shader.ps_export_highest + 1) | S_02884C_EXPORT_Z(export_z);

      std::memset(shader->static_state.stage.ps.spi_ps_input_cntl, 0,
                  sizeof(shader->static_state.stage.ps.spi_ps_input_cntl));
      /* TODO(Triang3l): Build SPI_BARYC_CNTL in the shader compiler rather than independently here
       * because GPR allocation in the shader depends on it.
       */
      shader->static_state.stage.ps.spi_baryc_cntl = 0;
      uint32_t interpolator_count = 0;
      r600_shader_io const * position_input = nullptr;
      uint32_t face_and_sample_mask_gpr = UINT32_MAX;
      uint32_t sample_id_gpr = UINT32_MAX;
      for (unsigned input_index = 0; input_index < shader->shader.ninput; ++input_index) {
         r600_shader_io const & input = shader->shader.input[input_index];
         if (input.varying_slot == VARYING_SLOT_POS) {
            assert(position_input == nullptr);
            position_input = &input;
         } else if (input.varying_slot == VARYING_SLOT_FACE ||
                    input.system_value == SYSTEM_VALUE_SAMPLE_MASK_IN) {
            assert(face_and_sample_mask_gpr == UINT32_MAX || face_and_sample_mask_gpr == input.gpr);
            face_and_sample_mask_gpr = input.gpr;
         } else if (input.system_value == SYSTEM_VALUE_SAMPLE_ID) {
            assert(sample_id_gpr == UINT32_MAX);
            sample_id_gpr = input.gpr;
         } else if (input.spi_sid != 0) {
            interpolator_count = MAX2(input.lds_pos + 1, interpolator_count);
            shader->static_state.stage.ps.spi_ps_input_cntl[input.lds_pos] =
               S_028644_SEMANTIC(input.spi_sid) |
               S_028644_FLAT_SHADE(input.interpolate == TGSI_INTERPOLATE_CONSTANT) |
               S_028644_PT_SPRITE_TEX(input.varying_slot == VARYING_SLOT_PNTC);
            bool const interpolator_is_linear = input.interpolate == TGSI_INTERPOLATE_LINEAR;
            if (interpolator_is_linear || input.interpolate == TGSI_INTERPOLATE_PERSPECTIVE ||
                input.interpolate == TGSI_INTERPOLATE_COLOR) {
               switch (input.interpolate_location) {
               case TGSI_INTERPOLATE_LOC_CENTER:
                  shader->static_state.stage.ps.spi_baryc_cntl |= interpolator_is_linear
                                                                     ? S_0286E0_LINEAR_CENTER_ENA(1)
                                                                     : S_0286E0_PERSP_CENTER_ENA(1);
                  break;
               case TGSI_INTERPOLATE_LOC_CENTROID:
                  shader->static_state.stage.ps.spi_baryc_cntl |=
                     interpolator_is_linear ? S_0286E0_LINEAR_CENTROID_ENA(1)
                                            : S_0286E0_PERSP_CENTROID_ENA(1);
                  break;
               case TGSI_INTERPOLATE_LOC_SAMPLE:
                  shader->static_state.stage.ps.spi_baryc_cntl |= interpolator_is_linear
                                                                     ? S_0286E0_LINEAR_SAMPLE_ENA(1)
                                                                     : S_0286E0_PERSP_SAMPLE_ENA(1);
                  break;
               default:
                  break;
               }
            }
         }
      }
      if (!shader->static_state.stage.ps.spi_baryc_cntl) {
         shader->static_state.stage.ps.spi_baryc_cntl = S_0286E0_PERSP_CENTER_ENA(1);
      }
      constexpr uint32_t spi_baryc_cntl_persp_clear =
         C_0286E0_PERSP_CENTER_ENA & C_0286E0_PERSP_CENTROID_ENA & C_0286E0_PERSP_SAMPLE_ENA &
         C_0286E0_PERSP_PULL_MODEL_ENA;
      constexpr uint32_t spi_baryc_cntl_linear_clear =
         C_0286E0_LINEAR_CENTER_ENA & C_0286E0_LINEAR_CENTROID_ENA & C_0286E0_LINEAR_SAMPLE_ENA;
      shader->static_state.stage.ps.spi_ps_in_control[0] =
         S_0286CC_NUM_INTERP(MAX2(interpolator_count, 1)) |
         S_0286CC_PERSP_GRADIENT_ENA(
            (shader->static_state.stage.ps.spi_baryc_cntl & ~spi_baryc_cntl_persp_clear) != 0) |
         S_0286CC_LINEAR_GRADIENT_ENA(
            (shader->static_state.stage.ps.spi_baryc_cntl & ~spi_baryc_cntl_linear_clear) != 0);
      if (position_input != nullptr) {
         /* Section "Sample Shading" of the Vulkan 1.4.349 specification says that when the shader
          * runs per sample, `FragCoord` is the sample's position rather than the pixel's centre.
          * A shader reading `SampleId` or `SamplePosition` always runs per sample - see
          * `per_sample_invocation` - so its position has to follow the sample.
          */
         bool const position_at_sample =
            position_input->interpolate_location == TGSI_INTERPOLATE_LOC_SAMPLE ||
            shader->fs.per_sample_invocation;
         shader->static_state.stage.ps.spi_ps_in_control[0] |=
            S_0286CC_POSITION_ENA(1) |
            S_0286CC_POSITION_CENTROID(!position_at_sample &&
                                       position_input->interpolate_location ==
                                          TGSI_INTERPOLATE_LOC_CENTROID) |
            S_0286CC_POSITION_SAMPLE(position_at_sample) |
            S_0286CC_POSITION_ADDR(position_input->gpr);
      }
      shader->static_state.stage.ps.spi_ps_in_control[1] = 0;
      if (face_and_sample_mask_gpr != UINT32_MAX) {
         shader->static_state.stage.ps.spi_ps_in_control[1] |=
            S_0286D0_FRONT_FACE_ENA(1) | S_0286D0_FRONT_FACE_ADDR(face_and_sample_mask_gpr);
      }
      if (sample_id_gpr != UINT32_MAX) {
         shader->static_state.stage.ps.spi_ps_in_control[1] |=
            S_0286D0_FIXED_PT_POSITION_ENA(1) | S_0286D0_FIXED_PT_POSITION_ADDR(sample_id_gpr);
      }
      shader->static_state.stage.ps.spi_input_z =
         S_0286D8_PROVIDE_Z_TO_SPI(position_input != nullptr);

      if (chip_info.is_terascale_1) {
         /* R600/R700 has no SPI_BARYC_CNTL. Build its per-input interpolation selections and
          * SPI_PS_IN_CONTROL payload from generation-neutral shader IO instead of passing the
          * Evergreen-shaped state to registers that have different meanings on this generation.
          */
         terakan_hw_config_draw_terascale_1_spi_ps_input
            r700_inputs[TERAKAN_HW_CONFIG_DRAW_TERASCALE_1_SPI_PS_INPUT_COUNT] = {};
         if (shader->shader.ninput > ARRAY_SIZE(r700_inputs)) {
            r600_bytecode_clear(&shader->shader.bc);
            return vk_errorf(device, VK_ERROR_UNKNOWN,
                             "Too many fragment shader inputs for TeraScale 1 SPI");
         }
         for (unsigned input_index = 0; input_index < shader->shader.ninput; ++input_index) {
            r600_shader_io const & input = shader->shader.input[input_index];
            r700_inputs[input_index] = {
               .semantic = static_cast<uint32_t>(input.spi_sid),
               .gpr = input.gpr,
               .position = input.varying_slot == VARYING_SLOT_POS,
               .front_face_or_sample_mask =
                  input.varying_slot == VARYING_SLOT_FACE ||
                  input.system_value == SYSTEM_VALUE_SAMPLE_MASK_IN,
               .sample_id = input.system_value == SYSTEM_VALUE_SAMPLE_ID,
               .flat = input.interpolate == TGSI_INTERPOLATE_CONSTANT,
               .centroid =
                  input.interpolate_location == TGSI_INTERPOLATE_LOC_CENTROID,
               .linear = input.interpolate == TGSI_INTERPOLATE_LINEAR,
               .point_sprite = input.varying_slot == VARYING_SLOT_PNTC,
               .sample = input.interpolate_location == TGSI_INTERPOLATE_LOC_SAMPLE,
            };
         }

         terakan_hw_config_draw_terascale_1_spi_ps r700_spi;
         if (!terakan_hw_config_draw_terascale_1_spi_ps_encode(
                r700_inputs, shader->shader.ninput, &r700_spi)) {
            r600_bytecode_clear(&shader->shader.bc);
            return vk_errorf(device, VK_ERROR_UNKNOWN,
                             "Invalid fragment shader input allocation for TeraScale 1 SPI");
         }
         std::memcpy(shader->static_state.stage.ps.spi_ps_input_cntl, r700_spi.input_control,
                     sizeof(r700_spi.input_control));
         shader->static_state.stage.ps.spi_ps_in_control[0] = r700_spi.in_control_0;
         shader->static_state.stage.ps.spi_ps_in_control[1] = r700_spi.in_control_1;
         shader->static_state.stage.ps.spi_input_z = r700_spi.input_z;
         /* Kept zero as software state too, so accidental future common emission is visible in
          * packet tests rather than looking like a plausible Evergreen barycentric mode.
          */
         shader->static_state.stage.ps.spi_baryc_cntl = 0;
      }

      shader->static_state.stage.ps.cb_shader_mask = shader->shader.ps_color_export_mask;
   } break;

   default:
      break;
   }

   if (nir->info.stage == MESA_SHADER_VERTEX) {
      for (unsigned input_index = 0; input_index < shader->shader.ninput; ++input_index) {
         struct r600_shader_io const * const input = &shader->shader.input[input_index];
         assert(input->gpr > 0);
         assert(input->gpr - 1 < TERAKAN_RESOURCE_HW_COUNT_FETCH);
         if (input->gpr > 0 && input->gpr - 1 < TERAKAN_RESOURCE_HW_COUNT_FETCH) {
            shader->vs.vertex_attributes_needed |= BITFIELD_BIT(input->gpr - 1);
         }
      }
   }

   return VK_SUCCESS;
}
