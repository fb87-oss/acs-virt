# chiplets-launcher.py

Python launcher for socket-mode runtime configs such as `samples/axi-x64.toml`
and `samples/axi-a64.toml`.

It reads top-level `ram_access`, the `[targets.qemu]` target, each enabled
`[[targets.qemu.devices]]` entry, and the matching `[[devices]]` entry's
inline `mmio` table. It builds the QEMU command line for x86_64 `microvm` or
ARM64 `virt`, resolves relative socket, backend, QEMU, kernel, and initrd paths
to absolute paths, starts backend daemons unless `--no-backend` is set, supports
`--dry-run`, and forwards extra QEMU arguments after `--`.

QEMU's data directory is derived from the configured `binary`: if
`../share/qemu` exists next to the binary's install prefix, the launcher passes it
with `-L`. Project-built QEMU installs copy `pc-bios` there.

The flake wrapper invokes this script with Nix-provided Python, so no host Python
installation is required for normal `nix run .#runvm` usage.

This launcher is separate from `scripts/chiplets-uio-x64.py`, which owns the
two-VM UIO smoke and benchmark topology.
