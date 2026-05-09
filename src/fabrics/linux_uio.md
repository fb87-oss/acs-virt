# linux_uio.c

`linux_uio.c` is the selectable backend fabric implementation for the
`axi-linux-uio` topology.

The topology still uses QEMU `axi` devices. The distinction is that the Unix
socket between frontend and backend QEMU carries only control messages, while the
backend daemon accesses MMIO, data, and notifications through Linux UIO.

The backend daemon runs inside a backend Linux guest. QEMU exposes the virtio-mmio
window and the frontend RAM aperture as UIO resources:

- UIO `map0`: shared virtio-mmio and IRQ-control window.
- UIO `map1`: shared frontend RAM DMA aperture.

The daemon blocks in `read(/dev/uioX)` until QEMU pulses the UIO interrupt. After
wake-up it re-enables the UIO interrupt, scans writable virtio-mmio registers,
dispatches changed values through the driver callback, and refreshes guest-visible
read registers from the driver state.

Endpoint form:

```text
uio:/dev/uioX[:irq-control-offset[:dma-base]]
```

Fields:

- `/dev/uioX`: Linux UIO character device.
- `irq-control-offset`: resource0 offset used to signal frontend IRQs, default
  `0x200`.
- `dma-base`: frontend guest physical base represented by UIO `map1`, default
  `0` for x86_64 microvm frontends and `0x40000000` for ARM64 `virt` frontends.

DMA behavior:

- `fabric_dma_read_into` and `fabric_dma_write` copy to or from UIO `map1`.
- `fabric_dma_map` returns a direct pointer into UIO `map1` when the requested
  guest physical range is in bounds.
- `fabric_dma_unmap` is a no-op because the UIO mapping is owned by the fabric for
  the lifetime of the daemon.

Interrupt behavior:

- Backend-to-frontend interrupts are requested by writing `1` or `0` to the
  configured IRQ-control word in UIO `map0`.
- Frontend-to-backend notifications arrive as Linux UIO interrupts, so the daemon
  does not poll while idle.

The implementation currently expects exactly one registered fabric device per
daemon, matching the process model used by `virtio-blkd` and `virtio-consoled`.
