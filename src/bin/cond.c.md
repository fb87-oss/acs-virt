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

The launcher starts `cond` with one comma-separated `key=value` argument:

```text
out/cond name=con0,socket=run/axi-console.sock,output=run/cond.out,ram_access=qemu-mediated
```

The launcher derives those values from this config shape:

```toml
[[devices]]
name = "con0"
type = "virtio-console"

[[targets.qemu.devices]]
name = "con0"
socket = "run/axi-console.sock"
output = "run/cond.out"
```

`cond.c` does not parse TOML; config joining is owned by the Python launcher.

## Runtime Role

`cond.c` opens the console output backend, creates the Unix socket, accepts QEMU
connections, creates one `cond_virtio_device` per connection, and dispatches the
connection to `cond_axi_serve()`.
