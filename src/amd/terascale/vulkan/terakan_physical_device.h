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

#ifndef TERAKAN_PHYSICAL_DEVICE_H
#define TERAKAN_PHYSICAL_DEVICE_H

#include "terakan_instance.h"
#include "terakan_queue.h"
#include "terakan_shader_generation.h"
#include "wsi_common.h"

#include "gallium/drivers/r600/r600_isa.h"
#include "amd_family.h"
#include "nir.h"
#include "vk_physical_device.h"
#include "vk_sync.h"
#include "util/u_atomic.h"
#include "vk_sync_binary.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TERAKAN_PHYSICAL_DEVICE_VENDOR_ID_ATI 0x1002

static inline enum radeon_family
terakan_physical_device_get_chip_family(uint32_t const pci_device_id)
{
   /* CHIP_UNKNOWN is not an error, as this function filters unsupported physical devices. */
   return terakan_shader_family_from_pci_id(pci_device_id);
}

static inline bool
terakan_physical_device_is_chip_family_supported(enum radeon_family const chip_family)
{
   return chip_family >= CHIP_R600 && chip_family <= CHIP_ARUBA;
}

/* TeraScale 1 (R600 and R700, the CHIP_R600..CHIP_RV740 block of the enum) has no tessellator, a
 * fixed 64-lane wavefront, and a flat, single-variant `SQ_THREAD_RESOURCE_MGMT`/
 * `SQ_GPR_RESOURCE_MGMT`/`SQ_STACK_RESOURCE_MGMT` register set rather than TeraScale 2/3's
 * tessellation-stage-indexed one, so it takes a genuinely different code path through
 * `terakan_physical_device_chip_info_init` and a genuinely different set of `chip_info` fields
 * (the `terascale_1` member below), not just different constants plugged into the R8xx/R9xx ones.
 */
static inline bool
terakan_physical_device_chip_family_is_terascale_1(enum radeon_family const chip_family)
{
   return chip_family >= CHIP_R600 && chip_family <= CHIP_RV740;
}

/* Only meaningful when terakan_physical_device_chip_family_is_terascale_1() is true for the same
 * chip_family: true for R700 (RV770/RV730/RV710/RV740, contiguous in enum radeon_family), false for
 * R600 (R600/RV610/RV630/RV670/RV620/RV635/RS780/RS880, also contiguous and immediately preceding
 * R700 in the enum). Matches the classic Gallium R600 driver's own R600-vs-R700 gfx_level split
 * (radeon_drm_winsys.c's DRV_R600 chip_family switch).
 */
static inline bool
terakan_physical_device_chip_family_is_r700(enum radeon_family const chip_family)
{
   return chip_family >= CHIP_RV770 && chip_family <= CHIP_RV740;
}

char const * terakan_physical_device_chip_family_name(enum radeon_family chip_family);

struct terakan_physical_device_chip_info {
   uint32_t pci_device_id;

   enum radeon_family chip_family;

   bool is_r9xx;
   /* See terakan_physical_device_chip_family_is_terascale_1(). When true, none of the R8xx/R9xx
    * fields below (everything up to and including `terascale_1`) are meaningful; `terascale_1` is
    * populated instead. `is_r9xx` is left false, since none of the code it gates applies either.
    */
   bool is_terascale_1;

   bool has_dedicated_vram;

   bool has_vertex_cache;
   /* 1 shader engine if false, up to 2 shader engines if true. */
   bool two_shader_engines_max;
   /* In the AMD Accelerated Parallel Processing OpenCL Programming Guide rev2.3 (July 2012) device
    * parameters table, this is "Max Wavefronts / GPU" divided by the number of shader engines (and
    * for Hemlock, also divided by the number of GPUs), and divided by the allocation granularity on
    * R8xx (8 wavefronts).
    * https://web.archive.org/web/20131111194717/http://developer.amd.com/wordpress/media/2012/10/AMD_Accelerated_Parallel_Processing_OpenCL_Programming_Guide4.pdf
    */
   uint32_t sq_max_threads_shr3;
   /* [Tessellation stages enabled][geometry stage enabled][register]. */
   uint32_t sq_thread_resource_mgmt_ts_gs_r8xx[2][2][2];
   uint32_t sq_max_stack_entries;
   /* Log2 of "Max Work-Items / GPU" divided by "Max Wavefronts / GPU" in the device parameters
    * table.
    */
   unsigned wave_lanes_log2;
   /* `SQ_PSTMP_RING_SIZE` increment per `SQ_PSTMP_RING_ITEMSIZE` preferred for the
    * `SQ_THREAD_RESOURCE_MGMT` configuration from this structure.
    */
   uint32_t sq_pstmp_ring_bytes_per_item_dword_shr8;
   /* Lanes * waves * shader engines. */
   uint32_t uav_immediate_size_elements;

