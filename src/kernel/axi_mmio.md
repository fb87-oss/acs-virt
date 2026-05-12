# axi_mmio.c

`axi_mmio.c` is a Linux kernel platform driver. It finds an `axi-mmio`
device, creates a normal Linux `virtio-mmio` child device, and gives that child
device custom DMA operations.

The short version:

- Linux still uses the standard `virtio-mmio` driver for the virtio protocol.
- This driver wraps `virtio-mmio` so DMA is restricted to a specific physical
  memory range.
- If virtio tries to DMA to memory outside that range, this driver copies the
  data through a safe temporary area inside the allowed range.

That temporary copy area is usually called a bounce buffer.

## Why This Driver Exists

Virtio devices need two things:

- MMIO registers, so the guest and device can talk about configuration and
  queue state.
- DMA memory, so the device can read and write virtqueue data.

In a normal system, the virtio device may be allowed to DMA directly into guest
RAM. In this project, the backend should only see a restricted memory aperture,
not all frontend guest RAM.

This driver enforces that rule by making virtio DMA addresses come from one
allowed memory range.

The platform device must provide these resources:

- Memory resource 0: the `virtio-mmio` register window.
- Memory resource 1: the restricted DMA aperture.
- IRQ 0: the interrupt used by the `virtio-mmio` device.

The driver matches devices with:

- Device tree compatible string: `axi,mmio`
- ACPI ID: `AXI0001`

## Main Objects

### `struct axi_mmio`

```c
struct axi_mmio {
    struct platform_device *virtio_mmio;
    struct axi_mmio_pool *pool;
};
```

This is the per-device state.

- `virtio_mmio` points to the child `virtio-mmio` platform device created by
  this driver.
- `pool` points to the restricted DMA pool used by that child device.

### `struct axi_mmio_pool`

```c
struct axi_mmio_pool {
    struct list_head node;
    struct list_head bounces;
    phys_addr_t dma_base;
    resource_size_t dma_size;
    void *dma_virt;
    unsigned long *dma_bitmap;
    unsigned long dma_pages;
    struct mutex dma_lock;
    refcount_t refs;
};
```

This describes one restricted DMA aperture.

- `dma_base` is the physical start address of the allowed DMA range.
- `dma_size` is the size of that range.
- `dma_virt` is the kernel virtual mapping of the same memory, created with
  `memremap()`.
- `dma_bitmap` tracks which pages inside the pool are currently allocated.
- `dma_pages` is the number of pages in the pool.
- `bounces` tracks active bounce mappings.
- `dma_lock` protects the bitmap and bounce list.
- `refs` allows multiple devices that use the same aperture to share one pool.

There is also a global list called `axi_mmio_pools`. It stores all shared pools.
The global mutex `axi_mmio_pools_lock` protects that list.

### `struct axi_mmio_bounce`

```c
struct axi_mmio_bounce {
    struct list_head node;
    dma_addr_t dma_handle;
    phys_addr_t phys;
    size_t size;
};
```

This records one bounced DMA mapping.

- `dma_handle` is the address inside the restricted DMA pool that the device
  sees.
- `phys` is the original physical address requested by the caller.
- `size` is the mapping size.

The driver needs this record later so it can copy data back to the original
physical address and free the temporary pool allocation.

## Restricted DMA Pool Management

The pool works like a simple page allocator.

### Creating Or Reusing A Pool

`axi_mmio_pool_get()` receives a physical base address and size.

It first checks the global pool list:

- If a matching pool already exists, it increments `refs` and returns it.
- If not, it creates a new pool.

When creating a new pool, it:

- Allocates the `struct axi_mmio_pool`.
- Stores the physical base and size.
- Calculates the number of pages.
- Maps the physical aperture into kernel virtual memory with `memremap()`.
- Allocates a bitmap with one bit per page.
- Adds the pool to the global list.

The function checks the global list twice. The second check handles a race where
another device creates the same pool after this function temporarily releases
the global lock.

### Releasing A Pool

`axi_mmio_pool_put()` decrements the pool reference count.

If other devices still use the pool, nothing else happens. If this was the last
reference, it:

- Removes the pool from the global list.
- Frees the bitmap.
- Unmaps the pool memory with `memunmap()`.
- Frees the pool object.

### Allocating Pool Memory

