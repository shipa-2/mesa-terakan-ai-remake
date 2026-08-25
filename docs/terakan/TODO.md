# Terakan development TODO

Last updated: 2026-08-23. Primary target: stable game rendering on AMD
CAICOS with DXVK-Sarek.

Importance measures the expected effect on real games. Complexity includes
implementation, hardware research, and the CAICOS regression test needed to
accept the work. Both use a 1–5 scale.

## P0 — game-rendering blockers

| Work item | Importance | Complexity | Feasibility | Acceptance criteria |
|---|---:|---:|---|---|
| Implement and validate FMASK/CMASK allocation, identity initialization and sampled MSAA addressing | 5/5 | 5/5 | Implementable; requires Evergreen tiling research. A real per-sample-decode bug was found and fixed (see the note below): the shader's fixed 4-bit-per-sample FMASK nibble decode didn't match the 2x/4x identity-fill constants, only 8x's happened to already agree. Fixing that took per-sample colour reads from 100% wrong to roughly 25% wrong on CAICOS, but chasing the remainder surfaced a deeper, still-open issue -- run-to-run instability (including one fence-wait timeout) suggesting the identity FMASK fill doesn't reliably cover the whole tiled surface, most likely a bank-rotation/macro-tile addressing mismatch. Genuine further tiling research is needed here, not a quick fix | Per-sample reads and resolved reads pass for 2x/4x/8x images without corrupting ordinary color targets |
| Fix stencil-only render targets on combined depth/stencil images | 4/5 | 3/5 | Root cause still unknown. One genuine failure was observed (a multisampled stencil-only draw reading back a constant wrong byte) but did not reproduce across 75 repeat runs in a later pass, including 15 deliberately preceded by a forced driver crash to rule out leftover-state contamination from this investigation's own tooling -- see the note below for the full, honestly-corrected account and what's been ruled out (`DB_Z_INFO.NUM_SAMPLES` masking, confirmed identical between working and broken observations). Not reproducible under any controlled condition tried across two passes: clear, single-sample draw, and multisample draw, with and without a preceding crash. Every current stencil user works around it by binding a depth attachment alongside anyway, which is how real applications shape a depth/stencil resolve, but an application that legitimately renders stencil alone on a combined-format image could still be affected by whatever this is | A stencil-only `vkCmdBeginRendering` (`pDepthAttachment == NULL`, `pStencilAttachment` naming a combined-format image) writes and reads back correctly without a depth attachment bound alongside, at any sample count, repeatably |
| Complete cache and barrier coherency | 5/5 | 4/5 | Implementable. Composition coverage now exists (`terakan_frame_chain` with the render-pass producer, the compute producer, `--compute-ssbo` for a storage-buffer producer/consumer alongside the storage-image one, `--multi-size` for a second, independently sized 4x4 chain recorded within the same per-frame block as the main 16x16 one, and `--compute-multi-pipeline` for six distinct `VkPipeline` objects bound round-robin across frames instead of rebinding one) and all pass, closing every concrete gap the original coverage note named. Whatever remains is narrower than this composition shape, not a specific named scenario | Focused attachment, texture, storage, transfer, graphics/compute and query producer-consumer chains pass without application-specific waits |
| Cover remaining copy, blit and resolve format/subresource combinations | 5/5 | 4/5 | Implementable; the vkCmdCopyImage subresource slice is fixed, and a real bug in color resolve subresource addressing was found and fixed along the way. `terakan_copy_image_subresource` regression-covers a non-zero-offset partial-extent copy across array layers, a copy between two different mip levels, and a copy spanning multiple array layers in one region -- all pass on real CAICOS hardware (8/8 repeat runs, in light of the earlier retracted "deterministic" claim above). `terakan_color_resolve_subresource` closed a previously nonexistent COLOR attachment resolve test (only depth/stencil resolve had coverage) and found that resolving into a destination array layer other than the multisample source's own layer did not fail cleanly -- it silently wrote the resolved color into the SOURCE's layer of the destination instead, corrupting whatever was there. This is Evergreen's CB_RESOLVE apparently sharing one per-draw array-slice-select state across both bound color buffers rather than addressing each RTV's slice independently (no errata research beyond this observed behavior). The fix extends `terakan_meta_resolve_region_is_fixed_function_compatible` in `terakan_meta_resolve.c` to require a matching source/destination array layer, same as it already required matching extents and offsets, so a cross-layer region is now skipped like any other CB_RESOLVE-incompatible one instead of corrupting the wrong layer; there is no shader fallback for the cross-layer case (the alternate shader resolve path is disabled elsewhere in that file) so it remains genuinely unsupported, just safely so. Multisampled vkCmdCopyImage is a separate, still-open gap: `terakan_meta_copy_image.c` has no `samples > 1` guard before its single-sample-shaped meta-draw copy path (see the `TODO(Triang3l)` comment there), so an MSAA copy currently falls through to that path rather than being rejected or handled correctly. Actually implementing it is comparable in scope to the FMASK/CMASK work and was deliberately not attempted here, but `terakan_copy_image_multisample_noop_test` establishes what currently happens: on real CAICOS hardware (6/6 repeat runs) the destination is left completely untouched rather than corrupted, most likely because a #MemoryIntegrity-style check inside the meta-draw's descriptor creation rejects the mismatched dimensionality (source bound as plain 2D_ARRAY regardless of actual sample count) and the copy loop silently skips the region. This downgrades the gap from "possible silent corruption" to "silent no-op, locked in as a regression so it cannot regress into corruption unnoticed" -- still wrong from an application's perspective, but not the worse failure mode originally suspected. Blit subresource coverage beyond the format-matrix cases already landed, and the full CTS-driven matrix, remain unverified (CTS is not installed on the test machine, see FUNCTIONAL_COVERAGE.md) | Boundary tests cover non-zero offsets, partial extents, mip levels, array/3D layers and every advertised compatible format class |
| Correct meta blit format coverage | 5/5 | 3/5 | Implementable; the typed, mirrored and layered/3D-depth cases are fixed. `terakan_blit_format_matrix` now regression-covers RGBA<->BGRA (straight and mirrored on X) and R32 identity blits -- all pass on real CAICOS hardware -- closing what the acceptance criteria name explicitly, though the CTS binary is still not installed on the test machine (see FUNCTIONAL_COVERAGE.md), so the full per-format matrix beyond these specific cases remains unverified | All basic CTS blits pass for RGBA, BGRA, R32, reversed source/destination axes and 3D slices |

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

## TeraScale 1 (R600/R700) port

A separate, secondary target from the CAICOS-focused P0-P2 lists above: real
R700 (RV710) hardware is now available for testing alongside CAICOS. TeraScale
1 needs a genuinely separate code path throughout, not just different register
values plugged into the R8xx/R9xx ones -- it has no tessellator, a fixed
64-lane wavefront, and a differently-shaped `SQ_THREAD_RESOURCE_MGMT`/
`SQ_GPR_RESOURCE_MGMT` register set. See the `terascale_1` member of
`terakan_physical_device_chip_info` for what is already ported (physical
device enumeration and property reporting) and its reference source
(`r600_init_atom_start_cs()` in `src/gallium/drivers/r600/r600_state.c`, the
classic Gallium R600 driver that has supported this hardware for years).

