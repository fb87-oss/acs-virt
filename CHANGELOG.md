# Changelog

## 2026-05-08

- Added target-aware QEMU build support for x86_64 microvm and AArch64 virt, including a shared `scripts/build-qemu.sh` helper and architecture-specific wrappers.
- Moved runtime configs to architecture-explicit samples: `samples/axi-bus-x64.toml` and `samples/axi-bus-a64.toml`.
- Added AArch64 `axi-bus` QEMU integration with FDT `virtio,mmio` discovery, dynamic sysbus allowlisting, and a `virt-axi` device config.
- Updated the launcher to support both `microvm` and AArch64 `virt` machine types.
- Added runtime config docs/schema and AArch64 smoke coverage.
- Kept both samples on the common AXI MMIO aperture at `0xfeb00000` and `0xfeb00200`, with target-specific IRQ wiring.
