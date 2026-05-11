# 0001-chiplets-qemu-support.patch

This patch carries the project-specific QEMU support needed by both active
topologies:

- `axi-socket`: one frontend VM with QEMU forwarding MMIO, DMA, and IRQs to a
  host backend daemon over a Unix socket.
- `axi-linux-uio`: frontend and backend VMs connected by shared MMIO/RAM files,
  QEMU-to-QEMU control sockets, and Linux UIO resources in the backend guest.

It intentionally combines the previous small QEMU patch stack into one ordered
patch. Keeping the QEMU integration in one file makes the applied source easier
to audit and avoids cross-patch dependencies between the custom `axi` device,
microvm configuration, and ACPI/FDT exposure.

## Changed Areas

The patch creates or modifies:

```text
configs/devices/x86_64-softmmu/microvm-minimal.mak
configs/devices/aarch64-softmmu/virt-minimal.mak
hw/arm/virt.c
hw/core/sysbus-fdt.c
hw/cxl/cxl-host-stubs.c
hw/i386/acpi-microvm.c
hw/i386/microvm.c
hw/misc/Kconfig
hw/misc/axi.c
hw/misc/meson.build
include/hw/i386/microvm.h
```

## Minimal Device Configs

The x86_64 minimal device config enables only:

```text
CONFIG_MICROVM=y
CONFIG_AXI=y
```

This supports `--without-default-devices` while still compiling the `microvm`
machine and custom `axi` sysbus device.

## Microvm Integration

The patch adds the `microvm` machine property:

```text
virtio-mmio-transports=<uint32>
```

The launcher uses it as:

```text
-machine microvm,pcie=off,ioapic2=on,virtio-mmio-transports=0,memory-backend=guestmem
```

This disables QEMU's built-in virtio-mmio transport slots for the active path,
where the custom `axi` device exposes the MMIO windows and external backends own
virtio device behavior.

The patch also reserves primary IO-APIC GSIs `16..23` for project `axi` devices
and allows dynamic `axi` sysbus devices on `microvm`.

## `axi` Device

The custom QEMU `axi` sysbus device accepts these key properties:

```text
base=<guest physical MMIO base>
size=<MMIO window size>
irq=<x86 GSI number or ARM GIC SPI number>
mode=<socket|uio>
role=<slave|master>
socket=<Unix socket path>
control-socket=<UIO control socket path>
ram-access=<shared-mem|qemu-mediated>
target=<backend target name>
memdev=<MMIO memory-backend object>
dma-memdev=<frontend RAM memory-backend object>
dma-base=<backend guest physical base for the frontend RAM aperture>
dma-size=<frontend RAM size>
notify-delay-us=<frontend notify delay>
notify-ack=<on|off>
```

Socket-mode example:

```text
-device axi,id=blk0,base=0xfeb00000,size=0x200,irq=16,
  socket=/.../run/axi.sock,ram-access=qemu-mediated,target=blk0
```

UIO-mode paired example:

```text
-device axi,id=blk0,mode=uio,role=slave,base=0x20feb00000,size=0x1000,
  irq=16,memdev=blkmmio,control-socket=/tmp/.../blk.control.sock,
  notify-delay-us=13000,notify-ack=on
-device axi,id=blk0,mode=uio,role=master,base=0x10feb00000,size=0x1000,
  irq=16,memdev=blkmmio,control-socket=/tmp/.../blk.control.sock,
  dma-memdev=frontendram,dma-base=0x1000000000,
  dma-size=536870912
```

The device is a transport/proxy layer only. It does not implement virtio block,
console, network, or endpoint semantics. Backend daemons own virtio-mmio register
behavior, virtqueue parsing, block image access, and interrupt-status state.

## Firmware Discovery

For x86_64 `microvm`, the patch exports frontend `axi` virtio windows through
ACPI using Linux's virtio-mmio ACPI identifier:

```text
_HID = "LNRO0005"
_UID = <gsi>
_CRS = Memory32Fixed(<base>, <size>) + Interrupt(edge, active-high, <gsi>)
```

For AArch64 `virt`, the patch creates FDT nodes:

- `virtio,mmio` for slave/frontend discovery.
- `chiplets,uio` for master/backend UIO discovery when `mode=uio`.

## Data Paths

Socket mode uses fixed-header messages for MMIO, IRQ, and QEMU-mediated DMA:

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

UIO mode uses control-socket messages for frontend notifications, frontend IRQ
assert/deassert, and optional notify acknowledgement. Payload DMA does not use
socket messages in UIO mode; the backend daemon maps frontend RAM through Linux
UIO `map1`.

Related documentation:

- `docs/axi-protocol.md`
- `docs/uio-fabric.md`
- `docs/project-state-and-design.md`
