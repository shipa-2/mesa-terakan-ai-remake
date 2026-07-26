# Arch Linux packaging

The maintained package recipe lives in the sibling repository:

```text
../vulkan-terakan/
```

It produces two split packages:

- `vulkan-terakan`: 64-bit Vulkan ICD and launch wrappers;
- `lib32-vulkan-terakan`: 32-bit ICD for 32-bit Wine/DXVK processes.

Build the committed Git branch:

```bash
cd ../vulkan-terakan
makepkg --cleanbuild --syncdeps --force
```

Package the current local source tree, including uncommitted and untracked
files:

```bash
cd ../vulkan-terakan
./build-local.sh --syncdeps
```

`build-local.sh` copies the working tree into an isolated temporary Git
snapshot below `vulkan-terakan/.makepkg/`. It never resets, cleans, or commits
the original source tree. Resulting packages are written to
`vulkan-terakan/packages/`.

Install both packages for mixed 64/32-bit Wine workloads:

```bash
sudo pacman -U packages/vulkan-terakan-*.pkg.tar.zst \
  packages/lib32-vulkan-terakan-*.pkg.tar.zst
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
