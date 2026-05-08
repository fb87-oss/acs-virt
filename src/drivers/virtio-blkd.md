# virtio-blkd.c

`virtio-blkd.c` implements the host-side virtio-blk daemon.

It combines three layers that are deployed as one daemon binary:

- block image backend operations
- transport-independent virtio-blk device semantics
- the current `virt-axi` plus `virtio-mmio` binding used by QEMU

Block backend responsibilities:

- open the configured disk image read-only or read-write
- record image length and expose capacity in 512-byte sectors
- perform complete positioned reads and writes with `pread`/`pwrite`
- reject out-of-bounds accesses, writes to read-only images, and offsets too large for `off_t`
- flush data with `fsync` for `VIRTIO_BLK_T_FLUSH`

Virtio-blk responsibilities:

- advertise `VIRTIO_BLK_F_BLK_SIZE`, `VIRTIO_BLK_F_FLUSH`, and `VIRTIO_F_VERSION_1`
- expose `capacity` and `blk_size` through the virtio config space callback
- initialize one split virtqueue with `BLKD_QUEUE_SIZE` entries
- process request descriptor chains from queue 0
- decode the 16-byte virtio-blk request header from guest memory
- support read (`VIRTIO_BLK_T_IN`), write (`VIRTIO_BLK_T_OUT`), and flush requests
- reject unsupported request types with `VIRTIO_BLK_S_UNSUPP`
- write the one-byte virtio-blk status descriptor back to guest memory
- publish used-ring entries and raise a virtqueue interrupt when work completes

Fabric and MMIO binding responsibilities:

- create a `struct virtio_mmio` transport over the `struct virtio_device`
- register a `struct virt_axi_device` with a 0x200-byte MMIO aperture
- route `virt-axi` MMIO reads and writes through `virtio_mmio_read` and `virtio_mmio_write`
- lower the external interrupt line on device reset and interrupt acknowledgement
- dispatch `VIRTIO_MMIO_QUEUE_NOTIFY` to the virtio-blk queue handler

Daemon responsibilities:

- parse launcher-provided `key=value` arguments
- require `socket` and `image`
- accept optional `readonly` and `ram_access` values
- start the `virt-axi` socket loop for this one device

Important boundaries:

- QEMU owns guest RAM and services DMA requests; this daemon asks for DMA through `virt_axi_dma_*`
- `virtio-mmio.c` owns register-level transport state; this file only binds notify and IRQ side effects
- `virtio.c` owns generic split-ring helpers; this file owns block-specific chain validation and I/O
- Linux guest drivers remain unchanged and see a normal virtio-mmio block device
