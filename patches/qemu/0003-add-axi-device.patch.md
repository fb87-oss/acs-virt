# 0003-add-axi-device.patch

This patch adds the custom QEMU `axi` sysbus device used by both the
`axi-socket` topology and the `axi-linux-uio` topology.

It modifies:

```text
configs/devices/aarch64-softmmu/virt-minimal.mak
hw/arm/virt.c
hw/core/sysbus-fdt.c
hw/cxl/cxl-host-stubs.c
hw/misc/Kconfig
hw/misc/meson.build
```

It creates:

```text
hw/misc/axi.c
```

## Purpose

`axi` is the QEMU-side anchor for external backend paths. In socket mode it
exposes a configurable MMIO window to the guest and forwards guest MMIO accesses
to a userspace backend over a Unix socket. In UIO mode paired frontend/backend
QEMU processes share MMIO/RAM files and coordinate notifications over a control
socket while the backend daemon uses Linux UIO devices.

It intentionally does not implement virtio, block, network, console, or endpoint
semantics. QEMU is only the frontend transport/proxy layer.

## Runtime Properties

The device accepts these properties:

```text
base=<guest physical MMIO base>
size=<MMIO window size>
irq=<x86 GSI number or ARM GIC SPI number>
mode=<socket|uio>
role=<frontend|backend>
socket=<Unix socket path>
control-socket=<UIO control socket path>
ram-access=<shared-mem|qemu-mediated>
target=<backend target name>
memdev=<MMIO memory-backend object>
dma-memdev=<frontend RAM memory-backend object>
dma-base=<frontend RAM base visible to the backend>
dma-size=<frontend RAM size>
virtio-node=<on|off>
notify-delay-us=<frontend notify delay>
notify-ack=<on|off>
```

The TOML launcher emits socket-mode devices like:

```text
-device axi,id=blk0,base=0xfeb00000,size=0x200,irq=16,
  socket=/.../run/axi.sock,ram-access=qemu-mediated,target=blk0
```

The UIO orchestrator emits paired devices like:

```text
-device axi,id=blk0,mode=uio,role=frontend,base=0x10feb00000,size=0x1000,
  irq=16,memdev=blkmmio,control-socket=/tmp/.../blk.control.sock,
  virtio-node=on,notify-delay-us=50000,notify-ack=on
-device axi,id=blk0,mode=uio,role=backend,base=0xfeb00000,size=0x1000,
  irq=16,memdev=blkmmio,control-socket=/tmp/.../blk.control.sock,
  virtio-node=off,dma-memdev=frontendram,dma-base=0x30000000,
  dma-size=536870912
```

For `microvm`, `irq` must be inside the project-reserved `axi` range,
`16..23`, and the machine must have `ioapic2=on`.

For AArch64 `virt`, the device maps the MMIO window, wires the configured GIC
SPI, and adds either a `virtio,mmio` FDT node for frontend discovery or a
`chiplets,uio` FDT node for backend UIO discovery.

## Protocol

The device forwards fixed-header messages between QEMU and the backend:

```text
MMIO_READ
MMIO_READ_REPLY
MMIO_WRITE
IRQ_ASSERT
IRQ_DEASSERT
DMA_READ
DMA_READ_REPLY
DMA_WRITE
ERROR
```

UIO frontend/backend control-socket messages include frontend notify, frontend
IRQ assert/deassert, and notify acknowledgement messages. They are internal to
the QEMU `axi` UIO pairing and are documented in `docs/uio-fabric.md`.

The protocol is documented in:

```text
docs/axi-protocol.md
```

## QEMU Responsibilities

The device owns:

- registering the guest MMIO window
- forwarding MMIO reads and writes over the socket
- mediating guest RAM reads and writes in `qemu-mediated` mode
- mapping shared MMIO and DMA memory backends in UIO mode
- forwarding UIO frontend/backend notification and IRQ control messages
- asserting the configured x86 GSI or ARM SPI when the backend sends `IRQ_ASSERT`
- deasserting the configured x86 GSI or ARM SPI when the backend sends `IRQ_DEASSERT`

## Backend Responsibilities

The backend owns:

- virtio-mmio register behavior
- virtqueue parsing
- block image reads and writes
- interrupt status state
- deciding when to ask QEMU for DMA reads/writes in socket mode or when to use
  direct UIO DMA mappings in UIO mode

## Current Data Paths

The TOML sample configs use socket mode with `qemu-mediated` RAM access. In this
mode, the backend does not directly map guest RAM. Instead, it asks QEMU to
perform guest memory accesses with `DMA_READ` and `DMA_WRITE` protocol messages.

The UIO smoke and benchmark wrappers use `mode=uio`, shared host-backed MMIO/RAM
files, and Linux UIO resources in the backend guest. That path does not use the
socket DMA messages.
