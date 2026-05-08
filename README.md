# chiplets-vmm-tools

This repository contains the MMIO-only QEMU/frontend and C backend bring-up
path for chiplet-oriented device modeling.

The current working path is:

```text
Linux guest
  -> upstream virtio_mmio + virtio_blk drivers
  -> MMIO window at 0xfeb00000
  -> QEMU axi-bus device
  -> Unix socket protocol
  -> C blkd
  -> run/blk0.img

Linux guest
  -> upstream virtio_mmio + virtio_console drivers
  -> MMIO window at 0xfeb00200
  -> QEMU axi-bus device
  -> Unix socket protocol
  -> C cond
  -> run/cond.out
```

QEMU does not implement the virtio block or console devices. QEMU only traps
guest MMIO, mediates guest RAM access, and injects interrupts. The external
backends own virtio-mmio register models, virtqueue processing, and endpoint I/O.

## Project Rules

- QEMU is fetched by CMake; put QEMU source changes in `patches/qemu/*.patch`.
- Keep QEMU-specific transport logic out of backend core logic where practical.
- Do not instantiate QEMU virtio endpoint devices for the active path.
- Do not use `vhost-user-blk`, `vhost-user-blk-pci`, `virtio-blk-device`, or
  QEMU-created virtio-mmio transports for the active path.
- Keep guest Linux unmodified; use upstream `virtio_mmio` and `virtio_blk`.
- Keep runtime configuration TOML-driven.
- The Python launcher owns backend lifecycle by default; use `--no-backend` for
  manual backend launches.
- Use `configs/axi-bus.toml` for the frontend VM and backend launch parameters.
- Use `run/axi-bus.sock` as the frontend/backend socket.
- Use `run/axi-console.sock` as the frontend/console-backend socket.
- Use `run/blk0.img` as the block image.
- Current RAM access mode is `qemu-mediated`; `shared-mem` is the planned fast
  path.
- For every new `.patch` or `.c` source file, add a matching
  `<filename>.md` explanation file.

## Important Files

```text
CMakeLists.txt                                C tools/tests and QEMU fetch target
flake.nix                                      Nix run wrapper and initrd setup
scripts/build-tools.sh                        CMake build for local C tools/tests
scripts/build-qemu-x64.sh                     CMake-backed minimal QEMU build script
scripts/chiplets-launcher.py                  TOML orchestrator and QEMU launcher
tests/run-tests.sh                            end-to-end smoke test
tests/run-benchmark.sh                        dd throughput benchmark
configs/axi-bus.toml                          combined frontend/backend config
src/bin/blkd.c                                C blkd main/config entry point
src/bin/blkd-axi.c                            C axi-bus socket transport
src/bin/blkd-virtio.c                         C virtio-mmio/virtio-blk device model
src/bin/blkd-block.c                          C block image backend
src/bin/cond.c                                C cond main/config entry point
src/bin/cond-axi.c                            C virtio-console axi-bus transport
src/bin/cond-virtio.c                         C virtio-mmio/virtio-console model
src/bin/cond-console.c                        C console output backend
docs/axi-bus-protocol.md                      QEMU/backend socket protocol
patches/qemu/*.patch                          QEMU source changes
```

## Build QEMU

Build the minimal x86_64 QEMU binary:

```sh
scripts/build-qemu-x64.sh
```

The script:

- re-enters a Nix shell with the required CMake and QEMU build tools
- uses CMake `ExternalProject` to fetch QEMU
- copies the fetched QEMU source to `build/qemu-src-x64-minimal`
- applies `patches/qemu/*.patch`
- uses `ccache`
- configures QEMU with `--without-default-features` and
  `--without-default-devices`
- builds `x86_64-softmmu`
- installs `out/qemu-x64-minimal/bin/qemu-system-x86_64`
- copies QEMU runtime BIOS/data files into `out/qemu-x64-minimal/share/qemu`

The launcher derives QEMU's data directory from the configured `binary` path and
passes it with `-L` when `../share/qemu` exists:

```text
out/qemu-x64-minimal/share/qemu
```

## Check QEMU Patches

Before or after editing QEMU patches, verify through the CMake-backed QEMU build:

```sh
scripts/build-qemu-x64.sh
```

Patch docs live beside the patches:

```text
patches/qemu/0001-add-x86-64-microvm-minimal-device-config.patch.md
patches/qemu/0002-add-microvm-virtio-mmio-transport-count.patch.md
patches/qemu/0003-add-axi-bus-device.patch.md
```

## Build C Tools

Build the C backends and unit-test binary:

```sh
scripts/build-tools.sh
```

The output binaries are:

```text
out/blkd
out/cond
out/c-backend-tests
```

For individual targets, invoke CMake directly after configuration, for example:

