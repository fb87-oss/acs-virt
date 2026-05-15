# VirtIO-Based Chiplet Communication Framework Concept

## Purpose

This document describes a concept for using VirtIO as a communication framework
between chiplets. The goal is to reuse as much mature open-source software as
possible while adapting the transport from VM-host communication to real
chiplet-to-chiplet communication.

VirtIO was originally designed for VM-to-host communication. This concept adapts
the same requester/service-provider model to real hardware, where two chiplets
replace the VM and host. The goal is to reuse VirtIO's mature queue, discovery,
feature negotiation, and interrupt semantics while mapping the transport onto
AXI, ATU/UCIe, and MSI-capable chiplet interconnects.

The current QEMU implementation is a virtual proof of concept. It exists to
validate the software split, memory model, interrupt flow, and backend service
model before equivalent hardware blocks are available.

## Hardware Vision

The target system has at least two chiplets connected through an AXI-capable
chiplet interconnect, with ATU/UCIe translation windows for remote memory access
and MSI or MSI-like interrupts delivered over AXI.

The basic roles are:

- Consumer chiplet: runs applications and normal frontend drivers.
- Service chiplet: owns a device function such as camera processing, AI
  acceleration, storage, networking, or another offload service.
- Interconnect: carries MMIO, translated memory accesses, doorbells, and
  interrupts between the chiplets.

The consumer chiplet should not need a custom protocol for each service. It
should use normal operating-system device interfaces wherever possible. VirtIO is
used as the common contract between frontend drivers on the consumer side and
backend service models on the service side.

## Target Use Cases

### Camera Streaming

In the camera use case, the service chiplet owns the camera sensor pipeline. It
captures frames, optionally performs ISP or preprocessing, and streams video data
to the consumer chiplet.

The consumer chiplet should see a standard camera/video device and use existing
open-source tools such as `ffmpeg` directly, without a custom chiplet-specific
camera application or protocol.

The desired software path is:

```text
ffmpeg / GStreamer / v4l2-ctl
        |
        v
Linux V4L2/media interface
        |
        v
VirtIO video/camera frontend
        |
        v
Virtqueues over AXI + ATU/UCIe + MSI
        |
        v
Camera service backend on service chiplet
```

The framework should let the product reuse as much of the Linux multimedia stack
as possible instead of defining a new camera protocol only for this chiplet
topology.

### AI Offloading

In the AI offload use case, the service chiplet exposes accelerator capability.
The consumer chiplet submits inference jobs, command descriptors, model
references, tensor buffers, and synchronization objects through VirtIO-style
queues.

The service chiplet schedules accelerator work, accesses shared or translated
memory through AXI + ATU/UCIe, writes completion status, and signals completion
through MSI over AXI.

The userspace API for AI is less standardized than camera/video. For now, the
intended interface is generic:

```text
Application
        |
        v
AI runtime or library
        |
        v
Kernel frontend driver
        |
        v
VirtIO queues
        |
        v
AI service backend on service chiplet
```

The framework should keep the transport and queue semantics reusable while
allowing the AI device model and userspace runtime interface to evolve.

## Why Reuse VirtIO

VirtIO is useful here because it already defines the core pieces needed for a
split requester/service-provider system:

- Device discovery and identification.
- Feature negotiation.
- Device configuration space.
- Virtqueues for command and data descriptors.
- Driver and device status lifecycle.
- Interrupt semantics for configuration changes and used buffers.
- Existing Linux frontend drivers and userspace-facing stacks.

Reusing VirtIO avoids inventing a new queue format, negotiation protocol,
descriptor lifecycle, or per-device control model for every chiplet service.
Instead, the chiplet-specific work is concentrated in the transport mapping,
address translation, isolation model, and backend service implementations.

## Adapting VirtIO From VM-Host To Chiplet-Chiplet

The adaptation is conceptual rather than tied to virtualization.

```text
Original VirtIO model:

VM guest driver <-> hypervisor or host backend

Chiplet model:

Consumer chiplet driver <-> service chiplet backend
```

The same VirtIO contract is preserved, but the transport is remapped:

- VirtIO MMIO registers become AXI-visible MMIO registers or equivalent control
  registers.
- Guest physical addresses become consumer physical addresses inside an allowed
  shared-memory aperture.
- Host memory access becomes service-chiplet access through ATU/UCIe translated
  windows.
- Virtual interrupts become MSI or MSI-like doorbell interrupts over AXI.
- Host backend daemons or device models become service-chiplet backend services.

QEMU is not the architecture. QEMU is only the current virtual environment used
to emulate the chiplet transport contract before the hardware exists.

## Communication Model

The logical communication model has four paths.

```text
Consumer chiplet                                      Service chiplet
================                                      ===============

Application / runtime
        |
        v
Kernel frontend driver
        |
        | 1. MMIO control/register access over AXI
        |---------------------------------------------->
        |
        | 2. Descriptor and payload memory in restricted aperture
        |<--------------------------------------------->
        |
        | 3. Queue notify / doorbell
        |---------------------------------------------->
        |
        | 4. Completion interrupt / MSI
        |<----------------------------------------------
```

The VirtIO frontend publishes virtqueue addresses and buffers. The service
backend reads descriptors, validates that they point into allowed shared memory,
performs the requested service work, writes used-ring entries or completion
state, and raises an interrupt when needed.

The end-to-end software layering is:

```text
Consumer chiplet software                         Service chiplet software
=========================                         ========================

  +----------------------+                         +----------------------+
  | Application / tool   |                         | Service application  |
  | ffmpeg, AI runtime   |                         | camera, AI, block    |
  +----------+-----------+                         +----------^-----------+
             |                                                |
             v                                                |
  +----------------------+                         +----------+-----------+
  | Standard OS API      |                         | VirtIO backend       |
  | V4L2, media, accel   |                         | device model         |
  +----------+-----------+                         +----------^-----------+
             |                                                |
             v                                                |
  +----------------------+                         +----------+-----------+
  | VirtIO frontend      |                         | Queue handlers       |
  | driver               |                         | descriptor checks    |
  +----------+-----------+                         +----------^-----------+
             |                                                |
             v                                                |
  +----------------------+    MMIO / memory / IRQ  +----------+-----------+
  | Chiplet transport    |<----------------------->| Chiplet transport    |
  | AXI requester, MSI   |                         | AXI target, ATU, MSI |
  +----------------------+                         +----------------------+

The application-facing API stays standard while the transport underneath is
chiplet-specific.
```

Queue notify and completion sequence:

```text
Consumer frontend driver                         Service backend
========================                         ===============

  1. Fill payload buffer in shared/CMA region
             |
             v
  2. Write descriptor chain into vring
             |
             v
  3. Update avail ring and index
             |
             v
  4. Ensure descriptor writes are visible
             |
             v
  5. Write queue notify / doorbell  ----------->  6. Receive notify event
                                                   |
                                                   v
                                                7. Read avail ring
                                                   |
                                                   v
                                                8. Validate descriptor range
                                                   |
                                                   v
                                                9. Run service operation
                                                   |
                                                   v
                                               10. Write used ring/status
                                                   |
                                                   v
 12. Interrupt handler observes completion <---- 11. Raise MSI/completion IRQ
             |
             v
 13. Frontend completes request to OS/application

Ordering requirement:
  Descriptor and payload writes must be visible before notify.
  Used-ring writes must be visible before completion interrupt.
```

## Framework User Interfaces

The framework must provide two kinds of interfaces. First, it must provide
standard consumer-facing interfaces so applications can reuse existing software
such as `ffmpeg` or AI runtimes. Second, it must provide service-side and
transport interfaces so chiplet vendors can implement backend services without
inventing a new protocol for each use case.

### Consumer Application Interface

For camera and video, the consumer chiplet should expose standard Linux media
interfaces:

- `/dev/videoX`.
- V4L2 and media-controller APIs.
- Compatibility with tools such as `ffmpeg`, GStreamer, and `v4l2-ctl`.

For AI offload, the consumer chiplet should expose a stable accelerator/offload
API through a runtime or library:

- Application-facing AI runtime or library.
- Kernel frontend driver for command submission and synchronization.
- VirtIO queues used below the API boundary.

### Consumer Driver Interface

The consumer-side kernel driver should look like a normal VirtIO frontend driver
where possible. It should not expose AXI, ATU/UCIe, MSI routing, or service-side
implementation details to applications.

In the Linux PoC, this means standard `virtio-mmio` frontend drivers continue to
bind to the device. The `axi_mmio` wrapper constrains the DMA-visible memory used
by the VirtIO device, but the normal VirtIO frontend model remains intact.

### Service-Side Device Model Interface

The service chiplet needs a stable interface for backend service models. A
backend should be able to provide:

- VirtIO device ID and feature bits.
- Device configuration space.
- Queue setup and reset handling.
- Descriptor-chain validation.
- Service-specific queue processing.
- Shared or translated memory access.
- Used-ring completion generation.
- Interrupt generation.
- Error reporting and recovery behavior.

This backend interface should be usable for camera, AI, block, console, and
future service types.

### Backend MMIO Mapping Through UIO

The service-side backend can run as a normal Linux userspace process on the
service chiplet. It still needs access to the same control and data paths that a
hardware service endpoint would use: MMIO registers, shared memory apertures, and
interrupts. Linux UIO can expose those hardware resources to userspace without
requiring a full kernel backend driver for every VirtIO device model.

The service chiplet can describe each backend endpoint as a platform device with
MMIO, shared-memory aperture, and interrupt resources. Linux can bind that device
to a UIO driver such as `uio_pdrv_genirq` or a small service-specific UIO helper.
The resulting `/dev/uioX` device gives the backend daemon:

- `map0`: the shared VirtIO-MMIO/control window.
- `map1`: the frontend shared-memory or CMA aperture.
- `read(/dev/uioX)`: notification when the frontend has updated the transport.
- `write(/dev/uioX)`: re-enable UIO interrupt delivery after userspace handles an
  event.

This means the backend daemon can behave like a service-chiplet device model:

```text
Backend userspace daemon
        |
        +-- mmap(/dev/uioX, map0) -> VirtIO-MMIO register window
        |
        +-- mmap(/dev/uioX, map1) -> frontend shared/CMA aperture
        |
        +-- read(/dev/uioX)      -> frontend queue notify event
        |
        +-- MMIO control word    -> request frontend interrupt assertion
```

Real-hardware UIO resource view:

```text
Service chiplet Linux
=====================

  +-------------------------------+
  | Backend userspace daemon      |
  | camera / AI / block / console |
  +---------------+---------------+
                  |
                  | open, mmap, read, write
                  v
  +-------------------------------+
  | /dev/uioX                     |
  +---------------+---------------+
                  |
                  | kernel maps platform resources
                  v
  +-------------------------------+
  | UIO platform device           |
  | - resource 0: control MMIO    |
  | - resource 1: remote aperture |
  | - irq: queue notify interrupt |
  +---------------+---------------+
                  |
                  v
  +-------------------------------+      +------------------------------+
  | AXI MMIO target/control block |<---->| Consumer AXI MMIO requester  |
  +-------------------------------+      +------------------------------+
                  |
                  v
  +-------------------------------+      +------------------------------+
  | ATU/UCIe translated aperture  |<---->| Consumer shared/CMA buffers  |
  +-------------------------------+      +------------------------------+
                  |
                  v
  +-------------------------------+      +------------------------------+
  | MSI / doorbell hardware       |<---->| Consumer interrupt/notify    |
  +-------------------------------+      +------------------------------+

UIO does not define the VirtIO protocol. It exposes the hardware resources that
let the userspace backend implement the VirtIO service contract.
```

The key design decision is to keep backend service logic in userspace while the
transport is still evolving. This has several advantages:

- Backend device models are easier to develop, debug, profile, and replace.
- The same userspace backend code can run against different fabrics through
  `fabric.h`.
- Camera, AI, block, console, and future services can share the same backend
  structure.
- A Linux-based service chiplet can deploy the same model against real MMIO,
  shared-memory aperture, and interrupt resources.
- The backend can move later from userspace into a kernel driver, firmware, or
  dedicated hardware block without changing the VirtIO service contract.

UIO is therefore a practical deployment option for a Linux-based service chiplet,
not only a bring-up mechanism. It provides a simple boundary where the kernel
owns resource discovery, interrupt registration, and memory mapping, while the
userspace backend owns the VirtIO device model and service behavior. In more
integrated products, the same backend contract can be implemented by a kernel
driver, firmware, or hardware state machine that accesses AXI MMIO registers,
ATU/UCIe translated memory windows, and MSI/doorbell logic directly.

### Transport And Hardware Abstraction Interface

Backend device models should not depend directly on QEMU, Linux UIO, AXI, or a
specific ATU/UCIe programming model. The project already has this pattern in
`src/fabrics/fabric.h`, with implementations for QEMU socket transport, Linux
UIO, and Linux `/dev/mem` access.

The hardware-oriented framework should keep this separation and define transport
operations for:

- MMIO/control access.
- Shared-memory or translated-memory mapping.
- DMA address validation and translation.
- Queue notification receive path.
- Interrupt or MSI generation.
- Reset and link-failure handling.
- Capability/version discovery.

## Chiplet Address Mapping

Chiplet-to-chiplet memory access should use AXI + ATU/UCIe translated apertures
rather than assuming a flat global 64-bit physical address space. Real chiplet
systems usually use practical physical address widths around 48 to 52 bits, not
the full 64-bit range.

The proposed service-side remote aperture base is:

```text
H'0010_0000_0000
0x0010_0000_0000
```

This sits around the 40-bit region. It is high enough to avoid common low local
memory and MMIO ranges while staying safely inside typical 48-52 bit physical
address implementations.

If the consumer memory starts at physical address zero, the translation can be:

```text
service_visible_addr = remote_aperture_base + consumer_physical_addr
```

If consumer memory does not start at zero, the translation should subtract the
consumer memory base:

```text
service_visible_addr =
    remote_aperture_base + (consumer_physical_addr - consumer_memory_base)
```

The current QEMU UIO PoC follows this idea. For x86_64 frontend memory starting
at `0x0`, the backend-visible aperture starts at `0x0010_0000_0000`. For ARM64
frontend memory starting at `0x4000_0000`, the backend-visible aperture maps
frontend RAM at `0x0010_4000_0000`.

For future multi-consumer systems, non-overlapping apertures can be reserved:

```text
consumer0 = H'0010_0000_0000
consumer1 = H'0020_0000_0000
consumer2 = H'0030_0000_0000
```

Address translation view:

```text
Consumer chiplet physical address space
=======================================

  low memory / kernel / applications
  +----------------------------------------------------------+
  | not shared with service chiplet                          |
  +----------------------------------------------------------+

  dedicated shared/CMA region
  +------------------------------+  consumer_shared_region_base
  | vring desc / avail / used    |
  | indirect descriptors         |
  | payload buffers              |
  +------------------------------+  consumer_shared_region_base + size


Service chiplet physical address space
======================================

  local service memory / local MMIO
  +----------------------------------------------------------+
  | not related to consumer memory                           |
  +----------------------------------------------------------+

  remote aperture programmed by ATU/UCIe
  +------------------------------+  remote_aperture_base
  | translated view of consumer  |
  | shared/CMA region only       |
  +------------------------------+  remote_aperture_base + size


Translation:
  service_visible_addr =
      remote_aperture_base + (consumer_phys_addr - consumer_shared_region_base)
```

## Restricted Shared Memory Aperture

The service chiplet should not get access to all consumer RAM by default. The
consumer should allocate a dedicated shared-memory region, such as a Linux
CMA-reserved region, and expose only that region through the AXI + ATU/UCIe
remote aperture.

VirtIO descriptor rings, available rings, used rings, indirect descriptors, and
payload buffers should be allocated from this restricted region. This creates a
clear DMA-visible boundary between the consumer and service chiplets.

The shared-region-aware translation rule is:

```text
service_visible_addr =
    remote_aperture_base + (consumer_phys_addr - consumer_shared_region_base)
```

with:

```text
remote_aperture_base = H'0010_0000_0000
```

Every descriptor and payload access should be checked against the allowed range:

```text
consumer_shared_region_base <= descriptor.addr
descriptor.addr + descriptor.len <=
    consumer_shared_region_base + shared_region_size
```

This restriction matters because it:

- Limits the service chiplet's DMA-visible memory.
- Prevents accidental or malicious access to unrelated kernel memory,
  application memory, or another service's buffers.
- Creates a clear buffer ownership boundary.
- Simplifies backend-side descriptor validation.
- Enables per-device and per-service aperture partitioning.
- Provides a path toward stronger future isolation.

Whole-RAM mapping should be kept only for early bring-up or debugging. The
hardware-oriented design should expose only the shared/CMA region required for
VirtIO queues and payload buffers.

Restricted-buffer ownership model:

```text
Consumer chiplet                                      Service chiplet
================                                      ===============

  alloc from shared/CMA region
          |
          v
  +-------------------------+
  | virtqueue rings         |
  | payload buffers         |
  +-------------------------+
          |
          | publish descriptor addresses through VirtIO queue
          v
  +-------------------------+       translated aperture       +-------------------------+
  | descriptor.addr/len     |-------------------------------> | validate addr/len       |
  +-------------------------+                                 +-----------+-------------+
                                                                      |
                                                                      v
                                                            +-------------------------+
                                                            | access only allowed     |
                                                            | shared/CMA bytes        |
                                                            +-------------------------+

Invalid descriptor examples rejected by the service backend:
  descriptor below shared region
  descriptor past shared region end
  descriptor length overflow
  descriptor crossing into another service's partition
```

## CMA Vring Frontend Driver Direction

The Linux frontend needs a way to keep VirtIO vrings and payload buffers inside
the restricted shared-memory aperture. The current implementation anchor is
`src/kernel/axi_mmio.c`.

The current `axi_mmio` driver is a platform driver that matches `axi,mmio` or
ACPI `AXI0001`. It consumes one resource for the VirtIO MMIO register window and
one resource for the restricted DMA aperture. It then creates a normal
`virtio-mmio` child device and installs custom DMA operations on that child.

This keeps the frontend software model simple:

```text
Linux VirtIO frontend driver
        |
        v
standard virtio-mmio transport
        |
        v
axi_mmio wrapper
        |
        v
restricted shared/CMA aperture
```

Frontend restricted-DMA view:

```text
Consumer Linux
==============

  +-----------------------------+
  | Application / filesystem /  |
  | media stack / AI runtime    |
  +--------------+--------------+
                 |
                 v
  +-----------------------------+
  | Standard VirtIO frontend    |
  | virtio-blk, video, accel    |
  +--------------+--------------+
                 |
                 | DMA map request for vring or payload
                 v
  +-----------------------------+
  | axi_mmio DMA operations     |
  | - allow shared/CMA buffers  |
  | - reject or bounce others   |
  +--------------+--------------+
                 |
                 v
  +-----------------------------+
  | Reserved shared/CMA region  |
  | - vring desc/avail/used     |
  | - indirect descriptors      |
  | - data buffers              |
  +--------------+--------------+
                 |
                 v
  +-----------------------------+
  | Service-visible aperture    |
  | through ATU/UCIe mapping    |
  +-----------------------------+
```

The current prototype proves the mechanism, but the target CMA-vring solution
should harden it in these ways:

- Bind the restricted aperture to a real reserved-memory or CMA-backed region.
- Allocate vrings and payload buffers directly from that shared region.
- Prefer strict rejection of buffers outside the aperture for production use.
- Keep bounce buffering only as a bring-up or debug option if needed.
- Add clear overflow-safe range checks for every DMA mapping.
- Support realistic 48-52 bit physical address systems rather than assuming that
  only a full 64-bit DMA mask is valid.
- Handle cache coherency, cache maintenance, and memory ordering explicitly.
- Ensure backend-side descriptor validation still checks the aperture bounds.

The frontend driver constrains what addresses Linux gives to the device. It does
not replace backend validation. The service chiplet must still reject descriptor
addresses outside the allowed shared aperture.

## What Needs To Be Implemented

### Common Framework Work

The common framework should provide:

- A stable VirtIO backend/device-model library outside QEMU.
- Device lifecycle handling for creation, reset, feature negotiation, queue
  setup, notify, processing, completion, and interrupt delivery.
- Address translation rules for restricted shared-memory apertures.
- Descriptor and payload bounds checking.
- Capability and version discovery.
- Error handling, reset handling, recovery, and link-failure behavior.
- Profiling and observability for queue latency, DMA access time, backend service
  time, and interrupt latency.

### Hardware Transport Work

Hardware-oriented transport work includes:

- AXI MMIO register block for VirtIO-compatible control registers.
- ATU/UCIe programming model for remote shared-memory apertures.
- MSI or MSI-like interrupt delivery over AXI.
- Doorbell or queue-notify mechanism from consumer to service chiplet.
- Address validation at the transport boundary.
- Cache coherency or explicit cache maintenance rules.
- Memory ordering rules for queue publication, descriptor visibility, used-ring
  updates, and interrupt delivery.
- Reset, hot reset, and link-failure behavior.

### Camera Streaming Work

Camera support needs:

- A VirtIO video/camera device model suitable for chiplet service use.
- A Linux frontend path that exposes standard V4L2/media interfaces.
- Backend camera service implementation for frame capture, metadata, controls,
  buffer completion, and error reporting.
