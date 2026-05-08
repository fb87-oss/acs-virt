# 0002-add-microvm-virtio-mmio-transport-count.patch

This patch changes QEMU's `microvm` machine so the number of built-in
`virtio-mmio` transport slots is configurable.

It modifies:

```text
hw/i386/microvm.c
include/hw/i386/microvm.h
```

## Added Machine Property

The patch adds this `microvm` machine property:

```text
virtio-mmio-transports=<uint32>
```

The launcher uses it as:

```text
-machine microvm,pcie=off,ioapic2=on,virtio-mmio-transports=0,memory-backend=guestmem
```

## Purpose

Stock `microvm` creates built-in QEMU `virtio-mmio` transport slots. Those slots
are useful for normal QEMU-owned virtio devices, but they are not the path used
here.

This project needs QEMU to expose only the custom `virt-axi` MMIO window and to
forward MMIO/DMA/IRQ activity to an external backend. The backend owns the
`virtio-mmio` register model and the virtio block device behavior.

Setting `virtio-mmio-transports=0` prevents QEMU from creating unused built-in
virtio-mmio transports.

## Default Behavior

The patch changes the default transport count to `0` in `microvm` initialization.

The property can still be set to a non-zero value if a debug run needs stock
QEMU virtio-mmio transports for comparison.

## Reserved virt-axi IRQ Range

The patch reserves primary IO-APIC GSIs `16..23` for project `virt-axi`
devices:

```text
MICROVM_VIRT_AXI_IRQ_BASE  = 16
MICROVM_VIRT_AXI_IRQ_COUNT = 8
```

When the second IO-APIC is active, built-in virtio-mmio transports start after
this range if the machine property requests any. The normal launcher sets
`virtio-mmio-transports=0`, so no built-in virtio-mmio windows are created for
the active path.

## virt-axi Allowlist

The patch also allows the custom sysbus device on `microvm`:

```text
machine_class_allow_dynamic_sysbus_dev(mc, "virt-axi")
```

Without this allowlist entry, QEMU would reject `-device virt-axi,...` on the
`microvm` machine.

## Runtime Relationship

This patch does not instantiate `virt-axi`. It only lets the launcher request it
and disables unrelated built-in transport slots.

The actual device still comes from:

```text
-device virt-axi,id=blk0,base=...,size=...,irq=16,socket=...
```
