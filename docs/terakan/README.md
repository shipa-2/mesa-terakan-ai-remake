# Terakan developer documentation

Terakan is the experimental Mesa Vulkan driver for AMD TeraScale GPUs. This
directory documents the workflow used by this repository; generic Mesa
documentation remains under `docs/`.

- [Build](BUILD.md) — dependencies and reproducible 64-bit development build.
- [Testing](TESTING.md) — unit tests, CAICOS smoke tests, games, and logs.
- [Packaging](PACKAGING.md) — Arch Linux split packages and local snapshots.
- [Status](STATUS.md) — verified functionality and known limitations.

The canonical source tree is this repository on branch
`Terakan_state_rework`. It is a complete Mesa tree: do not copy an old overlay,
patch stack, or development repository over it.

The separate `dxvk-sarek-terakan` branch is retained for DXVK-Sarek integration
history. It is not a Mesa build branch.
