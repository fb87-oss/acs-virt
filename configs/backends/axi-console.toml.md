# axi-console.toml

`configs/backends/axi-console.toml` is the backend config for the C
virtio-console daemon `cond`.

It is not part of the default block smoke test. It is provided so a second
`axi-bus` MMIO window can be wired to a virtio-console backend when needed.

## Fields

```toml
[console]
output = "run/cond.out"

[transport.qemu_mmio]
socket = "run/axi-console.sock"
ram_access = "qemu-mediated"
```

`output` is where guest-transmitted console bytes are appended. Use `"-"` to
write to stdout.

`socket` must match the frontend VM MMIO window that targets the console device.

`ram_access` currently stays `qemu-mediated`, matching the active QEMU-mediated
DMA path.
