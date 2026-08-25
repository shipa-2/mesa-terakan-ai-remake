# Terakan functional coverage

Last updated: 2026-08-23. Hardware under test: AMD CAICOS (`1002:6779`, R9xx)
and PALM/Wrestler (`1002:9807`, R8xx).
Reference device: AMD Radeon RX 6800 XT using RADV.

This document records the focused tests used while investigating Godot and
DXVK rendering faults. It is a development snapshot, not a Vulkan 1.1
conformance claim. Terakan reports conformance version `0.0.0.0` and dangerous
tests are excluded until their resource-limit handling is fixed.

## Current implementation changes

The current development cycle adds or changes the following behavior:

- SSBO runtime-array length queries are lowered to an indexed Evergreen buffer
  size query instead of leaving `get_ssbo_size` for the Gallium path.
- Reads from an SSBO that is writable in the same shader use a returning UAV
  operation, avoiding stale vertex-fetch data after RAT writes.
- compute image reads participate in UAV tracking;
- compute completion events are emitted before CB/UAV cache flushes, including
  queue semaphore and fence signaling;
- legacy `ALL_TRANSFER` barriers consume the same deferred copy hazards as the
  synchronization2 `COPY` stage;
- graphics/compute IB transitions replay destination read-cache invalidations;
- context-register packet mode follows the active hardware mode rather than
  merely the last bound application pipeline;
- `NumWorkgroups` and the attempted `BaseWorkgroup` implementation use Terakan
  driver constants instead of Gallium's private info buffer;
- runtime compute dumps include exact Vulkan descriptor and push-constant
  layouts, and the shader-corpus test accepts these `.spv` dumps;
- optional graphics SPIR-V, selected NIR and bytecode dumps are available for
  application-level diagnosis;
- the dynamic SSBO regression now covers large offsets, sparse screen
  positions, indexed and non-indexed triangle draws, a depth prepass, depth
  reuse, intervening depth targets and transfer-to-uniform synchronization;
- the image-copy regression includes `R32_UINT` and full-size HDR/R32 paths.
- compute wave allocation uses the SQ quad-pipe count with the two-pipe
  Evergreen minimum rather than deriving it solely from render backends;
- a reset command writer explicitly starts in graphics mode, preventing stale
  recycled state from skipping the first graphics-to-compute transition;
- SFN places a workgroup barrier in separate scheduling blocks. LDS operations
  are still pseudo-instructions while dependency chains are created, so this
  prevents the scheduler from moving the barrier before a preceding LDS write
  or after a following LDS read, including inside dynamically uniform control
  flow.

## Focused results

### Expanded safe CTS matrix

An expanded CAICOS run on Vulkan CTS 1.4.6.1 completed 1071 unattended-safe
cases outside the original basic-compute list. Follow-up fixes and reruns also
covered the selected clear and basic copy/blit matrices:

| Batch | Pass | Fail | NotSupported | Warning |
|---|---:|---:|---:|---:|
| info, version, smoke, command buffers and descriptors | 90 | 3 | 78 | 0 |
| object lifetime, multithreading, buffer fill/update and pipeline lifetime | 433 | 0 | 269 | 3 |
| basic image copy and blit, after typed/mirrored blit fixes | 104 | 56 | 35 | 0 |
| selected clear operations, after 3D descriptor fix | 281 | 180 | 444 | 0 |

The API/property run exposed four Vulkan 1.1 reporting gaps. Fixed:

- interpolation offset range and precision are now reported correctly;
- `VK_KHR_image_format_list`, required by the advertised
  `VK_KHR_swapchain_mutable_format`, is now exposed.

Still open:

- `multiview` remains unexposed even though CTS requires it for the reported
  Vulkan 1.1 API version.

All 46 basic `image_to_image` cases, including partial, NPOT, linear, general,
depth and stencil variants, pass. The typed and mirrored 2D blit failures were
caused by using `CopyImage` for format-converting or mirrored operations and by
discarding the sign of mirrored coordinate transforms. After fixing those
paths, 58 of 149 blit cases pass, 56 fail and 35 are unsupported. The remaining
failures were concentrated in 3D blits.

