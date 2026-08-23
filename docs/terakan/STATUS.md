# Terakan status

Last updated: 2026-08-23. Primary target: AMD CAICOS (R9xx). Compute coverage
is also verified on PALM/Wrestler (R8xx).

## Verified in the current development cycle

| Area | Result |
|---|---|
| Vulkan instance/device discovery | AMD CAICOS (R9xx) and PALM/Wrestler (R8xx) Terakan devices, API 1.1.318 |
| Properties and driver identity | `VK_KHR_driver_properties`; Terakan/Mesa identity; non-conformant version reported honestly |
| VRAM reporting | `VK_EXT_memory_budget`; per-process heap usage plus kernel-reported global VRAM/GTT pressure |
| Focused regression suite | 4/4 CPU and 19/19 CAICOS GPU tests pass; all 48 executed safe basic-compute CTS cases pass identically on CAICOS and PALM; 1071 additional unattended-safe CTS cases plus 65 isolated clear cases completed on CAICOS |
| Compute and subgroup behavior | SSBO, atomics, shared memory, conditional workgroup barriers, image access and indirect dispatch pass on R8xx and R9xx |
| Draw state | `firstInstance` plus dynamic SSBO offsets pass; graphics/compute transitions preserve descriptors and draw state |
| Events and sparse property queries | implemented and CAICOS-tested |
| Image copy and layout | layered buffer/image copies pass; mipmapped array storage uses hardware-compatible power-of-two slice padding |
| BC6H cube/array sampling | 6 faces x 8 mip levels pass through cube, single-level and 2D-array views |
| Color MSAA resolve | 2x, 4x, and 8x; full/partial, layers, RGBA/BGRA passed |
| Depth/stencil resolve | `VK_KHR_depth_stencil_resolve` with `VK_RESOLVE_MODE_SAMPLE_ZERO_BIT` for both aspects, resolved through a `vkCreateRenderPass2` subpass and checked by readback |
| Depth and stencil sampling | single-sample and multisample per-sample fetches of both aspects, including the stencil aspect of a combined format |
| Shader clip/cull distance | `shaderClipDistance` and `shaderCullDistance` with all three combined limits at 8 |
| Dynamic descriptor bounds | the hardware size field of `VK_DESCRIPTOR_TYPE_*_DYNAMIC` descriptors is reclamped for the dynamic offset, with a guard-region test and a negative control |
| Barrier composition | twenty-four-frame produce, sample, render and copy chains in one command buffer, with a render pass, a compute and a depth-only producer |
| Dynamic rendering | `VK_KHR_dynamic_rendering`, the driver's native rendering path, including pipelines built without a render pass and suspend/resume splits |
| Layered and 3D blits | destination slices select their source slice through the region's signed, scaled depth range; minified, magnified, mirrored and four-layer array blits are checked by readback |
| vkQuake3 | Vulkan renderer works in a 640x480 window |
| DXVK-Sarek | D3D11 FL 11_1; Katamari and tested Disco Elysium scenes render; Green Hell creates a 1920x1080 swapchain with Sarek |
| Godot workloads | Hangover Gallery and Fused 240 render correctly in user testing; Buckshot Roulette now renders real graphics but flickers |

The BC6H regression test validates 48 independently addressed samples and has
a negative control that corrupts one expected value. The normal run reports
zero mismatches; the corrupted oracle must fail. Its few values that differ
from Mesa's software decoder were cross-checked against the upstream r600
OpenGL driver on the same CAICOS hardware.

Two defects found while implementing depth/stencil resolve are worth recording
because both silently returned zeroes rather than failing. A view of the
stencil aspect alone was given the combined format's swizzle, which places
stencil in the second component, so sampling it read a constant zero; and
`MIP_ADDRESS`, which doubles as the FMASK pointer for multisample textures, was
left aliasing the base address instead of being zeroed, so the hardware treated
depth and stencil data as FMASK. The second was settled by comparing against
the r600 OpenGL driver on the same CAICOS, which fetches multisample stencil
correctly and so proved the hardware supports it.

The formatless storage-image regression dispatches compute shaders containing
the SPIR-V `StorageImageReadWithoutFormat` and
`StorageImageWriteWithoutFormat` capabilities. A transfer-initialized read and
an independently generated write both produce exact 17x13 `R32_UINT` results,
with untouched buffer guards. Multisample storage images remain disabled.

## Still experimental

- Depth/stencil resolve beyond sample zero: the averaging and min/max depth
  modes, partial regions, mip levels and array layers.
- FMASK/CMASK allocation, initialization, and sampled MSAA behavior.
- Cache and barrier synchronization across all game workloads. Single-hazard
  coverage and all three frame chain composition producers pass, yet Buckshot
  Roulette still flickers, so the remaining hazard is narrower than any of
  them. Its block corruption did clear when the sampling fixes landed; what is
  left is a flicker between a black background and white with red dots.
- Remaining resolve fallbacks for unusual formats and subresources.
- Long-session stability and untested scenes in DXVK-Sarek games.
- Full Vulkan 1.1 feature coverage and conformance testing.
- Blit coverage: typed, mirrored and layered blits are fixed, including the
  scaled and reversed depth ranges of 3D regions. What remains is the per-format
  matrix.
- Geometry and tessellation shaders: the hardware stage bindings and shader
  keys exist, but both feature bits stay `VK_FALSE` until the rest lands.
- Complete `vkCmdDispatchBase` and `gl_DeviceIndex` semantics.
- Vulkan conformance: Terakan is not a conformant implementation.

The prioritized implementation order, complexity estimates, hardware
limitations, and completion criteria are maintained in the
[development TODO](TODO.md).

The table records observed tests, not a claim of complete Vulkan 1.1 support.
Re-run `bin/terakan-test` after every clean build. A visually correct game
frame complements the readback tests but does not replace them.

The detailed CTS matrix, unsupported-feature breakdown and exact known
failures are recorded in [functional coverage](FUNCTIONAL_COVERAGE.md).
