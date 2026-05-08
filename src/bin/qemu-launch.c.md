# qemu-launch.c

C implementation of the TOML-to-QEMU frontend launcher.

It keeps the existing launcher command-line contract:

```text
qemu-launch --kernel <bzImage> --initrd <initrd> [--dry-run] <vm.toml> [-- <qemu-args>...]
```

The launcher parses VM configs with `fastoml`, validates the MMIO-only microvm
settings, converts relative paths to workspace-absolute paths, and then execs the
configured QEMU binary with `axi-bus` devices from `[[mmio_windows]]`.