The CTS binary is no longer installed on the test machine, so the 3D blit work
was driven from the driver code and a readback test instead. `terakan_blit_3d`
blits a 3D image whose every slice holds a distinct colour and reports which
source slice each destination slice received, for a minified, a magnified and a
mirrored depth range, then blits a four-layer 2D array. Against the code before
the fix all four groups fail: the minified and mirrored ranges hand every
destination slice the source's first slice, the magnified range leaves six of
its eight destination slices unwritten, and three of the four array layers
receive layer zero. The CTS blit batch has not been rerun since, so the numbers
above still describe the previous state.

With the CTS binary still unavailable, `terakan_blit_format_matrix` covers the
specific format-matrix gap the acceptance criteria name explicitly:
`VK_FORMAT_R8G8B8A8_UNORM` to `VK_FORMAT_B8G8R8A8_UNORM` (a format-converting
blit between formats with a different channel order in memory), the same pair
mirrored on X (combining the format conversion with a reversed
source/destination axis), and an identity `VK_FORMAT_R32_UINT` blit (a single
32-bit-channel format, a different pixel shader export/texture fetch shape
from the 8-bit-per-channel packed formats every other blit test here uses).
All three pass on real CAICOS hardware. This is real, previously-missing
regression coverage, not a bug found and fixed -- the full per-format matrix
CTS would exercise remains unverified without the CTS binary.

`terakan_copy_image_subresource` covers the vkCmdCopyImage subresource
gaps the "remaining copy, blit and resolve" acceptance criteria name
explicitly: a partial-extent copy at non-zero source and destination
offsets landing in a different array layer, a copy between two different
mip levels of the same image, and a copy region spanning multiple array
layers at once. The source image is filled per level/layer with a value
encoding its own mip, layer and (x, y) position and the destination
starts filled with a sentinel, so a readback distinguishes a
misplaced/mis-sized copy from a merely wrong value. All four cases pass
on real CAICOS hardware, repeated eight times with zero failures given
the earlier retracted "deterministic" MSAA claim above making single-run
results untrustworthy on their own. Multisampled vkCmdCopyImage is not
covered: `terakan_meta_copy_image.c` has no sample-count guard before its
single-sample-shaped meta-draw copy path, a separate, still-open gap
comparable in scope to the FMASK/CMASK work.

`terakan_color_resolve_subresource` is the first test in this suite to
exercise a COLOR attachment multisample resolve at all -- prior coverage
was depth/stencil only. It found a real bug: resolving into a
destination array layer other than the multisample source's own array
layer did not fail to write the requested layer, it silently wrote the
resolved color into the SOURCE's layer of the destination instead,
corrupting whatever was there. This looks like Evergreen's CB_RESOLVE
sharing one per-draw array-slice-select state across both bound color
buffers rather than addressing each RTV's slice independently. The fix,
in `terakan_meta_resolve_region_is_fixed_function_compatible`
(`terakan_meta_resolve.c`), requires a matching source/destination array
layer the same way it already required matching extents and offsets, so
a cross-layer resolve region is now skipped -- like any other
CB_RESOLVE-incompatible region -- instead of corrupting the wrong layer.
Three cases pass on real CAICOS hardware, repeated six times with zero
failures: a non-zero mip level with an offset render area, a non-zero
array layer shared identically by source and destination (the only
array-layer combination CB_RESOLVE supports), and a regression check
that a mismatched-layer resolve now leaves the sentinel intact in both
the requested layer and the source's own layer of the destination.
Cross-layer color resolve itself remains genuinely unsupported -- there
is no shader fallback available for it -- just safely so rather than
silently corrupting data.

The initial selected clear batch passed its first 64 1D and 2D color cases, then
`dEQP-VK.api.image_clearing.core.clear_color_image.3d.optimal.single_layer.r8g8b8a8_unorm`
returned `VK_ERROR_DEVICE_LOST`. The kernel rejected the command stream with:

```text
evergreen_cs_track_validate_texture: mipmap bo base 4793344 not aligned with 4096
[drm:radeon_cs_ioctl [radeon]] *ERROR* Invalid command stream !
```

The fix aligns the separately addressed 3D mip chain and uses each origin mip
level's actual array mode in its resource descriptor. The formerly fatal case
and the full selected color-clear matrix now pass without device loss. The 180
remaining clear failures are depth/stencil subresource and partial-attachment
semantics, not command-stream rejection.

### Local Terakan suite

The wrapper currently reports all four CPU tests passing:

```text
terakan_sfn_lowering
terakan_hw_config_loop_constants
terakan_descriptor_buffer
terakan_vertex_input
```

All nineteen CAICOS GPU tests pass:

```text
terakan_image_array_copy
terakan_instance_dynamic_ssbo
terakan_bc6_cube
terakan_bc6_cube_single_level_views
terakan_bc6_array_view
terakan_compute_loop
terakan_formatless_image_store
terakan_dynamic_offset_bounds
terakan_clip_distance
terakan_depth_readback
terakan_depth_msaa_fetch
terakan_depth_resolve
terakan_depth_stencil_resolve
terakan_stencil_readback
terakan_stencil_fetch
terakan_stencil_msaa_fetch
terakan_frame_chain
terakan_frame_chain_compute
terakan_frame_chain_depth
terakan_frame_chain_ssbo
terakan_frame_chain_multi_size
terakan_frame_chain_multi_pipeline
terakan_dynamic_rendering
terakan_blit_3d
terakan_blit_format_matrix
terakan_copy_image_subresource
terakan_color_resolve_subresource
terakan_depth_resolve_subresource
terakan_resolve_modes_2x
terakan_resolve_modes_4x
terakan_resolve_modes_8x
terakan_stencil_resolve_modes_2x
terakan_stencil_resolve_modes_4x
terakan_stencil_resolve_modes_8x
terakan_physical_device_properties
terakan_terascale_1_enumeration
```

`terakan_compute_loop` now inserts a compute shader-write to shader-read/write
dependency between its 12 dispatches. Its iterative CPU oracle passes on the
RX 6800 XT with RADV and on both CAICOS and PALM with Terakan; guard regions
also remain intact. The earlier failure was caused by the test omitting the
Vulkan dependency while assuming every dispatch observed the preceding write.

### Vulkan CTS basic compute

A safe list of 77 `dEQP-VK.compute.pipeline.basic` cases produced:

| Result | Count | Meaning |
|---|---:|---|
| Pass | 48 | Correct readback or CTS result |
| Fail | 0 | No failures in the safe list |
| NotSupported | 29 | Feature or queue not advertised |

All 48 tests that actually execute pass. SSBO reads and
writes, runtime arrays, atomics, shared variables, ordinary workgroup and
command barriers, image load/store, compute/image transitions and
`indirect_after_base_dispatch` pass.

The same current development build was copied without recompilation to an
R8xx PALM/Wrestler system and produced the identical `48 Pass / 0 Fail / 29
NotSupported` result. This includes SSBO and UBO access, LDS/shared variables,
shared atomics, command and workgroup barriers, storage images, large
image/SSBO copies and indirect dispatch. The older system-installed Terakan
build on PALM failed these paths, so results from that package are not
representative of the current tree.

The previous failures `write_ssbo_array` and `webgl_spirv_loop` now pass.

Do not run `max_local_size_x`, `max_local_size_y` or `max_local_size_z` on the
current driver. They previously caused a GPU lockup or device loss and are not
part of the percentage above.

### Synchronization, WSI and render-pass probes

All 13 selected single-queue producer/consumer hazards pass:

- clear image to copy, compute and fragment consumers;
- compute image writes to copy, compute and fragment consumers;
- draw writes to copy, compute and fragment consumers;
- fragment storage-image writes to copy, compute and fragment consumers;
- clear attachments to fragment sampling.

Both Wayland swapchain rendering cases pass. Five CTS custom-resolve cases are
`NotSupported` because `customResolve` is not advertised; this is not counted
as a failed implementation. `VK_KHR_depth_stencil_resolve` is now advertised
with `VK_RESOLVE_MODE_SAMPLE_ZERO_BIT` for depth and stencil, so those CTS
cases are worth re-running.

