# Terakan development TODO

Last updated: 2026-08-22. Primary target: stable game rendering on AMD
CAICOS with DXVK-Sarek.

Importance measures the expected effect on real games. Complexity includes
implementation, hardware research, and the CAICOS regression test needed to
accept the work. Both use a 1–5 scale.

## P0 — game-rendering blockers

| Work item | Importance | Complexity | Feasibility | Acceptance criteria |
|---|---:|---:|---|---|
| Complete depth/stencil resolve and layout transitions | 5/5 | 3/5 | Sample-zero depth resolve is implemented, exposed and regression-covered. Stencil resolve is implemented but cannot be advertised: multisample stencil does not read back at all, which is a defect below the resolve and is tracked separately below. The averaging and min/max depth modes and partial region and mip/layer coverage also remain | Depth and stencil readbacks pass for partial regions, mip levels, array layers and every advertised sample count |
| Fix multisample stencil sampling | 5/5 | 4/5 | Narrowed to multisample addressing. Single-sample stencil sampling was a swizzle bug and is fixed; the multisample case still reads zeroes with a correct swizzle, base address, pitch and format, so the remaining suspect is the tile split the stencil aspect inherits from depth. Blocks stencil resolve, and therefore dynamic rendering | `terakan_stencil_msaa_fetch` passes instead of being expected to fail |
| Implement and validate FMASK/CMASK allocation, identity initialization and sampled MSAA addressing | 5/5 | 5/5 | Implementable; requires Evergreen tiling research | Per-sample reads and resolved reads pass for 2x/4x/8x images without corrupting ordinary color targets |
| Complete cache and barrier coherency | 5/5 | 4/5 | Implementable | Focused attachment, texture, storage, transfer, graphics/compute and query producer-consumer chains pass without application-specific waits |
| Cover remaining copy, blit and resolve format/subresource combinations | 5/5 | 4/5 | Implementable | Boundary tests cover non-zero offsets, partial extents, mip levels, array/3D layers and every advertised compatible format class |
| Fix 3D image mip base alignment and descriptor addressing | 5/5 | 3/5 | Implementable; the radeon CS validator gives an exact 4096-byte alignment failure | The formerly device-losing 3D clear passes, adjacent mip/layer cases pass, and the kernel reports no invalid command stream |
| Correct meta blit formats, mirrored coordinates and 3D slices | 5/5 | 4/5 | Implementable | All basic CTS blits pass for RGBA, BGRA, R32, reversed source/destination axes and 3D slices |

## P1 — broad DXVK and D3D11 compatibility

| Work item | Importance | Complexity | Feasibility | Acceptance criteria |
|---|---:|---:|---|---|
| Enable geometry shaders and complete vertex-pipeline stage plumbing | 4/5 | 5/5 | Hardware-supported; see the vertex pipeline stage survey below | Focused GS tests pass and `geometryShader` is exposed only afterwards |
| Enable tessellation control/evaluation shaders | 4/5 | 5/5 | Hardware-supported; see the vertex pipeline stage survey below | Tessellation limits are reported from tested hardware behavior and representative pipelines pass |
| Complete stream output / transform feedback | 4/5 | 4/5 | Hardware-supported | SFN receives NIR stream-output metadata and D3D11 stream-output workloads pass readback tests |
| Complete storage-image/UAV format and atomic coverage | 4/5 | 4/5 | Mostly implementable; limited by hardware binding counts and formats | Every advertised storage-image format passes load, store and applicable integer-atomic tests |
| Implement multisample storage images | 4/5 | 4/5 | Implementable with format-aware lowering, FMASK work and RAT validation; single-sample formatless reads/writes now work | Multisample UAV loads/stores pass for every exposed format before the remaining feature bit is enabled |
| Implement extended image gather | 4/5 | 3/5 | Likely implementable through Evergreen texture instructions plus lowering | Component selection and constant/dynamic offset gather tests pass for all advertised sampled formats |
| Implement vertex-pipeline stores and atomics | 4/5 | 4/5 | Hardware-supported with stage-specific RAT synchronization work | VS/GS/TES storage writes and applicable atomics pass readback and cross-stage visibility tests before exposure |
| Enforce robust buffer and image bounds everywhere | 4/5 | 4/5 | Implementable with lowering and descriptor bounds; dynamic UNIFORM/STORAGE_BUFFER_DYNAMIC descriptor SIZE reclamping (the `resource[1]` read path) is done and regression-covered, the STORAGE_BUFFER_DYNAMIC UAV/color (tiling pitch/slice-encoded) path is still open | Guard regions remain intact for misaligned, dynamic and end-of-range accesses |
| Integrate query reset/copy/end synchronization with the common barrier machinery | 3/5 | 3/5 | Implementable | Occlusion, timestamp and pipeline-statistics queries pass reuse and cross-stage ordering tests |

