# blkd-virtio.c

`blkd-virtio.c` implements the C backend's minimal virtio-mmio and virtio-blk
device model.

It receives MMIO register reads/writes from `blkd-axi.c` and uses the DMA helper
functions in `blkd-axi.c` to read and write guest memory through QEMU.

## Responsibilities

- expose the virtio-mmio magic/version/device/vendor registers
- store negotiated feature words and device status
- store virtqueue addresses and queue readiness
- process queue `0` notifications
- read descriptor chains from guest RAM
- parse simple virtio-blk requests
- complete `VIRTIO_BLK_T_IN`, `VIRTIO_BLK_T_OUT`, and `VIRTIO_BLK_T_FLUSH`
- write used-ring entries and status bytes back to guest RAM
- request an interrupt after completing used descriptors

## Boundary

This file does not read from or write to the Unix socket directly except through
the `blkd_axi_*` helpers. It does not perform raw file I/O directly; block image
operations go through `blkd-block.c`.
