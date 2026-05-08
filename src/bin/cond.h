#ifndef COND_H
#define COND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COND_MSG_MMIO_READ 3
#define COND_MSG_MMIO_READ_REPLY 4
#define COND_MSG_MMIO_WRITE 5
#define COND_MSG_IRQ_ASSERT 6
#define COND_MSG_IRQ_DEASSERT 7
#define COND_MSG_DMA_READ 8
#define COND_MSG_DMA_READ_REPLY 9
#define COND_MSG_DMA_WRITE 10
#define COND_MSG_ERROR 0xffff

#define COND_HEADER_LEN 24
#define COND_QUEUE_SIZE 256u
#define COND_QUEUE_COUNT 2u

struct cond_config {
    char socket[4096];
    char ram_access[128];
    char output[4096];
};

struct cond_header {
    uint16_t kind;
    uint16_t flags;
    uint32_t window_id;
    uint64_t offset;
    uint32_t length;
};

struct cond_console_backend {
    int fd;
};

struct cond_queue {
    uint32_t num;
    uint32_t ready;
    uint64_t desc;
    uint64_t driver;
    uint64_t device;
    uint16_t last_avail_idx;
};

struct cond_virtio_device {
    struct cond_console_backend *backend;
    uint32_t device_features_sel;
    uint32_t driver_features_sel;
    uint32_t driver_features[2];
    uint32_t queue_sel;
    struct cond_queue queues[COND_QUEUE_COUNT];
    uint32_t status;
    uint32_t interrupt_status;
};

static inline uint16_t cond_load_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t cond_load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline uint64_t cond_load_le64(const uint8_t *p)
{
    return (uint64_t)cond_load_le32(p) | ((uint64_t)cond_load_le32(p + 4) << 32);
}

static inline void cond_store_le16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}

static inline void cond_store_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

static inline void cond_store_le64(uint8_t *p, uint64_t v)
{
    cond_store_le32(p, (uint32_t)v);
    cond_store_le32(p + 4, (uint32_t)(v >> 32));
}

bool cond_console_open(struct cond_console_backend *backend, const char *path);
void cond_console_close(struct cond_console_backend *backend);
bool cond_console_write(struct cond_console_backend *backend, const void *buf, size_t len);

void cond_virtio_init(struct cond_virtio_device *dev, struct cond_console_backend *backend);
uint64_t cond_virtio_mmio_read(const struct cond_virtio_device *dev, uint64_t offset, uint32_t len);
void cond_virtio_mmio_write(struct cond_virtio_device *dev, uint64_t offset, uint64_t raw_value);
bool cond_virtio_notify_queue(struct cond_virtio_device *dev, int axi_fd, uint32_t queue);

bool cond_axi_serve(int fd, struct cond_virtio_device *dev);
bool cond_axi_dma_read(int fd, uint64_t gpa, uint32_t len, uint8_t **data);
bool cond_axi_dma_read_u16(int fd, uint64_t gpa, uint16_t *value);
bool cond_axi_dma_write(int fd, uint64_t gpa, const void *data, uint32_t len);
bool cond_axi_raise_irq(int fd);
bool cond_axi_lower_irq(int fd);

#endif