- Shared-buffer handling suitable for streaming workloads.
- Compatibility testing with `ffmpeg`, GStreamer, and `v4l2-ctl`.

### AI Offload Work

AI support needs:

- A VirtIO accelerator or offload device model.
- Frontend driver support for job submission, command descriptors, tensor
  buffers, model references, and synchronization objects.
- Backend scheduler integration with the AI accelerator.
- Runtime or library integration on the consumer side.
- Clear rules for shared model and tensor memory ownership.

### Developer And Configuration Work

Developers need configuration and diagnostics for:

- Device type and feature selection.
- Shared aperture base, size, and partitioning.
- Queue sizes and queue counts.
- MSI/interrupt routing.
- Cache coherency mode.
- Debug mode versus strict aperture-enforcement mode.
- Backend service selection and version compatibility.

## Hardware Logical Blocks

The hardware direction can be viewed as these logical blocks:

```text
                         Target Hardware Architecture
================================================================================

  +-----------------------------------+        AXI / UCIe fabric        +-----------------------------------+
  | Consumer Chiplet                  |<------------------------------>| Service Chiplet                   |
  |                                   |                                |                                   |
  | Applications / runtimes           |                                | Camera / AI / service function    |
  |   - ffmpeg, GStreamer, v4l2-ctl   |                                |   - sensor pipeline / ISP         |
  |   - AI runtime or library         |                                |   - AI accelerator scheduler      |
  |                |                  |                                |                ^                  |
  |                v                  |                                |                |                  |
  | Standard OS device interface      |                                | VirtIO backend device model       |
  |   - /dev/videoX, V4L2/media       |                                |   - feature/config handling       |
  |   - accelerator/offload API       |                                |   - virtqueue processing          |
  |                |                  |                                |   - descriptor validation         |
  |                v                  |                                |                ^                  |
  | VirtIO frontend driver            |                                |                |                  |
  |                |                  |                                |                |                  |
  |                v                  |                                |                |                  |
  | AXI MMIO requester                |---- MMIO registers ----------->| AXI MMIO target/control block     |
  |                |                  |                                |                |                  |
  |                |                  |                                |                v                  |
  | Shared CMA / restricted buffers   |<--- ATU/UCIe translated ------>| Remote aperture mapping          |
  |   - vring desc/avail/used         |     memory window              |   - bounds-checked access         |
  |   - indirect descriptors          |                                |   - cache/order handling          |
  |   - payload buffers               |                                |                |                  |
  |                |                  |                                |                v                  |
  | Doorbell / queue notify           |---- AXI doorbell / notify ---->| Queue notify receiver             |
  |                ^                  |                                |                |                  |
  |                |                  |                                |                v                  |
  | MSI receiver                      |<--- MSI / completion IRQ ------| MSI generator                     |
  |                                   |                                |                                   |
  +-----------------------------------+                                +-----------------------------------+

Hardware responsibilities:
  AXI MMIO path      Device discovery, feature negotiation, queue setup, reset
  ATU/UCIe aperture  Restricted service-visible access to consumer buffers only
  Doorbell path      Consumer-to-service queue notification
  MSI path           Service-to-consumer completion/config interrupt
  Isolation checks   Aperture bounds, per-service partitioning, access control
```

The blocks do not require QEMU. QEMU only emulates these relationships in the
current PoC.

## Virtual PoC Implementation In QEMU

The current QEMU patch provides a virtual proof-of-concept environment for the
chiplet communication framework. It does not implement the final hardware
transport, but emulates the major hardware-visible pieces needed to validate the
software model before real chiplet silicon is available.

The patched QEMU `axi` device models:

- AXI-visible MMIO windows for VirtIO register access.
- Shared memory apertures used as restricted DMA/CMA regions.
- Frontend/backend split across one or two virtual machines.
- Interrupt delivery corresponding to MSI or doorbell behavior in the hardware
  design.
- UIO exposure on the service/backend side so backend daemons can access MMIO,
  shared memory, and interrupts from userspace.

In this PoC, QEMU acts as the virtual interconnect. The frontend VM sees an
`axi,mmio` device, which is wrapped by the Linux `axi_mmio` driver and then
exposed as a normal `virtio-mmio` child device. The backend VM sees a
`chiplets,uio` device and runs userspace backend daemons such as `virtio-blkd`
or `virtio-consoled`.

This arrangement allows the framework to validate the same logical model
intended for hardware:

- Consumer chiplet frontend driver uses standard VirtIO.
- Service chiplet backend owns the VirtIO device model.
- Virtqueues and payload buffers live in a restricted shared aperture.
- MMIO, DMA, and interrupt behavior are separated in the same way they would be
  across AXI, ATU/UCIe, and MSI-capable hardware.

The QEMU PoC is therefore a software emulation of the chiplet transport contract,
not the product architecture itself. Its purpose is to exercise driver behavior,
backend service models, address translation, interrupt flow, and restricted
shared-memory rules before replacing the virtual transport with real AXI,
ATU/UCIe, and MSI hardware blocks.

Virtual-to-hardware mapping in the PoC:

```text
Hardware concept                         QEMU PoC representation
================                         =======================

Consumer chiplet                         Frontend VM
Service chiplet                          Backend VM or host backend process
AXI MMIO control window                  QEMU axi MMIO region / shared MMIO file
ATU/UCIe translated aperture             Host-backed shared memory object
Queue notify / doorbell                  Control socket notify message or UIO IRQ
MSI / completion interrupt               QEMU interrupt injection / IRQ control
Service backend device model             virtio-blkd, virtio-consoled, future daemon
Transport abstraction                    fabric.h implementation

The PoC keeps the same logical split even though the physical interconnect is
emulated by QEMU, host memory objects, sockets, and UIO devices.
```

## Current PoC Devices

The current repository demonstrates the framework with simple VirtIO devices:

- `virtio-blkd`: a userspace VirtIO block backend daemon.
- `virtio-consoled`: a userspace VirtIO console backend daemon.
- `axi-socket`: a single-frontend-VM topology where QEMU mediates MMIO, DMA, and
  IRQ messages to a host backend daemon over Unix sockets.
- `axi-linux-uio`: a two-VM topology where the frontend VM runs normal VirtIO
  drivers and the backend VM runs userspace backend daemons through Linux UIO.

These devices are not the final product use cases. They are useful because block
and console are simple, well-understood VirtIO devices that validate queue setup,
descriptor parsing, shared-memory access, and interrupt delivery.

The framework should evolve from block/console bring-up toward camera/video and
AI-oriented queues.

## Design Principles

The main design principles are:

- Reuse existing VirtIO semantics rather than inventing a new chiplet protocol.
- Keep consumer-facing software standard wherever possible.
- Treat QEMU as a PoC vehicle, not as the architecture.
- Separate device models from transport implementations.
- Restrict service-chiplet memory visibility to shared apertures, not all
  consumer RAM.
- Validate every descriptor and payload access on the service side.
- Make address translation explicit and hardware-friendly.
- Keep the design suitable for real AXI, ATU/UCIe, and MSI hardware.

## Future Hardware Direction

The future hardware path should replace QEMU's virtual transport with real
chiplet interconnect blocks while preserving the software contract.

The expected migration is:

```text
Current PoC:
QEMU axi device + shared files + UIO + backend daemon

Hardware target:
AXI MMIO block + ATU/UCIe aperture + MSI + service-chiplet backend
```

The consumer-side application and frontend-driver stack should remain as stable
as possible across this migration. The service side should keep using a common
backend device-model interface with a hardware transport implementation replacing
the QEMU/UIO fabric.

## Current Limitations

Current limitations include:

- The QEMU patch is a virtual PoC, not a production hardware model.
- Existing backend examples are block and console, not camera or AI.
- The current `axi_mmio` driver is a prototype for restricted DMA and still needs
  real CMA/reserved-memory integration for the target vring solution.
- Bounce-buffer behavior is useful for bring-up but may hide violations of the
  restricted-aperture model.
- Cache coherency and memory ordering rules need to be made explicit for real
  hardware.
- Security and isolation are currently design goals rather than complete
  enforcement mechanisms.
- Multi-consumer aperture partitioning is only a proposed direction.
- Error recovery, reset behavior, and link-failure handling need more detailed
  specification and tests.

## Appendix A: Current Codebase Map

The repository is organized around a small QEMU transport patch, external C
VirtIO backend daemons, Linux frontend/backend helper drivers, and scripts that
assemble the VM topologies.

Important source areas:

```text
README.md                                      Repository overview and run guide
docs/project-state-and-design.md               Current topology and design notes
docs/uio-fabric.md                             Two-VM UIO topology and benchmarks
docs/axi-protocol.md                           Socket protocol for axi-socket mode
docs/runtime-config.md                         TOML runtime configuration guide

patches/qemu/0001-chiplets-qemu-support.patch  QEMU axi virtual transport device
patches/qemu/0001-chiplets-qemu-support.patch.md
                                                Explanation of the QEMU patch

src/virtio.c, src/virtio.h                     Shared VirtIO helper code
src/virtio-mmio.c, src/virtio-mmio.h           Backend VirtIO-MMIO register model
src/drivers/virtio-blkd.c                      Userspace VirtIO block backend
src/drivers/virtio-consoled.c                  Userspace VirtIO console backend

src/fabrics/fabric.h                           Backend transport abstraction
src/fabrics/qemu_socket.c                      Socket-mode backend fabric
src/fabrics/linux_uio.c                        Linux UIO backend fabric
src/fabrics/linux_devmem.c                     /dev/mem backend fabric

src/kernel/axi_mmio.c                          Frontend axi,mmio wrapper driver
src/kernel/chiplets_uio.c                      Backend UIO helper driver for x64

scripts/chiplets-launcher.py                   TOML-driven axi-socket launcher
scripts/chiplets-uio-x64.py                    Two-VM UIO launcher and benchmark
scripts/build-tools.sh                         C backend/tool build wrapper
scripts/build-qemu-x64.sh                      x86_64 QEMU patch/build wrapper
scripts/build-qemu-arm64.sh                    ARM64 QEMU patch/build wrapper

tests/run-tests.sh                             x64 UIO smoke test
tests/run-tests-a64.sh                         ARM64 UIO smoke test
tests/run-tests-*-frontend.sh                  Mixed-architecture smoke tests
tests/run-benchmark.sh                         x64 UIO throughput benchmark
tests/run-benchmark-a64.sh                     ARM64 UIO throughput benchmark
```

The important architectural split is that QEMU does not implement the VirtIO
block or console endpoint behavior in the active project paths. QEMU provides the
virtual transport: MMIO windows, shared memory apertures, control sockets, and
interrupt injection. The C daemons implement the VirtIO device model and service
behavior.

