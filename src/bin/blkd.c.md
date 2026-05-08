# blkd.c

`blkd.c` is the main process and config-loading entry point for the C AXI bus
virtio block backend.

It is named `blkd` because it is specifically a block-device daemon. The backend
is split into separate implementation files so transport, virtio state, and block
I/O stay isolated.

## Role

`blkd` connects to the same QEMU `axi-bus` frontend path:

```text
Linux virtio_blk
  -> Linux virtio_mmio
  -> QEMU axi-bus
  -> Unix socket protocol
  -> blkd
  -> run/blk0.img
```

QEMU still does not implement virtio-blk. The `blkd` binary owns the
virtio-mmio register model, virtqueue processing, request execution, and
interrupt-status state through the split source files.

## Split Files

```text
blkd.c          process entry, TOML config parsing, socket listener setup
blkd-axi.c      axi-bus socket protocol and QEMU-mediated DMA helpers
blkd-virtio.c   virtio-mmio register model and virtio-blk queue handling
blkd-block.c    block image open/read/write/flush helpers
blkd.h          shared structs, constants, and function declarations
```

## Config

The launcher starts the daemon with one comma-separated `key=value` argument:

```text
out/blkd name=blk0,socket=run/axi-bus.sock,image=run/blk0.img,readonly=false,ram_access=qemu-mediated
```

The launcher derives those values from this config shape:

```toml
[[devices]]
name = "blk0"
type = "virtio-blk"

[[targets.qemu.devices]]
name = "blk0"
socket = "run/axi-bus.sock"
image = "run/blk0.img"
readonly = false
```

`blkd.c` does not parse TOML; config joining is owned by the Python launcher.

## Protocol

`blkd` speaks the same fixed-header protocol documented in:

```text
docs/axi-bus-protocol.md
```

It handles QEMU messages:

```text
MMIO_READ
MMIO_WRITE
DMA_READ_REPLY
DMA_WRITE completion replies
```

It sends backend messages:

```text
MMIO_READ_REPLY
DMA_READ
DMA_WRITE
IRQ_ASSERT
ERROR acknowledgements
```

## Virtio-Blk Support

The daemon implements the minimal virtio-blk behavior used by the current guest
tests:

- virtio-mmio magic/version/device/vendor registers
- device feature selection
- driver feature storage
- queue setup registers
- interrupt status and acknowledge registers
- capacity and block-size config registers
- queue notify handling for queue `0`
- simple three-descriptor virtio-blk request chains
- `VIRTIO_BLK_T_IN`
- `VIRTIO_BLK_T_OUT`
- `VIRTIO_BLK_T_FLUSH`

## Limitations

`blkd` has intentional bring-up limitations:

- only the current `qemu-mediated` data path is active
- descriptor-chain handling is minimal and focused on the current Linux tests
- direct shared-memory queue walking is not implemented
- config parsing is deliberately narrow
