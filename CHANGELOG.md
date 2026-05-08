# Changelog

## 2026-05-08

- Added target-aware QEMU build support for x86_64 microvm and AArch64 virt, including a shared `scripts/build-qemu.sh` helper and architecture-specific wrappers.
- Moved runtime configs to architecture-explicit samples: `samples/virt-axi-x64.toml` and `samples/virt-axi-a64.toml`.
- Added AArch64 `virt-axi` QEMU integration with FDT `virtio,mmio` discovery, dynamic sysbus allowlisting, and a `virt-axi` device config.
- Updated the launcher to support both `microvm` and AArch64 `virt` machine types.
- Added runtime config docs/schema and AArch64 smoke coverage.
- Kept both samples on the common AXI MMIO aperture at `0xfeb00000` and `0xfeb00200`, with target-specific IRQ wiring.
- Renamed the emulated AXI transport from `axi-bus` to `virt-axi` across QEMU patches, samples, launcher output, docs, tests, and backend source.
- Reorganized C backend source into generic virtio core/transport files, `src/fabrics/virt-axi.*`, and driver daemons under `src/drivers/`.
- Merged split block and console backend implementations into `virtio-blkd` and `virtio-consoled` driver sources with updated source docs and Doxygen comments.
- Added clang-format configuration for 4-space, spaces-only C formatting and reformatted the `src` tree.