The backend daemon structure is:

```text
virtio-blkd / virtio-consoled
        |
        v
VirtIO device model and virtqueue processing
        |
        v
fabric.h abstraction
        |
        +--> qemu_socket.c   for axi-socket topology
        +--> linux_uio.c     for axi-linux-uio topology
        +--> linux_devmem.c  for physical/devmem experiments
```

This is the same separation the hardware-oriented framework should keep:
service logic should not depend directly on whether the transport is QEMU,
Linux UIO, `/dev/mem`, or real AXI + ATU/UCIe hardware.

## Appendix B: Current VM Setups

### axi-socket VM Setup

The `axi-socket` setup is the older single-frontend-VM topology. It is driven by
`samples/axi-x64.toml`, `samples/axi-a64.toml`, and
`scripts/chiplets-launcher.py`.

```text
Frontend Linux VM
-----------------
- Standard virtio_mmio frontend transport
- Standard virtio_blk or virtio_console frontend driver
- QEMU custom axi device in mode=socket
- MMIO window exported to Linux as a VirtIO-MMIO device

Host backend process
--------------------
- virtio-blkd or virtio-consoled
- qemu-socket fabric
- Unix socket carries MMIO/control, DMA read/write messages, and IRQ messages
```

In this mode, the socket is the full transport. Descriptor and payload data are
read or written through QEMU-mediated socket messages. This is useful for simple
bring-up and debugging, but it is not the target high-throughput shared-memory
model.

Typical x86_64 frontend machine setup:

```text
-machine microvm,pcie=off,ioapic2=on,virtio-mmio-transports=0,
         memory-backend=guestmem
```

The `virtio-mmio-transports=0` setting disables QEMU's built-in VirtIO-MMIO
transport slots so the project-specific `axi` windows are the active transport.

### axi-linux-uio VM Setup

The `axi-linux-uio` setup is the current two-VM topology used by smoke tests and
throughput benchmarks. It is driven by `scripts/chiplets-uio-x64.py` and the
`nix run .#runuio-*` wrappers.

Demo setup:

```text
                         Host / QEMU PoC Environment
================================================================================

  +------------------------------+        control socket        +------------------------------+
  | Frontend QEMU                |<---------------------------->| Backend QEMU                 |
  | -device axi,role=slave       |  notify, IRQ assert/deassert | -device axi,role=master      |
  | - exposes axi,mmio           |                              | - exposes chiplets,uio       |
  | - maps blk.mmio/con.mmio     |                              | - maps blk.mmio/con.mmio     |
  | - maps frontend.cma          |                              | - maps frontend.cma          |
  +---------------+--------------+                              +---------------+--------------+
                  |                                                             |
                  | guest MMIO + IRQ                                            | UIO maps + IRQ
                  v                                                             v
  +------------------------------+                              +------------------------------+
  | Frontend Linux VM            |                              | Backend Linux VM             |
  |                              |                              |                              |
  | Applications / test commands |                              | virtio-blkd / consoled       |
  |        |                     |                              |        |                     |
  |        v                     |                              |        v                     |
  | /dev/vda, /dev/hvc0          |                              | linux-uio fabric             |
  |        |                     |                              |        |                     |
  |        v                     |                              |        v                     |
  | virtio_blk / virtio_console  |                              | /dev/uio0, /dev/uio1         |
  |        |                     |                              |        |                     |
  |        v                     |                              |        v                     |
  | virtio-mmio child device     |                              | map0: shared MMIO window     |
  |        |                     |                              | map1: frontend CMA aperture  |
  |        v                     |                              |                              |
  | axi_mmio wrapper             |                              |                              |
  +---------------+--------------+                              +---------------+--------------+
                  |                                                             |
                  | vrings + payload buffers                                    | descriptor/data access
                  +-------------------------+-----------------------------------+
                                            |
                                            v
                         shared frontend.cma / restricted aperture

Shared host-backed objects:
  blk.mmio / con.mmio   VirtIO-MMIO register windows visible to both QEMUs
  frontend.cma          Restricted shared aperture for vrings and payload buffers
  *.control.sock        Per-device notify and interrupt-control channel
```

```text
Frontend Linux VM                             Backend Linux VM
-----------------                             ----------------
standard virtio_mmio                          Linux UIO device
standard virtio_blk/console                   virtio-blkd/consoled
axi device role=slave                         axi device role=master
axi,mmio frontend node                        chiplets,uio backend node
restricted DMA/CMA aperture                   mapped frontend aperture
```

The frontend and backend QEMU processes share host-backed files:

- One file per VirtIO MMIO register window.
- One file for the frontend RAM or restricted frontend CMA/shared aperture.
- One Unix control socket per device for notify and interrupt forwarding.

In UIO mode, payload DMA does not travel over the Unix socket. Backend daemons
map UIO `map1` and access descriptors and payload buffers as shared memory.

Two-VM data and control paths:

```text
Data path:
  frontend application
      -> frontend VirtIO driver
      -> vring descriptor in frontend.cma
      -> backend UIO map1
      -> backend daemon reads/writes payload
      -> backend image/file/service buffer

Control path:
  frontend VirtIO driver
      -> MMIO queue notify
      -> frontend QEMU axi device
      -> control socket
      -> backend QEMU axi device
      -> backend UIO interrupt/read event
      -> backend daemon processes queue
      -> IRQ assert through control path
      -> frontend VirtIO interrupt handler

Key distinction:
  Payload bytes use shared memory.
  Notify and interrupt events use the control path.
```

