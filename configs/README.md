# Runtime Configs

This directory contains hand-written runtime configs for the MMIO-only bring-up
path.

```text
qemu-vms/   frontend QEMU VM configs
backends/   external userspace backend configs
```

There is one frontend/backend pair:

```text
qemu-vms/axi-bus.toml     frontend QEMU config
backends/axi-bus.toml     block backend config
backends/axi-console.toml console backend config
```

The socket path must match between those two files.

The current backend uses `qemu-mediated` RAM access: the C backend asks QEMU
to perform guest DMA reads and writes over the proxy protocol. `shared-mem` is
kept as the later fast path for direct guest RAM/ATU-backed queue walking.

Each window's `irq` is from the project-reserved microvm `axi-bus` GSI range,
`16..23`. The launcher passes the value to QEMU, and patched microvm ACPI exports
it to Linux as the virtio-mmio interrupt resource:

```toml
irq = 16
```

The current frontend config has two MMIO windows:

```text
blk0  base=0xfeb00000 irq=16 socket=run/axi-bus.sock
con0  base=0xfeb00200 irq=17 socket=run/axi-console.sock
```