| Work item | Importance | Complexity | Feasibility | Acceptance criteria |
|---|---:|---:|---|---|
| ~~Determine real per-family render backend counts~~ | 5/5 | 2/5 | Done: `max_render_backends_log2` is now populated from `RADEON_INFO_NUM_BACKENDS`, queried by the drm_radeon winsys for TeraScale 1 devices and required to succeed, matching the classic R600 driver's own unconditional query-and-fail-if-missing behavior. Confirmed on real RV710 hardware (ioctl reports 1 backend). R8xx/R9xx keep their existing static table, untouched | A real value backs `max_render_backends_log2` for every recognized TeraScale 1 chip family, sourced from kernel query or documented per-family reference, not guessed |
| Wire the TeraScale 1 register-emission helpers into command buffer recording, keep surveying and porting per-draw CB/DB state, and build command stream submission | 5/5 | 5/5 | The begin-command-buffer atom is now wired in: `terakan_hw_config_shared_indirect_buffer_begun()` branches on `chip_info->is_terascale_1` and calls the already-tested `terakan_hw_config_shared_terascale_1_write_sq_config()`/`_write_context_defaults()` instead of the R8xx/R9xx SQ_CONFIG block. DB_DEPTH_CONTROL/CB_TARGET_MASK turned out to need no separate TeraScale 1 code at all -- see the "no separate emission path needed" bullet below -- so the dedicated writer functions for them were retired as redundant. Two latent correctness bugs were found and fixed in the same pass: `terakan_hw_config_shared_draw_emit_sq_thread_stack_resource_mgmt()`, `_compute_emit_sq_thread_stack_resource_mgmt()`, and the LDS resource management block in `terakan_hw_config_shared_draw_emit_modified()` were all guarded only against `is_r9xx`, so TeraScale 1 (`is_r9xx` false) would have fallen through into writing R_008C18_SQ_THREAD_RESOURCE_MGMT_1 and R_008E2C_SQ_LDS_RESOURCE_MGMT once it ever reached command buffer recording -- neither register exists in `r600d.h` at all, so this would have hit whatever unrelated register (if any) actually lives at those offsets on real hardware. All three now also check `is_terascale_1` and skip; TeraScale 1 compute/LDS thread-stack configuration remains unresearched (no register decided) rather than guessed. Most of the rest of CB/DB per-draw state remains unsurveyed, and where it is surveyed it is not uniformly compatible -- CB_COLOR_CONTROL at 0x028808, for instance, has a 3-bit field at the same bit position (4) that means SPECIAL_OP on R600/R700 and MODE on Evergreen-and-later, an incompatible field with no shared meaning, so each register needs checking against both `r600d.h` and `evergreend.h` before assuming either compatibility or divergence. CB_COLOR*_BASE/INFO/SIZE (the actual render target binding) is blocked on tiling/surface addressing, the item below, since it needs surface pitch/slice tile counts that do not exist for TeraScale 1 yet. `terakan_CreateDevice`'s explicit refusal of TeraScale 1 physical devices is unchanged -- none of this is reachable yet | `vkCreateDevice` succeeds on a TeraScale 1 physical device and a trivial compute dispatch completes |
| TeraScale 1 tiling/surface addressing (bank/pipe swizzle, macro-tile layout) | 5/5 | 5/5 | Started: the pitch/height/base-alignment math (`terakan_image_tiling_terascale_1.c`) is ported and unit-tested against real RV710 tiling parameters, but not wired into `terakan_image.c`'s surface layout computation yet, and the much larger remaining pieces -- per-level offset/size computation, degrade-to-1D-on-small-mip, and the DB_DEPTH_INFO/CB_COLOR_INFO field computation this blocks -- are still fully open. This remains the highest-risk area, since a wrong tiling computation corrupts memory silently rather than failing loudly | A buffer/image round trip through the tiled surface layout matches, the same class of check `terakan_image.c` already does for R8xx/R9xx |
| Port the hand-written meta shader bytecode (blit/resolve/clear/copy/query, all of `src/amd/terascale/vulkan/meta/`) to the R6xx/R7xx CF/ALU/TEX instruction encoding | 4/5 | 5/5 | The NIR-to-bytecode compiler (SFN, `src/gallium/drivers/r600/sfn/`) already accepts `amd_gfx_level` including `R600`/`R700` distinct from `EVERGREEN`, since the classic Gallium R600 driver already uses it for this hardware -- shaders reaching the driver through NIR may need only the right `gfx_level` threaded through, but the meta shaders are hand-written Evergreen-only bytecode and need real per-generation variants | Each meta operation this driver depends on (at minimum blit and clear) passes its existing readback test on TeraScale 1 hardware |
| Recognize R600/R700 PCI IDs and correctly plumb `gfx_level` through the NIR shader path for real application shaders | 3/5 | 2/5 | The `is_chip_family_supported`/`chip_info_init` work above already recognizes the full R600..RV740 range; what remains is confirming no Evergreen-specific assumption leaks into `terakan_nir_*` lowering | A real application vertex/fragment shader compiles and renders correctly on TeraScale 1 |

## Completed and regression-covered

- TeraScale 1 (R600/R700) physical device enumeration and property reporting:
  chip family recognition now covers `CHIP_R600`..`CHIP_RV740`, not just
  `CHIP_CEDAR`..`CHIP_ARUBA`. `terakan_physical_device_chip_info` gained a
  `terascale_1` member with the real per-family GPR/thread/stack-entry counts
  (transcribed from `r600_init_atom_start_cs()` in
  `src/gallium/drivers/r600/r600_state.c`, not guessed), since TeraScale 1 has
  no tessellator, a fixed 64-lane wavefront, and a flat, single-variant
  register set where R8xx/R9xx has a tessellation-stage-indexed one --
  populating that struct needed a genuinely separate code path through
  `terakan_physical_device_chip_info_init`, not new switch cases plugged into
  the existing one. `terakan_CreateDevice` explicitly and cleanly refuses
  TeraScale 1 physical devices (`VK_ERROR_INITIALIZATION_FAILED`) rather than
  building and submitting a command stream from hardware state that was never
  actually computed for this generation, since that configuration and command
  building do not exist yet (see the section above). Verified on real RV710
  (R700) hardware installed alongside a Cedar (R8xx) card on the same machine;
  `terakan_terascale_1_enumeration` covers it and passes trivially (nothing to
  check) on machines with no TeraScale 1 hardware installed.

  Getting there exposed a device-selection bug in three other tests
  (`terakan_compute_loop`, `terakan_dynamic_offset_bounds`,
  `terakan_formatless_image_store`) and in `bin/terakan-test`'s own trailing
  `vulkaninfo` sanity check: all of them assumed exactly one Terakan-capable
  device would ever be enumerated, which a machine with both a working card
  and a TeraScale 1 one now violates. The three tests now loop through
  enumerated devices and pick one whose name does not start with "TeraScale
  1" rather than assuming there is only one candidate or that the first
  match is usable; `vulkaninfo`'s own device-selection flags (`--json=N`)
  turned out not to help, since it creates a device for every physical
  device it finds regardless of which one is asked to be reported on, so
  `bin/terakan-test` instead falls back to the GPU test suite's own log
  when `vulkaninfo`'s summary aborts on the unusable device before printing
  anything useful.