```sh
cmake --build build/cmake --target blkd
cmake --build build/cmake --target cond
```

## Run Backend Manually

The launcher starts `blkd` and `cond` automatically. For manual debugging, create
the runtime image and start a backend with comma-separated arguments:

```sh
mkdir -p run
truncate -s 64M run/blk0.img
scripts/build-tools.sh
out/blkd name=blk0,socket=run/axi-bus.sock,image=run/blk0.img,readonly=false,ram_access=qemu-mediated
```

The backend listens on:

```text
run/axi-bus.sock
```

## Run Frontend VM

Launch the VM and its configured backends:

```sh
nix run .#runvm -- configs/axi-bus.toml
```

Inspect the generated QEMU command without launching:

```sh
nix run .#runvm -- --dry-run configs/axi-bus.toml
```

Pass extra QEMU arguments after `--`:

```sh
nix run .#runvm -- configs/axi-bus.toml -- -serial mon:stdio
```

Launch only QEMU and assume backend sockets are already served manually:

```sh
nix run .#runvm -- --no-backend configs/axi-bus.toml
```

## QEMU Machine Setup

The launcher currently emits:

```text
-machine microvm,pcie=off,ioapic2=on,virtio-mmio-transports=0,memory-backend=guestmem
```

Meaning:

- `microvm` is the minimal x86_64 frontend machine.
- `pcie=off` keeps the platform MMIO-only.
- `ioapic2=on` keeps the microvm IRQ topology explicit for `axi-bus`.
- `virtio-mmio-transports=0` disables QEMU's built-in virtio-mmio transport
  slots.
- `memory-backend=guestmem` uses the configured memfd RAM backend.

The launcher emits one custom QEMU device per active MMIO window:

```text
-device axi-bus,id=blk0,base=0xfeb00000,size=0x200,irq=16,
  socket=/.../run/axi-bus.sock,ram-access=qemu-mediated,target=blk0
-device axi-bus,id=con0,base=0xfeb00200,size=0x200,irq=17,
  socket=/.../run/axi-console.sock,ram-access=qemu-mediated,target=con0
```

## IRQ Rule

The frontend config assigns QEMU IO-APIC GSIs from the project-reserved microvm
`axi-bus` range, `16..23`:

```toml
irq = 16
```

The launcher passes the GSI to QEMU's `axi-bus` device. Patched microvm ACPI
exports the same GSI to Linux as the virtio-mmio interrupt resource, avoiding
manual Linux IRQ-number guessing.

The active devices use separate interrupt lines: `blkd` uses GSI `16`, and
`cond` uses GSI `17`.

## Smoke Test

Run the full backend/frontend smoke test:

```sh
tests/run-tests.sh
```

The test:

- creates `run/blk0.img`
- launches `blkd` and `cond` through `scripts/chiplets-launcher.py`
- boots the frontend VM
- waits for `/dev/vda`
- writes one 512-byte sector
- waits for `/dev/hvc0` and writes a console smoke string
- verifies guest and backend logs

Expected output:

```text
axi-bus backend/frontend smoke test passed
backend log: run/axi-bus-backend.log
console backend log: run/axi-console-backend.log
console output: run/cond.out
guest log:   run/axi-bus-guest.log
```

## Benchmark

Run the dd benchmark:

```sh
tests/run-benchmark.sh
```

The benchmark launches the configured backends through `scripts/chiplets-launcher.py`.

Defaults:

```text
BENCH_SIZE_MB=16
BENCH_BS=64K
BENCH_GUEST_TIMEOUT=180
```

Example with custom size and block size:

```sh
BENCH_SIZE_MB=32 BENCH_BS=128K tests/run-benchmark.sh
```

The benchmark prints parsed `dd` throughput lines for write and read. The current
backend uses `qemu-mediated` DMA, so benchmark numbers measure the debug
transport path, not the planned shared-memory fast path.

## Standard Verification Flow

After code, config, or patch changes, run:

```sh
scripts/build-tools.sh
```

If QEMU patches changed, rebuild QEMU:

```sh
scripts/build-qemu-x64.sh
```

Then run:

```sh
tests/run-tests.sh
```

Optionally run:

```sh
tests/run-benchmark.sh
```

## Generated Files

The following paths are generated and should not be treated as source:

```text
build/
out/
run/
.cache/
```

`run/` contains logs, sockets, and disk images from tests and manual runs.

## Current Limitations

- Backend guest RAM access is `qemu-mediated`, so descriptor/data access requires
  proxy round trips.
- The shared-memory fast path is not implemented yet.
- The backend is intentionally minimal and focused on bring-up correctness.
- `blkd` supports the descriptor chains used by current smoke and benchmark
  tests; broader virtio-blk coverage is intentionally minimal for bring-up.