### Confirmed failures

The previous state-dependent `branch_past_barrier` failure was traced to SFN
scheduling rather than cross-dispatch hardware state. Before LDS pseudo-ops
were split into ALU instructions, the dependency builder could not connect a
barrier to the preceding write; the scheduler consequently moved
`GROUP_BARRIER` to the start of the trigger shader. Separate scheduling blocks
now preserve `LDS_WRITE -> GROUP_BARRIER -> LDS_READ`. The formerly failing
two-test trigger and the complete safe list pass on both R8xx and R9xx.

`dEQP-VK.compute.pipeline.device_group.dispatch_base` still fails. Terascale's
`VGT_COMPUTE_START` does not directly provide Vulkan `DispatchBase` semantics,
so a driver-constant base is now lowered into the shader. The constants are
correct on the CPU side, but repeated segmented dispatches still behave as if
the GPU observes a zero or stale base. The simpler
`indirect_after_base_dispatch` regression passes because the indirect dispatch
is explicitly reset to base zero.

`dEQP-VK.compute.pipeline.device_group.device_index` also fails, indicating
that `gl_DeviceIndex` still needs an explicit Terakan lowering. Both failures
are low priority for ordinary single-GPU games, but are required for complete
Vulkan 1.1 device-group behavior.

The same three tests pass on the RX 6800 XT with RADV, confirming that the test
inputs and expected values are valid.

## Unsupported capability groups

The 29 basic-compute `NotSupported` results consist primarily of 21
`shaderReplicatedComposites` cases from newer Vulkan functionality. The rest
require one of the following:

- a second or compute-only queue;
- `VK_EXT_robustness2`;
- shader FP64, FP16 or 16-bit storage operations;
- a newer SPIR-V version.

Terakan also intentionally leaves geometry and tessellation shaders,
vertex-stage stores and atomics, extended storage-image formats, multisample
storage images, image gather extensions, clip/cull distances,
Int16/Int64/Float64, sparse
resources and depth bounds disabled. Vulkan 1.1 allows these feature bits to be
false, but they do not have equal game impact:

| Capability group | Game importance | Estimated complexity |
|---|---:|---:|
| Storage-image/UAV formats, formatless access and multisample access | 4/5 | 4/5 |
| Shader clip/cull distances and extended image gather | 4/5 | 3/5 |
| Geometry shaders | 4/5 | 5/5 |
| Vertex-pipeline stores and atomics | 4/5 | 4/5 |
| Tessellation shaders | 3/5 | 5/5 |
| FP16/Int16 storage and arithmetic | 2/5 | 4/5 |
| FP64/Int64, sparse resources and depth bounds | 1/5 | 4-5/5 |

Single-sample formatless storage-image reads and writes are enabled separately.
The focused CAICOS test uses both corresponding SPIR-V capabilities, compares
transfer-initialized reads and independently generated writes against exact
17x13 `R32_UINT` oracles, and verifies trailing guards.

The 21 replicated-composite cases, maintenance5, custom resolve and specialized
queue topology are not Vulkan 1.1 or ordinary D3D11 game blockers. They should
not displace the shader, UAV, MSAA and control-flow work above.

## Application evidence

User-observed testing after the compute, descriptor and synchronization work:

- Hangover Gallery renders correctly;
- Fused 240 renders correctly, with earlier menu-lighting faults resolved;
- Buckshot Roulette no longer has a stable correct result and can show a
  corrupted frame or strobe between incorrect backgrounds.

Because focused image hazards and WSI pass, the remaining Buckshot fault is
more likely to be in uncovered graphics shader/control-flow, blending,
render-pass dependency, descriptor update or format behavior than in the
already tested basic image barriers.

