# Project State And Design

This document explains the current project state, design decisions, runtime
paths, and known limitations. It is written for a junior systems engineer who is
comfortable with Linux and C, but may be new to QEMU device models, virtio-mmio,
and Linux UIO.

## Project Goal

This project builds a minimal MMIO-only device-emulation path for
chiplet-oriented experiments. The central idea is to keep Linux guests using
standard upstream virtio drivers while moving the actual device model out of
QEMU and into external C backend daemons.

QEMU does not implement the virtio block or console devices for the project
paths. QEMU only provides transport plumbing:

- trap guest MMIO accesses
- expose or mediate guest RAM access
- inject interrupts into the frontend guest
- bridge notifications between frontend and backend processes or VMs

The C backend daemons own device behavior:

- virtio-mmio register state
- virtqueue descriptor parsing
- virtio-blk request handling
- virtio-console TX handling
- block image I/O and console output

## Naming And Terminology

The most important naming rule is that **QEMU `axi` is used in both active
topologies**. The topology name tells us how QEMU `axi` is connected to the
backend, not whether QEMU `axi` exists.

```text
+----------------------+-----------------------------------------------+
| Name                 | Meaning                                       |
+----------------------+-----------------------------------------------+
| QEMU axi device      | QEMU sysbus MMIO/IRQ/control transport device |
| topology             | End-to-end frontend/backend arrangement       |
| backend fabric       | C daemon access layer behind fabric.h         |
| axi-socket           | QEMU axi mode=socket topology                 |
| axi-linux-uio        | QEMU axi mode=uio plus Linux UIO topology     |
| qemu-socket fabric   | src/fabrics/qemu_socket.c                     |
| linux-uio fabric     | src/fabrics/linux_uio.c                       |
| linux-devmem fabric  | src/fabrics/linux_devmem.c                    |
+----------------------+-----------------------------------------------+
```

Short version:

```text
axi-socket topology:
  QEMU axi mode=socket
  backend fabric = qemu-socket
  Unix socket carries control + DMA/data + IRQ

axi-linux-uio topology:
  QEMU axi mode=uio
  backend fabric = linux-uio
  Unix socket carries QEMU-to-QEMU control only
  DMA/data and MMIO use shared memory exposed through Linux UIO
```

## Current Runtime Topologies

There are two active runtime topologies.

## axi-socket Topology

The `axi-socket` topology is the older, single-frontend-VM path driven by TOML
runtime configs in `samples/` and `scripts/chiplets-launcher.py`.

Detailed setup:

```text
axi-socket topology
================================================================================

+-----------------------------------+
| Frontend Linux Guest              |
|                                   |
| Software layers:                  |
| - upstream virtio_mmio            |
| - upstream virtio_blk             |
| - upstream virtio_console         |
|                                   |
| Guest-visible MMIO windows:       |
| - blk0: 0xfeb00000 size 0x200     |
| - con0: 0xfeb00200 size 0x200     |
|                                   |
| Guest IRQs:                       |
| - blk0: 16 on x86_64 microvm      |
| - con0: 17 on x86_64 microvm      |
+------------------+----------------+
                   ^
                   | guest IRQ line
                   | guest MMIO accesses
                   v
                                     Host
+--------------------------------------------------------------------------------+
|                                                                                |
|  +-------------------------------+        Unix socket        +---------------+ |
|  | Frontend QEMU                 |<------------------------->| Backend       | |
|  |                               | full backend protocol     | Daemon        | |
|  | -device axi                   |                            |               | |
|  | mode=socket                   | carries:                   | virtio-blkd   | |
|  | socket=run/axi.sock           | - MMIO/control             | consoled      | |
|  | ram-access=qemu-mediated      | - DMA_READ / DMA_WRITE     |               | |
|  |                               | - IRQ assert/deassert      | backend       | |
|  | exposes guest MMIO window     |                            | fabric:       | |
|  | injects frontend IRQ          |                            | qemu-socket   | |
|  +-------------------------------+                            +-------+-------+ |
|                                                                       |         |
|                                                                       | host I/O |
|                                                                       v         |
|                                                               +-------+------+ |
|                                                               | run/blk0.img | |
|                                                               | run/cond.out | |
|                                                               +--------------+ |
+--------------------------------------------------------------------------------+
```

