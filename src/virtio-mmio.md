# virtio-mmio.c

`virtio-mmio.c` implements the virtio-mmio transport register model expected by
Linux's upstream `virtio_mmio` driver.

This file adapts MMIO register reads and writes onto the transport-independent
`struct virtio_device` state from `virtio.h`. It is not a block or console
driver, and it does not own the QEMU socket. The current socket transport is
provided by `src/fabrics/axi.c`; this file only sees offsets, access sizes,
and register values.

Main responsibilities:

- expose the virtio magic value, version 2, device id, and vendor id
- expose device features using the selected 32-bit feature bank
- store driver feature selections into the 64-bit `driver_features` field
- track `queue_sel` and return per-queue state for the selected queue
- store queue size, readiness, descriptor table, available ring, and used ring addresses
- expose and update virtio status and interrupt status registers
- dispatch device-specific config-space reads through `virtio_device_ops.get_config`
- reset transport and device state when the guest writes zero to `STATUS`
- mask read values to the requested MMIO access width

The file intentionally leaves `QUEUE_NOTIFY` side effects to the binding layer.
For the current daemons, `virtio-blkd.c` and `virtio-consoled.c` call
`virtio_mmio_write`, then trigger the device-specific queue callback when the
guest writes `VIRTIO_MMIO_QUEUE_NOTIFY`. This keeps notification dispatch close
to the fabric I/O object needed for DMA and IRQ operations.

Interrupt acknowledgement is split the same way. `virtio_mmio_write` clears bits
from `interrupt_status`; the driver/fabric binding lowers the external interrupt
line after `INTERRUPT_ACK` or full device reset.
