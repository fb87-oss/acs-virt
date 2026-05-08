#include "virtio.h"

#include <stdlib.h>

/**
 * @brief Resets generic virtio state and every configured queue.
 *
 * @param dev Device state to reset.
 */
void virtio_device_reset(struct virtio_device *dev) {
    dev->driver_features = 0;
    for (uint32_t i = 0; i < dev->queue_count; i++) {
        dev->queues[i].num = dev->queue_size;
        dev->queues[i].ready = 0;
        dev->queues[i].desc = 0;
        dev->queues[i].avail = 0;
        dev->queues[i].used = 0;
        dev->queues[i].last_avail_idx = 0;
    }
    dev->status = 0;
    dev->interrupt_status = 0;
    dev->config_generation = 0;
    if (dev->ops && dev->ops->reset) {
        dev->ops->reset(dev->opaque);
    }
}

/**
 * @brief Populates generic virtio device metadata and resets runtime state.
 *
 * @param dev Device state to initialize.
 * @param device_id Linux virtio device id to expose.
 * @param vendor_id Vendor id to expose through the transport.
 * @param queue_count Number of queues in the queues array.
 * @param queue_size Default maximum size for each queue.
 * @param queues Driver-owned queue storage.
 * @param opaque Device-specific callback state.
 * @param ops Device-specific operation table.
 */
void virtio_device_init(struct virtio_device *dev, uint32_t device_id,
                        uint32_t vendor_id, uint32_t queue_count,
                        uint32_t queue_size, struct virtio_queue *queues,
                        void *opaque, const struct virtio_device_ops *ops) {
    dev->device_id = device_id;
    dev->vendor_id = vendor_id;
    dev->queue_count = queue_count;
    dev->queue_size = queue_size;
    dev->queues = queues;
    dev->opaque = opaque;
    dev->ops = ops;
    virtio_device_reset(dev);
}

/**
 * @brief Reads one split-ring descriptor from guest memory.
 *
 * @param queue Queue containing the descriptor table address.
 * @param io Active fabric I/O context.
 * @param dma_read DMA callback used to fetch descriptor bytes.
 * @param index Descriptor index to read.
 * @param desc Output descriptor structure.
 * @return bool True on success, false on DMA failure.
 */
bool virtio_read_desc(const struct virtio_queue *queue, struct virt_axi_io *io,
                      virtio_dma_read_fn dma_read, uint16_t index,
                      struct virtio_desc *desc) {
    uint8_t *data = NULL;

    if (!dma_read(io, queue->desc + ((uint64_t)index * 16), 16, &data)) {
        return false;
    }

    desc->addr = virtio_load_le64(data);
    desc->len = virtio_load_le32(data + 8);
    desc->flags = virtio_load_le16(data + 12);
    desc->next = virtio_load_le16(data + 14);
    free(data);
    return true;
}

/**
 * @brief Writes a used-ring element for a completed descriptor chain.
 *
 * @param queue Queue containing the used ring address.
 * @param io Active fabric I/O context.
 * @param dma_read_u16 DMA callback used to read the current used index.
 * @param dma_write DMA callback used to write the used element and index.
 * @param head Descriptor chain head index being completed.
 * @param len Number of bytes written by the device for this chain.
 * @return bool True on success, false on DMA failure.
 */
bool virtio_add_used(const struct virtio_queue *queue, struct virt_axi_io *io,
                     virtio_dma_read_u16_fn dma_read_u16,
                     virtio_dma_write_fn dma_write, uint16_t head,
                     uint32_t len) {
    uint16_t used_idx;
    uint64_t elem;
    uint8_t buf[4];

    if (!dma_read_u16(io, queue->used + 2, &used_idx)) {
        return false;
    }

    elem = queue->used + 4 + ((uint64_t)(used_idx % (uint16_t)queue->num) * 8);
    virtio_store_le32(buf, head);
    if (!dma_write(io, elem, buf, 4)) {
        return false;
    }
    virtio_store_le32(buf, len);
    if (!dma_write(io, elem + 4, buf, 4)) {
        return false;
    }
    virtio_store_le16(buf, used_idx + 1);
    return dma_write(io, queue->used + 2, buf, 2);
}

/**
 * @brief Reads the next available-ring head without advancing last_avail_idx.
 *
 * @param queue Queue containing the available ring address and backend cursor.
 * @param io Active fabric I/O context.
 * @param dma_read_u16 DMA callback used to read available ring fields.
 * @param head Output descriptor chain head index when work is available.
 * @param available Output flag indicating whether a descriptor is available.
 * @return bool True on success, false on DMA failure.
 */
bool virtio_next_avail(const struct virtio_queue *queue, struct virt_axi_io *io,
                       virtio_dma_read_u16_fn dma_read_u16, uint16_t *head,
                       bool *available) {
    uint16_t avail_idx;
    uint64_t ring_off;

    if (!dma_read_u16(io, queue->avail + 2, &avail_idx)) {
        return false;
    }
    if (queue->last_avail_idx == avail_idx) {
        *available = false;
        return true;
    }

    ring_off =
        4 + ((uint64_t)(queue->last_avail_idx % (uint16_t)queue->num) * 2);
    if (!dma_read_u16(io, queue->avail + ring_off, head)) {
        return false;
    }

    *available = true;
    return true;
}
