# Building Terakan

## Get the canonical source

```bash
git clone --branch Terakan_state_rework --single-branch \
  https://github.com/shipa-2/mesa-terakan-ai-remake.git
cd mesa-terakan-ai-remake
```

This branch already contains the full Mesa tree and the Terakan driver. No
overlay, copied source directory, or separate patch application is required.

## Arch Linux dependencies

```bash
sudo pacman -S --needed base-devel git glslang libdrm libx11 libxcb \
  libxshmfence meson ninja python-mako python-packaging python-ply \
  python-yaml spirv-tools systemd-libs vulkan-headers vulkan-icd-loader \
  wayland wayland-protocols xcb-util-keysyms xorgproto zlib zstd
```

## Development build

From the repository root:

```bash
./bin/terakan-build
```

This configures `build-vulkan/` as a release build containing only the
`amd_terascale` Vulkan driver and the focused Terakan regression suite. With
the Vulkan loader and `glslang` available, the build includes four CPU tests,
seven CAICOS GPU tests, and the optional shader-corpus runner. It does not
install or replace the system Mesa stack.

Useful variants:

```bash
./bin/terakan-build --debug
./bin/terakan-build --clean
TERAKAN_BUILD_DIR="$PWD/build-debug" ./bin/terakan-build --debug
TERAKAN_CCACHE_DIR="$PWD/.local-cache/ccache" ./bin/terakan-build
```

`--clean` uses Meson's wipe operation only for an already configured Meson
directory. It refuses to delete an unrelated path.

For an incremental rebuild, run `./bin/terakan-build` again. Use `--clean`
after Meson options, compilers, or important dependencies change.

The main artifact is:

```text
build-vulkan/src/amd/terascale/vulkan/libvulkan_terascale.so
```

Run a program with this exact build without installing it:

```bash
./bin/terakan-run vulkaninfo --summary
./bin/terakan-run vkcube
./bin/terakan-run --mangohud vkcube
```

The launcher selects the generated development ICD manifest and disables
implicit Vulkan layers. With `--mangohud`, it allowlists only the installed
MangoHud layer while keeping device-selection and other overlays disabled. It
does not modify global environment or system files.

Build and run the complete focused suite:

```bash
./bin/terakan-test
```

Use `./bin/terakan-test --unit-only` on a machine without the target GPU. The
normal command deliberately refuses to accept a report from RADV, llvmpipe, or
another Vulkan ICD.

## Building with CMake

The build itself is described in Meson, because this repository is a Mesa fork
and Mesa's build is Meson: 308 `meson.build` files, a 2400-line root one and 330
rules that run code generators. `CMakeLists.txt` does not restate any of that.
It drives the same Meson build with the same options `bin/terakan-build` passes,
so the ordinary CMake commands work without a wrapper script and the build
description stays in one place:

```bash
cmake -B build
cmake --build build
ctest --test-dir build
```

`ctest` runs the same 81 tests `bin/terakan-test` does -- 13 CPU and 68 CAICOS
GPU -- one at a time, because they share the real GPU through the ICD they just
built. Select a subset the usual way:

```bash
ctest --test-dir build -R terakan_frame_chain
ctest --test-dir build --output-on-failure
```

The knobs are cache variables rather than environment variables:

| CMake variable | `bin/terakan-build` equivalent | Default |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `TERAKAN_BUILD_TYPE` | `Release` (`Debug` is Meson's `debug`, `RelWithDebInfo` its `debugoptimized`) |
| `TERAKAN_TARGET_GENERATION` | `TERAKAN_TARGET_GENERATION` | empty, leaving Meson's `auto` |
| `TERAKAN_CCACHE_DIR` | `TERAKAN_CCACHE_DIR` | `<build>/meson/.ccache` |
| `TERAKAN_PKG_CONFIG_PATH` | the `PKG_CONFIG_PATH` the script exports | `/usr/lib/pkgconfig:/usr/share/pkgconfig` |
| `CMAKE_INSTALL_PREFIX` | the script's `--prefix` | `/usr` |

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DTERAKAN_TARGET_GENERATION=r700
cmake --build build --target terakan-wipe   # Meson's wipe, like --clean
```

The Meson build directory is `build/meson`, and the driver is at
`build/meson/src/amd/terascale/vulkan/libvulkan_terascale.so`. Anything
Meson offers is still available there directly -- `meson configure
build/meson`, `meson test -C build/meson` and so on.

`bin/terakan-run` has no CMake equivalent and is still the way to run an
application against a build without installing it: it points the Vulkan loader
at the generated development ICD manifest, which is not something a build system
does.

## Manual Meson invocation

The wrapper is the source of truth for Meson options. To inspect the exact
configuration, read `bin/terakan-build` or run:

```bash
meson configure build-vulkan
```

Do not combine the Terakan development build with a full Gallium/OpenGL
installation. Terakan needs only its Vulkan ICD; the distribution's Mesa
packages should continue providing OpenGL.