Session findings (2026-08-23, CAICOS): the strobing main-menu background is
not cross-process VRAM reuse — content stayed a consistent magenta/black/white
block pattern regardless of what ran on the GPU immediately before (tested
against both vkcube and a fresh Fused 240 run, neither of which resembles the
corruption). The large (988x1156 and similar non-power-of-two) decoration
images upload through `terakan_CmdCopyBufferToImage2` as ~300 separate 64x64
RTV draws per image; forcing an unconditional `PARTIAL_FLUSH_CP_THROUGH_PS` +
`FLUSH_INV_CB_RTV_DATA` + `INV_TC` barrier after every chunk draw removed the
block-grid corruption pattern from a single captured frame, but the live
strobe was unchanged, meaning the fault recurs every frame rather than once at
upload time. This points away from the one-time buffer-to-image upload path
and toward a per-frame hazard (most likely a compute-driven screen/background
effect racing its CB or RAT cache visibility with a later sampled read),
consistent with the still-open cache/barrier coherency item in
[TODO.md](TODO.md). Not yet root-caused; needs a frame-by-frame trace
(`TERAKAN_DEBUG_RENDER`/`TERAKAN_DEBUG_RAT`/`TERAKAN_DEBUG_QUEUE_IBS`)
correlated against a screen recording rather than single-shot screenshots,
which cannot reliably catch the bad frame.

Since then `terakan_frame_chain` was added to test that shape directly:
twenty-four frames of produce, sample, render and copy in one command buffer.
It now runs with six variants — a render pass, a compute dispatch,
(`--depth`) a depth-only render pass whose result is sampled the way a shadow
map is, (`--compute-ssbo`) a compute dispatch that writes both a storage
image and a storage buffer, with the sampling pass reading the image into RGB
and the buffer into alpha so a stale read of either is independently visible,
(`--multi-size`) a second, independently sized 4x4 chain recorded right
after the main 16x16 one within the same per-frame block, with its own
colour range disjoint from the main chain's so a leak between the two sizes
is unambiguous, and (`--compute-multi-pipeline`) six distinct `VkPipeline`
objects sharing one shader module bound round-robin across frames instead of
rebinding the same pipeline every time. All six pass, closing the "mix
storage buffers with storage images", "several render target sizes in the
same frame" and "many distinct compute pipelines rather than one" gaps this
note used to name -- every concrete gap it originally listed. What real
applications do beyond this composition shape remains open, but nothing
specific is left unaddressed here.

Buckshot Roulette was retested after the stencil-aspect view swizzle and the
multisample `MIP_ADDRESS` fixes landed, and the symptom changed: it now renders
real graphics, where before the image was a grid of magenta and black blocks.
What remains is a flicker between a black background and white with red dots.
The block corruption is therefore explained by the sampling fixes; the flicker
is not, and is still consistent with the open cache/barrier coherency item.

A `TERAKAN_DEBUG_RENDER` trace of that run shows what its frames are made of,
which is useful for aiming further work: per frame roughly seventeen depth-only
passes (4096x4096, 512x512, 256x256 and 128x128 shadow maps plus a 960x540
depth prepass), a three-attachment 960x540 pass, several single-attachment
960x540 passes, and one 1920x1080 pass. Depth-only passes dominate by a wide
margin, which is what prompted the `--depth` producer above.

Only a person watching the screen can judge the flicker: single screenshots
repeatedly failed to catch a bad frame.

## Reproduction

Use the development ICD explicitly and disable implicit layers:

```bash
export VK_DRIVER_FILES="$PWD/build-vulkan/src/amd/terascale/vulkan/terascale_devenv_icd.x86_64.json"
export VK_ICD_FILENAMES="$VK_DRIVER_FILES"
export VK_LOADER_LAYERS_DISABLE='~implicit~'
export MESA_SHADER_CACHE_DISABLE=true
```

Run CTS from its Vulkan module build directory so Amber resources resolve:

```bash
cd ../VK-GL-CTS-1.4.6.1/build-vulkan/external/vulkancts/modules/vulkan
./deqp-vk --deqp-case='dEQP-VK.compute.pipeline.basic.branch_past_barrier'
```

Running the CTS binary from the Mesa source directory produces a
`ResourceError` for Amber files and is not a valid driver result.
