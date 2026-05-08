# cond.c

`cond.c` is the main process and config-loading entry point for the C
virtio-console daemon named `cond`.

It mirrors the split layout used by `blkd`, but models a virtio-console device
instead of virtio-blk.

## Split Files

```text
cond.c           process entry, TOML config parsing, socket listener setup
cond-axi.c       axi-bus socket protocol and QEMU-mediated DMA helpers
cond-virtio.c    virtio-mmio register model and virtio-console queues
cond-console.c   host-side console output backend
cond.h           shared structs, constants, and declarations
```

## Config

`cond` reads:

```text
configs/backends/axi-console.toml
```

The current config shape is:

```toml
[console]
output = "run/cond.out"

[transport.qemu_mmio]
socket = "run/axi-console.sock"
ram_access = "qemu-mediated"
```

TOML parsing uses `fastoml`, fetched by CMake.

## Runtime Role

`cond.c` opens the console output backend, creates the Unix socket, accepts QEMU
connections, creates one `cond_virtio_device` per connection, and dispatches the
connection to `cond_axi_serve()`.
