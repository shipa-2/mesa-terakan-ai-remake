# Testing Terakan

## Standard test

```bash
./bin/terakan-test
```

It performs three independently meaningful checks:

1. builds the current working tree;
2. runs the CPU-only `terakan_vertex_input` unit test;
3. runs `terakan_physical_device_properties` and `vulkaninfo --summary`
   against the generated local ICD, and rejects the
   result unless a Terakan or CAICOS device is actually reported.

This prevents a passing result from accidentally coming from RADV, llvmpipe,
or the system Vulkan driver.

On the current CAICOS test system the hardware report must identify an AMD
R8xx/CAICOS Terakan device and Vulkan API 1.1. A different device name means
that the intended ICD was not tested.

CPU-only test:

```bash
./bin/terakan-test --unit-only
```

Optional ten-second visual smoke test:

```bash
./bin/terakan-test --vkcube
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

## Evidence and failure handling

- Save full stderr/stdout for a failing test.
- Record the driver commit, DXVK build, Wine prefix, resolution, and GPU.
- For rendering faults, capture the application window rather than the whole
  desktop.
- A timeout only proves that a process survived until termination; it is not a
  rendering correctness result.
- Compare readbacks or screenshots where possible. A successful return code
  alone is insufficient for resolve, clear, copy, and synchronization tests.