`axi_mmio_pool_alloc()` allocates pages from the restricted aperture.

It:

- Rounds the requested size up to whole pages.
- Searches the bitmap for a free run of pages.
- Marks those pages as used.
- Returns a DMA address inside the restricted aperture.

Example:

```text
dma_base = 0x80000000
allocated page index = 4
returned DMA address = 0x80000000 + 4 * PAGE_SIZE
```

### Freeing Pool Memory

`axi_mmio_pool_free()` does the reverse.

It:

- Checks that the DMA address is inside the pool.
- Converts the DMA address back to a page offset.
- Clears the corresponding bits in the bitmap.

## Copy Helpers

The pool is physical memory, but the CPU needs a kernel virtual address to copy
bytes. These helper functions do that work.

`axi_mmio_pool_vaddr()` converts a DMA address inside the pool into a kernel
virtual address:

```c
return pool->dma_virt + (dma_handle - pool->dma_base);
```

`axi_mmio_copy_phys_to_pool()` copies from an original physical address into the
restricted DMA pool.

`axi_mmio_copy_pool_to_phys()` copies from the restricted DMA pool back to the
original physical address.

Both functions use `memremap()` to temporarily map the original physical memory,
then use `memcpy()`.

## DMA Operations

The most important part of the file is `axi_mmio_dma_ops`:

```c
static const struct dma_map_ops axi_mmio_dma_ops = {
    .alloc = axi_mmio_dma_alloc,
    .free = axi_mmio_dma_free,
    .map_phys = axi_mmio_dma_map_phys,
    .unmap_phys = axi_mmio_dma_unmap_phys,
    .map_sg = axi_mmio_dma_map_sg,
    .unmap_sg = axi_mmio_dma_unmap_sg,
    .sync_single_for_cpu = axi_mmio_dma_sync_single_for_cpu,
    .sync_single_for_device = axi_mmio_dma_sync_single_for_device,
    .dma_supported = axi_mmio_dma_supported,
};
```

Linux calls these functions when the `virtio-mmio` child device performs DMA.
Because this driver installs these operations on the child device, the virtio
core uses this driver's restricted-pool logic instead of normal DMA mapping.

### Coherent DMA Allocation

`axi_mmio_dma_alloc()` handles coherent DMA allocation.

It:

- Gets this driver's state from the parent device.
- Allocates space from the restricted pool.
- Returns both a CPU virtual address and a DMA address.

`axi_mmio_dma_free()` frees that allocation back to the pool.

This path is useful for virtio structures that should always live inside the
restricted aperture.

### Mapping A Physical Address

`axi_mmio_dma_map_phys()` maps an existing physical address for DMA.

There are two cases.

Case 1: the requested physical address is already inside the restricted pool.

```text
caller physical address -> already safe -> return the same address
```

Case 2: the requested physical address is outside the restricted pool.

```text
caller physical address -> allocate pool memory -> copy if needed -> return pool address
```

In case 2, the driver creates an `axi_mmio_bounce` record. The device only sees
the pool address, not the original physical address.

The copy direction depends on `enum dma_data_direction`:

- `DMA_TO_DEVICE`: copy original memory into the pool before the device reads it.
- `DMA_FROM_DEVICE`: do not copy first, because the device will write new data.
- `DMA_BIDIRECTIONAL`: copy original memory into the pool first, then copy back
  later.

`axi_mmio_dma_unmap_phys()` undoes this mapping.

If the mapping was bounced, it:

- Removes the bounce record from the list.
- Copies data back to the original physical address unless the direction was
  `DMA_TO_DEVICE`.
- Frees the temporary pool allocation.
- Frees the bounce record.

### Scatter-Gather Mapping

Scatter-gather DMA is used when a single logical buffer is split across multiple
memory segments.

`axi_mmio_dma_map_sg()` handles each scatterlist entry separately:

- Allocates restricted pool memory for the entry.
- Copies data into the pool unless the direction is `DMA_FROM_DEVICE`.
- Stores the restricted DMA address in the scatterlist entry.

If one entry fails to allocate, the function frees all entries it already
mapped and returns `-ENOMEM`.

`axi_mmio_dma_unmap_sg()` reverses the process:

- Copies data back unless the direction is `DMA_TO_DEVICE`.
- Frees the pool allocation.
- Clears the DMA address and length in the scatterlist entry.

