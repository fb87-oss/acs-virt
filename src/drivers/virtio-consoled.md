# virtio-consoled.c

`virtio-consoled.c` implements the host-side virtio-console daemon.

It combines three layers that are deployed as one daemon binary:

- console output backend operations
- transport-independent virtio-console device semantics
- the selected backend fabric plus `virtio-mmio` binding used by QEMU/UIO

Console backend responsibilities:

- open the configured output path in append mode
- use stdout when the output path is empty or `-`
- write all bytes from guest transmit buffers, retrying interrupted writes
- close non-stdout output descriptors on daemon shutdown

Virtio-console responsibilities:

- advertise `VIRTIO_CONSOLE_F_SIZE` and `VIRTIO_F_VERSION_1`
- expose a fixed 80x25 console size through the virtio config space callback
- initialize two split virtqueues: queue 0 for guest RX and queue 1 for guest TX
- leave queue 0 buffers pending because host input is not implemented yet
- consume queue 1 descriptor chains written by the guest
- ignore descriptors marked device-writeable in TX chains
- copy guest TX data through `fabric_dma_read` and write it to the host output backend
- publish used-ring entries and raise a virtqueue interrupt after consuming TX buffers

Fabric and MMIO binding responsibilities:

- create a `struct virtio_mmio` transport over the `struct virtio_device`
- register a `struct fabric_device` with a 0x200-byte MMIO aperture
- route fabric MMIO reads and writes through `virtio_mmio_read` and
  `virtio_mmio_write`
- lower the external interrupt line on device reset and interrupt acknowledgement
- dispatch `VIRTIO_MMIO_QUEUE_NOTIFY` to the virtio-console queue handler

Daemon responsibilities:

- parse launcher-provided `key=value` arguments
- require `socket`
- accept optional `output` and `ram_access` values
- start the selected fabric loop for this one device

Important boundaries:

- QEMU or the selected Linux fabric exposes guest RAM; this daemon accesses it
  only through `fabric_dma_*`
- `virtio-mmio.c` owns register-level transport state; this file only binds notify and IRQ side effects
- `virtio.c` owns generic split-ring helpers; this file owns console-specific queue behavior
- Linux guest drivers remain unchanged and see a normal virtio-mmio console device
