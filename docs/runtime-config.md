# Runtime Config

Runtime configs are TOML files consumed by `scripts/chiplets-launcher.py`. The
launcher is the only TOML parser in the runtime path. It joins the hardware
inventory in `[[devices]]` with target-specific enablement in
`[[targets.qemu.devices]]`, starts backend daemons, and then starts QEMU.

The sample configs are:

```text
samples/axi-x64.toml  x86_64 microvm
samples/axi-a64.toml  AArch64 virt
```

The machine-readable schema for the TOML data model, expressed as JSON Schema for
the decoded TOML object, is:

```text
docs/runtime-config.schema.json
```

## Top Level

```toml
ram_access = "qemu-mediated"
```

`ram_access` is optional and defaults to `shared-mem` in the launcher. The active
bring-up path should set it to `qemu-mediated`.

Supported values:

- `qemu-mediated`: backends ask QEMU to perform guest DMA reads and writes over
  the proxy protocol.
- `shared-mem`: reserved for the later direct guest RAM/ATU-backed queue-walking
  fast path.

## Device Inventory

`[[devices]]` is the canonical hardware inventory. A device entry does not enable
anything by itself. It only declares the stable device name, concrete virtio type,
and fixed MMIO resources.

```toml
[[devices]]
name = "blk0"
type = "virtio-blk"
mmio = { base = "0xfeb00000", size = "0x200", irq = 16 }
```

Fields:

- `name`: unique device identifier. The launcher reuses it as QEMU `id` and
  `target`.
- `type`: concrete device type. Supported values are `virtio-blk` and
  `virtio-console`.
- `mmio.base`: MMIO window base address. Use a decimal string or a `0x` hex
  string.
- `mmio.size`: MMIO window size. Use a decimal string or a `0x` hex string.
- `mmio.irq`: interrupt number passed to QEMU. For x86 microvm this is an
  IO-APIC GSI exported through patched ACPI. For AArch64 `virt` this is a GIC
  SPI exported through the generated FDT.

Guidelines:

- Keep names stable and human-readable, such as `blk0`, `con0`, or `net0`.
- Do not reuse `name`, MMIO ranges, or IRQs across devices.
- Keep `mmio.size = "0x200"` for the current virtio-mmio windows unless the QEMU
  proxy device changes.
- Adding a device to `[[devices]]` alone does not enable it. Add a matching
  target-device entry under `[[targets.qemu.devices]]`.

## QEMU Target

`[targets.qemu]` configures the QEMU target.

```toml
[targets.qemu]
type = "microvm"
binary = "out/qemu-x64-minimal/bin/qemu-system-x86_64"
parameters = { memory = "512M", kvm = true, pcie = false }
```

Fields:

- `type`: must be `microvm`.
- `binary`: QEMU executable. Relative paths are resolved against the workspace and
  passed to QEMU as absolute paths.
- `parameters.memory`: guest RAM size passed to both `-object` and `-m`.
- `parameters.kvm`: when true, the launcher adds `-enable-kvm`.
- `parameters.pcie`: must remain false for the MMIO-only path.
- `parameters.cpu`: optional CPU model for AArch64 `virt`; defaults to `max`.
- `parameters.append`: optional kernel command line override. The launcher
  defaults to `console=ttyS0 ...` for `microvm` and `console=ttyAMA0 ...` for
  AArch64 `virt`.

Supported QEMU target types:

- `microvm`: x86_64 microvm. Uses patched ACPI discovery for `axi` devices.
- `virt`: AArch64 virt. Uses generated FDT `virtio,mmio` discovery for `axi`
  devices.

QEMU data files:

- Do not configure `bios_dir` in runtime TOML.
- For project-built QEMU, the build copies `pc-bios` into
  `out/<qemu-target>/share/qemu`.
- The launcher derives the QEMU data dir from `binary` and passes it with `-L`.
- For external QEMU binaries without a sibling `../share/qemu`, the launcher
  omits `-L` and lets QEMU use its compiled-in data dir.

## Enabled QEMU Devices

`[[targets.qemu.devices]]` is the QEMU enable list. Only devices listed here are
emitted on the QEMU command line and launched through backend daemons.

```toml
[[targets.qemu.devices]]
name = "blk0"
socket = "run/axi.sock"
log = "run/axi-backend.log"
image = "run/blk0.img"
readonly = false

[[targets.qemu.devices]]
name = "con0"
socket = "run/axi-console.sock"
log = "run/axi-console-backend.log"
output = "run/cond.out"
```

Common fields:

- `name`: must match a `[[devices]]` entry.
- `socket`: Unix socket path used by QEMU and the backend daemon.
- `log`: optional backend stdout/stderr log path.

Fields for `type = "virtio-blk"`:

- `image`: block image path passed to `blkd`.
- `readonly`: optional boolean, defaults to false in `blkd`.

Fields for `type = "virtio-console"`:

- `output`: optional output path passed to `virtio-consoled`. If omitted, `virtio-consoled` writes to
  stdout.

Guidelines:

- Put target-specific paths such as sockets, backend logs, images, and console
  outputs in `[[targets.qemu.devices]]`.
- Keep `socket` unique per enabled device.
- If a device exists in `[[devices]]` but not in `[[targets.qemu.devices]]`, it is
  disabled for QEMU.
- The launcher resolves all configured paths to absolute paths before spawning
  backends or QEMU.

## Backend Commands

The launcher starts backend daemons with one comma-separated `key=value` argument.
Examples from the sample config:

```text
out/virtio-blkd name=blk0,socket=/.../run/axi.sock,ram_access=qemu-mediated,image=/.../run/blk0.img,readonly=false
out/virtio-consoled name=con0,socket=/.../run/axi-console.sock,ram_access=qemu-mediated,output=/.../run/cond.out
```

Use `--no-backend` to launch only QEMU and manage backend daemons manually.

## Extending The Config

To add a second console device:

```toml
[[devices]]
name = "con1"
type = "virtio-console"
mmio = { base = "0xfeb00400", size = "0x200", irq = 18 }

[[targets.qemu.devices]]
name = "con1"
socket = "run/axi-console-con1.sock"
log = "run/axi-console-con1-backend.log"
output = "run/con1.out"
```

Current limitation: `virtio-consoled` and `blkd` are single-device daemons. The launcher can
spawn one daemon per enabled device, but backend support for additional concrete
types must be implemented before adding types beyond `virtio-blk` and
`virtio-console`.
