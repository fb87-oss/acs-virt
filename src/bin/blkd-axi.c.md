# blkd-axi.c

`blkd-axi.c` implements the C backend's QEMU-facing `axi-bus` socket transport.

It owns the wire protocol between QEMU's `axi-bus` device and the C backend. It
does not know virtio-blk request semantics or block image layout.

## Responsibilities

- read and write fixed-size `axi-bus` protocol headers
- handle `MMIO_READ` requests by calling the virtio-mmio device model
- handle `MMIO_WRITE` requests by updating the virtio-mmio device model
- dispatch queue notifications to the virtio layer
- provide QEMU-mediated DMA helpers to the virtio layer
- send `IRQ_ASSERT` messages when the virtio layer requests an interrupt
- send `IRQ_DEASSERT` when the guest acknowledges the virtio-mmio interrupt

## Boundary

The transport file only speaks the socket protocol. Virtio register behavior and
virtqueue processing live in `blkd-virtio.c`. Disk image I/O lives in
`blkd-block.c`.
