# axi-blk.toml

This frontend VM config enables only the block `axi-bus` MMIO window.

It is used to validate the block path independently before adding the console
window:

```text
blk0 base=0xfeb00000 irq=16 socket=run/axi-bus.sock
```

The IRQ is from the project-reserved microvm `axi-bus` range, `16..23`, and is
exported to Linux through patched microvm ACPI.
