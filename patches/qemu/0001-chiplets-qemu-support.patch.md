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

The x86_64 minimal device config (`microvm-minimal.mak`) enables only:

```text
CONFIG_MICROVM=y
CONFIG_AXI=y
```

The AArch64 minimal device config (`virt-minimal.mak`) enables only:

```text
CONFIG_ARM_VIRT=y
CONFIG_AXI=y
```

Both support `--without-default-devices` while still compiling their respective
machine and the custom `axi` sysbus device.

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
and allows dynamic `axi` sysbus devices on `microvm`. The `axi` device's `realize`
function enforces this range on x86 — it returns an error if the requested `irq`
falls outside `[16, 23]`. On ARM `virt`, the GIC SPI range is enforced with an
upper bound of 256.

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
notify-policy=<all|barrier>
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

In UIO mode, the frontend `axi` device implements a queue-sel clearing behavior
on the slave side: when the frontend guest writes to `VIRTIO_MMIO_QUEUE_SEL`, the
device zeroes eight queue-related registers (`QUEUE_NUM`, `QUEUE_READY`,
`QUEUE_DESC_LOW/HIGH`, `QUEUE_AVAIL_LOW/HIGH`, `QUEUE_USED_LOW/HIGH`). This
prevents stale queue configuration data from persisting when the guest switches
queue index. The `notify-policy` property (`all` vs `barrier`) controls which MMIO
writes trigger a backend notification: `all` notifies on every write to the shared
MMIO window, while `barrier` notifies only on specific control writes
(features, queue ready, queue notify, status, interrupt ack, etc.).

## Firmware Discovery

### x86_64 ACPI

For x86_64 `microvm`, the patch generates three ACPI device forms depending on
the `axi` device configuration:

**Frontend without DMA aperture** (socket mode or UIO without `dma-memdev`):

```text
_HID = "LNRO0005"         (Linux virtio-mmio ACPI identifier)
_UID = <gsi>
_CRS = Memory32Fixed(<base>, <size>) + Interrupt(edge, active-high, <gsi>)
```

Linux binds its built-in `virtio-mmio` ACPI driver to `LNRO0005` entries.

**Frontend with DMA aperture** (UIO slave with `dma-memdev` and `dma-size`):

```text
_HID = "AXI0001"
_UID = <gsi>
_CRS = Memory32Fixed(<base>, <size>)
       Memory32Fixed(<dma-base>, <dma-size>)
       Interrupt(edge, active-high, <gsi>)
```

Linux binds the `axi_mmio` driver to `AXI0001`, which creates a standard
`virtio-mmio` child device with restricted DMA operations.

**Backend UIO** (UIO master/`mode=uio,role=master`):

```text
_HID = "PRP0001"
_UID = <gsi>
_CRS = Memory32Fixed(<base>, <size>)
       [Memory32Fixed(<dma-base>, <dma-size>)]
       Interrupt(edge, active-high, <gsi>)
_DSD = UUID {DAFFD814-6EBA-4D8C-8A91-BC9BBF4AA301}
       Package { "compatible", "generic-uio" }
```

The `PRP0001` HID with `generic-uio` compatible lets Linux bind
`uio_pdrv_genirq` or a similar UIO driver to expose MMIO registers and
interrupts to userspace backend daemons through `/dev/uioX`.

### AArch64 FDT

For AArch64 `virt`, the patch creates FDT nodes:

- `virtio,mmio` for slave/frontend discovery when no DMA aperture is configured.
- `axi,mmio` for slave/frontend discovery when `dma-size` is present (two
  `reg` cells: `[base, size]` and `[dma-base, dma-size]`).
- `chiplets,uio` for master/backend UIO discovery when `mode=uio,role=master`.

All device FDT nodes include the `dma-coherent` property to indicate that the
MMIO and shared-memory regions are cache-coherent from the guest's perspective.

## Data Paths

The `axi` device uses fixed-header messages over two types of Unix sockets.

### Socket Mode Messages

All operations (MMIO, DMA, IRQ) travel over a single socket between the frontend
QEMU and a host backend daemon:

```text
AXI_MSG_HELLO (1)             Backend-to-QEMU initial handshake
AXI_MSG_MEM_REGION (2)        Backend-to-QEMU memory region registration
AXI_MSG_MMIO_READ (3)         QEMU-to-backend MMIO read request
AXI_MSG_MMIO_READ_REPLY (4)   Backend-to-QEMU MMIO read response
AXI_MSG_MMIO_WRITE (5)        QEMU-to-backend MMIO write request
AXI_MSG_IRQ_ASSERT (6)        Backend-to-QEMU assert IRQ line
AXI_MSG_IRQ_DEASSERT (7)      Backend-to-QEMU deassert IRQ line
AXI_MSG_DMA_READ (8)          Backend-to-QEMU DMA read from guest memory
AXI_MSG_DMA_READ_REPLY (9)    QEMU-to-backend DMA read response
AXI_MSG_DMA_WRITE (10)        Backend-to-QEMU DMA write to guest memory
AXI_MSG_ERROR (0xFFFF)        Error response
```

### UIO Mode Control Socket Messages

Frontend and backend QEMU processes communicate over a per-device Unix control
socket. Payload DMA does not use socket messages — backend daemons map the
shared frontend RAM through Linux UIO `map1`:

```text
AXI_MSG_FRONTEND_NOTIFY (100)       Frontend QEMU → backend QEMU: guest wrote a
                                     control register that requires service-side
                                     processing (queue notify, status change, etc.)
AXI_MSG_FRONTEND_IRQ_ASSERT (101)   Backend QEMU → frontend QEMU: backend daemon
                                     requests guest IRQ assertion
AXI_MSG_FRONTEND_IRQ_DEASSERT (102) Backend QEMU → frontend QEMU: request IRQ
                                     deassertion
AXI_MSG_FRONTEND_NOTIFY_ACK (103)   Backend QEMU → frontend QEMU: acknowledge
                                     receipt of a FRONTEND_NOTIFY (used when
                                     notify-ack=on)
```

The `notify-ack` property controls whether the frontend waits for an explicit
acknowledgement from the backend after each notify, or uses a fixed delay
(`notify-delay-us`) before allowing the next guest MMIO write to proceed.

Related documentation:

- `docs/axi-protocol.md`
- `docs/uio-fabric.md`
- `docs/project-state-and-design.md`
