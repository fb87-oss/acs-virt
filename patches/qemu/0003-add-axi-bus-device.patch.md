# 0003-add-axi-bus-device.patch

This patch adds the custom QEMU `axi-bus` sysbus device.

It modifies:

```text
hw/misc/Kconfig
hw/misc/meson.build
```

It creates:

```text
hw/misc/axi-bus.c
```

## Purpose

`axi-bus` is the QEMU-side anchor for the external backend path. It exposes a
configurable MMIO window to the guest and forwards guest MMIO accesses to a
userspace backend over a Unix socket.

It intentionally does not implement virtio, block, network, console, or endpoint
semantics. QEMU is only the frontend transport/proxy layer.

## Runtime Properties

The device accepts these properties:

```text
base=<guest physical MMIO base>
size=<MMIO window size>
irq=<QEMU/x86 GSI number>
socket=<Unix socket path>
ram-access=<shared-mem|qemu-mediated>
target=<backend target name>
```

The current launcher emits a device like:

```text
-device axi-bus,id=blk0,base=0xfeb00000,size=0x200,irq=16,
  socket=/.../run/axi-bus.sock,ram-access=qemu-mediated,target=blk0
```

For `microvm`, `irq` must be inside the project-reserved `axi-bus` range,
`16..23`, and the machine must have `ioapic2=on`.

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

The protocol is documented in:

```text
docs/axi-bus-protocol.md
```

## QEMU Responsibilities

The device owns:

- registering the guest MMIO window
- forwarding MMIO reads and writes over the socket
- mediating guest RAM reads and writes in `qemu-mediated` mode
- asserting the configured x86 GSI when the backend sends `IRQ_ASSERT`
- deasserting the configured x86 GSI when the backend sends `IRQ_DEASSERT`

## Backend Responsibilities

The backend owns:

- virtio-mmio register behavior
- virtqueue parsing
- block image reads and writes
- interrupt status state
- deciding when to ask QEMU for DMA reads/writes

## Current Data Path

The current config uses `qemu-mediated` RAM access. In this mode, the backend
does not directly map guest RAM. Instead, it asks QEMU to perform guest memory
accesses with `DMA_READ` and `DMA_WRITE` protocol messages.

The planned `shared-mem` path is represented in the device property parsing, but
the current backend path still uses `qemu-mediated` for bring-up correctness.
