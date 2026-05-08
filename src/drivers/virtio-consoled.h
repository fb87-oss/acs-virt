#ifndef VIRTIO_CONSOLED_H
#define VIRTIO_CONSOLED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "virt-axi.h"
#include "virtio.h"

#define COND_QUEUE_SIZE 256u
#define COND_QUEUE_COUNT 2u

/** @brief Parsed runtime configuration for the virtio-console daemon. */
struct cond_config {
    char socket[4096];    ///< virt-axi Unix socket path.
    char ram_access[128]; ///< RAM access mode string passed by the launcher.
    char output[4096];    ///< Host output path, or '-' for stdout.
};

/** @brief Host console output backend state. */
struct cond_console_backend {
    int fd; ///< Output file descriptor, or STDOUT_FILENO.
};

/** @brief virtio-console device state owned by the daemon. */
struct cond_virtio_device {
    struct cond_console_backend
        *backend;              ///< Host output backend used for guest TX data.
    struct virtio_device vdev; ///< Transport-independent virtio state.
    struct virtio_queue queues[COND_QUEUE_COUNT]; ///< RX and TX virtqueues.
};

#define cond_load_le16 virtio_load_le16
#define cond_load_le32 virtio_load_le32
#define cond_load_le64 virtio_load_le64
#define cond_store_le16 virtio_store_le16
#define cond_store_le32 virtio_store_le32
#define cond_store_le64 virtio_store_le64

/**
 * @brief Opens the configured console output backend.
 *
 * @param backend Backend state to initialize.
 * @param path Output file path, empty string, or '-' for stdout.
 * @return bool True on success, false on open failure.
 */
bool cond_console_open(struct cond_console_backend *backend, const char *path);

/**
 * @brief Closes the console output backend.
 *
 * @param backend Backend state to close.
 */
void cond_console_close(struct cond_console_backend *backend);

/**
 * @brief Writes bytes to the console output backend.
 *
 * @param backend Open console backend.
 * @param buf Source bytes to write.
 * @param len Number of bytes to write.
 * @return bool True on success, false on write failure.
 */
bool cond_console_write(struct cond_console_backend *backend, const void *buf,
                        size_t len);

/**
 * @brief Initializes virtio-console device state over an output backend.
 *
 * @param dev Device state to initialize.
 * @param backend Console output backend used for guest TX data.
 */
void cond_virtio_init(struct cond_virtio_device *dev,
                      struct cond_console_backend *backend);

/**
 * @brief Processes a guest notification for a virtio-console queue.
 *
 * @param dev virtio-console device state.
 * @param io Active fabric I/O context used for DMA and IRQs.
 * @param queue Queue index notified by the guest.
 * @return bool True on success, false on DMA or queue processing failure.
 */
bool cond_virtio_notify_queue(struct cond_virtio_device *dev,
                              struct virt_axi_io *io, uint32_t queue);

/**
 * @brief Initializes a virt-axi MMIO device binding for virtio-console.
 *
 * @param device Fabric device descriptor to populate.
 * @param dev virtio-console device state.
 * @param backend Console backend used by the device.
 * @param socket_path Unix socket path for QEMU to connect to.
 */
void cond_virt_axi_init_device(struct virt_axi_device *device,
                               struct cond_virtio_device *dev,
                               struct cond_console_backend *backend,
                               const char *socket_path);

#endif
