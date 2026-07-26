# Terakan status

Last updated: 2026-07-26. Target hardware: AMD CAICOS.

## Verified in the current development cycle

| Area | Result |
|---|---|
| Vulkan instance/device discovery | Terakan CAICOS device reported |
| Focused unit tests | vertex input and physical-device properties pass |
| Compute smoke test | readback `{1,2,3,4}` passed |
| Events and sparse property queries | implemented and CAICOS-tested |
| Color MSAA resolve | 2x, 4x, and 8x; full/partial, layers, RGBA/BGRA passed |
| vkQuake3 | Vulkan renderer works in a 640x480 window |
| DXVK-Sarek device creation | D3D11 FL 11_1 reached without device loss |

## Still experimental

- Complete depth/stencil resolve semantics and layout transitions.
- Cache and barrier synchronization across all game workloads.
- Remaining resolve fallbacks for unusual formats and subresources.
- Rendering correctness in Katamari gameplay under DXVK-Sarek.
- Vulkan conformance: Terakan is not a conformant implementation.

The table records observed tests, not a claim of complete Vulkan 1.1 support.
Re-run `bin/terakan-test` after every clean build and use targeted GPU
readback tests for changes to meta, resolve, and synchronization code.