## P2 — optional Vulkan functionality

These are useful, but Vulkan 1.1 permits the corresponding feature bits to be
`VK_FALSE`. They do not block a legal Vulkan 1.1 capability report.

| Work item | Importance | Complexity | Feasibility | Notes |
|---|---:|---:|---|---|
| Expose dynamic rendering | 2/5 | 2/5 | Implementable; VK_KHR_depth_stencil_resolve is now exposed, so only stencil resolve is still missing from the dependency | Blocked only by stencil resolve now |
| Lower 64-bit buffer accesses and finish 64-bit shader coverage | 2/5 | 4/5 | Partly implementable through 32-bit lowering | Native performance will remain limited |
| Add 16-bit storage support | 2/5 | 4/5 | Implementable through packing/lowering where necessary | Rare in the current DXVK-Sarek target set |
| Add multiview | 1/5 | 3/5 | Implementable | Primarily useful for VR and Vulkan-native applications |
| Add sampler YCbCr conversion | 1/5 | 3/5 | Implementable, likely shader-assisted | Primarily video-oriented |
| Add variable pointers | 1/5 | 4/5 | Potentially implementable through lowering | Low game priority |
| Complete shader float-control rounding modes | 1/5 | 3/5 | Partly hardware-limited | Expose only modes verified on R8xx |
| Complete device-group compute system values | 1/5 | 3/5 | Implementable with explicit driver constants | `dispatch_base`, `dispatch_base_maintenance5` when exposed, and `device_index` pass |

## Not planned for CAICOS

| Item | Importance | Reason |
|---|---:|---|
| Protected memory | 0/5 | The required protected execution and allocation model is unavailable with this hardware and the `radeon` kernel interface |
| Native high-performance FP64 | 1/5 | CAICOS lacks the hardware needed for a useful implementation; software lowering may be used only where practical |

## Completed and regression-covered

- Stencil-aspect view swizzle: a view of the stencil aspect alone is a
  single-component image whose value the Vulkan specification requires in R,
  but the driver was applying the combined depth/stencil format's swizzle,
  which places stencil in G, so sampling it returned a constant zero. View
  descriptors now expose the stencil channel as R. The aspect format tables are
  left alone because transfers use them for source and destination alike, where
  any consistent placement works, which is why transfers never showed this.
  `terakan_stencil_fetch` covers it.
- Multisample stencil sampling is still broken, and the write side is now ruled
  out. `terakan_stencil_msaa_fetch` can allocate the image on host-visible
  memory and dump the surface with `TERAKAN_PROBE_DUMP_SURFACE=1`: after the
  render pass clear, the stencil region holds exactly 128 non-zero bytes, all
  equal to the clear value, which is precisely 8x8 texels times 2 samples. So
  DB writes multisample stencil correctly and completely, and SQ reads
  elsewhere. Sweeping the stencil aspect's tile split across every value it can
  take changed nothing, so inheriting the depth aspect's tile split is not the
  cause, despite r600 and AddrLib keeping a separate `stencil_tile_split`. The
  base address, pitch, format and swizzle in the texture descriptor are all
  correct. Dumping where the written bytes sit shows both aspects written
  contiguously from the start of their own surface: depth at offsets 0 to 511,
  visible as a non-zero byte every fourth byte because the cleared float has
  three zero bytes, and stencil at offsets 0 to 127, exactly one 8x8 micro tile
  of 64 pixels times 2 samples.

  Addressing is ruled out entirely. `TERAKAN_PROBE_ADDRESS_MAP` in the probe
  splits the work into two submissions so the host can rewrite the surface
  between the clear and the fetch. Filling every byte of the whole allocation
  with a marker still makes the fetch return zeroes, so SQ is not reading this
  allocation at all: the fetch itself does not happen, rather than happening at
  the wrong offset. Splitting the submissions without touching memory changes
  nothing either, so it is not a barrier between the depth/stencil write and
  the texture read.

  Computing the stencil aspect's own tiling instead of inheriting depth's, the
  way r600 does for texturing the stencil aspect, also changed nothing.

  What is left is the descriptor and format combination itself: an 8bpp
  multisample texture fetch. Single-sample stencil fetches work with the same
  format after the swizzle fix, and multisample depth fetches work with the
  same tiling and sample count, so it is specifically 8 bits per pixel combined
  with multisampling. Worth checking whether the hardware supports that fetch at
  all before assuming the descriptor is merely mis-programmed, since
  `sampledImageStencilSampleCounts` is currently advertised without evidence.
