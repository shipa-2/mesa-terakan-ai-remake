# Terakan

Terakan is an experimental Mesa Vulkan driver for AMD TeraScale GPUs. The
current development and release branch is `Terakan_state_rework`; it is a
complete Mesa source tree and does not require an overlay or an older Terakan
repository.

> [!WARNING]
> Terakan is not Vulkan-conformant yet. Keep the distribution Mesa installation
> for OpenGL and use the supplied launch wrapper to select Terakan per process.

## Hardware and current status

Development is validated on AMD CAICOS (Evergreen/R8xx) using the Linux
`radeon` kernel driver. A separate port to TeraScale 1 (R600/R700) is under
way on real RV610 and RV710 hardware; it is described below and is not usable
for rendering yet.

### Evergreen (CAICOS)

| Area | Current result |
|---|---|
| Vulkan device discovery | AMD R8xx (CAICOS) is detected as Terakan |
| Reported API version | Vulkan 1.1.318 |
| Properties/features validation | Focused report completes with 0 errors |
| Compute and shader control flow | All 48 executed safe basic-compute CTS cases pass on CAICOS and PALM |
| Draw state | `firstInstance`, dynamic SSBO offsets and graphics/compute state restoration pass |
| Image copies and sampling | Layered copies and mipmapped BC6H cube/array views pass on CAICOS |
| Color MSAA resolve | 2x/4x/8x, full/partial regions, layers, RGBA/BGRA pass |
| vkQuake3 | Vulkan renderer works in a 640x480 window |
| DXVK-Sarek | D3D11 FL 11_1; Katamari and Disco Elysium render, Green Hell creates its 1920x1080 swapchain |

Copies, blits and resolves are complete: `dEQP-VK.api.copy_and_blit.core.resolve_image`
passes 102 of 102, and none of a 8536-case stride sample across the whole group
fails. The main unfinished areas are complete cache/barrier coverage, the
vertex pipeline stages (geometry, tessellation, transform feedback), and Vulkan
conformance. See the detailed [status page](docs/terakan/STATUS.md).

### TeraScale 1 (R600/R700)

R600 and R700 need a genuinely separate code path rather than different values
in the Evergreen one: no tessellator, a fixed 64-lane wavefront, and a
differently shaped `SQ_THREAD_RESOURCE_MGMT`/`SQ_GPR_RESOURCE_MGMT` register
set. The work is deliberately incremental, and the boundary between what the
hardware has confirmed and what is only packet-level is kept explicit.

| Area | Current result |
|---|---|
| Device enumeration and properties | Real RV610 `1002:94c1` and RV710 `1002:954f` are recognized and report their own limits |
| Logical device creation | Passes create/destroy on both, ten consecutive runs |
| Image layout and allocation | Linear and tiled create/layout/allocate/bind pass on hardware, including mip chains and the classic FMASK/CMASK pair for MSAA |
| Shader compilation | Real application vertex and fragment pipelines compile for `ISA_CC_R600` and `ISA_CC_R700` |
| Command submission | An empty recorded command buffer completes its fence on RV710, once and then five more times, with a clean kernel journal |
| CP DMA | Chunk limits and packet construction have exact CPU oracles; a buffer-to-buffer copy probe is built |
| Rendering | **Not working.** No draw has produced correct output on TeraScale 1 |

Submission is off by default and stays off unless asked for explicitly:
`terakan_queue_submit` returns `VK_ERROR_DEVICE_LOST` unless
`TERAKAN_DEBUG_TERASCALE_1_SUBMIT=1` is set exactly. That guard is a bring-up
tool, not a switch that makes the driver usable -- submitting real work on this
path has locked the adapter and needed a reboot.

Most of the register work behind these rows is verified by exact CPU oracles
over the emitted packets rather than by the hardware: per-draw CB/DB/SPI state,
the direct indexed draw packet, the preamble, and the NIR meta shaders for
clearing are all in that state. Building for these generations:

```bash
TERAKAN_TARGET_GENERATION=r700 ./bin/terakan-build
```

`auto` -- the default -- reads the card in the build machine, so an Evergreen
machine labels the build `r800` whatever the target is; set the variable when
cross-building.

## Quick start

On Arch Linux:

```bash
sudo pacman -S --needed base-devel git glslang libdrm libx11 libxcb \
  libxshmfence meson ninja python-mako python-packaging python-ply \
  python-yaml spirv-tools systemd-libs vulkan-headers vulkan-icd-loader \
  wayland wayland-protocols xcb-util-keysyms xorgproto zlib zstd

git clone --branch Terakan_state_rework --single-branch \
  https://github.com/shipa-2/mesa-terakan-ai-remake.git
cd mesa-terakan-ai-remake
meson setup build-vulkan --native-file build-support/terakan.ini
meson compile -C build-vulkan
./bin/terakan-test
```

Meson builds the driver; `build-support/terakan.ini` carries every option the
build needs, so that setup line is the whole configuration. `./bin/terakan-build`
is a convenience wrapper around the same two commands and the same file, adding
only the build type and, if asked for, the target generation:

```bash
./bin/terakan-build          # same build as above
./bin/terakan-build --debug  # meson setup --buildtype=debug
```

`./bin/terakan-test` runs the focused suite -- 13 CPU tests and 69 CAICOS GPU
tests -- against the build in `build-vulkan/`.

Run applications against the local build without installing it:

```bash
./bin/terakan-run vulkaninfo --summary
./bin/terakan-run vkcube
./bin/terakan-run --mangohud vkcube
```

For Steam/Proton, install the per-user launcher and use it in the game's launch
options:

```bash
./bin/steam-terakan-run --install
steam-terakan-run --doctor
```

```text
steam-terakan-run %command%
```

The build and run helpers use `build-vulkan/` inside the source tree and do not
replace system Mesa. MangoHud mode keeps every other implicit Vulkan layer
disabled, so a device-selection layer cannot redirect the application to
another GPU.

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
- [Focused Vulkan functional coverage](docs/terakan/FUNCTIONAL_COVERAGE.md)
- [Prioritized development TODO](docs/terakan/TODO.md)

Generic Mesa developer documentation remains under [`docs/`](docs/).

## Repository branches

- `Terakan_state_rework` — canonical Mesa/Terakan development branch.
- `dxvk-sarek-terakan` — retained DXVK-Sarek integration history.

Historical overlay branches are not part of the supported build workflow.

## Upstream

Terakan was originally developed by Triang3l. This repository carries the
ongoing integration and hardware-validation work for the Mesa driver. The
remake is directed and hardware-tested by Shipa Snake, with implementation and
analysis performed in collaboration with AI coding agents; it is not presented
as the work of one person.
