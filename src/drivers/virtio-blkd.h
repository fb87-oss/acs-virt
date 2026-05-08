#ifndef VIRTIO_BLKD_H
#define VIRTIO_BLKD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "virt-axi.h"
#include "virtio.h"

#define BLKD_SECTOR_SIZE 512u
#define BLKD_QUEUE_SIZE 256u

/** @brief Parsed runtime configuration for the virtio-blk daemon. */
struct blkd_config {
    char image[4096];     ///< Disk image path.
    char socket[4096];    ///< virt-axi Unix socket path.
    char ram_access[128]; ///< RAM access mode string passed by the launcher.
    bool readonly;        ///< Whether the disk image is opened read-only.
};

/** @brief Host block image backend state. */
struct blkd_block_backend {
    int fd;             ///< Open disk image file descriptor.
    uint64_t image_len; ///< Disk image length in bytes.
    bool readonly;      ///< Whether writes should be rejected.
};

/** @brief virtio-blk device state owned by the daemon. */
struct blkd_virtio_device {
    struct blkd_block_backend
        *backend; ///< Block image backend used to service requests.
    uint64_t capacity_sectors; ///< Exposed device capacity in 512-byte sectors.
    struct virtio_device vdev; ///< Transport-independent virtio state.
    struct virtio_queue queue; ///< Single virtio-blk request queue.
};

#define blkd_load_le16 virtio_load_le16
#define blkd_load_le32 virtio_load_le32
#define blkd_load_le64 virtio_load_le64
#define blkd_store_le16 virtio_store_le16
#define blkd_store_le32 virtio_store_le32
#define blkd_store_le64 virtio_store_le64

/**
 * @brief Opens a block image backend.
 *
 * @param backend Backend state to initialize.
 * @param path Disk image path.
 * @param readonly Whether to open the image read-only.
 * @return bool True on success, false on open or stat failure.
 */
bool blkd_block_open(struct blkd_block_backend *backend, const char *path,
                     bool readonly);

/**
 * @brief Closes a block image backend.
 *
 * @param backend Backend state to close.
 */
void blkd_block_close(struct blkd_block_backend *backend);

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
                     void *buf, size_t len);

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
                      const void *buf, size_t len);

/**
 * @brief Flushes pending block image writes.
 *
 * @param backend Open block backend.
 * @return bool True on success, false on fsync failure.
 */
bool blkd_block_flush(struct blkd_block_backend *backend);

/**
 * @brief Initializes virtio-blk device state over an opened block backend.
 *
 * @param dev Device state to initialize.
 * @param backend Open block backend used to service requests.
 */
void blkd_virtio_init(struct blkd_virtio_device *dev,
                      struct blkd_block_backend *backend);

/**
 * @brief Processes a guest notification for the virtio-blk request queue.
 *
 * @param dev virtio-blk device state.
 * @param io Active fabric I/O context used for DMA and IRQs.
 * @param queue Queue index notified by the guest.
 * @return bool True on success, false on DMA or queue processing failure.
 */
bool blkd_virtio_notify_queue(struct blkd_virtio_device *dev,
                              struct virt_axi_io *io, uint32_t queue);

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
                               const char *socket_path);

#endif
