# cond-console.c

`cond-console.c` is the host-side output backend for the C virtio-console daemon.

It receives bytes from `cond-virtio.c` after the guest writes to the
virtio-console transmit queue.

## Responsibilities

- open the configured output path
- use stdout when `output = "-"` or no output path is configured
- append guest-transmitted bytes to the output file
- close the output file during shutdown

## Boundary

This file does not understand virtio descriptors, MMIO registers, or the
`axi-bus` socket protocol. It is only the host console sink.