Current x64 UIO address layout:

```text
Frontend block MMIO:       0x0020_feb0_0000
Frontend console MMIO:     0x0020_feb0_1000
Backend block UIO MMIO:    0x0010_feb0_0000
Backend console UIO MMIO:  0x0010_feb0_1000
Frontend RAM base:         0x0000_0000_0000
Frontend CMA/shared base:  frontend_ram_base + 512MiB + 0x1000_0000
Backend aperture base:     0x0010_0000_0000 + frontend_cma_base
Device window size:        0x1000
Block IRQ:                 16
Console IRQ:               17
```

Current ARM64 UIO address layout uses the same model, but frontend RAM starts at
`0x4000_0000` and the frontend/backend IRQs use GIC SPI lines such as `48` and
`49`.

### CMA Vring PoC Setup

The QEMU patch and launcher can expose a separate frontend CMA/shared-memory
object through `dma-memdev`, `dma-base`, and `dma-size`. When `dma-size` is
present, the frontend discovery node becomes `axi,mmio` or ACPI `AXI0001`, which
allows Linux to bind `src/kernel/axi_mmio.c`.

The intended discovery and driver stack is:

```text
QEMU frontend axi device with dma-memdev
        |
        v
ACPI AXI0001 or FDT compatible = "axi,mmio"
        |
        v
Linux axi_mmio platform driver
        |
        v
child platform device named "virtio-mmio"
        |
        v
standard Linux virtio-mmio transport driver
        |
        v
standard VirtIO frontend device driver
```

This is the virtual version of the future hardware model where the consumer
chiplet exposes only a restricted CMA/shared-memory region to the service chiplet
through an ATU/UCIe aperture.

## Appendix C: Running And Measuring Throughput

The current throughput benchmark uses the UIO topology and `virtio-blk` because
block I/O is easy to exercise with standard Linux tools. The benchmark is not the
final target workload, but it is useful for measuring queue overhead, shared
memory access, notification cost, and backend processing time.

Primary commands:

```sh
tests/run-benchmark.sh
tests/run-benchmark-a64.sh
```

Common benchmark variables:

```text
BENCH_SIZE_MB=1          Transfer size. Use 64 or larger for stable numbers.
BENCH_BS=64K             dd block size.
BENCH_REPEAT=1           Number of repeated runs in one VM pair.
BENCH_GUEST_TIMEOUT=300  x64 guest timeout in seconds.
```

Useful configurations:

```sh
BENCH_SIZE_MB=64 BENCH_BS=64K BENCH_REPEAT=3 tests/run-benchmark.sh

CHIPLETS_UIO_NOTIFY_POLICY=barrier \
BENCH_SIZE_MB=64 BENCH_BS=64K BENCH_REPEAT=3 \
tests/run-benchmark.sh

CHIPLETS_PROFILE_BACKEND=1 \
BENCH_SIZE_MB=64 BENCH_BS=64K \
tests/run-benchmark.sh

CHIPLETS_DIRECT_READ_DMA=1 \
BENCH_SIZE_MB=64 BENCH_BS=64K \
tests/run-benchmark.sh
```

The benchmark records three classes of numbers:

- Backend-native image write/read speed inside the backend VM.
- Frontend `/dev/vda` write/read speed through the full VirtIO/UIO path.
- Backend request counts and optional per-request timing profile.

Benchmark measurement flow:

```text
Frontend VM                                      Backend VM
===========                                      ==========

  dd / benchmark command
          |
          v
  /dev/vda block request
          |
          v
  virtio_blk frontend
          |
          | descriptors + payload buffers in shared aperture
          v
  axi_mmio / virtio-mmio transport  ---------->  UIO notify event
                                                   |
                                                   v
                                                virtio-blkd
                                                   |
                                                   v
                                                backend image file
                                                   |
                                                   v
  completion IRQ / used ring       <-----------  add used + interrupt
          |
          v
  dd reports elapsed time and MB/s

Optional backend profiling splits time into descriptor-chain handling,
guest/shared-memory access, image I/O, used-ring update, and IRQ generation.
```

Recorded baseline on 2026-05-09 with `BENCH_SIZE_MB=64 BENCH_BS=64K` after block
request coalescing improvements:

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

Before advertising `VIRTIO_BLK_F_SIZE_MAX` and `VIRTIO_BLK_F_SEG_MAX`, the same
64MiB write path issued `16384` backend write requests. That means the system was
effectively processing one 4KiB request at a time. Larger advertised segment
limits reduced request count and improved throughput.

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

The important performance lesson is that throughput is sensitive to both data
path and control path behavior:

- Smaller request sizes create many backend queue operations and reduce
  throughput.
- Shared-memory UIO avoids socket payload copies, but MMIO notification policy
  still matters.
- The default `all` notification policy preserves simple ordering but creates
  more backend wakeups.
- The `barrier` policy notifies only on transport barrier writes and coalesces
  queue notify behavior, improving throughput in the recorded x64 benchmark.
- Direct DMA read experiments can improve read-side data movement by avoiding an
  extra copy into intermediate buffers.

For future camera and AI workloads, these benchmark results should be treated as
transport indicators rather than product-performance claims. Camera streaming
will care more about sustained shared-buffer throughput, frame latency, and
buffer recycling. AI offload will care more about job submission overhead,
tensor-buffer mapping, accelerator scheduling latency, and completion interrupt
cost.
