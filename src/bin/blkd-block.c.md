# blkd-block.c

`blkd-block.c` implements the C backend's block image access layer.

It is intentionally small: the file exposes open, close, read, write, and flush
helpers for the disk image configured in `configs/backends/axi-bus.toml`.

## Responsibilities

- open the configured block image read-only or read-write
- record image length for virtio capacity reporting
- perform complete `pread` loops for block reads
- perform complete `pwrite` loops for block writes
- reject writes when the config is read-only
- reject reads or writes beyond the image size
- flush image state with `fsync`

## Boundary

This file does not know about virtio descriptors, MMIO registers, or the `axi-bus`
socket protocol. The virtio device model calls it through the `blkd_block_*`
functions declared in `blkd.h`.
