# build-tools.sh

Configures the CMake build and builds the local C tools:

- `out/virtio-blkd`
- `out/virtio-consoled`
- `out/c-backend-tests`

The script enters a Nix shell with CMake, GCC, and Git when needed. QEMU source
fetching is disabled for this target set so normal backend builds stay fast.

Set `CMAKE_BUILD_DIR` to override the default `build/cmake` directory. The older
`BUILD_DIR` variable is still accepted as a fallback.

Set `CHIPLETS_BACKEND_FABRIC` to select the backend fabric implementation passed
to CMake. Supported values are `axi` and `devmem`; `axi` is the
default fabric used by the QEMU samples and tests.
