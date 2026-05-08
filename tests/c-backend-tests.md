# c-backend-tests.c

Unit tests for deterministic C backend behavior using the `ctest.h`
single-header test framework fetched by CMake.

Coverage includes:

- little-endian helpers from `virtio-blkd.h` and `virtio-consoled.h`
- block image read/write/bounds handling in `src/drivers/virtio-blkd.c`
- console output file writes in `src/drivers/virtio-consoled.c`
- virtio-mmio reset behavior for both merged daemon implementations

The tests intentionally avoid launching QEMU or requiring backend sockets. The
integration smoke tests cover full guest/backend behavior separately.
