# Terakan status

Last updated: 2026-08-22. Primary target: AMD CAICOS (R9xx). Compute coverage
is also verified on PALM/Wrestler (R8xx).

## Verified in the current development cycle

| Area | Result |
|---|---|
| Vulkan instance/device discovery | AMD CAICOS (R9xx) and PALM/Wrestler (R8xx) Terakan devices, API 1.1.318 |
| Properties and driver identity | `VK_KHR_driver_properties`; Terakan/Mesa identity; non-conformant version reported honestly |
| VRAM reporting | `VK_EXT_memory_budget`; per-process heap usage plus kernel-reported global VRAM/GTT pressure |
| Focused regression suite | 4/4 CPU and 8/8 CAICOS GPU tests pass; all 48 executed safe basic-compute CTS cases pass identically on CAICOS and PALM; 1071 additional unattended-safe CTS cases plus 65 isolated clear cases completed on CAICOS |
| Compute and subgroup behavior | SSBO, atomics, shared memory, conditional workgroup barriers, image access and indirect dispatch pass on R8xx and R9xx |
| Draw state | `firstInstance` plus dynamic SSBO offsets pass; graphics/compute transitions preserve descriptors and draw state |
| Events and sparse property queries | implemented and CAICOS-tested |
| Image copy and layout | layered buffer/image copies pass; mipmapped array storage uses hardware-compatible power-of-two slice padding |
| BC6H cube/array sampling | 6 faces x 8 mip levels pass through cube, single-level and 2D-array views |
| Color MSAA resolve | 2x, 4x, and 8x; full/partial, layers, RGBA/BGRA passed |
| vkQuake3 | Vulkan renderer works in a 640x480 window |
| DXVK-Sarek | D3D11 FL 11_1; Katamari and tested Disco Elysium scenes render; Green Hell creates a 1920x1080 swapchain with Sarek |
| Godot workloads | Hangover Gallery and Fused 240 render correctly in user testing; Buckshot Roulette remains visually corrupted |

The BC6H regression test validates 48 independently addressed samples and has
a negative control that corrupts one expected value. The normal run reports
zero mismatches; the corrupted oracle must fail. Its few values that differ
from Mesa's software decoder were cross-checked against the upstream r600
OpenGL driver on the same CAICOS hardware.

The formatless storage-image regression dispatches compute shaders containing
the SPIR-V `StorageImageReadWithoutFormat` and
`StorageImageWriteWithoutFormat` capabilities. A transfer-initialized read and
an independently generated write both produce exact 17x13 `R32_UINT` results,
with untouched buffer guards. Multisample storage images remain disabled.

## Still experimental

- Complete depth/stencil resolve semantics and layout transitions.
- FMASK/CMASK allocation, initialization, and sampled MSAA behavior.
- Cache and barrier synchronization across all game workloads; attachment to
  texture coherency has focused coverage, not full application coverage.
- Remaining resolve fallbacks for unusual formats and subresources.
- Long-session stability and untested scenes in DXVK-Sarek games.
- Full Vulkan 1.1 feature coverage and conformance testing.
- Basic blit coverage: R32/BGRA, mirrored-coordinate and 3D variants currently
  fail readback, while the corresponding basic image copies pass.
- 3D image allocation/descriptor alignment: the first selected 3D clear is
  rejected by the radeon kernel CS validator and returns device lost.
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
