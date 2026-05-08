#ifndef BLKD_H
#define BLKD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLKD_MSG_MMIO_READ 3
#define BLKD_MSG_MMIO_READ_REPLY 4
#define BLKD_MSG_MMIO_WRITE 5
#define BLKD_MSG_IRQ_ASSERT 6
#define BLKD_MSG_IRQ_DEASSERT 7
#define BLKD_MSG_DMA_READ 8
#define BLKD_MSG_DMA_READ_REPLY 9
#define BLKD_MSG_DMA_WRITE 10
#define BLKD_MSG_ERROR 0xffff

#define BLKD_HEADER_LEN 24
#define BLKD_SECTOR_SIZE 512u
#define BLKD_QUEUE_SIZE 256u

struct blkd_config {
    char image[4096];
    char socket[4096];
    char ram_access[128];
    bool readonly;
};

struct blkd_header {
    uint16_t kind;
    uint16_t flags;
    uint32_t window_id;
    uint64_t offset;
    uint32_t length;
};

struct blkd_block_backend {
    int fd;
    uint64_t image_len;
    bool readonly;
};

struct blkd_virtio_device {
    struct blkd_block_backend *backend;
    uint64_t capacity_sectors;
    uint32_t device_features_sel;
    uint32_t driver_features_sel;
    uint32_t driver_features[2];
    uint32_t queue_sel;
    uint32_t queue_num;
    uint32_t queue_ready;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
    uint16_t last_avail_idx;
    uint32_t status;
    uint32_t interrupt_status;
};

static inline uint16_t blkd_load_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t blkd_load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline uint64_t blkd_load_le64(const uint8_t *p)
{
    return (uint64_t)blkd_load_le32(p) | ((uint64_t)blkd_load_le32(p + 4) << 32);
}

static inline void blkd_store_le16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}

static inline void blkd_store_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

static inline void blkd_store_le64(uint8_t *p, uint64_t v)
{
    blkd_store_le32(p, (uint32_t)v);
    blkd_store_le32(p + 4, (uint32_t)(v >> 32));
}

bool blkd_block_open(struct blkd_block_backend *backend, const char *path, bool readonly);
void blkd_block_close(struct blkd_block_backend *backend);
bool blkd_block_read(struct blkd_block_backend *backend, uint64_t offset, void *buf, size_t len);
bool blkd_block_write(struct blkd_block_backend *backend, uint64_t offset, const void *buf, size_t len);
bool blkd_block_flush(struct blkd_block_backend *backend);

void blkd_virtio_init(struct blkd_virtio_device *dev, struct blkd_block_backend *backend);
uint64_t blkd_virtio_mmio_read(const struct blkd_virtio_device *dev, uint64_t offset, uint32_t len);
void blkd_virtio_mmio_write(struct blkd_virtio_device *dev, uint64_t offset, uint64_t raw_value);
bool blkd_virtio_notify_queue(struct blkd_virtio_device *dev, int axi_fd, uint32_t queue);

bool blkd_axi_serve(int fd, struct blkd_virtio_device *dev);
bool blkd_axi_dma_read(int fd, uint64_t gpa, uint32_t len, uint8_t **data);
bool blkd_axi_dma_read_u16(int fd, uint64_t gpa, uint16_t *value);
bool blkd_axi_dma_write(int fd, uint64_t gpa, const void *data, uint32_t len);
bool blkd_axi_raise_irq(int fd);
bool blkd_axi_lower_irq(int fd);

#endif
