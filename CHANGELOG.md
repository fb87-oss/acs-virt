# Changelog

## 2026-05-09

- Added a configurable QEMU `axi.notify-delay-us` property for UIO frontend notifications.
- Tuned UIO orchestration to use a shorter notify delay for AArch64 frontends while preserving the stable x86_64 frontend delay.
- Improved UIO block throughput by advertising larger virtio-blk segment limits, tuning the maximum segment size to 256KiB, adding direct DMA reads into caller buffers, and using QEMU notify acknowledgements where stable.
- Added opt-in UIO block backend profiling with per-request timing for descriptor processing, guest DMA, image I/O, used-ring updates, and IRQ signaling.
- Added an opt-in direct UIO read-DMA path so experiments can read block-image data directly into mapped frontend RAM without the reusable payload buffer.
- Added `BENCH_REPEAT` support to UIO benchmark wrappers with per-run output and min/average/max throughput summaries.
- Updated Markdown docs and the runtime JSON Schema to reflect the current socket, UIO, profiling, direct-DMA, and repeated-benchmark paths.
- Renamed backend fabric terminology around `qemu-socket`, `linux-uio`, and `linux-devmem` to clarify that both active topologies use QEMU `axi`.

## 2026-05-08

- Added target-aware QEMU build support for x86_64 microvm and AArch64 virt, including a shared `scripts/build-qemu.sh` helper and architecture-specific wrappers.
- Moved runtime configs to architecture-explicit samples: `samples/axi-x64.toml` and `samples/axi-a64.toml`.
- Added AArch64 `axi` QEMU integration with FDT `virtio,mmio` discovery, dynamic sysbus allowlisting, and an `axi` device config.
- Updated the launcher to support both `microvm` and AArch64 `virt` machine types.
- Added runtime config docs/schema and AArch64 smoke coverage.
- Kept both samples on the common AXI MMIO aperture at `0xfeb00000` and `0xfeb00200`, with target-specific IRQ wiring.
- Renamed the emulated AXI transport from `axi-bus` to `axi` across QEMU patches, samples, launcher output, docs, tests, and backend source.
- Reorganized C backend source into generic virtio core/transport files, backend fabric implementations under `src/fabrics/`, and driver daemons under `src/drivers/`.
- Merged split block and console backend implementations into `virtio-blkd` and `virtio-consoled` driver sources with updated source docs and Doxygen comments.
- Added clang-format configuration for 4-space, spaces-only C formatting and reformatted the `src` tree.
- Added a generic backend `fabric.h` API and `CHIPLETS_BACKEND_FABRIC` CMake option so `axi` and Linux `/dev/mem` fabrics can be selected without changing virtio or driver code.
- Implemented the Linux `/dev/mem` fabric for memory-mapped virtio-mmio apertures, QEMU-independent DMA via physical memory mappings, and optional IRQ control-register writes.