- Multisample stencil sampling was also isolated by probes on CAICOS.
  `terakan_stencil_readback` clears and reads single-sample stencil back
  correctly, and `terakan_depth_msaa_fetch --combined` fetches multisample
  depth correctly out of the same `D32_SFLOAT_S8_UINT` format, but
  `terakan_stencil_msaa_fetch` reads zeroes for every sample of every texel.
  So this is specific to the stencil aspect of a multisample image, not to
  multisampling, not to the combined format, and not to stencil in general.
  It is also independent of the FMASK/CMASK item, since depth and stencil
  compression use HTILE. The descriptors are a lead: the stencil aspect of a
  combined image deliberately reuses the depth aspect's tiling, and its texture
  descriptor comes out with the same array mode, tile split and pitch as depth
  while carrying an 8-bit format instead of a 32-bit one. The stencil resolve
  shader and meta draw are implemented and wired, so only the fetch stands
  between here and advertising `supportedStencilResolveModes`.
- Sample-zero depth resolve: `VK_KHR_depth_stencil_resolve` and
  `VK_KHR_create_renderpass2` are exposed, advertising
  `VK_RESOLVE_MODE_SAMPLE_ZERO_BIT` for depth, no stencil mode, and
  `independentResolveNone`. The resolve samples the multisample source and
  exports depth from a meta pixel shader instead of decompressing through
  DB-to-CB, which returned zero. `terakan_depth_resolve` clears a 2x depth
  attachment, resolves it through a `vkCreateRenderPass2` subpass, and checks
  the readback; the destination is pre-filled with a different value first, so
  a resolve that never runs fails rather than passing on leftovers.
- Shader clip/cull distance: `shaderClipDistance`/`shaderCullDistance` and
  `maxClipDistances`/`maxCullDistances`/`maxCombinedClipAndCullDistances` (8,
  matching the combined PA_CL_VS_OUT_CNTL CCDIST0/CCDIST1 hardware export
  budget) are exposed. The `terakan_clip_distance` test draws a fullscreen
  triangle with `gl_ClipDistance[0]` set from clip-space X and verifies on
  CAICOS that the negative half is clipped to the clear color while the
  non-negative half is rasterized normally.
- Dynamic uniform/storage buffer descriptor bounds (`#MemoryIntegrity`): the
  `resource[1]` (hardware SIZE) field for `VK_DESCRIPTOR_TYPE_*_DYNAMIC`
  descriptors is reclamped against the real remaining `VkBuffer` extent when
  a dynamic offset is applied, not just the static `range`. The
  `terakan_dynamic_offset_bounds` test poisons a guard region past a small
  buffer's declared size and confirms on CAICOS that a deliberately invalid
  dynamic offset near the end of the buffer no longer exposes it (negative
  control: reverting the clamp makes the test observe the poison pattern).
- R8xx PALM basic compute parity with R9xx CAICOS: the same safe CTS list
  produces 48/48 executed passes on both generations.
- Workgroup barriers nested in dynamically uniform control flow: SFN keeps
  barriers in separate scheduling blocks, and `branch_past_barrier` plus the
  preceding shared-memory trigger pass on R8xx and R9xx.
- Repeated compute dispatch: the regression now contains the required
  shader-write dependency and its iterative oracle passes on RADV, R9xx and
  R8xx with intact guard regions.
- Ordinary compute dispatch, loop constants and non-barrier shader control flow.
- Singleton subgroup `BASIC` and `ARITHMETIC` lowering.
- Dynamic SSBO offsets, `firstInstance` and graphics/compute state restoration.
- Layered buffer/image copies.
- Mipmapped array and cubemap slice layout, including BC6H cube, single-level
  and 2D-array views.
