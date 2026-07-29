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
| Compute and shader control flow | Loop constants, singleton subgroup lowering, dispatch transitions and GPU readback pass |
| Draw state | `firstInstance`, dynamic SSBO offsets and graphics/compute state restoration pass |
| Image copies and sampling | Layered copies and mipmapped BC6H cube/array views pass on CAICOS |
| Color MSAA resolve | 2x/4x/8x, full/partial regions, layers, RGBA/BGRA pass |
| vkQuake3 | Vulkan renderer works in a 640x480 window |
| DXVK-Sarek | D3D11 FL 11_1; tested Katamari and Disco Elysium scenes render correctly |

The main unfinished areas are complete depth/stencil resolve behavior,
FMASK/CMASK-backed multisample sampling, unusual MSAA formats and subresources,
complete cache/barrier coverage, and Vulkan conformance. See the detailed
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

Install the current AUR recipe as a system Vulkan ICD on Arch Linux:

```bash
./bin/terakan-install
```

The installer keeps package sources, build files, and archives in
`.terakan-package/`, fetches the latest `Terakan_state_rework` revision, and
installs both 64-bit and 32-bit packages through pacman. Remove them with:

```bash
./bin/terakan-uninstall
# Compatibility path:
./bin/terakan/uninstall
```

## Documentation

- [Build instructions](docs/terakan/BUILD.md)
- [Testing and game launch instructions](docs/terakan/TESTING.md)
- [Arch Linux packaging](docs/terakan/PACKAGING.md)
- [Implemented functionality and known limitations](docs/terakan/STATUS.md)
- [Prioritized development TODO](docs/terakan/TODO.md)

Generic Mesa developer documentation remains under [`docs/`](docs/).

## Repository branches

- `Terakan_state_rework` — canonical Mesa/Terakan development branch.
- `dxvk-sarek-terakan` — retained DXVK-Sarek integration history.

Historical overlay branches are not part of the supported build workflow.

## Upstream

Terakan was originally developed by Triang3l. This repository carries the
ongoing integration and hardware-validation work for the Mesa driver.
