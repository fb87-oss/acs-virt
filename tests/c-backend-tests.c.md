# c-backend-tests.c

Unit tests for deterministic C backend behavior using the `ctest.h`
single-header test framework fetched by CMake.

Coverage includes:

- little-endian helpers from `blkd.h` and `cond.h`
- block image read/write/bounds handling in `blkd-block.c`
- console output file writes in `cond-console.c`
- virtio-mmio reset behavior in `blkd-virtio.c` and `cond-virtio.c`

The tests intentionally avoid launching QEMU or requiring backend sockets. The
integration smoke tests cover full guest/backend behavior separately.
