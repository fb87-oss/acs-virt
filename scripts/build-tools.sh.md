# build-tools.sh

Configures the CMake build and builds the local C tools:

- `out/blkd`
- `out/cond`
- `out/qemu-launch`
- `out/c-backend-tests`

The script enters a Nix shell with CMake, GCC, and Git when needed. QEMU source
fetching is disabled for this target set so normal backend builds stay fast.