- Color MSAA resolve for the currently tested sample counts, regions, layers
  and RGBA/BGRA formats.
- `VK_EXT_memory_budget` with per-process allocation accounting and
  kernel-informed VRAM/GTT budgets.
- SSBO runtime-array length, writable-SSBO returning reads, compute image
  hazards and graphics/compute cache transition coverage.
- Single-sample formatless storage-image reads and writes: transfer-initialized
  reads and independently generated writes pass exact 17x13 `R32_UINT`
  readback with intact guards. Both corresponding feature bits are exposed.

## Vertex pipeline stage survey (geometry and tessellation)

Surveyed 2026-08-23 while starting the geometry and tessellation items. Both
feature bits remain `VK_FALSE`, so no application can create a pipeline with
these stages yet and everything below is inert until the whole chain lands.

Already present before this survey: the pipeline layer routes all stages and
picks the hardware stage mapping; `VGT_SHADER_STAGES_EN` covers every
LS/HS/ES/GS/VS combination; thread and stack resource management and the
LSTMP/HSTMP/ESTMP/GSTMP scratch ring sizing are implemented; the shared r600
SFN backend already provides `TCSShader`, `TESShader` and `GeometryShader`
plus `r600_lower_tess_io` and `r600_append_tcs_TF_emission`.

Landed while surveying (regression-tested, inert until the feature bits flip):

- `SQ_PGM_START/RESOURCES/RESOURCES_2` binding for the LS, HS, ES and GS
  stages (`TERAKAN_HW_CONFIG_DRAW_ENTRY_SQ_PGM_{LS,HS,ES,GS}`), replacing the
  "Bind all the shaders" TODO. A `static_assert` pins the assumption that the
  three registers are consecutive for each of those stages.
- Shader keys for the vertex pipeline stages: `vs.as_ls`, `vs.as_es`,
  `tes.as_es` and `tcs.prim_mode`. The tessellator primitive mode is declared
  by the evaluation shader but needed when compiling the control shader, which
  is compiled first, so the evaluation shader is translated once up front just
  to read it.

- `VGT_LS_HS_CONFIG` and `VGT_TF_PARAM` as ordinary tracked draw config
  entries with setters, emitters and reset defaults. Nothing computes their
  values yet, so both stay at a defined zero; the hardware ignores them while
  the LS and HS stages are disabled.

Evergreen tessellation is on-chip through LDS, not an off-chip factor ring.
There is deliberately no `VGT_TF_MEMORY_BASE` anywhere in the tree: SFN takes
the factor base from a pinned `R0.w` that the hardware supplies, and
`store_tf_r600` lowers to the dedicated `WriteTFInstr` bytecode instruction.
So no new buffer object is needed, which makes this cheaper than a ring would
have been. `evergreen_setup_tess_constants` in `evergreen_state.c` is the
reference implementation of everything below.

Still missing, in rough dependency order:

1. The LDS patch layout: input/output vertex and patch sizes, the patch count
   per thread group, `output_patch0_offset` and `perpatch_output_offset`,
   derived from the control and evaluation shader NIR plus `patchControlPoints`
   from `VkPipelineTessellationStateCreateInfo`.
2. An LDS info constant buffer carrying that layout, bound to the vertex,
   control and evaluation stages. **This collides with Terakan's push constants
   and is the trap to plan for.** SFN's `emit_load_tcs_param_base` fetches two
   vec4s from `R600_LDS_INFO_CONST_BUFFER` at byte offsets 0 and 16, and that
   constant is 15 — the same kcache buffer Terakan already uses for push
   constants, whose first 48 bytes are
   `buffer_uav_base_granularity_offset[12]`. As it stands the tessellation
   parameter bases would read UAV granularity offsets. Either the driver
   constants have to move so bytes 0..31 can hold the eight-`uint32_t` layout
   (`input_patch_size`, `input_vertex_size`, `num_tcs_input_cp`,
   `num_tcs_output_cp`, `output_patch_size`, `output_vertex_size`,
   `output_patch0_offset`, `perpatch_output_offset`, in that order, matching
   `struct r600_lds_constant_buffer`), or `emit_load_tcs_param_base` has to be
   redirected the way `emit_get_lds_info_uint` already was for Terakan.
3. Dynamic `SQ_LDS_ALLOC` for the LS/HS pair, carrying the computed LDS size
   and the wave count, currently emitted as a constant zero on the draw path.
