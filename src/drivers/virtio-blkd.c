#define _GNU_SOURCE

#include "virtio-blkd.h"
#include "virtio-mmio.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/virtio_blk.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_mmio.h>
#include <linux/virtio_ring.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/**
 * @brief Reads exactly len bytes from a positioned file offset.
 *
 * @param fd File descriptor to read from.
 * @param buf Destination buffer.
 * @param len Number of bytes to read.
 * @param offset File offset to start reading from.
 * @return bool True on success, false on EOF or read failure.
 */
static bool pread_all(int fd, void *buf, size_t len, off_t offset) {
    uint8_t *pos = buf;

    while (len) {
        ssize_t ret = pread(fd, pos, len, offset);
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
        offset += ret;
    }

    return true;
}

/**
 * @brief Writes exactly len bytes to a positioned file offset.
 *
 * @param fd File descriptor to write to.
 * @param buf Source buffer.
 * @param len Number of bytes to write.
 * @param offset File offset to start writing to.
 * @return bool True on success, false on write failure.
 */
static bool pwrite_all(int fd, const void *buf, size_t len, off_t offset) {
    const uint8_t *pos = buf;

    while (len) {
        ssize_t ret = pwrite(fd, pos, len, offset);
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
        offset += ret;
    }

    return true;
}

/**
 * @brief Opens a block image backend.
 *
 * @param backend Backend state to initialize.
 * @param path Disk image path.
 * @param readonly Whether to open the image read-only.
 * @return bool True on success, false on open or stat failure.
 */
bool blkd_block_open(struct blkd_block_backend *backend, const char *path,
                     bool readonly) {
    struct stat st;

    backend->fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (backend->fd < 0) {
        perror("blkd: open image");
        return false;
    }

    if (fstat(backend->fd, &st) < 0) {
        perror("blkd: fstat image");
        close(backend->fd);
        backend->fd = -1;
        return false;
    }

    backend->image_len = (uint64_t)st.st_size;
    backend->readonly = readonly;
    return true;
}

/**
 * @brief Closes a block image backend.
 *
 * @param backend Backend state to close.
 */
void blkd_block_close(struct blkd_block_backend *backend) {
    if (backend->fd >= 0) {
        close(backend->fd);
        backend->fd = -1;
    }
}

/**
 * @brief Reads bytes from the block image at a byte offset.
 *
 * @param backend Open block backend.
 * @param offset Byte offset in the image.
 * @param buf Destination buffer.
 * @param len Number of bytes to read.
 * @return bool True on success, false on bounds or I/O failure.
 */
bool blkd_block_read(struct blkd_block_backend *backend, uint64_t offset,
                     void *buf, size_t len) {
    if (offset > backend->image_len || len > backend->image_len - offset ||
        offset > (uint64_t)INT64_MAX) {
        return false;
    }
    return pread_all(backend->fd, buf, len, (off_t)offset);
}

/**
 * @brief Writes bytes to the block image at a byte offset.
 *
 * @param backend Open block backend.
 * @param offset Byte offset in the image.
 * @param buf Source buffer.
 * @param len Number of bytes to write.
 * @return bool True on success, false on bounds, read-only, or I/O failure.
 */
bool blkd_block_write(struct blkd_block_backend *backend, uint64_t offset,
                      const void *buf, size_t len) {
    if (backend->readonly || offset > backend->image_len ||
        len > backend->image_len - offset || offset > (uint64_t)INT64_MAX) {
        return false;
    }
    return pwrite_all(backend->fd, buf, len, (off_t)offset);
}

/**
 * @brief Flushes pending block image writes.
 *
 * @param backend Open block backend.
 * @return bool True on success, false on fsync failure.
 */
bool blkd_block_flush(struct blkd_block_backend *backend) {
    return fsync(backend->fd) == 0;
}

/**
 * @brief Returns virtio-blk feature bits exposed to the guest.
 *
 * @param opaque Unused device callback state.
 * @return uint64_t Supported virtio feature bit mask.
 */
static uint64_t get_features(void *opaque) {
    (void)opaque;

    return (1ull << VIRTIO_BLK_F_BLK_SIZE) | (1ull << VIRTIO_BLK_F_FLUSH) |
           (1ull << VIRTIO_F_VERSION_1);
}

/**
 * @brief Reads virtio-blk config-space fields.
 *
 * @param opaque virtio-blk device state.
 * @param offset Config-space byte offset.
 * @param len Access width in bytes.
 * @return uint64_t Config-space value for the requested offset.
 */
