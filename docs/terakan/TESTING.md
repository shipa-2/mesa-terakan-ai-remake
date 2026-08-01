# Testing Terakan

## Standard test

```bash
./bin/terakan-test
```

It performs independently meaningful CPU, GPU, identity, and negative checks:

1. builds the current working tree;
2. runs the CPU-only shader lowering, loop-constant, descriptor-buffer, and
   vertex-input tests;
3. runs seven tests against the generated local ICD: layered image copies,
   instanced dynamic SSBO access, three BC6H cube/array view variants, compute
   loops, and the physical-device report;
4. corrupts one BC6H expected sample and requires the test to fail;
5. runs `vulkaninfo --summary` and rejects the result unless a Terakan or
   CAICOS device is actually reported.

The physical-device report also guards the current depth/stencil resolve
boundary: `VK_KHR_depth_stencil_resolve` and `VK_KHR_dynamic_rendering` must
remain unadvertised, with no supported resolve modes, until the staged
DB-to-color and depth-export implementation passes readback coverage.

This prevents a passing result from accidentally coming from RADV, llvmpipe,
or the system Vulkan driver.

On the current CAICOS test system the hardware report must identify an AMD
R8xx/CAICOS Terakan device and Vulkan API 1.1. A different device name means
that the intended ICD was not tested.

The report also validates `VK_KHR_driver_properties`. `driverName` must be
`Terakan`, `driverInfo` must identify Mesa, and `conformanceVersion` remains
`0.0.0.0` until the driver has passed the Vulkan conformance test suite.

CPU-only tests:

```bash
./bin/terakan-test --unit-only
```

Optional ten-second visual smoke test:

```bash
./bin/terakan-test --vkcube
```

## MangoHud

Enable MangoHud without allowing other implicit Vulkan layers:

```bash
./bin/terakan-run --mangohud vkcube
./bin/terakan-run \
  --mangohud-config 'fps,frametime,vram,proc_vram,gpu_name,vulkan_driver' \
  wine /path/to/game.exe
```

Use a project-local patched MangoHud without replacing the system package:

```bash
./bin/terakan-run \
  --mangohud-prefix ../MangoHud-debug/install-terakan64 \
  --mangohud-config 'fps,frametime,vram,gpu_name,vulkan_driver,gpu_list=0' \
  wine /path/to/game.exe
```

The launcher still selects only the local Terakan ICD. It uses the Vulkan
Loader allowlist to exempt `VK_LAYER_MANGOHUD_overlay*` from the default
`~implicit~` block, so Mesa device-select, Steam overlays, RenderDoc and other
implicit layers remain disabled. Both 64-bit and 32-bit MangoHud manifests are
supported when the corresponding distribution packages are installed.
On DRM Radeon, Terakan exposes `VK_EXT_memory_budget`: `heapUsage` tracks
successful application `VkDeviceMemory` allocations and frees, while
`heapBudget` also accounts for system-wide VRAM/GTT pressure reported by the
kernel. The CAICOS properties test allocates 16 MiB, verifies the matching
usage increase, frees it, and requires the counter to return to its baseline.

Run selected tests directly through Meson:

```bash
meson test -C build-vulkan --print-errorlogs \
  terakan_image_array_copy \
  terakan_instance_dynamic_ssbo \
  terakan_bc6_cube \
  terakan_bc6_cube_single_level_views \
  terakan_bc6_array_view \
  terakan_compute_loop
```

The BC6H test covers six cube faces across eight mip levels. It uploads every
subresource, samples through cube/array views, copies the result back, and
compares all 48 outputs. To verify that the oracle is live rather than a
pass-only smoke test:

```bash
./bin/terakan-run \
  build-vulkan/src/amd/terascale/vulkan/terakan_bc6_cube_test \
  build-vulkan/src/amd/terascale/vulkan/terakan_bc6_cube.vert.spv \
  build-vulkan/src/amd/terascale/vulkan/terakan_bc6_cube.frag.spv \
  --corrupt-expectation
echo "$?"  # must be 1
```

## Windowed vkQuake3 test

```bash
./bin/terakan-run \
  ../vkQuake3/build/release-linux-x86_64/ioquake3.x86_64 \
  +set fs_basepath "$PWD/../vkQuake3/build/release-linux-x86_64/q3a" \
  +set fs_homepath /tmp/terakan-q3 \
  +set cl_renderer vulkan \
  +set r_fullscreen 0 \
  +set r_mode 4 \
  +set com_introplayed 1 \
  +devmap q3dm1
```

`r_mode 4` is 640x480. Keep a timeout around unattended GUI runs.

## Wine and DXVK

Use a known Wine prefix containing DXVK-Sarek, then select the same local
Terakan ICD:

```bash
./bin/terakan-run wine /path/to/game.exe
```

Do not set `PROTON_USE_WINED3D=1`: that bypasses DXVK and therefore does not
test the Vulkan driver. Do not force unsupported Mesa/OpenGL extensions.

The repository branch `dxvk-sarek-terakan` is kept as integration history; it
is not selected or built by the Mesa helpers in `bin/`.

Current focused game evidence:

- Katamari Damacy REROLL: gameplay and display-settings scenes render correctly
  after fixing mipmapped cube/array slice layout.
- Disco Elysium: the tested in-game scene renders correctly after descriptor,
  shader, copy, and graphics/compute state fixes.
- SuperTuxKart: useful as an exploratory Vulkan workload, but its Vulkan
  renderer is not treated as a conformance oracle and driver behavior must be
  confirmed with focused tests.

## Evidence and failure handling

- Save full stderr/stdout for a failing test.
- Record the driver commit, DXVK build, Wine prefix, resolution, and GPU.
- For rendering faults, capture the application window rather than the whole
  desktop.
- A timeout only proves that a process survived until termination; it is not a
  rendering correctness result.
- Compare readbacks or screenshots where possible. A successful return code
  alone is insufficient for resolve, clear, copy, and synchronization tests.
