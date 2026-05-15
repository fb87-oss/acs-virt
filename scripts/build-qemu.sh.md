# build-qemu.sh

Configures and builds a patched QEMU target through CMake `ExternalProject`.

Usage:

```sh
scripts/build-qemu.sh
```

The script enters a Nix shell with the QEMU build dependencies when needed.

It builds both `qemu-system-x86_64` and `qemu-system-aarch64` in a single
configure/build step. Output binaries are in `build/out/qemu/bin/`.

