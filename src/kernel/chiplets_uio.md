# chiplets_uio.c

`chiplets_uio.c` is the small x86_64 backend-guest platform driver used by the
UIO smoke and benchmark topology.

The ARM64 UIO topology uses the kernel `uio_pdrv_genirq` driver with QEMU-created
`chiplets,uio` FDT nodes. The x86_64 microvm topology uses this custom module to
create stable UIO resources for the same backend daemon code.

Resources exposed per device:

- UIO memory region 0, named `mmio`: virtio-mmio and IRQ-control window.
- UIO memory region 1, named `frontend-ram`: frontend RAM DMA aperture.
- One interrupt line, defaulting to IRQ base `16 + index` when firmware does not
  provide an IRQ resource.

Default fallback layout:

```text
MMIO base: 0x0010_feb0_0000
MMIO size: 0x1000 per device
DMA base:  0x0010_0000_0000
DMA size:  0x20000000
IRQ base:  16
```

Device index selection:

- Prefer the first memory resource address when firmware supplies one.
- Fall back to the ACPI/platform device name for the second device.
- Default to index 0 otherwise.

Interrupt handling:

- The UIO interrupt handler disables the IRQ with `disable_irq_nosync` and marks
  it disabled.
- The UIO `irqcontrol` callback re-enables or disables the IRQ under a spinlock.
- This matches Linux UIO's userspace pattern where the daemon consumes an event
  with `read(/dev/uioX)` and then writes to the UIO fd to re-enable delivery.

The driver matches `PRP0001` ACPI devices and `chiplets,uio` device-tree nodes so
the same source can cover patched microvm-style platform devices and generated FDT
nodes when needed.
