# Arch Linux packaging

The maintained package recipe is in
[`shipa-2/mesa-terakan-patches`](https://github.com/shipa-2/mesa-terakan-patches)
on branch `packaging-rework`. It produces two split packages:

- `vulkan-ai-terakan`: 64-bit Vulkan ICD and launch wrappers;
- `lib32-vulkan-ai-terakan`: 32-bit ICD for 32-bit Wine/DXVK processes.

For a normal system installation from AUR:

```bash
./bin/terakan-install
```

This builds the latest `Terakan_state_rework` commit and installs both packages
through pacman. The package owns the ICD libraries and manifests under the
standard system locations, and provides `vulkan-driver` plus
`lib32-vulkan-driver`. Uninstall it with `./bin/terakan-uninstall`.

The AUR recipe follows the Git branch rather than a manually maintained release
archive. Rebuilding the package therefore picks up current driver fixes,
including shader/compiler, image-layout, copy/resolve, and synchronization
changes, without editing the AUR recipe for every Terakan commit.

Build the committed Git branch:

```bash
git clone --branch packaging-rework --single-branch \
  https://github.com/shipa-2/mesa-terakan-patches.git vulkan-terakan
cd vulkan-terakan
makepkg --cleanbuild --syncdeps --force
```

Package the current local source tree, including uncommitted and untracked
files. When the packaging and Mesa repositories are not siblings, pass the
source tree explicitly:

```bash
cd /path/to/vulkan-terakan
TERAKAN_SOURCE_TREE=/path/to/mesa-terakan-ai-upstreamed \
  ./build-local.sh --syncdeps
```

`build-local.sh` copies the working tree into an isolated temporary Git
snapshot below `vulkan-terakan/.makepkg/`. It never resets, cleans, or commits
the original source tree. Resulting packages are written to
`vulkan-terakan/packages/`.

Install both packages for mixed 64/32-bit Wine workloads:

```bash
sudo pacman -U packages/vulkan-ai-terakan-*.pkg.tar.zst \
  packages/lib32-vulkan-ai-terakan-*.pkg.tar.zst
```

After installation:

```bash
terakan-vulkan vulkaninfo --summary
terakan-vulkan vkcube
terakan-wine wine /path/to/game.exe
terakan-test-capabilities
```

The package intentionally does not install r600 Gallium/OpenGL, kernel module
options, or initramfs hooks. CAICOS continues using the distribution's
`radeon` kernel driver and OpenGL stack.
