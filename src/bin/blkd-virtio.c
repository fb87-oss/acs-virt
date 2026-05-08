#include "blkd.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIRTIO_MAGIC 0x74726976u
#define VIRTIO_VERSION 2u
#define VIRTIO_DEVICE_ID_BLOCK 2u
#define VIRTIO_VENDOR_ID 0x43484950u
#define VIRTQ_DESC_F_NEXT 1u
#define VIRTQ_DESC_F_WRITE 2u
#define VIRTIO_BLK_T_IN 0u
#define VIRTIO_BLK_T_OUT 1u
#define VIRTIO_BLK_T_FLUSH 4u
#define VIRTIO_BLK_S_OK 0u
#define VIRTIO_BLK_S_IOERR 1u
#define VIRTIO_BLK_S_UNSUPP 2u

struct descriptor {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

static uint32_t device_features(const struct blkd_virtio_device *dev)
{
    switch (dev->device_features_sel) {
    case 0:
        return (1u << 6) | (1u << 9);
    case 1:
        return 1u;
    default:
        return 0;
    }
}

static void reset_runtime_state(struct blkd_virtio_device *dev)
{
    dev->device_features_sel = 0;
    dev->driver_features_sel = 0;
    dev->driver_features[0] = 0;
    dev->driver_features[1] = 0;
    dev->queue_sel = 0;
    dev->queue_num = BLKD_QUEUE_SIZE;
    dev->queue_ready = 0;
    dev->queue_desc = 0;
    dev->queue_driver = 0;
    dev->queue_device = 0;
    dev->last_avail_idx = 0;
    dev->status = 0;
    dev->interrupt_status = 0;
}

void blkd_virtio_init(struct blkd_virtio_device *dev, struct blkd_block_backend *backend)
{
    memset(dev, 0, sizeof(*dev));
    dev->backend = backend;
    dev->capacity_sectors = backend->image_len / BLKD_SECTOR_SIZE;
    reset_runtime_state(dev);
}

uint64_t blkd_virtio_mmio_read(const struct blkd_virtio_device *dev, uint64_t offset, uint32_t len)
{
    uint64_t value;

    switch (offset) {
    case 0x000:
        value = VIRTIO_MAGIC;
        break;
    case 0x004:
        value = VIRTIO_VERSION;
        break;
    case 0x008:
        value = VIRTIO_DEVICE_ID_BLOCK;
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
        value = BLKD_QUEUE_SIZE;
        break;
    case 0x038:
        value = dev->queue_num;
        break;
    case 0x044:
        value = dev->queue_ready;
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
        value = (uint32_t)dev->capacity_sectors;
        break;
    case 0x104:
        value = (uint32_t)(dev->capacity_sectors >> 32);
        break;
    case 0x114:
        value = BLKD_SECTOR_SIZE;
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

void blkd_virtio_mmio_write(struct blkd_virtio_device *dev, uint64_t offset, uint64_t raw_value)
{
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
        dev->queue_num = value;
        break;
    case 0x044:
        dev->queue_ready = value;
        break;
    case 0x080:
        dev->queue_desc = (dev->queue_desc & ~0xffffffffull) | value;
        break;
    case 0x084:
        dev->queue_desc = (dev->queue_desc & 0xffffffffull) | ((uint64_t)value << 32);
        break;
    case 0x090:
        dev->queue_driver = (dev->queue_driver & ~0xffffffffull) | value;
        break;
    case 0x094:
        dev->queue_driver = (dev->queue_driver & 0xffffffffull) | ((uint64_t)value << 32);
        break;
    case 0x0a0:
        dev->queue_device = (dev->queue_device & ~0xffffffffull) | value;
        break;
    case 0x0a4:
        dev->queue_device = (dev->queue_device & 0xffffffffull) | ((uint64_t)value << 32);
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

static bool read_desc(struct blkd_virtio_device *dev, int axi_fd, uint16_t index, struct descriptor *desc)
{
    uint8_t *data = NULL;
    if (!blkd_axi_dma_read(axi_fd, dev->queue_desc + ((uint64_t)index * 16), 16, &data)) {
        return false;
    }

    desc->addr = blkd_load_le64(data);
    desc->len = blkd_load_le32(data + 8);
    desc->flags = blkd_load_le16(data + 12);
    desc->next = blkd_load_le16(data + 14);
    free(data);
    return true;
}

static bool add_used(struct blkd_virtio_device *dev, int axi_fd, uint16_t head, uint32_t len)
{
    uint16_t used_idx;
    uint64_t elem;
    uint8_t buf[4];

    if (!blkd_axi_dma_read_u16(axi_fd, dev->queue_device + 2, &used_idx)) {
        return false;
    }

    elem = dev->queue_device + 4 + ((uint64_t)(used_idx % (uint16_t)dev->queue_num) * 8);
    blkd_store_le32(buf, head);
    if (!blkd_axi_dma_write(axi_fd, elem, buf, 4)) {
        return false;
    }
    blkd_store_le32(buf, len);
    if (!blkd_axi_dma_write(axi_fd, elem + 4, buf, 4)) {
        return false;
    }
    blkd_store_le16(buf, used_idx + 1);
    if (!blkd_axi_dma_write(axi_fd, dev->queue_device + 2, buf, 2)) {
        return false;
    }
    return true;
}

static bool process_chain(struct blkd_virtio_device *dev, int axi_fd, uint16_t head, uint32_t *used_len)
{
    struct descriptor header_desc;
    struct descriptor status_desc;
    uint8_t *header = NULL;
    uint8_t status = VIRTIO_BLK_S_OK;
    uint32_t request_type;
    uint64_t sector;
    uint64_t offset;
    uint32_t data_len = 0;
    uint16_t index;
    uint32_t chain_seen = 0;

    fprintf(stderr, "blkd: read descriptor chain head=%u\n", head);

    if (!read_desc(dev, axi_fd, head, &header_desc) ||
        !(header_desc.flags & VIRTQ_DESC_F_NEXT) ||
        !blkd_axi_dma_read(axi_fd, header_desc.addr, 16, &header)) {
        return false;
    }

    request_type = blkd_load_le32(header);
    sector = blkd_load_le64(header + 8);
    free(header);

    if (sector > UINT64_MAX / BLKD_SECTOR_SIZE) {
        status = VIRTIO_BLK_S_IOERR;
        offset = 0;
    } else {
        offset = sector * BLKD_SECTOR_SIZE;
    }

    if (request_type != VIRTIO_BLK_T_IN &&
        request_type != VIRTIO_BLK_T_OUT &&
        request_type != VIRTIO_BLK_T_FLUSH) {
        status = VIRTIO_BLK_S_UNSUPP;
    }

    index = header_desc.next;
    for (;;) {
        struct descriptor desc;
        uint8_t *data = NULL;
        bool last;

        if (++chain_seen > dev->queue_num || !read_desc(dev, axi_fd, index, &desc)) {
            return false;
        }

        last = !(desc.flags & VIRTQ_DESC_F_NEXT);
        if (last) {
            status_desc = desc;
            break;
        }

        if (request_type == VIRTIO_BLK_T_IN) {
            if (!(desc.flags & VIRTQ_DESC_F_WRITE)) {
                status = VIRTIO_BLK_S_IOERR;
            } else if (status == VIRTIO_BLK_S_OK) {
                data = calloc(1, desc.len);
                if (!data || !blkd_block_read(dev->backend, offset + data_len, data, desc.len) ||
                    !blkd_axi_dma_write(axi_fd, desc.addr, data, desc.len)) {
                    status = VIRTIO_BLK_S_IOERR;
                }
                free(data);
            }
            data_len += desc.len;
        } else if (request_type == VIRTIO_BLK_T_OUT) {
            if (desc.flags & VIRTQ_DESC_F_WRITE) {
                status = VIRTIO_BLK_S_IOERR;
            } else if (status == VIRTIO_BLK_S_OK) {
                if (!blkd_axi_dma_read(axi_fd, desc.addr, desc.len, &data)) {
                    return false;
                }
                if (!blkd_block_write(dev->backend, offset + data_len, data, desc.len)) {
                    status = VIRTIO_BLK_S_IOERR;
                }
                free(data);
            }
            data_len += desc.len;
        }

        index = desc.next;
    }

    if (request_type == VIRTIO_BLK_T_FLUSH && !blkd_block_flush(dev->backend)) {
        status = VIRTIO_BLK_S_IOERR;
    }

    fprintf(stderr, "blkd: request type=%u sector=%" PRIu64 " data_len=%u status=%u\n",
            request_type, sector, data_len, status);

    *used_len = request_type == VIRTIO_BLK_T_IN ? data_len + 1 : 1;
    return blkd_axi_dma_write(axi_fd, status_desc.addr, &status, 1);
}

bool blkd_virtio_notify_queue(struct blkd_virtio_device *dev, int axi_fd, uint32_t queue)
{
    bool used_any = false;

    fprintf(stderr, "blkd: notify queue=%u\n", queue);
    if (queue != 0 || !dev->queue_ready) {
        return true;
    }

    for (;;) {
        uint16_t avail_idx;
        uint64_t ring_off;
        uint16_t head;
        uint32_t used_len;

        if (!blkd_axi_dma_read_u16(axi_fd, dev->queue_driver + 2, &avail_idx)) {
            return false;
        }
        if (dev->last_avail_idx == avail_idx) {
            break;
        }

        ring_off = 4 + ((uint64_t)(dev->last_avail_idx % (uint16_t)dev->queue_num) * 2);
        if (!blkd_axi_dma_read_u16(axi_fd, dev->queue_driver + ring_off, &head)) {
            return false;
        }

        fprintf(stderr, "blkd: process head=%u\n", head);
        if (!process_chain(dev, axi_fd, head, &used_len) || !add_used(dev, axi_fd, head, used_len)) {
            return false;
        }
        dev->last_avail_idx++;
        used_any = true;
    }

    if (used_any) {
        dev->interrupt_status |= 1;
        if (!blkd_axi_raise_irq(axi_fd)) {
            return false;
        }
    }

    return true;
}
