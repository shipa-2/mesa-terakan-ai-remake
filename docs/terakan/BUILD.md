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

Meson builds this driver, and no wrapper is needed for it. From the repository
root:

```bash
meson setup build-vulkan --native-file build-support/terakan.ini
meson compile -C build-vulkan
```

`build-support/terakan.ini` carries every option a Terakan build needs, so the
setup line above is the whole configuration. Add anything that varies on the
command line, where it takes precedence over the file:

```bash
meson setup build-vulkan --native-file build-support/terakan.ini --buildtype=debug
meson setup build-vulkan --native-file build-support/terakan.ini \
  -Dterakan-target-generation=r700
meson setup --wipe build-vulkan --native-file build-support/terakan.ini
```

`build-vulkan/` is the directory the helper scripts use by default, so building
into it directly keeps `./bin/terakan-test` and `./bin/terakan-run` working
against the same build. Meson writes its own `.gitignore` into a build
directory, so the name is otherwise free.

`bin/terakan-build` is a convenience wrapper around exactly that, and passes the
same file, so the two cannot describe different builds:

```bash
./bin/terakan-build
```

Either way the result is a release build containing only the `amd_terascale`
Vulkan driver and the focused Terakan regression suite. With the Vulkan loader
and `glslang` available, the build includes 13 CPU tests, 68 CAICOS GPU tests,
and the optional shader-corpus runner. It does not install or replace the
system Mesa stack.

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

## Manual Meson invocation

`build-support/terakan.ini` is the source of truth for Meson options; the
wrapper adds only the build type and, if asked for, the target generation. To
inspect what a configured directory actually ended up with:

```bash
meson configure build-vulkan
```

Its `pkg_config_path` is stated in that file because a `PKG_CONFIG_PATH` in the
environment pointing at `/usr/lib32/pkgconfig` -- which a machine set up to
build 32-bit software has -- otherwise makes every test binary fail to link
against the 32-bit `libvulkan`.

Do not combine the Terakan development build with a full Gallium/OpenGL
installation. Terakan needs only its Vulkan ICD; the distribution's Mesa
packages should continue providing OpenGL.
