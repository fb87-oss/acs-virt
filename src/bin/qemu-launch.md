# qemu-launch

`qemu-launch` is the frontend VM launcher. It converts a small TOML config into
the QEMU command line needed for the MMIO-only AXI bus bring-up path.

The launcher does not start the backend. The backend is a separate process using
`configs/backends/axi-bus.toml`. `qemu-launch` only starts the frontend guest VM
and attaches QEMU's `axi-bus` device to the configured MMIO window.

## Input Config

The active frontend config is:

```text
configs/qemu-vms/axi-bus.toml
```

It contains:

```toml
[machine]
type = "microvm"
memory = "512M"
kvm = true
pcie = false

[qemu]
binary = "out/qemu-x64-minimal/bin/qemu-system-x86_64"
bios_dir = "build/qemu-pc-bios"

[transport]
ram_access = "qemu-mediated"

[[mmio_windows]]
name = "blk0"
base = "0xfeb00000"
size = "0x200"
irq = 16
socket = "run/axi-bus.sock"
target = "blk0"
enabled = true

[[mmio_windows]]
name = "con0"
base = "0xfeb00200"
size = "0x200"
irq = 17
socket = "run/axi-console.sock"
target = "con0"
enabled = true
```

## Invocation

Normally it is run through the flake wrapper:

```sh
nix run .#runvm -- configs/qemu-vms/axi-bus.toml
```

The wrapper supplies the kernel and initrd paths:

```text
--kernel <bzImage>
--initrd <initrd>
```

For inspection without launching QEMU:

```sh
nix run .#runvm -- configs/qemu-vms/axi-bus.toml --dry-run
```

Extra QEMU arguments can be appended after `--`:

```sh
nix run .#runvm -- configs/qemu-vms/axi-bus.toml -- -serial mon:stdio
```

## Machine Setup

The launcher intentionally supports only `microvm` today:

```text
-machine microvm,pcie=off,ioapic2=on,virtio-mmio-transports=0,memory-backend=guestmem
```

Important properties:

- `pcie=off` keeps the platform MMIO-only.
- `ioapic2=on` keeps the microvm IRQ topology explicit for `axi-bus`.
- `virtio-mmio-transports=0` disables QEMU's built-in virtio-mmio transport
  slots.
- `memory-backend=guestmem` uses the configured memfd RAM backend.
- QEMU must not instantiate a QEMU virtio block device.

The launcher also creates shared guest RAM backing:

```text
-object memory-backend-memfd,id=guestmem,size=<memory>,share=on
```

The current backend path still uses `qemu-mediated` DMA, but shared RAM backing
keeps the frontend VM layout compatible with the later `shared-mem` fast path.

## Device Enumeration

QEMU exports enabled `axi-bus` windows through microvm ACPI. Linux discovers them
as standard `LNRO0005` virtio-mmio devices and maps their interrupts through ACPI
IRQ routing.

Each window's `irq` is from the project-reserved microvm `axi-bus` GSI range,
`16..23`:

```toml
irq = 16
```

The launcher passes that GSI to QEMU's `axi-bus` device. The patched microvm ACPI
builder exports the same GSI to Linux, so block and console use distinct
interrupt lines from the reserved range without `virtio_mmio.device=` guessing.

## Proxy Device Setup

For each enabled MMIO window, the launcher adds one QEMU device:

```text
-device axi-bus,
  id=<name>,
  base=<base>,
  size=<size>,
  irq=<irq>,
  socket=<resolved socket path>,
  ram-access=<transport mode>,
  target=<target>
```

For the current config this resolves to a device like:

```text
-device axi-bus,id=blk0,base=0xfeb00000,size=0x200,irq=16,
  socket=/.../run/axi-bus.sock,ram-access=qemu-mediated,target=blk0
-device axi-bus,id=con0,base=0xfeb00200,size=0x200,irq=17,
  socket=/.../run/axi-console.sock,ram-access=qemu-mediated,target=con0
```

Relative socket paths are resolved against the workspace directory so QEMU sees
an absolute Unix socket path.

## Validation

`qemu-launch` rejects unsupported frontend configurations:

- only `machine.type = "microvm"` is allowed
- `pcie = true` is rejected
- MMIO `base` and `size` must parse as decimal or `0x` hex numbers
- `irq` must be less than or equal to `255`

It also checks that the configured QEMU binary exists unless `--dry-run` is used.

## Runtime Responsibilities

`qemu-launch` owns frontend VM command construction:

```text
TOML config
  -> kernel command line
  -> QEMU machine/memory args
  -> axi-bus device args
  -> QEMU process
```

It does not own:

- backend process startup
- block image creation
- virtio-mmio register behavior
- virtqueue processing
- block I/O semantics

Those are handled by `blkd` and `cond`.

## End-To-End Placement

The launcher's output puts QEMU in the middle of the guest/backend path:

```text
Linux guest
  -> ACPI LNRO0005 virtio-mmio window at <base>
  -> QEMU axi-bus
  -> Unix socket
  -> blkd / cond
```

The guest remains unmodified and uses upstream Linux `virtio_mmio`, `virtio_blk`,
and `virtio_console` drivers.
