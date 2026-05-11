# chiplets-vmm-tools

This repository contains the MMIO-only QEMU/frontend and C backend bring-up
path for chiplet-oriented device modeling. It supports the `axi-socket` topology
and the `axi-linux-uio` topology. Both topologies use QEMU's custom `axi` device;
they differ in what travels over Unix sockets and how the backend daemon accesses
MMIO, DMA, and interrupts.

The `axi-socket` topology used by `samples/axi-*.toml` is:

```text
Linux guest
  -> upstream virtio_mmio + virtio_blk drivers
  -> MMIO window at 0xfeb00000
  -> QEMU axi device, mode=socket
  -> Unix socket protocol carrying control, DMA/data, and IRQ messages
  -> C blkd
  -> run/blk0.img

Linux guest
  -> upstream virtio_mmio + virtio_console drivers
  -> MMIO window at 0xfeb00200
  -> QEMU axi device, mode=socket
  -> Unix socket protocol carrying control, DMA/data, and IRQ messages
  -> C cond
  -> run/cond.out
```

The `axi-linux-uio` topology used by `tests/run-tests*.sh` and
`tests/run-benchmark*.sh` is:

```text
frontend Linux guest
  -> upstream virtio_mmio + virtio_blk/virtio_console drivers
  -> QEMU axi frontend device with virtio,mmio discovery
  -> Unix control socket only, plus shared MMIO/RAM files
  -> QEMU axi backend device
  -> backend Linux guest UIO device
  -> virtio-blkd / virtio-consoled using CHIPLETS_BACKEND_FABRIC=linux-uio
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
- Use `samples/axi-x64.toml` and `samples/axi-a64.toml` as sample
  frontend VM and backend launch configs.
- Use `run/axi.sock` as the frontend/backend socket.
- Use `run/axi-console.sock` as the frontend/console-backend socket.
- Use `run/blk0.img` as the block image.
- Runtime TOML samples use the `axi-socket` topology with
  `ram_access = "qemu-mediated"`. `axi-linux-uio` tests use shared host-backed
  MMIO/RAM files and do not use the TOML launcher path.
- For every new `.patch` or `.c` source file, add a matching
  `<filename>.md` explanation file.

## Important Files

```text
CMakeLists.txt                                C tools/tests and QEMU fetch target
flake.nix                                      Nix run wrapper and initrd setup
scripts/build-tools.sh                        CMake build for local C tools/tests
scripts/build-qemu-x64.sh                     CMake-backed minimal QEMU build script
scripts/build-qemu-arm64.sh                   CMake-backed AArch64 QEMU build script
scripts/chiplets-launcher.py                  TOML orchestrator and QEMU launcher
tests/run-tests.sh                            end-to-end smoke test
tests/run-benchmark.sh                        dd throughput benchmark
tests/run-tests-a64.sh                        ARM64 UIO smoke test
tests/run-benchmark-a64.sh                    ARM64 UIO dd benchmark
samples/axi-x64.toml                      x86_64 sample frontend/backend config
samples/axi-a64.toml                      AArch64 sample frontend/backend config
docs/runtime-config.md                        config schema and authoring guide
docs/uio-fabric.md                            two-VM UIO architecture and benchmarks
docs/runtime-config.schema.json               machine-readable runtime schema
docs/qemu-target-toolchains.md                QEMU target file guide
src/fabrics/fabric.h                           C backend fabric API
src/fabrics/qemu_socket.c                       C qemu-socket backend fabric
src/fabrics/linux_devmem.c                      C linux-devmem backend fabric
src/fabrics/linux_uio.c                         C linux-uio backend fabric
src/drivers/virtio-blkd.c                     C virtio-blk daemon and device model
src/drivers/virtio-consoled.c                 C virtio-console daemon and device model
docs/axi-protocol.md                      QEMU/backend socket protocol
patches/qemu/*.patch                          QEMU source changes
```

## Build QEMU

Build the minimal x86_64 QEMU binary:

```sh
scripts/build-qemu-x64.sh
```

The script:

- re-enters a Nix shell with the required CMake and QEMU build tools
- uses CMake `ExternalProject` to fetch the QEMU release tarball
- selects the QEMU target file `cmake/qemu-targets/x64-minimal.cmake`
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
patches/qemu/0001-chiplets-qemu-support.patch.md
```

## Build C Tools

Build the C backends and unit-test binary:

```sh
scripts/build-tools.sh
```

The output binaries are:

```text
out/virtio-blkd
out/virtio-consoled
out/c-backend-tests
```

The backend drivers build against a stable `fabric.h` API. Select the fabric
implementation at configure time with `CHIPLETS_BACKEND_FABRIC`:

```sh
CHIPLETS_BACKEND_FABRIC=qemu-socket scripts/build-tools.sh
cmake -S . -B build/cmake -G Ninja -DCHIPLETS_BACKEND_FABRIC=linux-devmem
```

`qemu-socket` is the backend fabric used by the `axi-socket` TOML samples. Its
Unix socket carries control, DMA/data, and IRQ messages. `linux-devmem` is the
Linux `/dev/mem` backend fabric for physical virtio-mmio apertures and guest
memory. `linux-uio` is the backend fabric used by the `axi-linux-uio` two-VM
smoke and benchmark wrappers. In that topology Unix sockets carry only QEMU
control messages; data and MMIO access use shared memory exposed through Linux
UIO maps.

For `devmem`, pass the aperture via environment or a compact endpoint string:

```sh
CHIPLETS_BACKEND_FABRIC=linux-devmem \
CHIPLETS_DEVMEM_MMIO_BASE=0xfeb00000 \
scripts/build-tools.sh

out/virtio-blkd 'name=blk0,socket=devmem:0xfeb00000,image=run/blk0.img,ram_access=devmem'
```

Optional devmem variables include `CHIPLETS_DEVMEM_PATH`,
`CHIPLETS_DEVMEM_MMIO_SIZE`, `CHIPLETS_DEVMEM_POLL_US`,
`CHIPLETS_DEVMEM_IRQ_ADDR`, `CHIPLETS_DEVMEM_IRQ_ASSERT`, and
`CHIPLETS_DEVMEM_IRQ_DEASSERT`.

For individual targets, invoke CMake directly after configuration, for example:

```sh
cmake --build build/cmake --target virtio-blkd
cmake --build build/cmake --target virtio-consoled
```

## Run Backend Manually

The launcher starts `virtio-blkd` and `virtio-consoled` automatically. For manual debugging, create
the runtime image and start a backend with comma-separated arguments:

```sh
mkdir -p run
truncate -s 64M run/blk0.img
scripts/build-tools.sh
out/virtio-blkd name=blk0,socket=run/axi.sock,image=run/blk0.img,readonly=false,ram_access=qemu-mediated
```

The backend listens on:

```text
run/axi.sock
```

## Run Frontend VM

Launch the VM and its configured backends:

```sh
nix run .#runvm-x64 -- samples/axi-x64.toml
```

Inspect the generated QEMU command without launching:

```sh
nix run .#runvm-x64 -- --dry-run samples/axi-x64.toml
```

Pass extra QEMU arguments after `--`:

```sh
nix run .#runvm-x64 -- samples/axi-x64.toml -- -serial mon:stdio
```

Launch only QEMU and assume backend sockets are already served manually:

```sh
nix run .#runvm-x64 -- --no-backend samples/axi-x64.toml
```

## QEMU Machine Setup

The launcher currently emits:

```text
-machine microvm,pcie=off,ioapic2=on,virtio-mmio-transports=0,memory-backend=guestmem
```

Meaning:

- `microvm` is the minimal x86_64 frontend machine.
- `pcie=off` keeps the platform MMIO-only.
- `ioapic2=on` keeps the microvm IRQ topology explicit for `axi`.
- `virtio-mmio-transports=0` disables QEMU's built-in virtio-mmio transport
  slots.
- `memory-backend=guestmem` uses the configured memfd RAM backend.

The launcher emits one custom QEMU device per active MMIO window:

```text
-device axi,id=blk0,base=0xfeb00000,size=0x200,irq=16,
  socket=/.../run/axi.sock,ram-access=qemu-mediated,target=blk0
-device axi,id=con0,base=0xfeb00200,size=0x200,irq=17,
  socket=/.../run/axi-console.sock,ram-access=qemu-mediated,target=con0
```

## IRQ Rule

The frontend config assigns QEMU IO-APIC GSIs from the project-reserved microvm
`axi` range, `16..23`:

```toml
irq = 16
```

The launcher passes the GSI to QEMU's `axi` device. Patched microvm ACPI
exports the same GSI to Linux as the virtio-mmio interrupt resource, avoiding
manual Linux IRQ-number guessing.

The active devices use separate interrupt lines: `blkd` uses GSI `16`, and
`virtio-consoled` uses GSI `17`.

## Smoke Test

Run the x86_64 UIO backend/frontend smoke test:

```sh
tests/run-tests.sh
```

The test:

- builds backend daemons with `CHIPLETS_BACKEND_FABRIC=linux-uio`
- packages a temporary initrd with the needed UIO module support
- launches backend and frontend VMs through `scripts/chiplets-uio-x64.py`
- waits for `/dev/vda`
- writes one 512-byte sector
- waits for `/dev/hvc0` and writes a console smoke string
- verifies guest and backend logs

Expected output:

```text
x86_64 backend / x86_64 frontend UIO smoke test passed
run dir: /tmp/uio-x64-smoke.XXXXXX
frontend log: /tmp/uio-x64-smoke.XXXXXX/frontend.log
backend log:  /tmp/uio-x64-smoke.XXXXXX/backend.log
```

ARM64 and mixed-architecture UIO smoke wrappers are also available:

```sh
tests/run-tests-a64.sh
tests/run-tests-a64-backend-x64-frontend.sh
tests/run-tests-x64-backend-a64-frontend.sh
```

## Benchmark

Run the dd benchmark:

```sh
tests/run-benchmark.sh
```

The benchmark launches the two-VM UIO topology through `scripts/chiplets-uio-x64.py`.

Defaults:

```text
BENCH_SIZE_MB=1
BENCH_BS=64K
BENCH_REPEAT=1
BENCH_GUEST_TIMEOUT=300
```

Example with custom size, block size, and repeated runs:

```sh
BENCH_SIZE_MB=32 BENCH_BS=128K BENCH_REPEAT=3 tests/run-benchmark.sh
```

The benchmark prints each parsed `dd` throughput line and, when `BENCH_REPEAT` is
greater than one, min/average/max throughput summaries. Optional switches include
`CHIPLETS_PROFILE_BACKEND=1` for backend timing and `CHIPLETS_DIRECT_READ_DMA=1`
for the experimental direct read-DMA path.

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

- Socket-mode TOML samples use `qemu-mediated` RAM access, so descriptor/data
  access requires proxy round trips on that path.
- UIO shared-memory DMA is implemented for the two-VM benchmark path, but direct
  block-image reads into frontend RAM remain opt-in while benchmark variance is
  characterized.
- The backend is intentionally minimal and focused on bring-up correctness.
- `blkd` supports the descriptor chains used by current smoke and benchmark
  tests; broader virtio-blk coverage is intentionally minimal for bring-up.