MMIO mapping in `axi-socket`:

```text
Frontend guest view              QEMU axi,mode=socket          Backend view
-------------------              --------------------          ------------
0xfeb00000..+0x1ff  --MMIO-->    blk0 axi window      --socket MMIO msg-->
0xfeb00200..+0x1ff  --MMIO-->    con0 axi window      --socket MMIO msg-->

The backend daemon does not mmap these MMIO windows. It receives register reads
and writes as socket protocol messages handled by src/fabrics/qemu_socket.c.
```

DMA mapping in `axi-socket`:

```text
Backend daemon wants to read or write frontend guest RAM:

  virtqueue descriptor contains frontend_gpa
        |
        v
  qemu-socket backend fabric sends DMA_READ or DMA_WRITE over Unix socket
        |
        v
  QEMU translates frontend_gpa inside the frontend guest RAM address space
        |
        v
  data bytes return over the same Unix socket or are written by QEMU

The Unix socket carries both control and payload data in this topology.
```

Important files:

- `samples/axi-x64.toml`
- `samples/axi-a64.toml`
- `scripts/chiplets-launcher.py`
- `src/fabrics/qemu_socket.c`
- `docs/runtime-config.md`
- `docs/axi-protocol.md`

The sample TOML configs use `ram_access = "qemu-mediated"`. In that mode the C
backend asks QEMU to read and write guest RAM with socket protocol messages. The
socket therefore carries both control and data, similar in spirit to a vhost-user
style backend protocol.

## axi-linux-uio Topology

The `axi-linux-uio` topology is the newer performance and topology path. It
launches two Linux guests: a frontend guest running normal virtio drivers and a
backend guest running the C backend daemons against Linux UIO devices.

Detailed setup:

```text
axi-linux-uio topology
================================================================================

+----------------+----------------+          +---------------+------------------+
| Frontend Linux Guest            |          | Backend Linux Guest              |
|                                 |          |                                  |
| Software layers:                |          | Software layers:                 |
| - upstream virtio_mmio          |          | - chiplets_uio.ko on x86_64      |
| - upstream virtio_blk           |          | - uio_pdrv_genirq on ARM64       |
| - upstream virtio_console       |          | - /dev/uio0 for blk0             |
|                                 |          | - /dev/uio1 for con0             |
| Guest-visible MMIO windows:     |          |                                  |
| - blk0: 0x20feb00000 size 0x1000|          | UIO maps per device:             |
| - con0: 0x20feb01000 size 0x1000|          | - map0: MMIO/control window      |
|                                 |          | - map1: frontend RAM DMA aperture|
| Frontend RAM GPA base:          |          | - irq: frontend notify           |
| - x86_64: 0x0                   |          |                                  |
| - ARM64:  0x40000000            |          | Backend daemons:                 |
|                                 |          | - virtio-blkd                    |
|                                 |          | - virtio-consoled                |
|                                 |          | - backend fabric: linux-uio      |
+---------------------------------+          +---------------+------------------+
                 ^                                           ^
                 | guest MMIO + guest IRQ                    | backend UIO device
                 |                                           |
                                      Host
+--------------------------------------------------------------------------------+
|                                                                                |
|  +----------------------------+     control socket     +---------------------+ |
|  | Frontend QEMU              |<---------------------->| Backend QEMU        | |
|  |                            | QEMU-to-QEMU control   |                     | |
|  | -device axi                | only:                  | -device axi         | |
|  | mode=uio                   | - frontend notify      | mode=uio            | |
|  | role=frontend              | - IRQ assert/deassert  | role=backend        | |
|  | virtio-node=on             | - optional notify ack  | virtio-node=off     | |
|  |                            |                        | exposes UIO device  | |
|  +-------------+--------------+                        +---+-----------------+ |
|                ^                                           ^                   |
|                | shared mmap                               | shared mmap       |
|                |                                           |                   |
|  Shared host-backed files                                                      |
|                                                                                |
|  +-------------+--------------+        +-------------------+-----------------+ |
|  | blk.mmio                   |        | frontend.ram                        | |
|  | con.mmio                   |        | frontend guest RAM backing file     | |
|  |                            |        |                                     | |
|  | shared virtio-mmio state   |        | mapped by frontend QEMU as RAM      | |
|  | plus backend IRQ-control   |        | mapped by backend QEMU as DMA       | |
|  | word at offset 0x200       |        | aperture for backend VM             | |
|  +----------------------------+        +-------------------+-----------------+ |
|                                                                  |             |
|                                                                  | backend I/O |
|                                                                  v             |
|                                                        +---------+-----------+ |
|                                                        | /blk0.img           | |
|                                                        | console output      | |
|                                                        +---------------------+ |
+--------------------------------------------------------------------------------+
```

