# devmem.c

`devmem.c` is the selectable backend fabric implementation for a Linux
`/dev/mem` physical-memory-mapped deployment.

It implements the common `fabric.h` entry points so the backend drivers and
virtio core do not depend on a concrete transport. The fabric maps a physical
virtio-mmio register aperture through `/dev/mem`, polls guest-written registers,
dispatches MMIO writes to the driver callback, refreshes guest-visible read
registers from the driver callback, and maps guest physical memory on demand for
DMA reads and writes.

Runtime configuration is supplied through environment variables or a compact
endpoint string in the backend `socket` argument.

Environment variables:

- `CHIPLETS_DEVMEM_PATH`: character device to open, default `/dev/mem`
- `CHIPLETS_DEVMEM_MMIO_BASE`: required physical base of the virtio-mmio aperture
- `CHIPLETS_DEVMEM_MMIO_SIZE`: optional aperture size, default from the driver
- `CHIPLETS_DEVMEM_POLL_US`: optional poll interval, default `1000`
- `CHIPLETS_DEVMEM_IRQ_ADDR`: optional physical IRQ control register address
- `CHIPLETS_DEVMEM_IRQ_ASSERT`: optional IRQ assert value, default `1`
- `CHIPLETS_DEVMEM_IRQ_DEASSERT`: optional IRQ deassert value, default `0`

Endpoint form:

```text
socket=devmem:<mmio-base>[:<mmio-size>[:<poll-us>]]
```

The endpoint form is useful for manually starting a daemon without setting
environment variables. It intentionally avoids commas and equals signs so it fits
the existing single-argument daemon parser.

Limitations:

- The MMIO aperture must behave like shared memory from both Linux and the daemon.
- Repeated guest writes of the same value can only be detected for `QUEUE_NOTIFY`,
  which the daemon rewrites to an idle sentinel after processing.
- IRQ delivery requires a platform-specific control register via
  `CHIPLETS_DEVMEM_IRQ_ADDR`; without it, interrupt helpers are no-ops.
