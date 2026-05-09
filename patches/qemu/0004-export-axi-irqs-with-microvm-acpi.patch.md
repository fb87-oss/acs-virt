# 0004-export-axi-irqs-with-microvm-acpi.patch

This patch exports frontend `axi` virtio windows through microvm ACPI so Linux
maps the project-reserved primary IO-APIC GSIs through normal ACPI IRQ routing.

It modifies:

```text
hw/i386/acpi-microvm.c
```

## Behavior

During DSDT construction, QEMU scans dynamic sysbus devices for `axi`
instances. It emits them in reserved GSI order, `16..23`, so `blk0` on GSI `16`
appears before `con0` on GSI `17`.

Each ACPI node uses:

```text
_HID = "LNRO0005"
_UID = <gsi>
_CRS = Memory32Fixed(<base>, <size>) + Interrupt(edge, active-high, <gsi>)
```

`LNRO0005` is Linux's virtio-mmio ACPI identifier. The MMIO window still maps to
the QEMU `axi` transport proxy; external backends continue to own virtio device
semantics. UIO backend devices are not exposed to the frontend as
`virtio,mmio`; they use the UIO resource path described in `docs/uio-fabric.md`.

## IRQ Range

The exported IRQs must come from the project-reserved microvm range:

```text
16..23
```

The current frontend uses `16` for `blkd` and `17` for `cond`.
