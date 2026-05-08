#include "cond.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIRTIO_MAGIC 0x74726976u
#define VIRTIO_VERSION 2u
#define VIRTIO_DEVICE_ID_CONSOLE 3u
#define VIRTIO_VENDOR_ID 0x43484950u
#define VIRTQ_DESC_F_NEXT 1u
#define VIRTQ_DESC_F_WRITE 2u

struct descriptor {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

static struct cond_queue *selected_queue(struct cond_virtio_device *dev)
{
    if (dev->queue_sel >= COND_QUEUE_COUNT) {
        return NULL;
    }
    return &dev->queues[dev->queue_sel];
}

static const struct cond_queue *selected_queue_const(const struct cond_virtio_device *dev)
{
    if (dev->queue_sel >= COND_QUEUE_COUNT) {
        return NULL;
    }
    return &dev->queues[dev->queue_sel];
}

static uint32_t device_features(const struct cond_virtio_device *dev)
{
    switch (dev->device_features_sel) {
    case 0:
        return 1u;
    case 1:
        return 1u;
    default:
        return 0;
    }
}

static void reset_runtime_state(struct cond_virtio_device *dev)
{
    dev->device_features_sel = 0;
    dev->driver_features_sel = 0;
    dev->driver_features[0] = 0;
    dev->driver_features[1] = 0;
    dev->queue_sel = 0;
    for (uint32_t i = 0; i < COND_QUEUE_COUNT; i++) {
        dev->queues[i].num = COND_QUEUE_SIZE;
        dev->queues[i].ready = 0;
        dev->queues[i].desc = 0;
        dev->queues[i].driver = 0;
        dev->queues[i].device = 0;
        dev->queues[i].last_avail_idx = 0;
    }
    dev->status = 0;
    dev->interrupt_status = 0;
}

void cond_virtio_init(struct cond_virtio_device *dev, struct cond_console_backend *backend)
{
    memset(dev, 0, sizeof(*dev));
    dev->backend = backend;
    reset_runtime_state(dev);
}

uint64_t cond_virtio_mmio_read(const struct cond_virtio_device *dev, uint64_t offset, uint32_t len)
{
    const struct cond_queue *queue = selected_queue_const(dev);
    uint64_t value;

    switch (offset) {
    case 0x000:
        value = VIRTIO_MAGIC;
        break;
    case 0x004:
        value = VIRTIO_VERSION;
        break;
    case 0x008:
        value = VIRTIO_DEVICE_ID_CONSOLE;
        break;
    case 0x00c:
        value = VIRTIO_VENDOR_ID;
        break;
    case 0x010:
        value = device_features(dev);
        break;
    case 0x014:
        value = dev->device_features_sel;
        break;
    case 0x020:
        value = dev->driver_features[dev->driver_features_sel < 2 ? dev->driver_features_sel : 1];
        break;
    case 0x024:
        value = dev->driver_features_sel;
        break;
    case 0x030:
        value = dev->queue_sel;
        break;
    case 0x034:
        value = dev->queue_sel < COND_QUEUE_COUNT ? COND_QUEUE_SIZE : 0;
        break;
    case 0x038:
        value = queue ? queue->num : 0;
        break;
    case 0x044:
        value = queue ? queue->ready : 0;
        break;
    case 0x060:
        value = dev->interrupt_status;
        break;
    case 0x070:
        value = dev->status;
        break;
    case 0x0fc:
        value = 0;
        break;
    case 0x100:
        value = 80;
        break;
    case 0x102:
        value = 25;
        break;
    default:
        value = 0;
        break;
    }

    switch (len) {
    case 1:
        return value & 0xff;
    case 2:
        return value & 0xffff;
    case 4:
        return value & 0xffffffff;
    default:
        return value;
    }
}

void cond_virtio_mmio_write(struct cond_virtio_device *dev, uint64_t offset, uint64_t raw_value)
{
    struct cond_queue *queue = selected_queue(dev);
    uint32_t value = (uint32_t)raw_value;

    switch (offset) {
    case 0x014:
        dev->device_features_sel = value;
        break;
    case 0x020:
        dev->driver_features[dev->driver_features_sel < 2 ? dev->driver_features_sel : 1] = value;
        break;
    case 0x024:
        dev->driver_features_sel = value;
        break;
    case 0x030:
        dev->queue_sel = value;
        break;
    case 0x038:
        if (queue) {
            queue->num = value;
        }
        break;
    case 0x044:
        if (queue) {
            queue->ready = value;
        }
        break;
    case 0x080:
        if (queue) {
            queue->desc = (queue->desc & ~0xffffffffull) | value;
        }
        break;
    case 0x084:
        if (queue) {
            queue->desc = (queue->desc & 0xffffffffull) | ((uint64_t)value << 32);
        }
        break;
    case 0x090:
        if (queue) {
            queue->driver = (queue->driver & ~0xffffffffull) | value;
        }
        break;
    case 0x094:
        if (queue) {
            queue->driver = (queue->driver & 0xffffffffull) | ((uint64_t)value << 32);
        }
        break;
    case 0x0a0:
        if (queue) {
            queue->device = (queue->device & ~0xffffffffull) | value;
        }
        break;
    case 0x0a4:
        if (queue) {
            queue->device = (queue->device & 0xffffffffull) | ((uint64_t)value << 32);
        }
        break;
    case 0x064:
        dev->interrupt_status &= ~value;
        break;
    case 0x070:
        if (value == 0) {
            reset_runtime_state(dev);
        } else {
            dev->status = value;
        }
        break;
    default:
        break;
    }
}

static bool read_desc(struct cond_queue *queue, int axi_fd, uint16_t index, struct descriptor *desc)
{
    uint8_t *data = NULL;
    if (!cond_axi_dma_read(axi_fd, queue->desc + ((uint64_t)index * 16), 16, &data)) {
        return false;
    }

    desc->addr = cond_load_le64(data);
    desc->len = cond_load_le32(data + 8);
    desc->flags = cond_load_le16(data + 12);
    desc->next = cond_load_le16(data + 14);
    free(data);
    return true;
}

static bool add_used(struct cond_queue *queue, int axi_fd, uint16_t head, uint32_t len)
{
    uint16_t used_idx;
    uint64_t elem;
    uint8_t buf[4];

    if (!cond_axi_dma_read_u16(axi_fd, queue->device + 2, &used_idx)) {
        return false;
    }

    elem = queue->device + 4 + ((uint64_t)(used_idx % (uint16_t)queue->num) * 8);
    cond_store_le32(buf, head);
    if (!cond_axi_dma_write(axi_fd, elem, buf, 4)) {
        return false;
    }
    cond_store_le32(buf, len);
    if (!cond_axi_dma_write(axi_fd, elem + 4, buf, 4)) {
        return false;
    }
    cond_store_le16(buf, used_idx + 1);
    return cond_axi_dma_write(axi_fd, queue->device + 2, buf, 2);
}

static bool process_tx_chain(struct cond_virtio_device *dev, struct cond_queue *queue, int axi_fd, uint16_t head, uint32_t *used_len)
{
    struct descriptor desc;
    uint16_t index = head;
    uint32_t total = 0;

    for (;;) {
        uint8_t *data = NULL;
        if (!read_desc(queue, axi_fd, index, &desc)) {
            return false;
        }
        if (!(desc.flags & VIRTQ_DESC_F_WRITE) && desc.len) {
            if (!cond_axi_dma_read(axi_fd, desc.addr, desc.len, &data)) {
                return false;
            }
            if (!cond_console_write(dev->backend, data, desc.len)) {
                free(data);
                return false;
            }
            total += desc.len;
            free(data);
        }
        if (!(desc.flags & VIRTQ_DESC_F_NEXT)) {
            break;
        }
        index = desc.next;
    }

    *used_len = total;
    return true;
}

bool cond_virtio_notify_queue(struct cond_virtio_device *dev, int axi_fd, uint32_t queue_index)
{
    struct cond_queue *queue;
    bool used_any = false;

    fprintf(stderr, "cond: notify queue=%u\n", queue_index);
    if (queue_index >= COND_QUEUE_COUNT) {
        return true;
    }
    queue = &dev->queues[queue_index];
    if (!queue->ready) {
        return true;
    }

    if (queue_index == 0) {
        /* No host input source exists yet, so keep guest RX buffers pending. */
        return true;
    }

    for (;;) {
        uint16_t avail_idx;
        uint64_t ring_off;
        uint16_t head;
        uint32_t used_len = 0;

        if (!cond_axi_dma_read_u16(axi_fd, queue->driver + 2, &avail_idx)) {
            return false;
        }
        if (queue->last_avail_idx == avail_idx) {
            break;
        }

        ring_off = 4 + ((uint64_t)(queue->last_avail_idx % (uint16_t)queue->num) * 2);
        if (!cond_axi_dma_read_u16(axi_fd, queue->driver + ring_off, &head)) {
            return false;
        }

        fprintf(stderr, "cond: process queue=%u head=%u\n", queue_index, head);
        if (!process_tx_chain(dev, queue, axi_fd, head, &used_len)) {
            return false;
        }
        if (!add_used(queue, axi_fd, head, used_len)) {
            return false;
        }
        queue->last_avail_idx++;
        used_any = true;
    }

    if (used_any) {
        dev->interrupt_status |= 1;
        if (!cond_axi_raise_irq(axi_fd)) {
            return false;
        }
    }

    return true;
}
