# axi-linux-uio Topology

The `axi-linux-uio` topology runs the virtio backend daemon inside a second Linux
guest and uses Linux UIO for backend MMIO, DMA, and notification access. It still
uses QEMU's custom `axi` device on both frontend and backend QEMU processes. The
Unix socket in this topology is a QEMU-to-QEMU control socket only; payload DMA
does not travel over that socket.

## Topology

`nix run .#runuio-x64` starts two x86_64 microVMs. `nix run .#runuio-a64`
starts the same two-VM topology with ARM64 `virt` machines under TCG:

- The frontend VM boots the normal virtio-mmio drivers and sees the block and
  console devices through ACPI `virtio-mmio` nodes.
- The backend VM boots the backend daemons and sees matching UIO platform
  devices as `/dev/uio0` and `/dev/uio1`.
- QEMU `axi` devices in both VMs map the same host-backed files for each MMIO
  device window.
- The backend QEMU also maps the frontend RAM file into the backend guest at the
  configured DMA aperture.

The current x64 smoke topology is:

- Block frontend MMIO: `0x20feb00000`
- Console frontend MMIO: `0x20feb01000`
- Block backend UIO MMIO: `0x10feb00000`
- Console backend UIO MMIO: `0x10feb01000`
- Backend frontend-RAM aperture: `0x001000000000 + frontend_gpa`, size `512MiB`
- Device windows: `0x1000` bytes
- IRQs: block `16`, console `17`

For x86_64 frontends, frontend RAM starts at GPA `0x0`, so the backend maps the
shared RAM aperture at `0x001000000000`. For ARM64 frontends, frontend RAM starts
at GPA `0x40000000`, so the backend maps it at `0x001040000000`.

The current ARM64 smoke topology uses the same MMIO and DMA layout with GIC SPI
IRQs block `48` and console `49`. QEMU exports frontend `virtio,mmio` FDT nodes
and backend `chiplets,uio` FDT nodes.

## Backend Devices

The x64 backend guest loads `chiplets_uio.ko`, a small platform UIO driver used
by the smoke/benchmark topology. ARM64 uses the kernel `uio_pdrv_genirq` module
against QEMU-provided `chiplets,uio` device-tree nodes. Both paths register two
UIO devices:

- `map0`: the shared virtio-mmio/control window.
- `map1`: the frontend RAM DMA aperture.

The backend daemon endpoint syntax is:

```text
uio:/dev/uioX[:irq-control-offset[:dma-base]]
```

If the IRQ control offset is omitted, the backend uses `0x200`. Writing `1` to
that offset requests a frontend IRQ assertion; writing `0` requests deassertion.
`dma-base` is the frontend guest physical address corresponding to the first byte
of UIO `map1`; it is `0` for x64 microvm frontends and `0x40000000` for ARM64
`virt` frontends.

## Notification Flow

The frontend and backend QEMU processes use a Unix control socket per device.
By default, frontend MMIO reads and writes send a frontend-notify message to the
backend QEMU. The backend QEMU turns that message into an interrupt on the
backend UIO device. The backend daemon blocks in `read(/dev/uioX)` until Linux
delivers the interrupt, then re-enables the UIO IRQ and scans the writable
virtio-mmio registers.

Set `CHIPLETS_UIO_NOTIFY_POLICY=barrier` to reduce frontend-to-backend control
round trips. In this mode, frontend QEMU updates the shared MMIO window for all
accesses but only notifies the backend for transport barrier writes: feature
selection/negotiation, queue selection, queue ready, interrupt ACK, queue notify,
and device status. This keeps setup, kicks, and ACKs ordered while avoiding a
backend round trip for ordinary cached reads and queue address writes.

With `barrier`, queue notify writes are sent asynchronously and coalesced while
an ACK is outstanding. Setup and status barriers still wait for the backend ACK
so feature negotiation and queue publication observe fresh shared-MMIO state, but
data-path kicks do not serialize the frontend on backend request completion.

