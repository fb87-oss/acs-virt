# fabric.h

`fabric.h` defines the stable backend fabric API used by the virtio core and
driver daemons.

The API covers device registration, MMIO callback dispatch, guest-memory DMA
helpers, and interrupt helpers. Backend drivers include this header instead of a
specific fabric implementation, so switching between `axi`, `devmem`, and
other future fabrics is a CMake link-time choice.

Current implementations:

- `axi.c`: active QEMU socket fabric used by the existing samples and tests
- `devmem.c`: Linux `/dev/mem` fabric for physical virtio-mmio apertures
