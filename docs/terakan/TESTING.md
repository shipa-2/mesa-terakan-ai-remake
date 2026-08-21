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
R9xx/CAICOS Terakan device and Vulkan API 1.1. A different device name means
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

## Safe basic-compute CTS parity

Run the Vulkan CTS binary from its Vulkan module directory so its Amber
resources resolve. The three maximum-local-size cases remain excluded because
they can lock up or lose the device on current hardware:

```bash
./deqp-vk \
  --deqp-case='dEQP-VK.compute.pipeline.basic.*' \
  --deqp-exclude-case='dEQP-VK.compute.pipeline.basic.max_local_size_x,dEQP-VK.compute.pipeline.basic.max_local_size_y,dEQP-VK.compute.pipeline.basic.max_local_size_z'
```

The 2026-08-13 current build produces the same result on CAICOS/R9xx and
PALM/Wrestler/R8xx: 48 passed, 0 failed, 29 not supported. This includes the
stateful `shared_var_multiple_groups` then `branch_past_barrier` sequence that
previously exposed SFN moving `GROUP_BARRIER` before an LDS write.

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

### Steam launch option

Install the repository launcher into the user command path once:

```bash
./bin/steam-terakan-run --install
steam-terakan-run --doctor
```

Then put this in the game's Steam launch options:

```text
steam-terakan-run %command%
```

The launcher follows its symbolic link back to this repository, delegates ICD
selection and implicit-layer filtering to `bin/terakan-run`, and preserves
Steam/Proton arguments without shell re-evaluation. If a sibling
`../DXVK-Sarek/x64` build is present, it also prepends that directory to
`WINEDLLPATH`, requests native `d3d11`, `d3d10`, `d3d10core` and `dxgi`, and
temporarily links the available Sarek DLLs next to the Windows game executable.
The adjacent links are removed when Proton exits. This last step is necessary
because Proton refreshes its own PE DXVK DLLs in the prefix after the launcher
starts; `WINEDLLPATH` alone does not override those files.
Override it with `TERAKAN_DXVK_DIR`, pass `--dxvk-sarek DIR`, or disable DLL
selection for a Vulkan-native game with:

```text
steam-terakan-run --no-dxvk-sarek %command%
```

The launcher never replaces an existing adjacent DLL. It exits with an error
if the game directory already contains `dxgi.dll`, `d3d11.dll`, or
`d3d10core.dll` from another override, or if the directory is not writable.

`--install` creates a link in `~/.local/bin` and, when the user-owned legacy
Steam runtime is present, in its `game-bin` directory too. Desktop-launched
Steam commonly omits `~/.local/bin` from `PATH`, while `game-bin` is prepended
to game commands. If a runtime update removes that link, run `--install`
again. The always-valid fallback is:

```text
/home/shipa/.local/bin/steam-terakan-run %command%
```

The wrapper must precede `%command%`. Supplying only `steam-terakan-run`, or
placing it after `%command%`, merely passes the name to the Windows game as an
ordinary argument and cannot affect its Vulkan environment.

The default implicit-layer block disables Steam's Vulkan overlay and
Fossilize layers as well as device-selection and capture layers. This keeps a
game from being redirected away from the selected Terakan ICD while the
driver is experimental.

The repository branch `dxvk-sarek-terakan` is kept as integration history; it
is not selected or built by the Mesa helpers in `bin/`.

Current focused game evidence:

- Katamari Damacy REROLL: gameplay and display-settings scenes render correctly
  after fixing mipmapped cube/array slice layout.
- Disco Elysium: the tested in-game scene renders correctly after descriptor,
  shader, copy, and graphics/compute state fixes.
- Green Hell: D3D11 FL 11_1 device creation and a 1920x1080 three-image
  swapchain succeed. The earlier generic "DirectX 11" dialog was caused by
  `shaderStorageImageWriteWithoutFormat` being rejected during `vkCreateDevice`,
  not by a missing DirectX installation. This is startup evidence, not yet a
  claim that gameplay rendering is correct.
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

## Focused Vulkan CTS coverage

The current safe compute matrix, image/graphics synchronization results,
unsupported capability breakdown and known failing cases are maintained in
[functional coverage](FUNCTIONAL_COVERAGE.md). In particular, do not include
the three `max_local_size_*` cases in unattended runs until their previous GPU
lockup has been diagnosed.

The expanded 2026-08-22 CAICOS matrix established an additional hard denylist
entry:

```text
dEQP-VK.api.image_clearing.core.clear_color_image.3d.*
```

The first optimal `R8G8B8A8_UNORM` case was rejected by the radeon kernel
command stream validator because its mipmap BO base was not 4096-byte aligned,
and CTS reported `VK_ERROR_DEVICE_LOST`. Exclude all 3D clear/image variants
from unattended runs until the image allocation and texture descriptor path is
fixed. Use `--deqp-watchdog=enable`,
`--deqp-terminate-on-device-lost=enable` and an external process timeout for
every newly expanded batch.