- TeraScale 1 (R600/R700) "begin command buffer" register atom: the whole of
  `r600_init_atom_start_cs()` (`src/gallium/drivers/r600/r600_state.c`), split
  across two functions. `terakan_hw_config_shared_terascale_1_write_sq_config()`
  writes `SQ_CONFIG`/`SQ_GPR_RESOURCE_MGMT_2`/`SQ_THREAD_RESOURCE_MGMT`/
  `SQ_STACK_RESOURCE_MGMT_1`/`SQ_STACK_RESOURCE_MGMT_2`, the block
  `chip_info->terascale_1`'s fields feed directly.
  `terakan_hw_config_shared_terascale_1_write_context_defaults()` writes
  everything else the reference function does: `VC_ENHANCE`, the R700-vs-R600
  branch (`VGT_ENHANCE`/`SQ_DYN_GPR_CNTL_PS_FLUSH_REQ`/`DB_DEBUG`/
  `DB_WATERMARKS`/`SPI_THREAD_GROUPING`), the `SQ_*_RING_ITEMSIZE`,
  `ALU_CONST_BUFFER_SIZE_*` and `VGT_OUTPUT_PATH_CNTL` zero-initialization
  blocks, a long run of individual VGT/PA/SPI/DB/CB/SQ context and ctl_const
  registers, and the three initial `SQ_LOOP_CONST` values -- R700 writes 3
  registers (`VGT_ENHANCE`, `PA_SC_EDGERULE`, `SX_MISC`) that R600 has no
  equivalent for at all, which the `is_r700` parameter selects. Both kept in
  the same translation unit, including only `r600d.h`/`r600d_common.h` and
  never `evergreend.h`. The two headers give the *same offset* a different
  meaning on this generation (`0x8C0C` is `SQ_THREAD_RESOURCE_MGMT` here,
  `SQ_GPR_RESOURCE_MGMT_3` -- an entirely different register -- on
  Evergreen-and-later), and while the handful of field macros this file
  actually uses happen to be bit-identical between the two headers where
  their names coincide, mixing both in one translation unit is not something
  to bet real hardware state on; the classic Gallium R600 driver keeps the
  same split between `r600_state.c` and `evergreen_state.c`.
  `terakan_hw_config_shared_terascale_1_test` checks the exact packet bytes
  against RV710's real reference values (`r600_state.c`) for the R700 path,
  dword for dword, plus the R600 branch's distinct values and total length,
  rather than the driver's own logic reproduced back at itself. Streaming
  output is out of scope (see TODO.md's existing R8xx/R9xx item for it), so
  the reference function's streamout-conditional stores are not written.
  Neither function is called from anywhere yet -- see the P0-equivalent item
  above for what wiring them in still needs.

- TeraScale 1 (R600/R700) `DB_DEPTH_CONTROL`/`CB_TARGET_MASK`: confirmed to
  need no separate emission path at all, not just no separate
  value-computation logic as first thought. `DB_DEPTH_CONTROL`'s fields
  (`STENCIL_ENABLE`, `Z_ENABLE`, `Z_WRITE_ENABLE`, `ZFUNC`,
  `BACKFACE_ENABLE`, the `STENCILFUNC`/`FAIL`/`ZPASS`/`ZFAIL` group and their
  `_BF` back-face counterparts) have identical bit positions and widths in
  both `r600d.h` and `evergreend.h`; `CB_TARGET_MASK` has no named fields in
  either header at all, just a plain 4-bits-per-render-target write mask. On
  top of that, `R_028800_DB_DEPTH_CONTROL` and `R_028238_CB_TARGET_MASK` are
  the same numeric register offset in both headers, and
  `R600_CONTEXT_REG_OFFSET`/`R600_CONFIG_REG_OFFSET` (`r600d_common.h`) are
  numerically identical to the `EVERGREEN_CONTEXT_REG_OFFSET`/
  `EVERGREEN_CONFIG_REG_OFFSET` constants Terakan's `TERAKAN_CONTEXT_REG_OFFSET`/
  `TERAKAN_CONFIG_REG_OFFSET` macros are built from. That means the existing
  R8xx/R9xx `terakan_hw_config_draw_emit_db_depth_control()`/
  `_emit_cb_target_mask()` in `terakan_hw_config_draw.c` -- unmodified --
  already emit byte-identical, correct packets for TeraScale 1. The
  dedicated `terakan_hw_config_draw_terascale_1_write_db_depth_control()`/
  `_write_cb_target_mask()` functions from the previous pass were retired as
  redundant once this was confirmed, along with their test and `meson.build`
  entries -- keeping a parallel implementation that duplicates code the
  driver already runs would be exactly the kind of unnecessary abstraction
  this project's conventions ask not to add. Not every neighboring register
  is like this: see the note on `CB_COLOR_CONTROL` in the P0-equivalent item
  above, checked and found to diverge in the course of finding these two, so
  this offset/base-constant equality is a fact to check per-register, not
  something to assume applies more broadly without checking.

- TeraScale 1 (R600/R700) `RADEON_INFO_TILING_CONFIG` decode:
  `terakan_physical_device_decode_tiling_config()`
  (`terakan_physical_device_tiling_config.c`) replaces the inline struct
  literal that used to sit in `terakan_physical_device_drm_radeon.c` and was
  only ever written for R8xx/R9xx. Checked directly against libdrm's
  `radeon/radeon_surface.c` (`r6_init_hw_info()` vs `eg_init_hw_info()`, fetched
  via `curl` since `gitlab.freedesktop.org`'s raw file is behind anti-bot
  protection that blocks `WebFetch`), because the ioctl's bit layout is
  generation-specific, not just its interpretation: on TeraScale 1 the pipe
  count field starts one bit later (bits [1:3] instead of [0:3]) and the pipe
  interleave ("group bytes") field lives at bits [6:7] instead of [8:11] -- a
  different position, not a narrower field at the same one. There is also no
  row size / `TILE_SPLIT` concept on this hardware at all (zero `TILE_SPLIT`
  occurrences in `r600d.h`, and `r6_surface_best()` in the reference is a
  literal no-op for r6xx/r7xx), so `row_bytes_log2` is set to 0 for TeraScale 1
  rather than a guessed nonzero value, so a caller that starts treating it as
  real before rendering is implemented fails loudly instead of silently using
  a fabricated tile split. `bank_interleave_log2` stays 0 for both
  generations, confirmed correct rather than assumed, since libdrm's
  `struct radeon_hw_info` has no such field for either. The existing
  R8xx/R9xx decode was cross-checked against the same reference in the same
  pass and confirmed already correct, so only its formulas moved, unchanged,
  into this new isolated function. Covered by
  `terakan_physical_device_tiling_config_test`, which checks every switch case
  (including the reference's undefined-input fallbacks) for both generations.
  This is a prerequisite for tiled surface addressing, not the tiling work
  itself -- the actual pitch/height alignment, macro-tile-aspect selection,
  and bank-swizzle math for TeraScale 1 in `terakan_image.c` has not been
  started; see the tiling/surface addressing row in the P0-equivalent table
  above.

- TeraScale 1 (R600/R700) render backend count:
  `max_render_backends_log2` is now populated for TeraScale 1 instead of
  staying at its deliberate zero placeholder. Unlike R8xx/R9xx, which use a
  per-family static table in `terakan_physical_device_chip_info_init`, there
  is no such table for TeraScale 1 in this driver -- and the classic Gallium
  R600 driver does not use one either: `radeon_drm_winsys.c` queries
  `RADEON_INFO_NUM_BACKENDS` from the kernel unconditionally for every R600+
  chip and treats a failed query as fatal. Terakan does the same, but only
  for TeraScale 1: the drm_radeon winsys
  (`terakan_physical_device_drm_radeon.c`) queries it only when the chip is
  TeraScale 1, and refuses the physical device
  (`VK_ERROR_INCOMPATIBLE_DRIVER`, the same error already used for the
  tiling config and GEM info queries) if the query fails or returns 0.
  R8xx/R9xx are entirely unaffected -- the ioctl is not issued for them, so
  they gain no new kernel-version dependency.
  `terakan_physical_device_chip_info_init` gained a `terascale_1_num_backends`
  parameter carrying this value through to `max_render_backends_log2`;
  passing 0 (the WDDM winsys does this today, since TeraScale 1 is not
  brought up there yet) leaves the old placeholder behavior unchanged. The
  raw count-to-log2 conversion is `terakan_physical_device_backend_count_to_log2()`
  (`terakan_physical_device_backend_count.c`), pulled into its own file and
  unit-tested the same way the tiling config decode was, since
  `chip_info_init` itself cannot be linked into a lightweight standalone test
  binary (it pulls in the same driver-wide Vulkan/NIR machinery as the rest
  of `terakan_physical_device.c` -- confirmed by trying that first and
  hitting undefined references at link time before extracting this
  function). It rounds up rather than down so a real backend count is never
  underrepresented, though every publicly documented R600/R700 backend count
  is a power of two anyway (1, 2, or 4), confirmed on real hardware: the RV710
  in the dual-GPU test machine reports exactly 1 backend via a standalone
  ioctl probe, matching its known single-render-backend spec.

- TeraScale 1 (R600/R700) begin-command-buffer atom wired in, and two latent
  bugs found and fixed while doing it:
  `terakan_hw_config_shared_indirect_buffer_begun()` now branches on
  `chip_info->is_terascale_1` and calls the already-tested
  `terakan_hw_config_shared_terascale_1_write_sq_config()`/
  `_write_context_defaults()` in place of the R8xx/R9xx SQ_CONFIG block, so
  the atom written and tested in an earlier pass is now actually reachable
  from command buffer recording (once `terakan_CreateDevice`'s refusal is
  eventually lifted, which it still is not). Auditing the surrounding code
  for other places gated on `is_r9xx` alone -- since `is_terascale_1` chips
  have `is_r9xx == false` too, the same as R8xx -- turned up two functions
  that would have miscompiled TeraScale 1 command buffers had they run
  before this: `terakan_hw_config_shared_draw_emit_sq_thread_stack_resource_mgmt()`
  and `_compute_emit_sq_thread_stack_resource_mgmt()` both wrote
  `R_008C18_SQ_THREAD_RESOURCE_MGMT_1` per draw/dispatch, and the LDS setup
  block in `terakan_hw_config_shared_draw_emit_modified()` wrote
  `R_008E2C_SQ_LDS_RESOURCE_MGMT` on every compute-to-draw switch -- neither
  register is defined at all in `r600d.h`, so on real TeraScale 1 hardware
  these would have written to whatever unrelated register (if any) actually
  lives at those offsets, not a differently-laid-out version of the same
  one. All three now also check `is_terascale_1` and skip. This is a real,
  currently-undocumented gap, not a no-op: TeraScale 1 has no tessellator
  and doesn't re-switch thread/stack allocation per draw the way R8xx does,
  so the draw-time skip is permanently correct, but TeraScale 1's
  compute/LDS thread and stack configuration has not been researched at all
  (`chip_info->terascale_1` has no LS-stage field the way it has
  `num_ps_gprs`/`num_vs_gprs`/etc. for the graphics stages), so a TeraScale 1
  compute dispatch would currently run with no compute thread/stack
  configuration whatsoever -- tracked here rather than guessed at.

- TeraScale 1 (R600/R700) CB/DB/PA/SPI/SQ register compatibility audit:
  every `R_XXXXXX_*` register `terakan_hw_config_draw.c` and
  `terakan_hw_config_shared.c` reference was checked mechanically against
  both `r600d.h` and `evergreend.h` -- offset presence, field-macro name
  sets, and field-macro bit-shift/mask formulas (comment and whitespace
  differences normalized out, so this isn't textual diffing) -- extending
  the DB_DEPTH_CONTROL/CB_TARGET_MASK/tiling-config-offset-base findings
  from earlier passes into a full survey rather than a couple of one-off
  checks. Findings, most important first:

  - **Same offset, entirely different register on R600/R700 -- never reuse
    R8xx/R9xx emission code for these without a real TeraScale 1 port**:
    most dangerously, the entire DB_* block Terakan currently emits at
    0x028000-0x02805C (`DB_RENDER_CONTROL`, `DB_COUNT_CONTROL`,
    `DB_RENDER_OVERRIDE`, `DB_RENDER_OVERRIDE2`, `DB_Z_INFO`,
    `DB_Z_READ_BASE`, `DB_STENCIL_READ_BASE`, `DB_DEPTH_SIZE`,
    `DB_DEPTH_SLICE`) sits on top of `CB_COLOR0_BASE`/`_2_BASE`/`_3_BASE`/
    `_6_BASE`/`_7_BASE` on R600/R700 -- render target base *addresses*, not
    depth/stencil control bits, so blindly reusing this code for TeraScale 1
    would write GPU addresses into whatever these DB fields decode to, or
    vice versa. Also colliding: the `SQ_PGM_START`/`SQ_PGM_RESOURCES(_2)`
    range for GS/ES/FS/HS/LS (0x028874-0x0288D8) is fully remapped --
    Evergreen's HS/LS tessellation-stage shader pointers occupy addresses
    that hold R600/R700's `SQ_ESGS_RING_ITEMSIZE`/`SQ_VSTMP_RING_ITEMSIZE`/
    `SQ_PSTMP_RING_ITEMSIZE`/`SQ_FBUF_RING_ITEMSIZE`/`SQ_PGM_CF_OFFSET_*`
    instead, and even the ES/FS shader-start pointers are shifted by one
    register slot relative to each other between the two generations.
    `CB_COLOR8_BASE`/`_9_BASE` (0x028E40/0x028E5C) collide with
    `PA_CL_UCP2_X`/`_UCP3_W` (user clip plane coefficients!) on R600/R700.
    `PA_SC_AA_MASK` (0x028C3C) collides with `CB_CLRCMP_MSK`, and the
    address `PA_SC_AA_SAMPLE_LOCS_7` uses on R8xx/R9xx is `CB_CLRCMP_DST`
    on R600/R700. `SPI_BARYC_CNTL`/`SPI_PS_IN_CONTROL_2` collide with the
    R600/R700-only `SPI_FOG_FUNC_SCALE`/`_BIAS`. `SQ_GPR_RESOURCE_MGMT_3`/
    `SQ_GLOBAL_GPR_RESOURCE_MGMT_1`/`_2` (0x008C0C/0x008C10/0x008C14) are
    `SQ_THREAD_RESOURCE_MGMT`/`SQ_STACK_RESOURCE_MGMT_1`/`_2` on R600/R700 --
    already correctly handled by the dedicated TeraScale 1 begin-atom
    writer, not by the shared R8xx code, so no action needed there, but
    listed here since it's the same class of finding, now independently
    reconfirmed by this mechanical pass rather than by the earlier
    by-hand read of `r600_init_atom_start_cs()`.

  - **Same offset and register, but divergent field layout -- needs real
    per-register TeraScale 1 value-computation logic, not just an offset
    fix**: `DB_EQAA` (0x028804) is `CB_BLEND_CONTROL` in field terms on
    R600/R700 (a completely different feature under a coincidentally
    DB-shaped name on Evergreen); `CB_COLOR_CONTROL` (0x028808, the
    already-known `SPECIAL_OP`-vs-`MODE` divergence at bit 4);
    `PA_SC_MODE_CNTL_1`/`_0` (0x028A4C/0x028A48) restructure most of their
    bits between generations; `DB_SHADER_CONTROL`, `DB_RENDER_OVERRIDE(2)`,
    `DB_RENDER_CONTROL`, `DB_COUNT_CONTROL` (already covered above as
    address collisions, but even where the DB *name* is right on both
    sides the field layout still differs completely, so this is a
    double failure mode, not just a naming coincidence to work around).

  - **Same offset, register, and field positions, but a narrower mask on
    R600/R700 -- safe to reuse as-is within the narrower range, but a
    real range limit, not just a compatibility footnote**: the screen/
    window/generic scissor rectangle registers (`PA_SC_SCREEN_SCISSOR_TL`/
    `_BR`, `PA_SC_WINDOW_SCISSOR_TL`/`_BR`, `PA_SC_GENERIC_SCISSOR_TL`/
    `_BR`) use the same bit positions on both generations, but R600/R700's
    `TL_X`/`TL_Y`/`BR_X`/`BR_Y` fields are 15 bits (screen scissor) or 14
    bits (window/generic scissor) wide versus 16/15 on R8xx/R9xx --
    values within the narrower range need no change, but this is a
    real ceiling to respect once TeraScale 1 draw-time scissor emission
    is actually ported, not merely a historical curiosity.

  - **Confirmed safe, zero code needed, offset and every field bit-for-bit
    identical** (extending the DB_DEPTH_CONTROL/CB_TARGET_MASK finding):
    `VGT_PRIMITIVE_TYPE`, `SQ_VTX_START_INST_LOC` (both already emitted
    unconditionally for every chip family with no `is_r9xx`/`is_terascale_1`
    branch at all -- now confirmed genuinely safe as-is, not merely
    unguarded by oversight), `SQ_GPR_RESOURCE_MGMT_1`/`_2`, `CB_SHADER_MASK`,
    `SX_MISC`, `SX_ALPHA_TEST_CONTROL`, `DB_STENCILREFMASK`,
    `PA_CL_VPORT_XSCALE_0` (and by extension its `_1`/`_2`/`_3` siblings,
    same field layout), `SPI_PS_INPUT_CNTL_0..31`, `SPI_VS_OUT_CONFIG`,
    `SPI_INTERP_CONTROL_0`, `SPI_INPUT_Z`, `PA_CL_CLIP_CNTL`,
    `PA_SU_SC_MODE_CNTL`, `PA_CL_VTE_CNTL`, `PA_CL_VS_OUT_CNTL`,
    `PA_CL_NANINF_CNTL`, `SQ_PGM_START_PS`, `PA_SU_POINT_SIZE`,
    `PA_SU_POINT_MINMAX`, `PA_SU_LINE_CNTL`, `PA_SC_LINE_STIPPLE`,
    `VGT_PRIMITIVEID_EN`, `VGT_MULTI_PRIM_IB_RESET_EN`, `IA_MULTI_VGT_PARAM`,
    and the Cayman/NI (`is_r9xx`) address forms of `PA_SC_LINE_CNTL`/
    `PA_SC_AA_CONFIG`/`PA_SU_VTX_CNTL`/`PA_CL_GB_VERT_CLIP_ADJ`
    (0x028C00-0x028C0C) -- these happen to coincide with the R600/R700
    native addresses for the same registers with the same fields, an
    accident of the address space rather than a designed compatibility,
    worth double-checking again if this list is ever acted on rather than
    assumed to stay true.

  - **Evergreen-only concepts, absent on TeraScale 1 entirely -- not a
    porting gap, a real hardware difference**: tessellation-related state
    (`VGT_SHADER_STAGES_EN`, `VGT_LS_HS_CONFIG`, `VGT_TF_PARAM`,
    `VGT_HOS_*`), streamout config (`VGT_STRMOUT_CONFIG`/
    `_BUFFER_CONFIG`), `DB_ALPHA_TO_MASK`, `DB_PRELOAD_CONTROL`,
    `DB_SRESULTS_COMPARE_STATE0`/`1`, `SQ_LDS_ALLOC`/`_PS`, `SPI_LDS_MGMT`,
    `SPI_CONFIG_CNTL`/`_1`, `PA_SU_HARDWARE_SCREEN_OFFSET`,
    `SQ_DYN_GPR_RESOURCE_LIMIT_1`, `PA_CL_ENHANCE`, and the R8xx/R9xx-only
    `SQ_STATIC_THREAD_MGMT1`/`2`/`3` (already unreachable for TeraScale 1
    today, since it's part of the R8xx-only branch in
    `terakan_hw_config_shared_indirect_buffer_begun()` -- see the
    begin-atom wiring above -- so no action needed, just confirming that
    branch is correctly scoped). `PA_SC_EDGERULE` is a partial case: the
    register nominally exists on both, but `r600d.h` defines no field
    macros for it at all, matching what the earlier begin-atom port
    already found and handled (it's one of exactly the three R700-only
    registers `terakan_hw_config_shared_terascale_1_write_context_defaults()`
    gates on `is_r700`).

  No code changes accompany this pass -- it is a survey to work from, not
  a set of fixes. Only DB_DEPTH_CONTROL, CB_TARGET_MASK, VGT_PRIMITIVE_TYPE,
  and SQ_VTX_START_INST_LOC are confirmed to need nothing further; every
  other register above needs a real, individually-checked-against-
  `r600_state.c` port (for the divergent-field and Evergreen-only cases)
  or at minimum an explicit `is_terascale_1` guard to prevent the same
  class of bug already found and fixed for `SQ_THREAD_RESOURCE_MGMT_1`/
  `SQ_LDS_RESOURCE_MGMT` (for the same-offset-different-register cases)
  before any of this code becomes reachable.

- TeraScale 1 (R600/R700) DB render-control/override and depth-view, and
  guards for the whole dangerous DB block found by the audit above:
  `terakan_hw_config_draw_terascale_1_write_db_render_control_override()`
  writes `R_028D0C_DB_RENDER_CONTROL`/`R_028D10_DB_RENDER_OVERRIDE`
  together (confirmed against `r600_state.c`'s
  `r600_emit_db_misc_state()`: R600/R700 has no `DB_RENDER_OVERRIDE2`
  register at all, so unlike R8xx/R9xx this is two registers in one
  packet, not three across two), and
  `terakan_hw_config_draw_terascale_1_write_db_depth_view()` writes
  `R_028004_DB_DEPTH_VIEW`, confirmed field-for-field identical
  (`SLICE_START`/`SLICE_MAX`, both 11 bits) to R8xx/R9xx's own
  `DB_DEPTH_VIEW` at a different offset (`R_028008`) -- so, like
  `DB_DEPTH_CONTROL`/`CB_TARGET_MASK` before it, needs no new
  value-computation logic, only the offset-isolating wrapper. Every
  current caller of the R8xx/R9xx equivalents only ever passes the
  all-zero default for render-control/override (this driver has no
  dynamic per-draw logic for either register on either generation yet),
  so the TeraScale 1 port needs nothing beyond that same baseline.
  `terakan_hw_config_draw_emit_db_render_control()` now emits both
  registers together for TeraScale 1 (one PKT3, so the two
  independently-dirty-tracked R8xx/R9xx entries don't each try to emit
  half a pair); `_emit_db_render_override()` becomes a no-op for
  TeraScale 1 since the other entry already covered it.

  Also guarded, not ported -- these are the exact registers the audit
  flagged as colliding with unrelated R600/R700 registers at the same
  offset, and were confirmed to have no `is_r9xx`/`is_terascale_1` guard
  at all before this pass, the same latent-bug class already found once
  for `SQ_THREAD_RESOURCE_MGMT_1`/`SQ_LDS_RESOURCE_MGMT`:
  `terakan_hw_config_draw_emit_db_render_override2()` (no R600/R700
  equivalent exists at all -- confirmed against `r600_state.c`, not
  assumed from the header alone -- and its offset is `DB_DEPTH_INFO`
  there), `_emit_db_count_control()` (its offset is `DB_DEPTH_VIEW` on
  R600/R700, and no `DB_COUNT_CONTROL`-equivalent occlusion-query-hazard
  register was found in `r600_state.c` to replace it with -- not yet
  researched, not guessed), and `_emit_db_depth_stencil_buffer()` --
  the single most dangerous one, since its whole
  `R_028040_DB_Z_INFO..R_02805C_DB_DEPTH_SLICE` range is
  `R_028040_CB_COLOR0_BASE..R_02805C_CB_COLOR7_BASE` (render target
  *addresses*) on R600/R700. This last one is not merely unguarded but
  genuinely not portable yet regardless: R600/R700 binds depth and
  stencil through one combined surface and base address
  (`R_02800C_DB_DEPTH_BASE`, no separate stencil base at all, unlike
  R8xx/R9xx's four independent read/write Z/stencil base registers), and
  `DB_DEPTH_INFO`'s `ARRAY_MODE` field needs real tiling information this
  driver doesn't have for TeraScale 1 yet (`terakan_image.c`'s
  surface/macro-tile address math -- see the tiling/surface addressing row
  in the P0-equivalent table above and the bullet below for what exists
  of it so far).

- TeraScale 1 (R600/R700) surface pitch/height/base-alignment math:
  `terakan_image_tiling_terascale_1_alignments_linear_aligned()`/
  `_1d_tiled_thin1()`/`_2d_tiled_thin1()` (`terakan_image_tiling_terascale_1.c`),
  ported from libdrm's `radeon/radeon_surface.c` (`r6_surface_init_linear_aligned()`/
  `_1d()`/`_2d()`, the same reference already used for the
  `RADEON_INFO_TILING_CONFIG` decode and the begin-command-buffer atom) and
  unit-tested against the real RV710 tiling parameters
  (`terakan_physical_device_tiling_config_test` already established:
  `group_bytes=512`, `num_pipes=2`, `num_banks=8`). This is deliberately a
  much smaller port than R8xx/R9xx's AddrLib-derived tiling code in
  `terakan_image.c`: TeraScale 1 has no `TILE_SPLIT`, no per-surface
  `bank_width`/`bank_height`/`macro_tile_aspect` selection, and no
  macro-tile-aspect-ratio search at all -- confirmed by the tiling-config
  decode work (no `TILE_SPLIT` field exists in `r600d.h` for any CB/DB
  register) and by `r6_surface_best()` in the reference being a literal
  no-op ("no value to optimize for r6xx/r7xx") -- so the whole algorithm
  is a handful of fixed formulas over group bytes/pipes/banks/bytes-per-
  element/sample count, not an optimization search.

  A follow-up pass added per-level layout on top of that alignment math:
  `terakan_image_tiling_terascale_1_mip_extent()` (`mip_minify()` in the
  reference -- plain `u_minify()` for the base level, rounded up to the
  next power of two for every level above it, since higher mips need
  power-of-two dimensions for the tiled addressing scheme; confirmed this
  isn't R8xx/R9xx-specific by finding the same "pow2Pad" rounding already
  in `terakan_image_surface_aspect_compute()`, not assumed to carry over)
  and `terakan_image_tiling_terascale_1_level_layout()` (`surf_minify()`
  in the reference -- aligns already-minified pixel dimensions up to a
  tiling mode's pitch/height alignment granularity and computes
  pitch/slice byte sizes, including the degrade-to-1D-on-small-mip check
  `r6_surface_init_2d()` uses before calling back into
  `r6_surface_init_1d()` for a level that's too small to stay 2D-tiled).
  Both are pure functions taking already-computed inputs (minified pixel
  dimensions, alignment granularity from the `_alignments_*()` functions),
  so walking a full mip chain to cumulative byte offsets -- what
  `surf_minify()`'s own `offset`/`surf->bo_size` accumulation does across
  levels in the reference -- is still the caller's job and not yet
  written.

  A second follow-up pass added the base-level array-mode decision:
  `terakan_image_tiling_terascale_1_select_array_mode()` transcribes
  `terakan_image_surface_tiling_compute()`'s policy (prefer 2D-tiled
  unless linear tiling was requested, a debug override is set, the
  format requires linear, or it's a non-multisampled non-DB 1D image
  whose format doesn't require tiling) rather than re-deriving it, since
  that policy is a driver-level Vulkan design choice, not something
  hardware-specific to re-research -- only the array-mode constants
  (`V_0280A0_ARRAY_*` vs `V_028C70_ARRAY_*`, confirmed numerically
  identical) and the tiled-only format table (`terascale_format.h`
  already has `TERASCALE_FORMATS_TILED_ONLY_R6XX` sitting next to the
  R8xx one, unused until now) differ. Takes plain booleans rather than
  `VkImageType`/`VkImageTiling`/`terascale_format_index` so the file
  stays decoupled from Vulkan and format-table headers the way the rest
  of it already is. Also confirmed against `r6_surface_init()` in the
  reference that R600/R700 zbuffers (depth/stencil images) can never be
  plain linear -- only 1D-tiled or 2D-tiled -- and that MSAA surfaces
  must always be 2D-tiled; both are already true of the existing
  R8xx/R9xx policy this was transcribed from, so no additional branching
  was needed, just confirmed rather than assumed to still hold here.

  A third follow-up pass added the mip-chain-to-offsets walk itself:
  `terakan_image_tiling_terascale_1_mip_chain_layout()` calls
  `mip_extent()`/`level_layout()` for every level and accumulates
  offsets, mirroring the reference's own `offset`/`surf->bo_size` running
  state across `r6_surface_init_1d()`/`_2d()`'s per-level loop --
  including `r6_surface_init_2d()` calling back into
  `r6_surface_init_1d()` for the level that degrades and every level
  after it, ported here as a `degraded_to_1d` flag that latches once set
  and is never cleared, matching the reference exactly (a 2D-tiled chain
  never reverts to 2D for a smaller, later mip once it degrades).
  Array-layer/3D-depth-plane multiplication is folded in via a
  `depth_minifies_per_level` flag distinguishing a 3D image's `npix_z`
  (mip-minified like width/height) from a 2D image's array layer count
  (constant across the chain) -- the same distinction
  `terakan_image_surface_aspect_compute()` already makes for R8xx/R9xx,
  so this is Vulkan-level surface shape rather than something to
  re-derive from the reference, unlike the tiling math itself.
  Unit-tested with five hand-derived cases (a chain that never degrades,
  one that degrades at the base level and stays degraded, a
  fixed-1D-from-the-start chain with no degrade check at all, array
  layers, and 3D depth planes), reusing the same RV710 alignment
  parameters as every prior tiling test in this pass.

  A fourth follow-up pass wired all of the above into
  `terakan_image.c`'s real `terakan_image_surface_compute()`, behind an
  `is_terascale_1` branch that returns early before any R8xx/R9xx-shaped
  code runs (so this cannot regress R8xx/R9xx -- verified by full local
  and real-hardware test suite runs after the change, both green).
  `terakan_image_surface_compute_terascale_1()`
  (`terakan_image_surface_terascale_1.c`) assembles the pieces above into
  a complete `terakan_image_surface` the same shape
  `terakan_image_surface_compute()` itself produces, including the
  combined-depth-stencil array-mode sharing R8xx/R9xx already does.
  **This integration is UNTESTED beyond code review and does not have a
  unit test**, unlike every other piece of the TeraScale 1 port so far:
  `terakan_CreateDevice` still refuses TeraScale 1 physical devices, so
  there is no way to actually call `vkCreateImage` on this generation and
  observe real behavior, and the function is not unit-testable in
  isolation the way the pure tiling-math functions it calls are (it needs
  real `VkImageCreateInfo`/`terakan_format_info`/`terakan_physical_device`
  inputs). Treat it as a draft to re-verify once TeraScale 1 device
  creation exists, not as verified working code -- its own header comment
  says the same. Two real mistakes were caught by review during this
  pass specifically because of that lack of a safety net, worth recording
  so a future re-check knows to look here first: an earlier draft
  pre-multiplied the base width by `surfels_per_block` before calling
  `mip_chain_layout()`, which rounds the wrong quantity (surfel count
  instead of texel count) to a power of two for mip levels above 0 --
  fixed by excluding 3x-expand formats (8_8_8, 16_16_16, 32_32_32)
  entirely for now rather than getting the multiplication order right
  blind; and a copy-paste error passed `bytes_per_element * surfels_per_block`
  (i.e. `bytes_per_block` again) to three tiling calls instead of the
  intended per-surfel byte size, caught the same way. Also does not
  handle 4x4-compressed formats (BC1-7) or 8x1/2x1 subsampled formats at
  all yet (returns `false` rather than guessing at the
  `block_texels_log2`-based block-width/height division
  `terakan_image_surface_tiling_compute()` has for R8xx/R9xx) -- see the
  header comment on `terakan_image_surface_compute_terascale_1()` for the
  full list of what is and isn't handled.

  Still not done, and each a substantial piece of its own: the
  `DB_DEPTH_INFO`/`CB_COLOR_INFO` register field computation this
  unblocks, which still needs its own per-field compatibility check
  against `r600d.h` (not yet done -- these registers weren't in the
  CB/DB/PA/SPI/SQ audit above, since that audit only covered registers
  the R8xx/R9xx code currently references, and this tiling work is what
  would make TeraScale 1's own field set relevant for the first time;
  spot-checked while writing an earlier pass, though, and R600/R700's
  `CB_COLOR0_INFO`/`DB_DEPTH_INFO` field sets are consistent with the
  simpler algorithm above -- no `BANK_WIDTH`/`BANK_HEIGHT`/`NUM_BANKS`/
  `MACRO_TILE_ASPECT` fields on either register, matching the tiling math
  needing none of those inputs); block-compressed and subsampled format
  support; and 3x-expand format support (both just described above).

  A fifth follow-up pass ported the emission (not value-computation) side
  of the actual depth surface binding registers:
  `terakan_hw_config_draw_terascale_1_write_db_depth_size()`
  (`R_028000_DB_DEPTH_SIZE`, `PITCH_TILE_MAX`/`SLICE_TILE_MAX` -- no
  R8xx/R9xx equivalent at this offset or shape, since R600/R700 packs the
  whole slice tile count into this one register instead of splitting
  pitch/height across `DB_DEPTH_SIZE` and a separate `DB_DEPTH_SLICE`
  register the way R8xx/R9xx does; R600/R700 has no `DB_DEPTH_SLICE` at
  all) and
  `terakan_hw_config_draw_terascale_1_write_db_depth_base_info()`
  (`R_02800C_DB_DEPTH_BASE`/`R_028010_DB_DEPTH_INFO` together, matching
  `r600_state.c`'s own `radeon_set_context_reg_seq(cs,
  R_02800C_DB_DEPTH_BASE, 2)` sequencing). Both take fully
  caller-computed values, same as every other TeraScale 1 register
  emission function -- the VALUE COMPUTATION side (deriving a real
  `DB_DEPTH_INFO` `FORMAT`/`ARRAY_MODE` and `DB_DEPTH_BASE` address from
  a Vulkan depth/stencil attachment) is not written, and is a genuinely
  open research question, not just unstarted work: R600/R700 binds depth
  and stencil through one combined surface and base address (no separate
  stencil base at all), which doesn't map onto Terakan's existing
  `terakan_depth_stencil_descriptor` (built around separate
  `z_base`/`stencil_base`/`z_info`/`stencil_info`) without first figuring
  out how the packed combined-surface model actually stores stencil data
  alongside depth on this hardware. These two functions exist so the
  register shape is ready once that research is done, not because the
  value-computation problem is solved -- it remains the largest concrete
  gap left before a TeraScale 1 depth attachment could actually be bound.

- TeraScale 1 (R600/R700) `CB_COLORn_INFO`/`DB_Z_INFO` field-position
  comparison, following up on the spot-check above with the real
  per-field breakdown: none of R8xx/R9xx's `CB_COLOR0_BASE..CB_COLOR0_DIM`
  register offsets (0x028C60-0x028C78) exist at all in `r600d.h` --
  confirmed with the same mechanical offset/field comparison the
  CB/DB/PA/SPI/SQ audit used, not just read by eye -- consistent with
  R600/R700's entirely different, already-known CB_COLOR0 block location
  (`R_028040_CB_COLOR0_BASE`, `R_028060_CB_COLOR0_SIZE`,
  `R_0280A0_CB_COLOR0_INFO`). Comparing `R_0280A0_CB_COLOR0_INFO`'s
  fields against `R_028C70_CB_COLOR0_INFO`'s directly (different offsets,
  so the mechanical same-offset audit can't do this part, only checked by
  reading both) found a real partial overlap, not simply "compatible" or
  "incompatible": `ENDIAN`, `FORMAT`, `ARRAY_MODE`, and `NUMBER_TYPE` sit
  at the identical bit positions on both (bits 0-1, 2-7, 8-11, 12-14
  respectively), but every field after that diverges -- R600/R700 has an
  extra `READ_SIZE` bit at 15 that R8xx/R9xx's `COMP_SWAP` starts at
  instead, and the two field sets past that point are different sizes
  entirely (R600/R700 has `TILE_MODE`/`CLEAR_COLOR`/`BLEND_FLOAT32`, none
  of which exist on R8xx/R9xx; R8xx/R9xx has `FAST_CLEAR`/`COMPRESSION`/
  `RAT`/`RESOURCE_TYPE`, none of which exist on R600/R700), so nothing
  past `NUMBER_TYPE` can be assumed compatible even though the register
  is conceptually the same feature on both. `DB_Z_INFO` was already fully
  covered by the standout finding in the CB/DB/PA/SPI/SQ audit above (its
  R8xx/R9xx offset is R600/R700's `CB_COLOR0_BASE`), so this is recorded
  here as the `CB_COLOR0_INFO` counterpart to that finding, not a new
  discovery about `DB_Z_INFO` itself.

- Reducing stencil resolve, `VK_RESOLVE_MODE_MIN_BIT` and
  `VK_RESOLVE_MODE_MAX_BIT`: the same shaders and dispatch as the depth reducing
  modes, differing only in the reducing opcode (an unsigned integer comparison)
  and the export slot. `terakan_stencil_resolve_modes` covers it at 2x, 4x and
  8x the same way the depth test does: a different stencil value per sample,
  written through `STENCIL_REPLACE` with a static per-pipeline reference and a
  sample mask confining each draw to one sample.

  Getting there surfaced a real hardware quirk, worth recording so it is not
  rediscovered: **a stencil-only render target on a combined depth/stencil
  image writes and reads back the wrong values.** Writing a uniform stencil
  value with no depth attachment bound (`pDepthAttachment == NULL`,
  `pStencilAttachment` naming a `D32_SFLOAT_S8_UINT` image) reads back a
  per-column pattern with no relationship to what was written; a roundtrip of
  the same pattern through `vkCmdCopyBufferToImage`/`vkCmdCopyImageToBuffer`
  with no rendering involved is exact, which places the defect in the DB write
  path specifically, not the tiled surface or the copy engine. Binding a depth
  attachment of the same image alongside — even one neither read nor written —
  makes it disappear, which is what `terakan_stencil_resolve_modes` does and
  what `VK_KHR_depth_stencil_resolve` shapes a real resolve into anyway, since
  VUID-06085 requires the same view when both attachments are non-null. The
  driver's own masking of `DB_Z_INFO` when no depth attachment is bound already
  preserves the tiling fields the code identifies as shared with stencil
  (`ARRAY_MODE`, the bank fields), so the cause is not that masking; it is left
  open above as its own item rather than guessed at further.

  A follow-up pass found and fixed a real, related inconsistency in the same
  function, `terakan_image_create_depth_stencil_descriptor()`
  (`terakan_image.c`): `DB_Z_INFO`'s `TILE_SPLIT` field was set from
  `surface_depth->tiling.attrib_tile_split` only inside the
  `view_depth_format != TERASCALE_R8XX_DEPTH_FORMAT_INVALID` branch, unlike
  `ARRAY_MODE`/`BANK_WIDTH`/`BANK_HEIGHT`/`MACRO_TILE_ASPECT` just above it,
  which already correctly come from `surface_main_aspect` (falls back to the
  stencil surface when depth is invalid) -- so a stencil-only descriptor
  always got `TILE_SPLIT = 0` regardless of the stencil surface's real tile
  split, the same class of bug as the `ARRAY_MODE`/bank-field case already
  ruled out above, just missed by that earlier pass. Fixed to use
  `surface_main_aspect` like its neighboring fields do.

  **This fix was empirically confirmed to NOT reproduce or resolve the
  documented symptom above**, so it does not close this item, and is
  recorded here specifically so the next attempt doesn't re-derive and
  re-test the same hypothesis: a stencil-only clear-and-readback test
  (`vkCmdBeginRenderPass`/`vkCmdEndRenderPass` with a `LOAD_OP_CLEAR`
  stencil-only view of a `D32_SFLOAT_S8_UINT` image, common-runtime-translated
  into the same `terakan_CmdBeginRendering()` path a native dynamic-rendering
  call would take -- confirmed by reading `terakan_vk_render_pass.c`, not
  assumed) passed identically with and without the `TILE_SPLIT` fix at every
  size tried (8x8, 256x256, 2048x2048 on real CAICOS hardware), meaning
  `attrib_tile_split` stayed `0` for this aspect/format/size combination
  regardless -- either 2D macro-tiling is never selected for a stencil-only
  aspect of a combined depth/stencil image at these sizes on this hardware
  (in which case the field genuinely doesn't matter here, though the fix is
  still correct to keep for whatever case does exercise it), or the true
  root cause is unrelated to `TILE_SPLIT` entirely. The fix is kept because
  it is independently correct regardless of this investigation's outcome
  (the same reasoning that already justified the `ARRAY_MODE`/bank-field
  fallback), but the documented per-column corruption itself remains
  unreproduced by a clear -- the next attempt should try an actual draw
  through the rasterizer's stencil test/`STENCIL_REPLACE` path (matching
  what `terakan_stencil_resolve_modes` exercises when it works around the
  bug by binding a depth attachment alongside), since a bare `LOAD_OP_CLEAR`
  may simply not exercise whatever code path the original symptom came from.

  **A follow-up pass retracts the "deterministic" claim above -- it did
  not hold up under proper repeated testing, and the corrected story is
  important enough to leave in place rather than silently rewrite.** The
  finding above (`terakan_stencil_resolve_modes_test.cpp` with
  `pDepthAttachment` forced to `nullptr` at 2x/4x/8x samples, reading
  back a constant `0x5A` on every texel) was real -- it happened, was
  observed directly, and every value quoted above was actually printed by
  that run -- but was only ever run *once* per sample count before being
  written up as "deterministic and identical every run." A later pass
  that actually repeated it found the opposite: 75 additional runs of the
  identical scenario (50 back-to-back, then 15 more each deliberately
  preceded by a forced driver crash to test whether the first failure was
  caused by corrupted state left over from an earlier, unrelated crash in
  this investigation's own test harness -- an initial repro attempt had
  crashed with `SIGSEGV` from calling `vkCmdBeginRenderingKHR` without
  enabling `VK_KHR_dynamic_rendering`, confirmed by exit code 139) **all
  passed, 75/75, with zero reproductions.** Register-level instrumentation
  added during the same pass (temporary, not committed) showed
  `DB_Z_INFO`/`DB_STENCIL_INFO`/base-address values that looked correct
  and consistent between passing and failing states alike, and
  `DB_Z_INFO.NUM_SAMPLES`'s masking (the lead the previous version of
  this note pointed at) is identical in both the working and broken
  cases, so it is very unlikely to be the cause -- ruled out, not just
  deprioritized. Whatever produced the one observed failure remains
  unexplained: possibly a genuine, rare race condition this pass simply
  didn't get unlucky enough to hit again, possibly some transient
  condition specific to that one session unrelated to Terakan's own
  logic. The honest state of this bug going into the next pass is:
  **not reproduced under any controlled condition tried across two full
  investigation passes** (clear, single-sample draw, multisample draw,
  with and without a preceding crash), despite one genuine but
  unrepeated observation. Anyone picking this up next should not treat
  the specific repro recipe above as reliable, but also should not
  assume the underlying bug doesn't exist -- both a "real, rare bug" and
  "environmental fluke unrelated to Terakan" remain live possibilities,
  and this note exists so the next attempt spends its effort finding
  out which, rather than re-discovering that the "deterministic" repro
  doesn't actually reproduce on the first try and wondering whether it
  did something wrong.

- Reducing depth resolve, `VK_RESOLVE_MODE_MIN_BIT` and `VK_RESOLVE_MODE_MAX_BIT`:
  every sample is fetched and combined in the pixel shader, one program per
  sample count, and `independentResolve` is now true because each aspect
  resolves from its own draw and its own shader. `terakan_resolve_modes` gives
  each sample of the attachment a different depth, by confining every draw to
  one sample with the pipeline's sample mask, then resolves all three modes out
  of that one attachment; the depths are picked so sample zero, the minimum and
  the maximum are three different values at 4x and 8x.

  Three things were learned the hard way and are worth not rediscovering. An
  ALU clause's count is in eight-byte slots, so each pair of literal dwords
  counts as a slot of its own alongside the instruction reading it -- counting
  only instructions truncated the clause and silently dropped the last samples.
  Push constants do not reach this shader: whatever it reads back is neither the
  uploaded values nor zeroes, which is why the sample indices are literals and
  the sample count selects the program instead of being clamped on the CPU.
  Grouping ALU copies of different channels aliases one source channel onto
  another, the same effect the image blit shader documents, so each copy is its
  own group. Fetching a sample that does not exist returns zero, which a maximum
  would survive but a minimum would not.

- Depth/stencil rendering into a mip level other than zero: the depth/stencil
  descriptor took its base address from the aspect while taking its pitch and
  slice from the requested level, so anything rendering, clearing or resolving
  into a level above zero wrote level zero's memory at another level's
  dimensions. `vkCmdClearDepthStencilImage` goes through the same descriptor, so
  a cleared level above zero was never actually cleared and read back as
  uninitialized memory. The base now comes from the level, whose offset already
  includes the aspect's. `terakan_depth_resolve_subresource` resolves into level
  one, layer one of a two-level, two-layer destination through a partial render
  area, which also covers the array layer and the render area offset reaching
  the resolve; against the previous code every texel it reads is uninitialized.

- Dynamic rendering: `VK_KHR_dynamic_rendering` is advertised. It is the
  driver's native rendering path rather than a new one — the common render pass
  implementation lowers `VkRenderPass` onto `terakan_CmdBeginRendering`, suspend
  and resume flags included — so exposing it was a matter of clearing its
  `VK_KHR_depth_stencil_resolve` dependency. `terakan_dynamic_rendering` covers
  the surface that render passes never reach: attachments described per
  recording, a pipeline built from `VkPipelineRenderingCreateInfo` with no
  render pass at all, and one render split across a suspending and a resuming
  half.

- Multi-stage push constant ranges: a `VkPushConstantRange` covering more than
  one stage used to lose every other stage, because the loop distributing the
  range's extent cleared the scanned bit twice — `u_bit_scan` already clears it,
  and an extra `&= x - 1` cleared the next one too. The canonical
  `VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT` range therefore
  reached the vertex shader and left the fragment shader reading zeros. Found
  while bringing up `terakan_dynamic_rendering`, which covers it: its fragment
  shader takes its colour from a push constant shared with the vertex stage.

- 3D image mip base alignment and descriptor addressing: the separately
  addressed 3D mip chain is aligned and each origin mip level's own array mode
  is used in its resource descriptor. The clear that previously returned
  `VK_ERROR_DEVICE_LOST` through the radeon CS validator, and the full selected
  color-clear matrix, now pass without device loss.
- Layered and 3D blit slice selection: a blit covering more than one slice
  wrote the wrong ones. Two defects combined. The destination slice count was
  forced to equal the source slice count, so a 3D region whose two depth ranges
  differ in size produced too few or too many slices, and the depth axis was
  reduced with `MIN2`, which discards the sign a reversed range carries, so a
  mirrored depth range was not mirrored. Separately, the r8xx pixel shader
  samples the source at a constant array layer, while the loop drew several
  destination slices per draw and advanced the source descriptor only between
  draws, so every layer of a multi-layer array blit received the source's first
  layer. The destination slice now selects its source slice through the
  region's signed, scaled depth range, and one destination slice is drawn per
  draw with the source descriptor re-based for it. `terakan_blit_3d` covers
  minified, magnified and mirrored depth ranges plus a four-layer array blit;
  against the previous code all four groups fail.

- Typed and mirrored 2D blits: `CopyImage` is no longer used for
  format-converting or mirrored operations, and the sign of mirrored coordinate
  transforms is preserved.
- Stencil-aspect view swizzle: a view of the stencil aspect alone is a
  single-component image whose value the Vulkan specification requires in R,
  but the driver was applying the combined depth/stencil format's swizzle,
  which places stencil in G, so sampling it returned a constant zero. View
  descriptors now expose the stencil channel as R. The aspect format tables are
  left alone because transfers use them for source and destination alike, where
  any consistent placement works, which is why transfers never showed this.
  `terakan_stencil_fetch` covers it.
- Multisample depth and stencil texture fetch: `MIP_ADDRESS` doubles as the
  FMASK pointer for multisample textures, and depth and stencil have no FMASK,
  but the driver was leaving it aliasing the base address instead of zeroing
  it, so the hardware treated the surface data itself as FMASK. r600 zeroes it
  explicitly for multisample depth textures. Comparing against r600 running
  OpenGL on the same CAICOS settled it after probes had ruled out addressing,
  tiling, the barrier and the write side: r600 fetches multisample stencil
  correctly there, which proved the hardware supports the fetch and the fault
  was Terakan's. `terakan_stencil_msaa_fetch` covers it.
- Sample-zero depth and stencil resolve: `VK_KHR_depth_stencil_resolve` and
  `VK_KHR_create_renderpass2` are exposed, advertising
  `VK_RESOLVE_MODE_SAMPLE_ZERO_BIT` for both aspects and
  `independentResolveNone`. The resolve samples the multisample source and
  exports from a meta pixel shader, one draw per aspect, instead of
  decompressing through DB-to-CB, which returned zero. `terakan_depth_resolve`
  and `terakan_depth_stencil_resolve` clear a 2x attachment, resolve it through
  a `vkCreateRenderPass2` subpass, and check the readback; the destination is
  pre-filled with a different value first, so a resolve that never runs fails
  rather than passing on leftovers.
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

## Cache and barrier coherency: what is covered so far

The single-hazard tests all pass while applications still show frame-to-frame
corruption, so `terakan_frame_chain` tests the composition instead: twenty-four
frames recorded into one command buffer, each producing a colour unique to that
frame, transitioning it to be sampled, sampling it into a second attachment,
transitioning that for transfer and copying it into its own slice of a readback
buffer, with only the barriers a correct application would issue. A frame that
observes a neighbour's colour has seen through a barrier, and the failure names
the frame whose colour it actually saw.

All variants pass on CAICOS: a render pass clear, a compute shader writing a
storage image, (`--compute-ssbo`) a compute shader writing both a storage
image and a storage buffer, with the sampling pass reading the image into RGB
and the buffer into alpha so a stale read of either channel is independently
visible, (`--multi-size`) a second, independently sized 4x4 chain recorded
within the same per-frame block as the main 16x16 one, with its own colour
range disjoint from the main chain's so a texel leaking between the two sizes
is unambiguous, and (`--compute-multi-pipeline`) six distinct `VkPipeline`
objects sharing one shader module and layout, bound round-robin across
frames instead of rebinding the same pipeline object every time. The image
write is chosen because the application that strobes issues thousands of
dispatches per second and the driver's compute RAT coherency is the least
covered path; the buffer producer closed the "storage buffers alongside
storage images" gap this note used to list, `--multi-size` closed "several
render target sizes in the same frame", and `--compute-multi-pipeline`
closed "many distinct compute pipelines rather than one" -- every concrete
gap this note originally named. So whatever the application hits is narrower
than this composition shape.

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

Per-sample colour reads were probed directly for the first time (no test in
this suite had ever exercised `texelFetch` on a multisample colour image --
`terakan_color_msaa_fetch_test`, mirroring `terakan_depth_msaa_fetch`) and
found completely broken: every texel read back wrong, mostly `(0,0,0,0)`.
Root cause found and fixed: `LowerTexToBackend::lower_txf_ms()`
(`sfn/sfn_instr_tex.cpp`) decodes the fetched FMASK dword with a fixed 4-bit
(one nibble) field per sample index (`shift = ms_index * 4, mask = 0xF`)
regardless of the actual sample count, but the identity-fill constants in
`terakan_barrier.c` (`identity_fmask[]`) did not follow that convention for
2x and 4x -- only the 8x constant (`0x76543210`, decoding to nibbles
`0..7`) happened to already match it. Decoded under the shader's own
convention, the old 2x/4x constants gave nibble sequences `(2,0,2,0,...)`
and `(4,14,4,14,...)`, both reading out-of-range or wrong planes for most
sample indices, exactly matching the observed failure (sample index 0 always
wrong on 2x, decoding to plane 2, which does not exist on a 2-plane
surface). Corrected to `0x10101010` (2x) and `0x32103210` (4x), following
the same nibble-per-sample identity as the already-correct 8x value.

This is a real, verified improvement, not a full fix: with the corrected
constants, mismatches on CAICOS dropped from 100% to roughly 25% of texels
across 2x/4x/8x (previously fully broken output became mostly-correct with a
residual pattern -- position-dependent for 2x/4x, always sample indices 6-7
for 8x), and the full existing regression suite stays green with the fix
applied (no other test exercises this shader path, so nothing regressed).
Chasing the remaining ~25% further surfaced something more serious than a
constant-value bug: repeated runs of the same probe gave different
mismatch counts (32, then stably 92, then a fence-wait timeout on a 4x run,
then 8x alternating between 0 and 448 mismatches across otherwise-identical
runs), which points to the identity FMASK fill not reliably covering the
whole tiled surface -- most likely a bank-rotation or macro-tile addressing
mismatch between the linear CP_DMA fill and the FMASK surface's real tiled
byte layout, leaving some tile-scrambled locations uninitialized and
dependent on leftover VRAM content from a prior allocation. That is a
genuine Evergreen tiling-research question (matching this item's own
complexity rating), not something to guess further at blindly, especially
given the observed fence timeout: probing was stopped once it produced a
hang risk rather than pushed further this session, and the driver's overall
health was reconfirmed (full regression suite green) immediately after.
`terakan_color_msaa_fetch_test` is committed as source (with a
`--samples=N` selector for 2x/4x/8x) for whoever continues this, but is
deliberately not wired into `bin/terakan-test`'s required-green suite since
it does not reliably pass yet.

A TODO is complete only when:

1. the implementation builds from a clean configuration;
2. a focused readback test covers normal, boundary and negative behavior;
3. the test explicitly selects and reports the Terakan CAICOS ICD;
4. existing CPU and GPU regression tests still pass;
5. a relevant game test confirms rendering, without treating a surviving
   process or a single screenshot as sufficient proof.
