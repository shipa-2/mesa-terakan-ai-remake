# Building Terakan

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
`amd_terascale` Vulkan driver and focused unit tests. It does not install or
replace the system Mesa stack.

Useful variants:

```bash
./bin/terakan-build --debug
./bin/terakan-build --clean
TERAKAN_BUILD_DIR="$PWD/build-debug" ./bin/terakan-build --debug
TERAKAN_CCACHE_DIR="$PWD/.local-cache/ccache" ./bin/terakan-build
```

`--clean` uses Meson's wipe operation only for an already configured Meson
directory. It refuses to delete an unrelated path.

The main artifact is:

```text
build-vulkan/src/amd/terascale/vulkan/libvulkan_terascale.so
```

Run a program with this exact build without installing it:

```bash
./bin/terakan-run vulkaninfo --summary
./bin/terakan-run vkcube
```

The launcher selects the generated development ICD manifest and disables
implicit Vulkan layers. It does not modify global environment or system files.

## Manual Meson invocation

The wrapper is the source of truth for Meson options. To inspect the exact
configuration, read `bin/terakan-build` or run:

```bash
meson configure build-vulkan
```

Do not combine the Terakan development build with a full Gallium/OpenGL
installation. Terakan needs only its Vulkan ICD; the distribution's Mesa
packages should continue providing OpenGL.
