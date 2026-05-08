#include "blkd.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static bool io_all(int fd, void *buf, size_t len, bool write_op)
{
    uint8_t *pos = buf;

    while (len) {
        ssize_t ret = write_op ? write(fd, pos, len) : read(fd, pos, len);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (ret == 0) {
            return false;
        }
        pos += ret;
        len -= (size_t)ret;
    }

    return true;
}

static bool read_header(int fd, struct blkd_header *h)
{
    uint8_t buf[BLKD_HEADER_LEN];
    if (!io_all(fd, buf, sizeof(buf), false)) {
        return false;
    }

    h->kind = blkd_load_le16(buf);
    h->flags = blkd_load_le16(buf + 2);
    h->window_id = blkd_load_le32(buf + 4);
    h->offset = blkd_load_le64(buf + 8);
    h->length = blkd_load_le32(buf + 16);
    return true;
}

static bool write_header(int fd, const struct blkd_header *h)
{
    uint8_t buf[BLKD_HEADER_LEN] = {0};
    blkd_store_le16(buf, h->kind);
    blkd_store_le16(buf + 2, h->flags);
    blkd_store_le32(buf + 4, h->window_id);
    blkd_store_le64(buf + 8, h->offset);
    blkd_store_le32(buf + 16, h->length);
    return io_all(fd, buf, sizeof(buf), true);
}

static bool write_read_reply(int fd, const struct blkd_header *request, uint64_t value)
{
    uint8_t bytes[8];
    uint32_t len = request->length < 8 ? request->length : 8;
    struct blkd_header reply = {
        .kind = BLKD_MSG_MMIO_READ_REPLY,
        .flags = request->flags,
        .window_id = request->window_id,
        .offset = request->offset,
        .length = len,
    };

    blkd_store_le64(bytes, value);
    return write_header(fd, &reply) && io_all(fd, bytes, len, true);
}

static bool read_value(int fd, uint32_t len, uint64_t *value)
{
    uint8_t bytes[8] = {0};
    if (len > sizeof(bytes)) {
        return false;
    }
    if (!io_all(fd, bytes, len, false)) {
        return false;
    }
    *value = blkd_load_le64(bytes);
    return true;
}

bool blkd_axi_dma_read(int fd, uint64_t gpa, uint32_t len, uint8_t **data)
{
    struct blkd_header request = {
        .kind = BLKD_MSG_DMA_READ,
        .flags = 0,
        .window_id = 0,
        .offset = gpa,
        .length = len,
    };
    struct blkd_header reply;
    uint8_t *buf = NULL;

    if (!write_header(fd, &request) || !read_header(fd, &reply)) {
        return false;
    }
    if (reply.kind != BLKD_MSG_DMA_READ_REPLY || reply.length != len) {
        return false;
    }

    if (len) {
        buf = malloc(len);
        if (!buf) {
            return false;
        }
        if (!io_all(fd, buf, len, false)) {
            free(buf);
            return false;
        }
    }

    *data = buf;
    return true;
}

bool blkd_axi_dma_read_u16(int fd, uint64_t gpa, uint16_t *value)
{
    uint8_t *data = NULL;
    if (!blkd_axi_dma_read(fd, gpa, 2, &data)) {
        return false;
    }
    *value = blkd_load_le16(data);
    free(data);
    return true;
}

bool blkd_axi_dma_write(int fd, uint64_t gpa, const void *data, uint32_t len)
{
    struct blkd_header request = {
        .kind = BLKD_MSG_DMA_WRITE,
        .flags = 0,
        .window_id = 0,
        .offset = gpa,
        .length = len,
    };
    struct blkd_header reply;

    if (!write_header(fd, &request)) {
        return false;
    }
    if (len && !io_all(fd, (void *)data, len, true)) {
        return false;
    }
    if (!read_header(fd, &reply)) {
        return false;
    }
    return reply.kind == BLKD_MSG_ERROR;
}

bool blkd_axi_raise_irq(int fd)
{
    struct blkd_header irq = {
        .kind = BLKD_MSG_IRQ_ASSERT,
        .flags = 0,
        .window_id = 0,
        .offset = 0,
        .length = 0,
    };

    return blkd_axi_lower_irq(fd) && write_header(fd, &irq);
}

bool blkd_axi_lower_irq(int fd)
{
    struct blkd_header irq = {
        .kind = BLKD_MSG_IRQ_DEASSERT,
        .flags = 0,
        .window_id = 0,
        .offset = 0,
        .length = 0,
    };

    return write_header(fd, &irq);
}

bool blkd_axi_serve(int fd, struct blkd_virtio_device *dev)
{
    for (;;) {
        struct blkd_header h;
        if (!read_header(fd, &h)) {
            return false;
        }

        switch (h.kind) {
        case BLKD_MSG_MMIO_READ: {
            uint64_t value = blkd_virtio_mmio_read(dev, h.offset, h.length);
            fprintf(stderr, "blkd: read offset=0x%" PRIx64 " len=%u -> 0x%" PRIx64 "\n",
                    h.offset, h.length, value);
            if (!write_read_reply(fd, &h, value)) {
                return false;
            }
            break;
        }
        case BLKD_MSG_MMIO_WRITE: {
            uint64_t value;
            struct blkd_header ack = {
                .kind = BLKD_MSG_ERROR,
                .flags = 0,
                .window_id = 0,
                .offset = 0,
                .length = 0,
            };
            if (!read_value(fd, h.length, &value)) {
                return false;
            }
            fprintf(stderr, "blkd: write offset=0x%" PRIx64 " len=%u value=0x%" PRIx64 "\n",
                    h.offset, h.length, value);
            blkd_virtio_mmio_write(dev, h.offset, value);
            if (h.offset == 0x070 && value == 0 && !blkd_axi_lower_irq(fd)) {
                return false;
            }
            if (h.offset == 0x064 && !blkd_axi_lower_irq(fd)) {
                return false;
            }
            if (h.offset == 0x050 && !blkd_virtio_notify_queue(dev, fd, (uint32_t)value)) {
                return false;
            }
            if (!write_header(fd, &ack)) {
                return false;
            }
            break;
        }
        default:
            fprintf(stderr, "blkd: unsupported message kind=%u\n", h.kind);
            return false;
        }
    }
}
