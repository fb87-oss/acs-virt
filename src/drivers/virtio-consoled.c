#define _GNU_SOURCE

#include "virtio-consoled.h"
#include "virtio-mmio.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/virtio_config.h>
#include <linux/virtio_console.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_mmio.h>
#include <linux/virtio_ring.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * @brief Writes exactly len bytes to a file descriptor.
 *
 * @param fd File descriptor to write to.
 * @param buf Source buffer.
 * @param len Number of bytes to write.
 * @return bool True on success, false on write failure.
 */
static bool write_all(int fd, const void *buf, size_t len) {
    const uint8_t *pos = buf;

    while (len) {
        ssize_t ret = write(fd, pos, len);
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

/**
 * @brief Opens the configured console output backend.
 *
 * @param backend Backend state to initialize.
 * @param path Output file path, empty string, or '-' for stdout.
 * @return bool True on success, false on open failure.
 */
bool cond_console_open(struct cond_console_backend *backend, const char *path) {
    if (!path || !path[0] || !strcmp(path, "-")) {
        backend->fd = STDOUT_FILENO;
        return true;
    }

    backend->fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0666);
    if (backend->fd < 0) {
        perror("cond: open output");
        return false;
    }
    return true;
}

/**
 * @brief Closes the console output backend.
 *
 * @param backend Backend state to close.
 */
void cond_console_close(struct cond_console_backend *backend) {
    if (backend->fd >= 0 && backend->fd != STDOUT_FILENO) {
        close(backend->fd);
    }
    backend->fd = -1;
}

/**
 * @brief Writes bytes to the console output backend.
 *
 * @param backend Open console backend.
 * @param buf Source bytes to write.
 * @param len Number of bytes to write.
 * @return bool True on success, false on write failure.
 */
bool cond_console_write(struct cond_console_backend *backend, const void *buf,
                        size_t len) {
    return write_all(backend->fd, buf, len);
}

/**
 * @brief Returns virtio-console feature bits exposed to the guest.
 *
 * @param opaque Unused device callback state.
 * @return uint64_t Supported virtio feature bit mask.
 */
static uint64_t get_features(void *opaque) {
    (void)opaque;

    return (1ull << VIRTIO_CONSOLE_F_SIZE) | (1ull << VIRTIO_F_VERSION_1);
}

/**
 * @brief Reads virtio-console config-space fields.
 *
 * @param opaque Unused device callback state.
 * @param offset Config-space byte offset.
 * @param len Access width in bytes.
 * @return uint64_t Config-space value for the requested offset.
 */
static uint64_t get_config(void *opaque, uint64_t offset, uint32_t len) {
    (void)opaque;
    (void)len;

    switch (offset) {
    case offsetof(struct virtio_console_config, cols):
        return 80;
    case offsetof(struct virtio_console_config, rows):
        return 25;
    default:
        return 0;
    }
}

static const struct virtio_device_ops virtio_ops = {
    .get_features = get_features,
    .get_config = get_config,
    .notify_queue = (virtio_notify_queue_fn)cond_virtio_notify_queue,
};

/**
 * @brief Initializes virtio-console device state over an output backend.
 *
 * @param dev Device state to initialize.
 * @param backend Console output backend used for guest TX data.
 */
void cond_virtio_init(struct cond_virtio_device *dev,
                      struct cond_console_backend *backend) {
    memset(dev, 0, sizeof(*dev));
    dev->backend = backend;
    virtio_device_init(&dev->vdev, VIRTIO_ID_CONSOLE, VIRTIO_VENDOR_ID_LOCAL,
                       COND_QUEUE_COUNT, COND_QUEUE_SIZE, dev->queues, dev,
                       &virtio_ops);
}

/**
 * @brief Processes one guest TX descriptor chain.
 *
 * @param dev virtio-console device state.
 * @param queue Queue containing the descriptor chain.
 * @param io Active fabric I/O context used for DMA.
 * @param head Descriptor chain head index.
 * @param used_len Output length to report in the used ring.
 * @return bool True on success, false on DMA or output failure.
 */
static bool process_tx_chain(struct cond_virtio_device *dev,
                             struct virtio_queue *queue, struct fabric_io *io,
                             uint16_t head, uint32_t *used_len) {
    struct virtio_desc desc;
    uint16_t index = head;
    uint32_t total = 0;

    for (;;) {
        uint8_t *data = NULL;
        if (!virtio_read_desc(queue, io, fabric_dma_read, index, &desc)) {
            return false;
        }
        if (!(desc.flags & VRING_DESC_F_WRITE) && desc.len) {
            if (!fabric_dma_read(io, desc.addr, desc.len, &data)) {
                return false;
            }
            if (!cond_console_write(dev->backend, data, desc.len)) {
                free(data);
                return false;
            }
            total += desc.len;
            free(data);
        }
        if (!(desc.flags & VRING_DESC_F_NEXT)) {
            break;
        }
        index = desc.next;
    }

    *used_len = total;
    return true;
}

/**
 * @brief Processes a guest notification for a virtio-console queue.
 *
 * @param dev virtio-console device state.
 * @param io Active fabric I/O context used for DMA and IRQs.
 * @param queue_index Queue index notified by the guest.
 * @return bool True on success, false on DMA or queue processing failure.
 */
bool cond_virtio_notify_queue(struct cond_virtio_device *dev,
                              struct fabric_io *io, uint32_t queue_index) {
    struct virtio_queue *queue;
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
        uint16_t head;
        uint32_t used_len = 0;
        bool available;

        if (!virtio_next_avail(queue, io, fabric_dma_read_u16, &head,
                               &available)) {
            return false;
        }
        if (!available) {
            break;
        }

        fprintf(stderr, "cond: process queue=%u head=%u\n", queue_index, head);
        if (!process_tx_chain(dev, queue, io, head, &used_len)) {
            return false;
        }
        if (!virtio_add_used(queue, io, fabric_dma_read_u16, fabric_dma_write,
                             head, used_len)) {
            return false;
        }
        queue->last_avail_idx++;
        used_any = true;
    }

    if (used_any) {
        dev->vdev.interrupt_status |= VIRTIO_INTERRUPT_VRING;
        if (!fabric_raise_irq(io)) {
            return false;
        }
    }

    return true;
}

