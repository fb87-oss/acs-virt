# build-tools.sh

Configures the CMake build and builds the local C tools:

- `build/out/virtio-blkd`
- `build/out/virtio-consoled`
- `build/out/c-backend-tests`

The script enters a Nix shell with CMake, GCC, and Git when needed. QEMU source
fetching is disabled for this target set so normal backend builds stay fast.

Set `CMAKE_BUILD_DIR` to override the default `build/cmake` directory. The older
`BUILD_DIR` variable is still accepted as a fallback.

Set `CHIPLETS_BACKEND_FABRIC` to select the backend fabric implementation passed
to CMake. Supported values are `qemu-socket`, `linux-devmem`, and `linux-uio`.
The default is `qemu-socket`, which is used by the TOML QEMU samples. Older
aliases `axi`, `devmem`, and `uio` are still accepted by CMake for compatibility.
