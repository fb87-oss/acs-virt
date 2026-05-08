# cond-axi.c

`cond-axi.c` implements the QEMU-facing `axi-bus` socket transport for `cond`.

It owns the protocol framing and DMA helper functions. It does not implement
virtio-console semantics directly.

## Responsibilities

- read and write fixed-size `axi-bus` protocol headers
- dispatch `MMIO_READ` to the virtio-console model
- dispatch `MMIO_WRITE` to the virtio-console model
- provide QEMU-mediated `DMA_READ` and `DMA_WRITE` helpers
- send `IRQ_ASSERT` when the virtio-console model completes used buffers
- send `IRQ_DEASSERT` when the guest acknowledges the virtio-mmio interrupt

## Boundary

Virtio registers and queue processing live in `cond-virtio.c`. Host-side console
output lives in `cond-console.c`.
