# virtio.c

`virtio.c` contains the shared virtio core used by the C backend drivers.

This file is transport-independent: it does not know about virtio-mmio
registers, QEMU devices, sockets, block requests, console queues, or Linux guest
discovery. It owns only the common virtio state and ring mechanics needed by
the backend drivers.

Main responsibilities:

- initialize and reset `struct virtio_device` instances
- reset all queues to the negotiated default queue size
- clear driver features, status, interrupt status, and config generation
- call an optional device-specific reset hook
- read virtqueue descriptors from guest memory through a caller-provided DMA function
- append completed buffers to a queue's used ring
- read the next available-ring head without advancing `last_avail_idx`
- provide little-endian helpers through `virtio.h`

The DMA functions are injected by the fabric layer through callback pointers.
Today those callbacks are `fabric_dma_read`, `fabric_dma_read_u16`, and
`fabric_dma_write`, but this file only depends on the callback signatures from
`virtio.h`. That keeps the virtio queue code reusable across backend fabrics such
as `qemu-socket`, `linux-uio`, and `linux-devmem`.

Descriptor processing is deliberately minimal. `virtio_read_desc` loads the raw
16-byte split-ring descriptor. `virtio_next_avail` observes whether the driver
has posted new work and returns the head descriptor index. `virtio_add_used`
writes the used-ring element and increments the used index. Device drivers remain
responsible for validating descriptor direction, walking chains, advancing
`last_avail_idx`, setting interrupt status, and raising interrupts.

Device-specific behavior lives outside this file:

- `src/drivers/virtio-blkd.c` interprets virtio-blk request headers and disk I/O
- `src/drivers/virtio-consoled.c` interprets virtio-console TX/RX queues
- `src/virtio-mmio.c` maps Linux virtio-mmio registers to `struct virtio_device`
- `src/fabrics/qemu_socket.c` moves MMIO, DMA, and IRQ messages over the full
  QEMU `axi mode=socket` backend socket
- `src/fabrics/linux_uio.c` accesses QEMU `axi mode=uio` shared resources through
  Linux UIO
