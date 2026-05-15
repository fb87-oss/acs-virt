# build-qemu.sh

Configures and builds a patched QEMU target through CMake `ExternalProject`.

Usage:

```sh
scripts/build-qemu.sh x64
scripts/build-qemu.sh a64
```

The script enters a Nix shell with the QEMU build dependencies when needed.

Target mapping:

- `x64`: builds `build/out/qemu-x64-minimal/bin/qemu-system-x86_64` using `cmake/qemu-targets/x64-minimal.cmake`.
- `a64` or `arm64`: builds `build/out/qemu-arm64-default/bin/qemu-system-aarch64` using `cmake/qemu-targets/arm64-default.cmake`.

The architecture-specific wrapper scripts call this helper for convenience.