MMIO shared-file mapping in `axi-linux-uio`:

```text
Frontend VM view                    Shared file      Backend VM UIO view
----------------                    -----------      --------------------
blk0 0x20feb00000..0x20feb00fff <-> blk.mmio   <->  /dev/uio0 map0
con0 0x20feb01000..0x20feb01fff <-> con.mmio   <->  /dev/uio1 map0

Each device window is 0x1000 bytes.

Backend UIO map0 layout per device:

offset 0x000..0x1ff   virtio-mmio register window used by backend daemon
offset 0x200          IRQ-control word written by backend daemon
offset 0x204..0xfff   reserved in current setup
```

DMA/frontend RAM mapping in `axi-linux-uio`:

```text
Frontend QEMU maps frontend.ram as frontend guest RAM.
Backend QEMU maps the same frontend.ram into the backend VM as UIO map1.

Frontend descriptor contains:

  frontend_gpa = descriptor.addr

linux-uio backend fabric translates frontend_gpa into map1 offset:

  offset_in_uio_map1 = frontend_gpa - dma_base
  backend_pointer    = uio_map1_base + offset_in_uio_map1

Endpoint examples:

  x86_64 frontend:
    uio:/dev/uio0:0x200:0x0
    dma_base = 0x0

  ARM64 frontend:
    uio:/dev/uio0:0x200:0x40000000
    dma_base = 0x40000000

Backend frontend-RAM aperture visible in the backend guest:

  backend_phys = 0x0010_0000_0000 + frontend_gpa

  x86_64 frontend:
    0x0010_0000_0000..0x0010_1fff_ffff  maps frontend.ram

  ARM64 frontend:
    0x0010_4000_0000..0x0010_5fff_ffff  maps frontend.ram
```

What travels over the Unix control socket in `axi-linux-uio`:

```text
Frontend notify path:
  frontend guest MMIO write
    -> frontend QEMU axi,mode=uio
    -> Unix control socket
    -> backend QEMU axi,mode=uio
    -> backend VM UIO IRQ
    -> backend daemon wakes from read(/dev/uioX)

Backend IRQ path:
  backend daemon writes UIO map0 offset 0x200
    -> backend QEMU axi,mode=uio observes IRQ-control word
    -> Unix control socket
    -> frontend QEMU axi,mode=uio
    -> frontend guest IRQ

Payload DMA path:
  frontend.ram shared file
    -> backend VM UIO map1
    -> backend daemon memcpy or direct mapped access

Payload DMA does not use the Unix control socket in this topology.
```

Important files:

- `scripts/chiplets-uio-x64.py`
- `tests/run-tests.sh`
- `tests/run-tests-a64.sh`
- `tests/run-tests-a64-backend-x64-frontend.sh`
- `tests/run-tests-x64-backend-a64-frontend.sh`
- `tests/run-benchmark.sh`
- `tests/run-benchmark-a64.sh`
- `src/fabrics/linux_uio.c`
- `src/kernel/chiplets_uio.c`
- `docs/uio-fabric.md`