4. Values for `VGT_LS_HS_CONFIG` (patch control point counts, patch count per
   thread group) and `VGT_TF_PARAM` (partitioning, topology, spacing) derived
   from the same state, applied through `terakan_app_config_draw`.
5. Per-stage members of `terakan_shader_static.stage`, which only has `vs` and
   `ps`, plus the matching cases in the `terakan_shader_sfn.cpp` stage switch,
   which only handles `MESA_SHADER_VERTEX` and `MESA_SHADER_FRAGMENT`.
6. Geometry shaders additionally need the copy shader that the hardware VS runs
   in `VS_STAGE_COPY_SHADER` mode, plus the GSVS ring and `VGT_GS_MODE` /
   `VGT_GS_OUT_PRIM_TYPE`. `generate_gs_copy_shader` in `r600_shader.c` builds
   one directly with the bytecode builder, but it takes an `r600_context` and
   would have to be adapted.
7. Tessellation and geometry limits in `terakan_physical_device.c`, which are
   still the two remaining TODOs there.

Tessellation without a geometry shader is the cheaper of the two to finish
first: the evaluation shader runs as the hardware VS (`VS_STAGE_DS`), which is
already bound, so it needs no copy shader.

## Completion policy

Depth/stencil implementation note: Evergreen's `DB_RENDER_CONTROL` copy path
writes to a separately allocated color-compatible flushed-depth surface. The
discarded direct experiment bound the final depth-layout image as a CB target
and left all 256 D32 readback values unchanged. Do not restore that path; use a
transient flushed-depth surface followed by a shader depth/stencil export.

The first staged CAICOS experiment narrowed the remaining problem further. A
single-sample R32 companion was safe and the R32-to-D32 pixel-shader depth
export wrote the destination, but DB-to-CB produced zero in the companion. A
matching 2x companion caused the submission fence to time out and must not be
retried without FMASK/CMASK-safe multisample allocation and addressing. The
next implementation should therefore either complete that metadata first or
extract sample zero into a single-sample surface without binding a multisample
color target.

Reading depth back needs no decompression pass, which narrows the remaining
problem considerably. The `terakan_depth_readback` probe clears a single-sample
`D32_SFLOAT` image through a render pass, copies the depth aspect into a buffer
and gets every texel back exactly, so the ordinary transfer path already reads
depth through an SQ texture fetch. That is consistent with Terakan not
implementing HTILE: depth is stored uncompressed, so there is nothing for the
DB-to-CB copy to decompress.

Multisample depth fetches per sample as well. The `terakan_depth_msaa_fetch`
probe clears a 2x `D32_SFLOAT` image, reads every sample of every texel with
`texelFetch` on a `sampler2DMS` from a compute shader, and gets all 128 values
back exactly, with no hang and no FMASK-style metadata involved. It never binds
a multisample color target, which is what previously timed out the submission
fence, so this is the escape hatch recorded above rather than a retry of the
path that failed. Depth compression uses HTILE, which Terakan does not
implement, so this is independent of the FMASK/CMASK item.

Both halves of a resolve therefore exist, and neither needs the DB-to-CB step
that returned zero: sample the source depth per sample, and write the
destination with the pixel-shader depth export the earlier experiment already
confirmed. `DEPTH_COPY_ENABLE` should not be revived at all. That reduces the
remaining work to a meta draw that binds the source as a multisample texture
and the destination as DB, with a pixel shader applying the requested
`VkResolveModeFlagBits` across the samples and exporting `gl_FragDepth`, plus
the stencil equivalent, the render pass plumbing for
`VK_KHR_depth_stencil_resolve`, and the readback tests that gate exposing it.

FMASK/CMASK memory layout and deferred initialization are now implemented for
2x/4x/8x color images. Memory-requirement checks and bounded CAICOS submissions
for identity FMASK plus compressed-state CMASK initialization pass. This does
not complete the item yet: per-sample texture reads and real color-target
resolve coverage are still required before reusing an MSAA color companion for
depth resolve.

A TODO is complete only when:

1. the implementation builds from a clean configuration;
2. a focused readback test covers normal, boundary and negative behavior;
3. the test explicitly selects and reports the Terakan CAICOS ICD;
4. existing CPU and GPU regression tests still pass;
5. a relevant game test confirms rendering, without treating a surviving
   process or a single screenshot as sufficient proof.
