# AXI Bus Protocol

This protocol connects QEMU's `axi-bus` device to an external device
model. QEMU does not implement virtio devices; it only forwards MMIO accesses,
RAM access requests, and IRQ events.

## Modes

`shared-mem` is the fast path. QEMU sends guest memory region file descriptors
to the backend, and the backend walks queues directly.

`qemu-mediated` is the debug path. The backend requests guest memory reads and
writes through QEMU using `DMA_READ` and `DMA_WRITE` messages.

## Messages

All messages begin with a fixed header:

```text
u16 kind
u16 flags
u32 window_id
u64 offset
u32 length
```

Initial message kinds:

```text
HELLO           = 1
MEM_REGION      = 2
MMIO_READ       = 3
MMIO_READ_REPLY = 4
MMIO_WRITE      = 5
IRQ_ASSERT      = 6
IRQ_DEASSERT    = 7
DMA_READ        = 8
DMA_READ_REPLY  = 9
DMA_WRITE       = 10
ERROR           = 0xffff
```

`MEM_REGION` carries a guest RAM fd with `SCM_RIGHTS` in `shared-mem` mode.

`DMA_READ` and `DMA_WRITE` are only valid in `qemu-mediated` mode.

`IRQ_ASSERT` raises the configured interrupt line. `IRQ_DEASSERT` lowers it when
the guest acknowledges the virtio-mmio interrupt. This matches the level-triggered
microvm ACPI export for `axi-bus` devices.

## Boundary

The backend owns endpoint semantics such as `virtio-mmio` registers, feature
negotiation, virtqueues, block I/O, network packets, and console data. QEMU owns
only MMIO trapping, guest RAM exposure or mediation, and IRQ injection.
