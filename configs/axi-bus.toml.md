# axi-bus.toml

Single runtime config for the `axi-bus` frontend and C backends.

Top-level `ram_access` configures the VM-wide RAM access mode used by QEMU and
all backend daemons.

`[[devices]]` entries define the canonical device inventory. Each device has a
unique `name`, a concrete virtio `type`, and an inline `mmio` table for fixed
hardware resources such as base, size, and IRQ.

`[targets.qemu]` configures the QEMU frontend target. Its nested
`[[targets.qemu.devices]]` entries are the QEMU enable list: only devices listed
there are emitted on the QEMU command line. Each target device entry carries
QEMU-specific settings such as `socket` and optional log paths, plus backend
runtime fields consumed by the launcher when it starts the daemon.

The launcher starts backend daemons with comma-separated `key=value` arguments
derived from the joined device and target-device entries. Use `--no-backend` to
skip backend startup and launch only QEMU.
