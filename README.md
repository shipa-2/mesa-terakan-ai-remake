# Terakan

Terakan is an experimental Mesa Vulkan driver for AMD TeraScale GPUs. The
current development and release branch is `Terakan_state_rework`; it is a
complete Mesa source tree and does not require an overlay or an older Terakan
repository.

> [!WARNING]
> Terakan is not Vulkan-conformant yet. Keep the distribution Mesa installation
> for OpenGL and use the supplied launch wrapper to select Terakan per process.

## Hardware and current status

Development is currently validated on AMD CAICOS using the Linux `radeon`
kernel driver.

| Area | Current result |
|---|---|
| Vulkan device discovery | AMD R8xx (CAICOS) is detected as Terakan |
| Reported API version | Vulkan 1.1.318 |
| Properties/features validation | Focused report completes with 0 errors |
| Compute smoke test | GPU readback `{1,2,3,4}` passes |
| Color MSAA resolve | 2x/4x/8x, full/partial regions, layers, RGBA/BGRA pass |
| vkQuake3 | Vulkan renderer works in a 640x480 window |
| DXVK-Sarek | D3D11 FL 11_1 device creation works; game rendering remains experimental |

The main unfinished areas are complete depth/stencil resolve behavior, unusual
MSAA formats and subresources, cache/barrier synchronization, and rendering
correctness in demanding DXVK-Sarek games. See the detailed
[status page](docs/terakan/STATUS.md).

## Quick start

On Arch Linux:

```bash
sudo pacman -S --needed base-devel git glslang libdrm libx11 libxcb \
  libxshmfence meson ninja python-mako python-packaging python-ply \
  python-yaml spirv-tools systemd-libs vulkan-headers vulkan-icd-loader \
  wayland wayland-protocols xcb-util-keysyms xorgproto zlib zstd

git clone --branch Terakan_state_rework --single-branch \
  https://github.com/shipa-2/mesa-terakan-ai-upstreamed.git
cd mesa-terakan-ai-upstreamed
./bin/terakan-build
./bin/terakan-test
```

Run applications against the local build without installing it:

```bash
./bin/terakan-run vulkaninfo --summary
./bin/terakan-run vkcube
```

The build and run helpers use `build-vulkan/` inside the source tree and do not
replace system Mesa.

## Documentation

- [Build instructions](docs/terakan/BUILD.md)
- [Testing and game launch instructions](docs/terakan/TESTING.md)
- [Arch Linux packaging](docs/terakan/PACKAGING.md)
- [Implemented functionality and known limitations](docs/terakan/STATUS.md)

Generic Mesa developer documentation remains under [`docs/`](docs/).

## Repository branches

- `Terakan_state_rework` — canonical Mesa/Terakan development branch.
- `dxvk-sarek-terakan` — retained DXVK-Sarek integration history.

Historical overlay branches are not part of the supported build workflow.

## Upstream

Terakan was originally developed by Triang3l. This repository carries the
ongoing integration and hardware-validation work for the Mesa driver.