/** @brief State binding virtio-console, virtio-mmio, and axi together. */
struct cond_fabric_binding {
    struct cond_virtio_device *dev;
    struct cond_console_backend *backend;
    struct virtio_mmio mmio;
};

/**
 * @brief Initializes per-connection virtio-mmio state after QEMU connects.
 *
 * @param opaque Console fabric binding state.
 */
static void cond_connect(void *opaque) {
    struct cond_fabric_binding *binding = opaque;

    cond_virtio_init(binding->dev, binding->backend);
    virtio_mmio_init(&binding->mmio, &binding->dev->vdev);
}

/**
 * @brief Handles a fabric MMIO read for the console device.
 *
 * @param opaque Console fabric binding state.
 * @param offset Device-relative MMIO offset.
 * @param len Access width in bytes.
 * @return uint64_t Register value returned to QEMU.
 */
static uint64_t cond_mmio_read(void *opaque, uint64_t offset, uint32_t len) {
    struct cond_fabric_binding *binding = opaque;

    return virtio_mmio_read(&binding->mmio, offset, len);
}

/**
 * @brief Handles a fabric MMIO write for the console device.
 *
 * @param opaque Console fabric binding state.
 * @param io Active fabric I/O context.
 * @param offset Device-relative MMIO offset.
 * @param raw_value Register value written by the guest.
 * @param len Access width in bytes.
 * @return bool True on success, false on notification or IRQ failure.
 */
static bool cond_mmio_write(void *opaque, struct fabric_io *io, uint64_t offset,
                            uint64_t raw_value, uint32_t len) {
    struct cond_fabric_binding *binding = opaque;

    (void)len;
    virtio_mmio_write(&binding->mmio, offset, raw_value);
    if (offset == VIRTIO_MMIO_STATUS && raw_value == 0 &&
        !fabric_lower_irq(io)) {
        return false;
    }
    if (offset == VIRTIO_MMIO_INTERRUPT_ACK && !fabric_lower_irq(io)) {
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
 * @brief Initializes a fabric MMIO device binding for virtio-console.
 *
 * @param device Fabric device descriptor to populate.
 * @param dev virtio-console device state.
 * @param backend Console backend used by the device.
 * @param socket_path Unix socket path for QEMU to connect to.
 */
void cond_fabric_init_device(struct fabric_device *device,
                             struct cond_virtio_device *dev,
                             struct cond_console_backend *backend,
                             const char *socket_path) {
    static struct cond_fabric_binding binding;
    static const struct fabric_device_ops ops = {
        .connect = cond_connect,
        .read = cond_mmio_read,
        .write = cond_mmio_write,
    };

    binding.dev = dev;
    binding.backend = backend;
    device->name = "cond";
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
 * @brief Parses the daemon key-value argument string.
 *
 * @param arg Comma-separated key=value argument string.
 * @param cfg Output console daemon configuration.
 * @return bool True on success, false on malformed or incomplete config.
 */
static bool parse_config_arg(const char *arg, struct cond_config *cfg) {
    char *copy;
    char *save = NULL;

    cfg->socket[0] = '\0';
    strcpy(cfg->ram_access, "shared-mem");
    strcpy(cfg->output, "-");

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
        if (!strcmp(part, "socket")) {
            if (!copy_value(value, cfg->socket, sizeof(cfg->socket))) {
                free(copy);
                return false;
            }
        } else if (!strcmp(part, "output")) {
            if (!copy_value(value, cfg->output, sizeof(cfg->output))) {
                free(copy);
                return false;
            }
        } else if (!strcmp(part, "ram_access")) {
            if (!copy_value(value, cfg->ram_access, sizeof(cfg->ram_access))) {
                free(copy);
                return false;
            }
        } else if (strcmp(part, "name")) {
            free(copy);
            return false;
        }
    }

    free(copy);
    return cfg->socket[0];
}

#ifndef BACKEND_TEST

/**
 * @brief Runs the virtio-console daemon process.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return int Process exit status.
 */
int main(int argc, char **argv) {
    struct cond_config cfg;
    struct cond_console_backend backend;
    struct cond_virtio_device dev;
    struct fabric_device fabric_device;
    struct fabric bus;

    if (argc != 2) {
        fprintf(
            stderr,
            "usage: cond "
            "name=<name>,socket=<path>[,output=<path>][,ram_access=<mode>]\n");
        return 2;
    }
    if (!parse_config_arg(argv[1], &cfg)) {
        fprintf(stderr, "cond: failed to parse config args\n");
        return 1;
    }
    if (!cond_console_open(&backend, cfg.output)) {
        return 1;
    }
    fprintf(stderr, "cond: serving console on %s (%s), output=%s\n", cfg.socket,
            cfg.ram_access, cfg.output);

    fabric_init(&bus);
    cond_fabric_init_device(&fabric_device, &dev, &backend, cfg.socket);
    if (!fabric_register(&bus, &fabric_device) || !fabric_run(&bus)) {
        cond_console_close(&backend);
        return 1;
    }

    cond_console_close(&backend);
    return 0;
}
#endif