The `axi-linux-uio` topology still uses Unix sockets, but only for QEMU-to-QEMU
control. Payload DMA does not use socket messages. The backend daemon maps
frontend RAM through UIO `map1` and can copy descriptor/data payloads directly
inside the backend guest process.

## Core Architecture

## QEMU `axi` Device

The custom QEMU `axi` sysbus device is added by
`patches/qemu/0003-add-axi-device.patch`.

Despite the name, this is not a complete hardware AXI controller. It is a small
transport/proxy device used to expose a configurable MMIO window and interrupt to
Linux.

In `axi-socket` topology, `axi mode=socket` forwards MMIO/control, DMA/data, and
IRQ operations over a Unix socket to a host backend daemon.

In `axi-linux-uio` topology, paired frontend/backend QEMU instances use
`axi mode=uio`, map shared files for the virtio-mmio window and frontend RAM,
and communicate over a per-device control socket to forward frontend
notifications and backend IRQ requests.

Key QEMU properties:

- `base`: guest physical MMIO base
- `size`: MMIO aperture size
- `irq`: frontend or backend interrupt number
- `mode`: `socket` or `uio`
- `role`: `frontend` or `backend` in UIO mode
- `socket`: backend Unix socket path for socket mode
- `control-socket`: frontend/backend QEMU control socket in UIO mode
- `memdev`: host-backed MMIO memory backend in UIO mode
- `dma-memdev`: frontend RAM memory backend visible to backend QEMU in UIO mode
- `dma-base`: backend guest address for the mapped frontend RAM aperture
- `dma-size`: frontend RAM mapping size
- `virtio-node`: whether to expose a frontend `virtio,mmio` discovery node
- `notify-delay-us`: delay before frontend notify forwarding
- `notify-ack`: whether frontend notification waits for backend ack

## Virtio-MMIO Split

Linux guests use the upstream `virtio_mmio` transport driver. From Linux's point
of view, the device is a normal virtio-mmio device with a register window and an
interrupt.

The project moves the virtio-mmio register model into C backend code:

- `src/virtio-mmio.c` owns register-level transport behavior.
- `src/virtio.c` owns generic split-ring helpers.
- `src/drivers/virtio-blkd.c` owns virtio-blk device semantics.
- `src/drivers/virtio-consoled.c` owns virtio-console device semantics.

This split is intentional. It keeps QEMU generic and makes the backend device
logic reusable across fabrics.

## Backend Fabric API

The stable C fabric API is `src/fabrics/fabric.h`. Backend drivers call this API
instead of directly depending on a specific transport.

Current fabric implementations:

- `src/fabrics/qemu_socket.c`: backend fabric for `axi-socket` topology
- `src/fabrics/linux_devmem.c`: `/dev/mem` physical-memory fabric for experiments
- `src/fabrics/linux_uio.c`: backend fabric for `axi-linux-uio` topology

Important API operations:

- `fabric_register`: register one device with the fabric
- `fabric_run`: run the fabric event loop
- `fabric_dma_read`: allocate and copy guest memory
- `fabric_dma_read_into`: copy guest memory into caller buffer
- `fabric_dma_write`: copy caller buffer into guest memory
- `fabric_dma_map`: optionally map guest memory directly
- `fabric_dma_unmap`: release a direct mapping
- `fabric_raise_irq`: signal frontend interrupt
- `fabric_lower_irq`: deassert frontend interrupt

Only the `linux-uio` backend fabric currently supports `fabric_dma_map`.
`qemu-socket` and `linux-devmem` return false, causing drivers to use copy
helpers.

## Device Backends

## virtio-blkd

`src/drivers/virtio-blkd.c` implements a virtio block backend daemon.

Responsibilities:

- parse `key=value` runtime arguments
- open the configured image file
- expose capacity in 512-byte sectors
- advertise virtio-blk features
- process request chains from queue 0
- support read, write, and flush requests
- reject unsupported request types
- publish used-ring entries
- raise virtqueue interrupts

Current advertised features:

- `VIRTIO_BLK_F_SIZE_MAX`
- `VIRTIO_BLK_F_SEG_MAX`
- `VIRTIO_BLK_F_BLK_SIZE`
- `VIRTIO_BLK_F_FLUSH`
- `VIRTIO_F_VERSION_1`

Current limits:

- `BLKD_QUEUE_SIZE = 256`
- `BLKD_MAX_SEG_SIZE = 256 KiB`
- `BLKD_MAX_SEGMENTS = 128`

The larger segment limits are important. They let Linux merge many small block
operations into larger virtio requests. Before this optimization, a 64 MiB write
could produce `16384` backend write requests. With the current limits it is
roughly `132` to `146` write requests depending on topology and run variance.

Optional runtime switches:

- `CHIPLETS_BLKD_PROFILE=1`: emit per-notification timing counters.
- `CHIPLETS_BLKD_DIRECT_READ=1`: allow direct image reads into mapped guest RAM
  when the fabric supports `fabric_dma_map`.

The test wrappers expose these as:

- `CHIPLETS_PROFILE_BACKEND=1`
- `CHIPLETS_DIRECT_READ_DMA=1`

Direct read DMA is intentionally opt-in. It improves read-side behavior in some
runs but benchmark variance and write-side interactions are still being studied.

## virtio-consoled

`src/drivers/virtio-consoled.c` implements the virtio-console backend daemon.

Responsibilities:

- parse `key=value` runtime arguments
- open an output file or use stdout
- expose fixed console size
- process guest TX queue descriptor chains
- write guest console bytes to the configured output
- publish used-ring entries and raise interrupts

Host input to the guest is not implemented. Queue 0 buffers remain pending.

## Linux UIO Components

## UIO Backend Guest Model

The backend guest sees one UIO device per emulated virtio device.

Each UIO device exposes:

- `map0`: shared virtio-mmio and IRQ-control window
- `map1`: frontend RAM DMA aperture
- IRQ: frontend-to-backend notification interrupt

The backend daemon blocks on `read(/dev/uioX)`. When QEMU pulses the interrupt,
Linux wakes the daemon. The daemon re-enables the UIO interrupt, scans writable
virtio-mmio registers, services notifications, and refreshes readable registers.

This design avoids idle polling in the backend daemon.

## x86_64 Backend UIO Driver

The x86_64 backend VM uses `src/kernel/chiplets_uio.c`, a small platform UIO
driver packaged into the temporary initrd by the flake wrapper.

Fallback layout:

```text
MMIO base: 0x0010_feb0_0000
MMIO size: 0x1000 per device
DMA base:  0x0010_0000_0000
DMA size:  0x20000000
IRQ base:  16
```

The driver exposes the resources as Linux UIO maps and handles IRQ enable/disable
using the standard UIO `irqcontrol` pattern.

## ARM64 Backend UIO Driver

The ARM64 backend VM uses the upstream `uio_pdrv_genirq` module with
`of_id=chiplets,uio`. QEMU creates `chiplets,uio` FDT nodes for backend UIO
devices.

ARM64 frontend discovery uses `virtio,mmio` FDT nodes. Backend UIO discovery uses
`chiplets,uio` FDT nodes. These are intentionally separate roles.

## Address And Interrupt Layout

Common UIO topology:

```text
frontend blk MMIO: 0x0020_feb0_0000
frontend con MMIO: 0x0020_feb0_1000
backend blk MMIO:  0x0010_feb0_0000
backend con MMIO:  0x0010_feb0_1000
backend RAM aperture base: 0x0010_0000_0000 + frontend RAM GPA base
MMIO window size:  0x1000
IRQ control word:  offset 0x200 in backend UIO map0
```