   /* For R8xx/R9xx, sourced from a per-chip-family static table, like everything else in this
    * function. For TeraScale 1, there is no such table in this driver -- the classic Gallium R600
    * driver itself does not use one either, it queries RADEON_INFO_NUM_BACKENDS from the kernel
    * unconditionally for every R600+ chip (see radeon_drm_winsys.c) and treats a failed query as
    * fatal -- so this is populated from that same kernel query, passed in via
    * chip_info_init's terascale_1_num_backends parameter, rather than guessed or left at the
    * deliberate zero placeholder chip_info_init used before this was wired up.
    */
   unsigned max_render_backends_log2;

   /* TeraScale 1 (R600/R700) only; see is_terascale_1 above. One flat set of GPR/thread/stack-entry
    * counts per shader stage, unlike R8xx/R9xx's tessellation-indexed sq_thread_resource_mgmt.
    * Sourced from the per-chip-family switch in r600_init_atom_start_cs() in
    * src/gallium/drivers/r600/r600_state.c, the classic Gallium R600 driver that has supported this
    * hardware for years and is the authoritative reference here, not a guess.
    *
    * Minimal logical-device creation consumes these values on hardware-validated R600 and R700.
    * Queue submission is still refused before the winsys until command streams built from this
    * state are validated independently on both generations.
    */
   struct {
      uint32_t num_ps_gprs, num_vs_gprs, num_temp_gprs, num_gs_gprs, num_es_gprs;
      uint32_t num_ps_threads, num_vs_threads, num_gs_threads, num_es_threads;
      uint32_t num_ps_stack_entries, num_vs_stack_entries, num_gs_stack_entries,
         num_es_stack_entries;
   } terascale_1;
};

/* terascale_1_num_backends is the raw render backend count from the RADEON_INFO_NUM_BACKENDS DRM
 * ioctl, or 0 if not queried/available. It is ignored for R8xx/R9xx, which use a per-family static
 * table instead (see max_render_backends_log2's comment). It is required to get a real
 * max_render_backends_log2 for TeraScale 1, which has no such table in this driver: passing 0
 * leaves the deliberate zero placeholder described there.
 */
void
terakan_physical_device_chip_info_init(uint32_t pci_device_id, uint32_t terascale_1_num_backends,
                                       struct terakan_physical_device_chip_info * chip_info_out);

struct terakan_physical_device_tiling_info {
   uint8_t pipes_log2;
   uint8_t banks_log2;
   uint8_t pipe_interleave_bytes_log2;
   uint8_t bank_interleave_log2;
   uint8_t row_bytes_log2;
};

static inline bool
terakan_physical_device_tiling_info_equal(
   struct terakan_physical_device_tiling_info const * const a,
   struct terakan_physical_device_tiling_info const * const b)
{
   return a->pipes_log2 == b->pipes_log2 && a->banks_log2 == b->banks_log2 &&
          a->pipe_interleave_bytes_log2 == b->pipe_interleave_bytes_log2 &&
          a->row_bytes_log2 == b->row_bytes_log2;
}

struct terakan_physical_device_submission_info {
   enum terakan_queue_relocation_type relocation_type;
   /* Maximum amount of additional data that may be added to submissions in the queue `submit`
    * function by the winsys, but that doesn't need to be reserved inside the `submit` arguments
    * themselves.
    */
   struct terakan_queue_submission_size submission_outer_reserved_amount;
};

struct terakan_physical_device_submission_info_gfx {
   struct terakan_physical_device_submission_info base;