static uint64_t get_config(void *opaque, uint64_t offset, uint32_t len) {
    const struct blkd_virtio_device *dev = opaque;

    (void)len;
    switch (offset) {
    case offsetof(struct virtio_blk_config, capacity):
        return (uint32_t)dev->capacity_sectors;
    case offsetof(struct virtio_blk_config, capacity) + 4:
        return (uint32_t)(dev->capacity_sectors >> 32);
    case offsetof(struct virtio_blk_config, blk_size):
        return BLKD_SECTOR_SIZE;
    default:
        return 0;
    }
}

static const struct virtio_device_ops virtio_ops = {
    .get_features = get_features,
    .get_config = get_config,
    .notify_queue = (virtio_notify_queue_fn)blkd_virtio_notify_queue,
};

/**
 * @brief Initializes virtio-blk device state over an opened block backend.
 *
 * @param dev Device state to initialize.
 * @param backend Open block backend used to service requests.
 */
void blkd_virtio_init(struct blkd_virtio_device *dev,
                      struct blkd_block_backend *backend) {
    memset(dev, 0, sizeof(*dev));
    dev->backend = backend;
    dev->capacity_sectors = backend->image_len / BLKD_SECTOR_SIZE;
    virtio_device_init(&dev->vdev, VIRTIO_ID_BLOCK, VIRTIO_VENDOR_ID_LOCAL, 1,
                       BLKD_QUEUE_SIZE, &dev->queue, dev, &virtio_ops);
}

/**
 * @brief Processes one virtio-blk descriptor chain.
 *
 * @param dev virtio-blk device state.
 * @param io Active fabric I/O context used for DMA.
 * @param head Descriptor chain head index.
 * @param used_len Output length to report in the used ring.
 * @return bool True on success, false on malformed descriptors or DMA failure.
 */
