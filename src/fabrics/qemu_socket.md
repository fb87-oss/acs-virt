# qemu_socket.c

`qemu_socket.c` owns the backend side of the QEMU `axi mode=socket` protocol.

QEMU provides an `axi` SysBus device that exposes a guest MMIO window and
forwards accesses to an external backend over a Unix socket. This file is the C
backend fabric for that full-control/full-data socket protocol. It is not a real
hardware AXI driver and it does not interpret virtio registers.

Main responsibilities:

- create parent directories for configured socket paths
- bind and listen on one Unix socket for the registered backend device
- accept QEMU connections and serve MMIO requests until disconnect
- decode and encode the fixed 24-byte little-endian protocol header
- validate that MMIO accesses target window 0 and stay inside the registered device range
- translate absolute guest bus addresses into device-relative offsets
- dispatch MMIO reads and writes to `struct fabric_device_ops`
- provide fabric DMA helpers that request guest memory reads and writes through QEMU
- reject direct DMA mapping requests because socket-mode RAM access is mediated
  by protocol messages rather than a local long-lived mapping
- provide interrupt helpers that ask QEMU to assert or deassert the configured IRQ line
- force a low-to-high IRQ transition in `fabric_raise_irq` by lowering before asserting

The current implementation requires exactly one registered device per daemon.
That matches today's process model where `virtio-blkd` and `virtio-consoled` run
as separate daemons with separate sockets and IRQs. The `struct fabric` shape
still keeps a small device array so a future combined daemon can register more
than one device without changing the driver-facing API.

Protocol boundaries:

- MMIO messages are synchronous request/reply operations initiated by QEMU
- DMA messages are synchronous backend-initiated requests serviced by QEMU
- `fabric_dma_map` is unsupported and returns false, so drivers fall back to
  `fabric_dma_read_into`/`fabric_dma_write`
- IRQ messages are backend-initiated notifications with no payload
- `AXI_MSG_ERROR` is used as the zero-length acknowledgement for successful MMIO writes and DMA writes

What this file deliberately does not own:

- virtio feature negotiation, status bits, or queue register semantics
- virtqueue descriptor-chain interpretation
- block image I/O or console output I/O
- Linux guest discovery through ACPI or FDT
- QEMU-side memory-region or IRQ wiring

Those responsibilities live in the driver files, `virtio-mmio.c`, and the QEMU
patches respectively.