Unlike `map_phys`, the scatter-gather path does not create separate
`axi_mmio_bounce` records. The scatterlist itself stores the DMA address and
length needed for cleanup.

### Syncing DMA Data

Some DMA mappings are synchronized more than once while they are active.

`axi_mmio_dma_sync_single_for_device()` prepares data for the device. For bounced
mappings, it copies from the original physical memory into the pool unless the
direction is `DMA_FROM_DEVICE`.

`axi_mmio_dma_sync_single_for_cpu()` prepares data for the CPU. For bounced
mappings, it copies from the pool back to the original physical memory unless
the direction is `DMA_TO_DEVICE`.

These functions only affect mappings that have an `axi_mmio_bounce` record.

### DMA Mask Support

`axi_mmio_dma_supported()` returns true only for a 64-bit DMA mask:

```c
return mask == DMA_BIT_MASK(64);
```

The child device is also configured with a 64-bit coherent DMA mask during
probe.

## Probe Flow

`axi_mmio_probe()` is called when Linux finds a matching `axi,mmio` or `AXI0001`
device.

The probe function does this:

1. Reads memory resource 1, the restricted DMA aperture.
2. Allocates the driver's private `struct axi_mmio`.
3. Checks that the DMA aperture base and size are page-aligned.
4. Gets or creates an `axi_mmio_pool` for that aperture.
5. Stores the driver state with `platform_set_drvdata()`.
6. Copies memory resource 0 and IRQ 0 for the future child device.
7. Allocates a child platform device named `virtio-mmio`.
8. Sets the parent, firmware node, DMA mask, and custom DMA operations on the
   child device.
9. Adds the copied MMIO and IRQ resources to the child.
10. Registers the child device.
11. Registers cleanup actions so the child device and DMA pool are released
    automatically when the parent device goes away.

After this, the normal Linux `virtio-mmio` driver can bind to the child device.
From the virtio driver's point of view, it is just using a normal `virtio-mmio`
device. The difference is that DMA calls are intercepted by this file's custom
DMA operations.

## Resource Copying

`axi_mmio_copy_resources()` copies only the resources that the child
`virtio-mmio` device needs:

- Memory resource 0, the virtio MMIO register window.
- IRQ 0, the virtio interrupt.

It does not pass memory resource 1 to the child as a normal MMIO resource.
Resource 1 is used internally by this driver as the restricted DMA pool.

## Cleanup Model

The driver uses `devm_*` helpers where possible.

Important cleanup hooks are:

- `axi_mmio_pool_release()` releases a reference to the shared DMA pool.
- `axi_mmio_unregister_child()` unregisters the child `virtio-mmio` device.

These are registered with `devm_add_action_or_reset()`, so cleanup happens
automatically if probe fails partway through or when the parent device is
removed.

## End-To-End Example

Imagine a virtio device wants to send a buffer to the backend.

If the buffer is already inside the restricted DMA aperture:

```text
virtio buffer physical address
        |
        v
axi_mmio_dma_map_phys()
        |
        v
same address returned to device
```

If the buffer is outside the restricted DMA aperture:

```text
original guest RAM buffer
        |
        | copy
        v
temporary buffer inside restricted DMA pool
        |
        v
device receives restricted DMA address
```

When the mapping is unmapped, data is copied back if the device may have written
to it.

## Key Ideas For Junior Developers

- This file does not implement the virtio protocol itself.
- It creates a normal `virtio-mmio` child device and lets the existing Linux
  `virtio-mmio` driver handle virtio.
- Its main job is controlling where DMA memory comes from.
- The restricted DMA pool is managed with a bitmap, one bit per page.
- Bounce buffers are used when the original memory is outside the allowed DMA
  range.
- Direction matters: data is copied only when the CPU or device needs to see the
  latest version.
- The global pool list allows devices with the same DMA aperture to share one
  pool safely.

## Important Limitations To Notice

- Allocations are page-granular, so small DMA mappings still consume at least
  one page in the restricted pool.
- The pool allocator is simple: it searches the bitmap for a contiguous free
  range. It does not compact or defragment memory.
- `memremap()` and `memcpy()` are used for bouncing, so bounced DMA has extra
  CPU overhead.
- If the restricted pool is too small, DMA mapping can fail.
