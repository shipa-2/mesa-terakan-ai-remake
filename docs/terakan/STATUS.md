# Terakan status

Last updated: 2026-07-29. Target hardware: AMD CAICOS.

## Verified in the current development cycle

| Area | Result |
|---|---|
| Vulkan instance/device discovery | AMD R8xx (CAICOS) Terakan device, API 1.1.318 |
| Properties and driver identity | `VK_KHR_driver_properties`; Terakan/Mesa identity; non-conformant version reported honestly |
| Focused regression suite | 4 CPU tests and 7 CAICOS GPU tests pass |
| Compute and subgroup behavior | loop constants, singleton subgroup lowering, descriptor buffers, dispatch state and readback pass |
| Draw state | `firstInstance` plus dynamic SSBO offsets pass; graphics/compute transitions preserve descriptors and draw state |
| Events and sparse property queries | implemented and CAICOS-tested |
| Image copy and layout | layered buffer/image copies pass; mipmapped array storage uses hardware-compatible power-of-two slice padding |
| BC6H cube/array sampling | 6 faces x 8 mip levels pass through cube, single-level and 2D-array views |
| Color MSAA resolve | 2x, 4x, and 8x; full/partial, layers, RGBA/BGRA passed |
| vkQuake3 | Vulkan renderer works in a 640x480 window |
| DXVK-Sarek | D3D11 FL 11_1; Katamari display settings and tested Disco Elysium gameplay scene render correctly |

The BC6H regression test validates 48 independently addressed samples and has
a negative control that corrupts one expected value. The normal run reports
zero mismatches; the corrupted oracle must fail. Its few values that differ
from Mesa's software decoder were cross-checked against the upstream r600
OpenGL driver on the same CAICOS hardware.

## Still experimental

- Complete depth/stencil resolve semantics and layout transitions.
- FMASK/CMASK allocation, initialization, and sampled MSAA behavior.
- Cache and barrier synchronization across all game workloads; attachment to
  texture coherency has focused coverage, not full application coverage.
- Remaining resolve fallbacks for unusual formats and subresources.
- Long-session stability and untested scenes in DXVK-Sarek games.
- Full Vulkan 1.1 feature coverage and conformance testing.
- Vulkan conformance: Terakan is not a conformant implementation.

The table records observed tests, not a claim of complete Vulkan 1.1 support.
Re-run `bin/terakan-test` after every clean build. A visually correct game
frame complements the readback tests but does not replace them.