static bool process_chain(struct blkd_virtio_device *dev,
                          struct virt_axi_io *io, uint16_t head,
                          uint32_t *used_len) {
    struct virtio_desc header_desc;
    struct virtio_desc status_desc;
    uint8_t *header = NULL;
    uint8_t status = VIRTIO_BLK_S_OK;
    uint32_t request_type;
    uint64_t sector;
    uint64_t offset;
    uint32_t data_len = 0;
    uint16_t index;
    uint32_t chain_seen = 0;

    fprintf(stderr, "blkd: read descriptor chain head=%u\n", head);

    if (!virtio_read_desc(&dev->queue, io, virt_axi_dma_read, head,
                          &header_desc) ||
        !(header_desc.flags & VRING_DESC_F_NEXT) ||
        !virt_axi_dma_read(io, header_desc.addr, 16, &header)) {
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

    if (request_type != VIRTIO_BLK_T_IN && request_type != VIRTIO_BLK_T_OUT &&
        request_type != VIRTIO_BLK_T_FLUSH) {
        status = VIRTIO_BLK_S_UNSUPP;
    }

    index = header_desc.next;
    for (;;) {
        struct virtio_desc desc;
        uint8_t *data = NULL;
        bool last;

        if (++chain_seen > dev->queue.num ||
            !virtio_read_desc(&dev->queue, io, virt_axi_dma_read, index,
                              &desc)) {
            return false;
        }

        last = !(desc.flags & VRING_DESC_F_NEXT);
        if (last) {
            status_desc = desc;
            break;
        }

        if (request_type == VIRTIO_BLK_T_IN) {
            if (!(desc.flags & VRING_DESC_F_WRITE)) {
                status = VIRTIO_BLK_S_IOERR;
            } else if (status == VIRTIO_BLK_S_OK) {
                data = calloc(1, desc.len);
                if (!data ||
                    !blkd_block_read(dev->backend, offset + data_len, data,
                                     desc.len) ||
                    !virt_axi_dma_write(io, desc.addr, data, desc.len)) {
                    status = VIRTIO_BLK_S_IOERR;
                }
                free(data);
            }
            data_len += desc.len;
        } else if (request_type == VIRTIO_BLK_T_OUT) {
            if (desc.flags & VRING_DESC_F_WRITE) {
                status = VIRTIO_BLK_S_IOERR;
            } else if (status == VIRTIO_BLK_S_OK) {
                if (!virt_axi_dma_read(io, desc.addr, desc.len, &data)) {
                    return false;
                }
                if (!blkd_block_write(dev->backend, offset + data_len, data,
                                      desc.len)) {
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

    fprintf(stderr,
            "blkd: request type=%u sector=%" PRIu64 " data_len=%u status=%u\n",
            request_type, sector, data_len, status);

    *used_len = request_type == VIRTIO_BLK_T_IN ? data_len + 1 : 1;
    return virt_axi_dma_write(io, status_desc.addr, &status, 1);
}

/**
 * @brief Processes a guest notification for the virtio-blk request queue.
 *
 * @param dev virtio-blk device state.
 * @param io Active fabric I/O context used for DMA and IRQs.
 * @param queue Queue index notified by the guest.
 * @return bool True on success, false on DMA or queue processing failure.
 */
bool blkd_virtio_notify_queue(struct blkd_virtio_device *dev,
                              struct virt_axi_io *io, uint32_t queue) {
    bool used_any = false;

    fprintf(stderr, "blkd: notify queue=%u\n", queue);
    if (queue != 0 || !dev->queue.ready) {
        return true;
    }

    for (;;) {
        uint16_t head;
        uint32_t used_len;
        bool available;

        if (!virtio_next_avail(&dev->queue, io, virt_axi_dma_read_u16, &head,
                               &available)) {
            return false;
        }
        if (!available) {
            break;
        }

        fprintf(stderr, "blkd: process head=%u\n", head);
        if (!process_chain(dev, io, head, &used_len) ||
            !virtio_add_used(&dev->queue, io, virt_axi_dma_read_u16,
                             virt_axi_dma_write, head, used_len)) {
            return false;
        }
        dev->queue.last_avail_idx++;
        used_any = true;
    }

    if (used_any) {
        dev->vdev.interrupt_status |= VIRTIO_INTERRUPT_VRING;
        if (!virt_axi_raise_irq(io)) {
            return false;
        }
    }

    return true;
}

/** @brief State binding virtio-blk, virtio-mmio, and virt-axi together. */
struct blkd_virt_axi_binding {
    struct blkd_virtio_device *dev;
    struct blkd_block_backend *backend;
    struct virtio_mmio mmio;
};

/**
 * @brief Initializes per-connection virtio-mmio state after QEMU connects.
 *
 * @param opaque Block virt-axi binding state.
 */
static void blkd_connect(void *opaque) {
    struct blkd_virt_axi_binding *binding = opaque;

    blkd_virtio_init(binding->dev, binding->backend);
    virtio_mmio_init(&binding->mmio, &binding->dev->vdev);
}

/**
 * @brief Handles a virt-axi MMIO read for the block device.
 *
 * @param opaque Block virt-axi binding state.
 * @param offset Device-relative MMIO offset.
 * @param len Access width in bytes.
 * @return uint64_t Register value returned to QEMU.
 */
static uint64_t blkd_mmio_read(void *opaque, uint64_t offset, uint32_t len) {
    struct blkd_virt_axi_binding *binding = opaque;

    return virtio_mmio_read(&binding->mmio, offset, len);
}

/**
 * @brief Handles a virt-axi MMIO write for the block device.
 *
 * @param opaque Block virt-axi binding state.
 * @param io Active fabric I/O context.
 * @param offset Device-relative MMIO offset.
 * @param raw_value Register value written by the guest.
 * @param len Access width in bytes.
 * @return bool True on success, false on notification or IRQ failure.
 */
static bool blkd_mmio_write(void *opaque, struct virt_axi_io *io,
                            uint64_t offset, uint64_t raw_value, uint32_t len) {
    struct blkd_virt_axi_binding *binding = opaque;

    (void)len;
    virtio_mmio_write(&binding->mmio, offset, raw_value);
    if (offset == VIRTIO_MMIO_STATUS && raw_value == 0 &&
        !virt_axi_lower_irq(io)) {
        return false;
    }
    if (offset == VIRTIO_MMIO_INTERRUPT_ACK && !virt_axi_lower_irq(io)) {
        return false;
    }
    if (offset == VIRTIO_MMIO_QUEUE_NOTIFY &&
        !binding->dev->vdev.ops->notify_queue(binding->dev->vdev.opaque, io,
                                              (uint32_t)raw_value)) {
        return false;
    }
    return true;
}

/**
 * @brief Initializes a virt-axi MMIO device binding for virtio-blk.
 *
 * @param device Fabric device descriptor to populate.
 * @param dev virtio-blk device state.
 * @param backend Block backend used by the device.
 * @param socket_path Unix socket path for QEMU to connect to.
 */
void blkd_virt_axi_init_device(struct virt_axi_device *device,
                               struct blkd_virtio_device *dev,
                               struct blkd_block_backend *backend,
                               const char *socket_path) {
    static struct blkd_virt_axi_binding binding;
    static const struct virt_axi_device_ops ops = {
        .connect = blkd_connect,
        .read = blkd_mmio_read,
        .write = blkd_mmio_write,
    };

    binding.dev = dev;
    binding.backend = backend;
    device->name = "blkd";
    device->socket_path = socket_path;
    device->addr = 0;
    device->size = 0x200;
    device->opaque = &binding;
    device->ops = &ops;
}

/**
 * @brief Copies a config value into a fixed-size output buffer.
 *
 * @param value Source string.
 * @param out Destination buffer.
 * @param out_len Destination buffer length.
 * @return bool True on success, false if the value is too long.
 */
static bool copy_value(const char *value, char *out, size_t out_len) {
    size_t len = strlen(value);

    if (len >= out_len) {
        return false;
    }
    memcpy(out, value, len + 1);
    return true;
}

/**
 * @brief Parses a boolean config value.
 *
 * @param value Input string.
 * @param out Output boolean value.
 * @return bool True on success, false if the string is not a boolean.
 */
static bool parse_bool_value(const char *value, bool *out) {
    if (!strcmp(value, "true") || !strcmp(value, "1")) {
        *out = true;
        return true;
    }
    if (!strcmp(value, "false") || !strcmp(value, "0")) {
        *out = false;
        return true;
    }
    return false;
}

/**
 * @brief Parses the daemon key-value argument string.
 *
 * @param arg Comma-separated key=value argument string.
 * @param cfg Output block daemon configuration.
 * @return bool True on success, false on malformed or incomplete config.
 */
static bool parse_config_arg(const char *arg, struct blkd_config *cfg) {
    char *copy;
    char *save = NULL;

    cfg->image[0] = '\0';
    cfg->socket[0] = '\0';
    strcpy(cfg->ram_access, "shared-mem");
    cfg->readonly = false;

    copy = strdup(arg);
    if (!copy) {
        return false;
    }

    for (char *part = strtok_r(copy, ",", &save); part;
         part = strtok_r(NULL, ",", &save)) {
        char *value = strchr(part, '=');
        if (!value) {
            free(copy);
            return false;
        }
        *value++ = '\0';
        if (!strcmp(part, "image")) {
            if (!copy_value(value, cfg->image, sizeof(cfg->image))) {
                free(copy);
                return false;
            }
        } else if (!strcmp(part, "socket")) {
            if (!copy_value(value, cfg->socket, sizeof(cfg->socket))) {
                free(copy);
                return false;
            }
        } else if (!strcmp(part, "ram_access")) {
            if (!copy_value(value, cfg->ram_access, sizeof(cfg->ram_access))) {
                free(copy);
                return false;
            }
        } else if (!strcmp(part, "readonly")) {
            if (!parse_bool_value(value, &cfg->readonly)) {
                free(copy);
                return false;
            }
        } else if (strcmp(part, "name")) {
            free(copy);
            return false;
        }
    }

    free(copy);
    return cfg->image[0] && cfg->socket[0];
}

#ifndef BACKEND_TEST

/**
 * @brief Runs the virtio-blk daemon process.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return int Process exit status.
 */
int main(int argc, char **argv) {
    struct blkd_config cfg;
    struct blkd_block_backend backend;
    struct blkd_virtio_device dev;
    struct virt_axi_device virt_axi_device;
    struct virt_axi bus;

    if (argc != 2) {
        fprintf(stderr,
                "usage: blkd "
                "name=<name>,socket=<path>,image=<path>[,readonly=<bool>][,"
                "ram_access=<mode>]\n");
        return 2;
    }

    if (!parse_config_arg(argv[1], &cfg)) {
        fprintf(stderr, "blkd: failed to parse config args\n");
        return 1;
    }

    if (!blkd_block_open(&backend, cfg.image, cfg.readonly)) {
        return 1;
    }

    fprintf(stderr, "blkd: serving %" PRIu64 " sectors on %s (%s)\n",
            backend.image_len / BLKD_SECTOR_SIZE, cfg.socket, cfg.ram_access);

    virt_axi_init(&bus);
    blkd_virt_axi_init_device(&virt_axi_device, &dev, &backend, cfg.socket);
    if (!virt_axi_register(&bus, &virt_axi_device) || !virt_axi_run(&bus)) {
        blkd_block_close(&backend);
        return 1;
    }

    blkd_block_close(&backend);
    return 0;
}
#endif
