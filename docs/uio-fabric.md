# UIO Fabric

The UIO fabric runs the virtio backend daemon inside a second Linux guest and
uses Linux UIO interrupts instead of a host socket backend. This gives the
backend a guest-visible MMIO window and a DMA mapping of the frontend guest RAM,
while keeping the backend event loop interrupt-driven.

## Topology

`nix run .#runuio-x64` starts two x86_64 microVMs:

- The frontend VM boots the normal virtio-mmio drivers and sees the block and
  console devices through ACPI `virtio-mmio` nodes.
- The backend VM boots the backend daemons and sees matching UIO platform
  devices as `/dev/uio0` and `/dev/uio1`.
- QEMU `axi` devices in both VMs map the same host-backed files for each MMIO
  device window.
- The backend QEMU also maps the frontend RAM file into the backend guest at the
  configured DMA aperture.

The current x64 smoke topology is:

- Block frontend MMIO: `0x10feb00000`
- Console frontend MMIO: `0x10feb01000`
- Block backend UIO MMIO: `0xfeb00000`
- Console backend UIO MMIO: `0xfeb01000`
- Backend DMA aperture: `0x30000000`, size `512MiB`
- Device windows: `0x1000` bytes
- IRQs: block `16`, console `17`

## Backend Devices

The backend guest loads `chiplets_uio.ko`, a small platform UIO driver used by
the smoke/benchmark topology. It registers two UIO devices:

- `map0`: the shared virtio-mmio/control window.
- `map1`: the frontend RAM DMA aperture.

The backend daemon endpoint syntax is:

```text
uio:/dev/uioX[:irq-control-offset]
```

If the IRQ control offset is omitted, the backend uses `0x200`. Writing `1` to
that offset requests a frontend IRQ assertion; writing `0` requests deassertion.

## Notification Flow

The frontend and backend QEMU processes use a Unix control socket per device.
Frontend MMIO reads and writes send a frontend-notify message to the backend
QEMU. The backend QEMU turns that message into an interrupt on the backend UIO
device. The backend daemon blocks in `read(/dev/uioX)` until Linux delivers the
interrupt, then re-enables the UIO IRQ and scans the writable virtio-mmio
registers.

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

The backend daemon maps UIO `map1` and treats guest physical addresses from the
frontend virtqueues as offsets into that mapping. Descriptor reads, block data
copies, and console payload reads are therefore direct memory copies inside the
backend guest process. No host socket DMA messages are used in UIO mode.

## Test And Benchmark Entry Points

The x64 test and benchmark scripts now exercise the UIO fabric:

```sh
tests/run-tests.sh
tests/run-benchmark.sh
```

Both scripts call `nix run .#runuio-x64`, which builds backend daemons with
`CHIPLETS_BACKEND_FABRIC=uio`, packages them into a temporary initrd with
`chiplets_uio.ko`, and launches the two-VM orchestrator in
`scripts/chiplets-uio-x64.py`.

The benchmark defaults to a small `1MiB` transfer because the current UIO path is
functional but slow; larger transfers can be requested with `BENCH_SIZE_MB`.
