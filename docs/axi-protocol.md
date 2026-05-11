# QEMU axi Socket Backend Protocol

This protocol connects QEMU's `axi mode=socket` device to an external backend
daemon. QEMU does not implement virtio devices; it only forwards MMIO accesses,
RAM access requests, and IRQ events. The `axi-linux-uio` topology uses the same
QEMU `axi` device in `mode=uio`, but its Unix socket carries only QEMU-to-QEMU
control messages. Data and MMIO access are shared memory exposed to the backend
daemon through Linux UIO.

## Modes

`shared-mem` is accepted by QEMU's socket-mode property parser, but the current C
`axi` fabric still uses the mediated DMA message helpers.

`qemu-mediated` is the path used by the TOML samples. The backend requests guest
memory reads and writes through QEMU using `DMA_READ` and `DMA_WRITE` messages.

## Messages

All messages begin with a fixed header:

```text
u16 kind
u16 flags
u32 window_id
u64 offset
u32 length
```

Socket-mode message kinds:

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
microvm ACPI export for `axi` devices.

## Boundary

The backend owns endpoint semantics such as `virtio-mmio` registers, feature
negotiation, virtqueues, block I/O, network packets, and console data. QEMU owns
only MMIO trapping, guest RAM exposure or mediation, and IRQ injection.

## UIO Distinction

In UIO mode the QEMU `axi` devices are launched with properties such as
`mode=uio`, `role=slave|master`, `control-socket=...`, `memdev=...`, and
`dma-memdev=...`. Frontend and backend QEMU processes share host-backed files for
the virtio-mmio window and frontend RAM. The backend daemon talks to Linux UIO
resources instead of this socket protocol, so `DMA_READ` and `DMA_WRITE` messages
are not used on the UIO benchmark path.