x86_64 frontend RAM base:

```text
0x0
```

ARM64 frontend RAM base:

```text
0x40000000
```

x86_64 interrupts:

```text
block IRQ:   16
console IRQ: 17
```

ARM64 interrupts:

```text
block IRQ:   48
console IRQ: 49
```

## Notification And IRQ Design

Frontend-to-backend notification:

1. Linux frontend driver writes a virtio-mmio register such as `QUEUE_NOTIFY`.
2. Frontend QEMU observes the MMIO activity.
3. Frontend QEMU sends a notify message over the control socket.
4. Backend QEMU pulses the backend guest UIO interrupt.
5. Linux wakes the backend daemon from `read(/dev/uioX)`.
6. The backend daemon scans writable registers and services available work.

Backend-to-frontend interrupt:

1. Backend daemon completes a virtqueue request.
2. Backend daemon sets virtio interrupt status.
3. Backend daemon writes to offset `0x200` in UIO `map0`.
4. Backend QEMU observes the IRQ-control word.
5. Backend QEMU sends IRQ assert/deassert control messages to frontend QEMU.
6. Frontend QEMU drives the frontend guest interrupt line.

The offset `0x200` is part of the current contract and is shared by block and
console devices through their separate 0x1000 windows.

`notify-ack` is enabled for stable topologies. It is disabled for the known
fragile ARM64-backend/x86_64-frontend topology.

## QEMU Patch Set

Current QEMU patches:

- `0001-add-x86-64-microvm-minimal-device-config.patch`: adds a minimal x86_64
  device config.
- `0002-add-microvm-virtio-mmio-transport-count.patch`: lets microvm disable
  built-in virtio-mmio transports and reserves IRQs for `axi`.
- `0003-add-axi-device.patch`: adds the custom `axi` sysbus device for the
  `axi-socket` and `axi-linux-uio` topologies.
- `0004-export-axi-irqs-with-microvm-acpi.patch`: exports frontend `axi` windows
  through microvm ACPI using Linux's virtio-mmio HID.

Important decision: QEMU remains a transport proxy. Virtio endpoint semantics
stay in C backend daemons.

## Runtime Config Design

The TOML runtime config path is for `axi-socket` runs.

The launcher reads:

- top-level `ram_access`
- `[targets.qemu]`
- `[[devices]]`
- `[[targets.qemu.devices]]`

The schema is `docs/runtime-config.schema.json`. It validates the decoded TOML
shape and allowed field names. The launcher still performs semantic validation
that JSON Schema cannot express easily, such as checking that enabled QEMU device
names refer to inventory devices.

Important defaults in `scripts/chiplets-launcher.py`:

- `ram_access`: `shared-mem`
- QEMU target type: `microvm`
- QEMU binary: `out/qemu-x64-minimal/bin/qemu-system-x86_64`
- memory: `512M`
- KVM: enabled by default
- PCIe: disabled by default
- enabled QEMU devices: empty list if omitted

The checked-in samples explicitly set `ram_access = "qemu-mediated"`, which is
the supported socket data path for the current `qemu-socket` backend fabric.

## Build And Run Commands

Build QEMU:

```sh
scripts/build-qemu-x64.sh
scripts/build-qemu-arm64.sh
```

Build C tools:

```sh
scripts/build-tools.sh
```

Run `axi-socket` TOML samples:

```sh
nix run .#runvm-x64 -- samples/axi-x64.toml
nix run .#runvm-a64 -- samples/axi-a64.toml
```

Run `axi-linux-uio` smoke tests:

```sh
tests/run-tests.sh
tests/run-tests-a64.sh
tests/run-tests-a64-backend-x64-frontend.sh
tests/run-tests-x64-backend-a64-frontend.sh
```

Run `axi-linux-uio` benchmarks:

```sh
tests/run-benchmark.sh
tests/run-benchmark-a64.sh
```

Useful benchmark environment variables:

- `BENCH_SIZE_MB`: transfer size, default `1`
- `BENCH_BS`: dd block size, default `64K`
- `BENCH_REPEAT`: repeated write/read loops inside one VM pair, default `1`
- `BENCH_GUEST_TIMEOUT`: guest command timeout
- `CHIPLETS_PROFILE_BACKEND=1`: enable backend timing profile output
- `CHIPLETS_DIRECT_READ_DMA=1`: enable experimental direct read-DMA path

Example repeated benchmark:

```sh
BENCH_SIZE_MB=64 BENCH_REPEAT=3 tests/run-benchmark.sh
```

## Benchmark State

The major throughput improvement so far came from advertising larger virtio-blk
segment limits and reducing request count. Current recorded 64 MiB benchmark
records are in `docs/uio-fabric.md`.

Known observations:

- Larger request coalescing reduced 64 MiB write request count from `16384` to
  roughly `132` to `146`.
- `BLKD_MAX_SEG_SIZE = 256 KiB` was the best tested segment size.
- `128 KiB`, `512 KiB`, and `1 MiB` were worse in tested runs.
- Backend profiling showed guest DMA/copy work dominates over image file I/O in
  the buffered path.
- Direct read DMA can improve read throughput, but remains opt-in while write
  variance and cross-run stability are characterized.

Because write measurements are noisy, `BENCH_REPEAT` was added before promoting
more performance experiments to defaults.

## Design Decisions

Key decisions and rationale:

- Keep Linux guests unmodified. This proves compatibility with upstream
  `virtio_mmio`, `virtio_blk`, and `virtio_console` drivers.
- Keep QEMU endpoint-neutral. QEMU should not own block or console behavior.
- Use MMIO-only machines. This avoids PCI complexity and keeps discovery simple.
- Use a stable C fabric API. Device drivers should not care whether transport is
  socket, `/dev/mem`, or UIO.
- Use UIO for the two-VM backend path. It gives the backend daemon a normal Linux
  userspace interrupt and mmap model without writing a full kernel driver.
- Do not poll in the backend daemon while idle. The UIO daemon blocks on
  `read(/dev/uioX)` and wakes on interrupts.
- Keep `notify-ack` topology-specific. It improves stable topologies but is not
  currently safe for ARM64-backend/x86_64-frontend.
- Keep direct read DMA opt-in. It is promising, but the benchmark data is not yet
  stable enough to make it default.

## Current Limitations

- Socket-mode samples still use `qemu-mediated` RAM access and are not the fast
  path.
- UIO direct read DMA is experimental and opt-in.
- ARM64-backend/x86_64-frontend has known notification fragility and disables
  `notify-ack`.
- Backend daemons are single-device processes.
- virtio-console host input is not implemented.
- virtio-blk support is intentionally minimal and focused on current Linux smoke
  and benchmark behavior.
- Benchmark throughput, especially write throughput, has visible run-to-run
  variance.

## Recommended Next Work

1. Run repeated 64 MiB benchmarks for default and direct-read `axi-linux-uio`
   paths on x86_64 and ARM64.
2. Split backend profile accounting by request type so read and write costs are
   visible separately.
3. Investigate write-side variance before promoting direct DMA changes to default.
4. Stabilize or redesign the mixed-topology notification acknowledgement path.
5. Expand unit coverage for direct DMA mapping behavior and virtio-blk config
   limits.

## Reading Order For New Engineers

Recommended reading sequence:

1. `README.md`
2. `docs/project-state-and-design.md`
3. `docs/uio-fabric.md`
4. `docs/runtime-config.md`
5. `src/fabrics/fabric.md`
6. `src/drivers/virtio-blkd.md`
7. `patches/qemu/0003-add-axi-device.patch.md`

After that, read the C files in this order:

1. `src/virtio.c`
2. `src/virtio-mmio.c`
3. `src/fabrics/linux_uio.c`
4. `src/drivers/virtio-blkd.c`
5. `scripts/chiplets-uio-x64.py`