   /* Whether the kernel driver incorrectly validates buffer UAVs, treating them as images.
    *
    * In CB_COLOR registers of buffer UAVs, Terakan specifies the smallest possible PITCH_TILE_MAX
    * and SLICE_TILE_MAX taking into account the alignment requirements for the element size.
    * Therefore, the kernel driver assumes that the CB_COLOR points to a single row with
    * `terakan_format_pitch_alignment_linear_surfels` elements, and expects them to be inside the
    * bounds of the BO.
    *
    * This means that a 16 bytes per element UAV placed at 256 bytes in the BO, considering that the
    * minimum possible pitch is 64 elements, will be assumed to span bytes [256, 1280), so the size
    * of the BO must be sufficiently padded to make it possible to create a UAV (including in meta
    * actions) at any offset allowed for the given usage scenario by Vulkan.
    *
    * It may be possible to avoid adding too much padding, however, by moving the base address
    * offsetting to shaders partially or fully. In the example provided above, if the base address
    * is set to 0 instead (with the 256-byte offset applied in the shader), the BO only needs to be
    * padded to 1024 bytes rather than to 1280.
    *
    * Given that there already is shader offsetting logic that lets applications create storage
    * buffers and storage texel buffers with alignment requirements smaller than the pipe
    * interleave, it's trivial to extend that functionality to also handle this.
    */
   bool buffer_uav_validated_as_image;

   /* Whether submissions referencing kcache buffers need to reset the constants mode to DX10 using
    * the MODE_CONTROL packet beforehand.
    */
   bool need_sq_alu_const_mode_control;
};

struct terakan_physical_device;
struct terakan_device;

struct terakan_physical_device_winsys_fn {
   /* Called before vk_physical_device_init to get capabilities of additional extensions offered by
    * the winsys, as well as the device UUID. It's assumed that before the call, the fields this
    * function is supposed to set are zero in the capability structures.
    */
   void (*get_winsys_extensions)(struct terakan_physical_device const * device,
                                 struct vk_device_extension_table * extensions,
                                 struct vk_features * features, struct vk_properties * properties);

   VkResult (*create_device)(struct terakan_physical_device * physical_device,
                             VkDeviceCreateInfo const * create_info,
                             VkAllocationCallbacks const * allocator,
                             struct terakan_device ** device_out);

   /* Returns system-wide usage reported by the kernel. Optional when
    * VK_EXT_memory_budget isn't exposed by the winsys. */
   void (*get_memory_usage)(struct terakan_physical_device const * device,
                            VkDeviceSize * vram_usage_out, VkDeviceSize * gtt_usage_out);

   void (*destroy)(struct terakan_physical_device * device);
};

/* Partially implemented by the winsys. */
struct terakan_physical_device {
   struct vk_physical_device vk;

   /* Private, use wrappers externally. */
   struct terakan_physical_device_winsys_fn const * winsys_fn;

   struct terakan_physical_device_chip_info chip_info;

   VkDeviceSize max_memory_allocation_size;

   struct terakan_physical_device_tiling_info tiling_info;
   VkDeviceSize buffer_image_bo_alignment;

   struct terakan_physical_device_submission_info_gfx submission_info_gfx;

   /* nir_shader_compiler_options's lifetime must be at least as long as that of any NIR shader. */
   nir_shader_compiler_options nir_options_non_fs;
   nir_shader_compiler_options nir_options_fs;
   struct r600_isa * isa;

   VkPhysicalDeviceMemoryProperties memory_properties;
   uint64_t memory_heap_usage[VK_MAX_MEMORY_HEAPS];

   struct wsi_device wsi_device;
};

VK_DEFINE_HANDLE_CASTS(terakan_physical_device, vk.base, VkPhysicalDevice,
                       VK_OBJECT_TYPE_PHYSICAL_DEVICE)

VkExternalMemoryHandleTypeFlags terakan_physical_device_supported_external_memory_types(
   struct terakan_physical_device const * device);

void terakan_physical_device_finish(struct terakan_physical_device * device);

void terakan_physical_device_destroy(struct vk_physical_device * device);

/* vram_visible is included in vram_size.
 * clock_crystal_frequency_hz can be 0 if not available, in this case timestamp queries will be
 * disabled.
 * terascale_1_num_backends is forwarded to chip_info_init -- see its comment there.
 */
VkResult terakan_physical_device_init(
   struct terakan_physical_device * device, struct terakan_instance * instance,
   struct terakan_physical_device_winsys_fn const * winsys_fn_static, uint32_t pci_device_id,
   VkDeviceSize gtt_allocation_granularity, VkDeviceSize gtt_size, VkDeviceSize vram_size,
   VkDeviceSize vram_visible, VkDeviceSize max_memory_allocation_size,
   VkDeviceSize min_memory_map_alignment,
   struct terakan_physical_device_tiling_info const * tiling_info,
   struct terakan_physical_device_submission_info_gfx const * submission_info_gfx,
   uint32_t clock_crystal_frequency_hz, uint32_t terascale_1_num_backends,
   struct vk_sync_type const * const * supported_sync_types_static);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_PHYSICAL_DEVICE_H */