Backend-to-frontend interrupts use the same shared MMIO window. When a daemon
needs to raise or lower a virtio interrupt, it writes the control word at offset
`0x200`. The backend QEMU observes that write and sends an IRQ assert/deassert
message to the frontend QEMU, which drives the frontend guest interrupt line.

## Register Synchronization

The shared MMIO file contains the virtio-mmio transport registers. Frontend
writes update the file directly. Backend daemons refresh guest-readable
registers after every UIO wake by calling the device model callbacks and storing
the values into the shared window.

Virtio queue registers are scoped by `VIRTIO_MMIO_QUEUE_SEL`, but the shared
window has only one set of queue register offsets. To avoid replaying stale
queue state when the frontend switches queues, QEMU clears the queue-scoped
writable registers whenever the frontend writes `QUEUE_SEL`. The backend then
observes only the queue setup values that the frontend actually writes for the
new selected queue.

## DMA Flow

The backend daemon maps UIO `map1` and translates frontend guest physical
addresses by subtracting the configured `dma-base`. Descriptor reads, block data
copies, and console payload reads are therefore direct memory copies inside the
backend guest process. No host socket DMA messages are used in UIO mode.

## How It Works

The UIO topology is a two-VM virtio split. The frontend VM is the normal Linux
driver side, so it only needs regular `virtio-mmio` device nodes. The backend VM
is the device-emulation side, so it receives UIO devices whose resources expose
the same transport register files plus a DMA window onto the frontend RAM file.

Each virtual device has two host-backed shared files. One file is a `0x1000`
virtio-mmio register window mapped by both QEMU processes. The second shared
object is the frontend RAM backing file, additionally mapped into the backend VM
at the backend DMA aperture. Frontend register accesses therefore become shared
memory updates, and backend descriptor/data accesses become ordinary loads and
stores through UIO `map1`.

Interrupts are bridged by a small QEMU control protocol rather than by polling.
When the frontend driver reads or writes a transport register, the frontend QEMU
sends a notify message to the backend QEMU over the per-device Unix control
socket. The backend QEMU pulses the backend VM's UIO interrupt line. Linux wakes
the backend daemon from `read(/dev/uioX)`, the daemon re-enables the UIO IRQ, then
scans the writable virtio-mmio registers and runs the block or console device
model callbacks.

The reverse interrupt direction uses a reserved control word inside the backend
UIO MMIO window. When the backend has consumed a virtqueue descriptor or has data
available, it writes `1` or `0` at offset `0x200`. Backend QEMU interprets that as
frontend IRQ assert or deassert and forwards it over the control socket to the
frontend QEMU, which drives the frontend guest IRQ line.

The orchestrator starts the frontend QEMU paused, starts the backend QEMU, boots
the backend guest, launches the backend daemons, waits until the daemons report
that their UIO mappings are active, and only then resumes the frontend VM through
QMP. This avoids the frontend Linux virtio drivers probing before the backend has
published valid feature registers.

Architecture-specific pieces are deliberately small. x86_64 uses the custom
`chiplets_uio.ko` driver to provide stable platform UIO resources for the
microVM topology. ARM64 uses `uio_pdrv_genirq` with QEMU-generated
`chiplets,uio` FDT nodes. The backend endpoint's `dma-base` tells the daemon how
to translate frontend guest physical addresses into offsets inside the DMA UIO
mapping: `0` for x86_64 microVM frontends and `0x40000000` for ARM64 `virt`
frontends.

## Test And Benchmark Entry Points

The x64 test and benchmark scripts exercise the `axi-linux-uio` topology:

```sh
tests/run-tests.sh
tests/run-benchmark.sh
```

Both scripts call `nix run .#runuio-x64`, which builds backend daemons with
`CHIPLETS_BACKEND_FABRIC=linux-uio`, packages them into a temporary initrd with
`chiplets_uio.ko`, and launches the two-VM orchestrator in
`scripts/chiplets-uio-x64.py`.

`tests/run-tests-a64.sh` calls `nix run .#runuio-a64`, cross-builds ARM64 backend
daemons, packages them with the ARM64 kernel modules, and launches the same
orchestrator with `--arch a64`.

Mixed-architecture smoke wrappers exercise both cross directions:

```sh
tests/run-tests-a64-backend-x64-frontend.sh
tests/run-tests-x64-backend-a64-frontend.sh
```

They call `runuio-a64-backend-x64-frontend` and
`runuio-x64-backend-a64-frontend`, respectively, which pass separate frontend
and backend kernels/initrds into the two-VM orchestrator.

The ARM64 benchmark wrapper is:

```sh
tests/run-benchmark-a64.sh
```

The benchmark defaults to a small `1MiB` transfer for quick smoke coverage;
larger transfers can be requested with `BENCH_SIZE_MB`.
Set `BENCH_REPEAT` to run the write/read benchmark multiple times within the
same VM pair and print min/average/max throughput summaries:

```sh
BENCH_REPEAT=3 BENCH_SIZE_MB=64 tests/run-benchmark.sh
```

The AXI UIO frontend notification policy can be switched at runtime. The default
`all` policy preserves one backend notification per frontend MMIO access. The
`barrier` policy batches shared-MMIO updates and notifies only on transport
barrier writes:

```sh
CHIPLETS_UIO_NOTIFY_POLICY=barrier BENCH_REPEAT=3 BENCH_SIZE_MB=64 tests/run-benchmark.sh
```

Backend request timing can be enabled for benchmark runs with
`CHIPLETS_PROFILE_BACKEND=1`. The orchestrator passes
`CHIPLETS_BLKD_PROFILE=1` into `virtio-blkd` and reports average per-request
time spent in descriptor-chain processing, guest DMA, image I/O, used-ring
updates, and frontend IRQ signaling:

```sh
CHIPLETS_PROFILE_BACKEND=1 BENCH_SIZE_MB=64 tests/run-benchmark.sh
```

An experimental direct read-DMA path can be enabled with
`CHIPLETS_DIRECT_READ_DMA=1`. For UIO-backed devices this lets `virtio-blkd`
read block-image data directly into the mapped frontend RAM aperture. The switch
is opt-in while write-side throughput remains sensitive to benchmark variance:

```sh
CHIPLETS_DIRECT_READ_DMA=1 BENCH_SIZE_MB=64 tests/run-benchmark.sh
```

## Benchmark Records

The `perf/uio-throughput` branch improved block request coalescing by advertising
`VIRTIO_BLK_F_SIZE_MAX` and `VIRTIO_BLK_F_SEG_MAX`, with `size_max=256K` and
`seg_max=128`. It also adds direct DMA reads into caller-provided buffers and a
QEMU notify-ack path for topologies where that handshake is stable.

Recorded on 2026-05-09 with `BENCH_SIZE_MB=64 BENCH_BS=64K`:

```text
x64 UIO benchmark:
  write: 67108864 bytes (64.0MB) copied, 3.546474 seconds, 18.0MB/s
  read:  67108864 bytes (64.0MB) copied, 1.897987 seconds, 33.7MB/s
  backend requests: read=20 write=140 flush=0

ARM64 UIO benchmark:
  write: 67108864 bytes (64.0MB) copied, 3.162889 seconds, 20.2MB/s
  read:  67108864 bytes (64.0MB) copied, 2.809209 seconds, 22.8MB/s
  backend requests: read=20 write=132 flush=0
```

Before advertising the segment limits, the same 64MiB write path issued `16384`
backend write requests, effectively one 4KiB request at a time.

Recorded on 2026-05-14 on branch `perf/uio-mmio-notify-cache` with
`CHIPLETS_UIO_NOTIFY_POLICY=barrier BENCH_SIZE_MB=64 BENCH_BS=64K
BENCH_REPEAT=3` after async/coalesced queue notify:

```text
x64 UIO barrier benchmark:
  write summary: min=83.0MiB/s avg=87.2MiB/s max=95.0MiB/s
  read summary:  min=94.6MiB/s avg=95.6MiB/s max=96.8MiB/s
  backend requests: read=58 write=396 flush=0
  backend profile: requests=454 chain_avg_us=3083.5 guest_dma_avg_us=147.9
                   image_io_avg_us=197.5 add_used_avg_us=0.7 irq_avg_us=0.1
```
