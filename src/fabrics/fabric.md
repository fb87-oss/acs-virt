# fabric.h

`fabric.h` defines the stable backend fabric API used by the virtio core and
driver daemons.

The API covers device registration, MMIO callback dispatch, guest-memory DMA
helpers, optional direct DMA mapping, and interrupt helpers. Backend drivers
include this header instead of a specific fabric implementation, so switching
between `qemu-socket`, `linux-devmem`, `linux-uio`, and other future fabrics is
a CMake link-time choice.

Current implementations:

- `qemu_socket.c`: backend fabric for QEMU `axi mode=socket`; its Unix socket
  carries control, DMA/data, and IRQ messages.
- `linux_devmem.c`: backend fabric for Linux `/dev/mem` physical mappings.
- `linux_uio.c`: backend fabric for QEMU `axi mode=uio`; its Unix socket is only
  the QEMU-to-QEMU control path, while backend MMIO/data access uses Linux UIO.

DMA helpers:

- `fabric_dma_read` allocates and returns copied guest memory.
- `fabric_dma_read_into` copies guest memory into a caller-provided buffer.
- `fabric_dma_write` copies caller data into guest memory.
- `fabric_dma_map` optionally returns a direct pointer to guest memory when the
  selected fabric can expose one. `linux_uio.c` supports this for its mapped
  frontend RAM aperture; `qemu_socket.c` and `linux_devmem.c` currently return
  false and rely on the copy helpers.
- `fabric_dma_unmap` releases a pointer returned by `fabric_dma_map`; it is a
  no-op for the current UIO mapping because the fabric owns the long-lived mmap.
