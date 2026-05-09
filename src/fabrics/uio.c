#define _GNU_SOURCE

#include "fabric.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/virtio_mmio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define UIO_NOTIFY_IDLE 0xffffffffu
#define UIO_IRQ_CONTROL_OFFSET 0x200u

/** @brief Runtime configuration for one Linux UIO-backed device. */
struct uio_config {
    char path[256]; ///< `/dev/uioX` character device path.
    uint64_t
        irq_control_offset; ///< Resource0 offset used to signal frontend IRQs.
};

/** @brief One mmap()ed UIO resource. */
struct uio_mapping {
    uint8_t *addr; ///< Mapped resource base address.
    uint64_t size; ///< Mapped resource size in bytes.
};

/** @brief Active UIO fabric state for the single registered backend device. */
struct uio_state {
    int fd;                   ///< Open `/dev/uioX` file descriptor.
    struct uio_config config; ///< Parsed endpoint configuration.
    struct uio_mapping mmio;  ///< Resource0: virtio-mmio and control window.
    struct uio_mapping dma;   ///< Resource1: frontend RAM DMA window.
};

static struct uio_state g_uio = {.fd = -1};

/**
 * @brief Loads a little-endian 32-bit value from a mapped register.
 *
 * @param p Register address.
 * @return uint32_t Decoded host-endian value.
 */
static uint32_t load_le32(const volatile uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/**
 * @brief Stores a host-endian 32-bit value as little-endian bytes.
 *
 * @param p Register address.
 * @param value Value to store.
 */
static void store_le32(volatile uint8_t *p, uint32_t value) {
    p[0] = value & 0xff;
    p[1] = (value >> 8) & 0xff;
    p[2] = (value >> 16) & 0xff;
    p[3] = (value >> 24) & 0xff;
}

/**
 * @brief Parses a 64-bit integer with C integer literal syntax.
 *
 * @param value String value to parse.
 * @param out Output parsed value.
 * @return bool True on success, false on malformed input.
 */
static bool parse_u64(const char *value, uint64_t *out) {
    char *end = NULL;
    unsigned long long parsed;

    if (!value || !value[0]) {
        return false;
    }
    errno = 0;
    parsed = strtoull(value, &end, 0);
    if (errno || !end || *end) {
        return false;
    }
    *out = (uint64_t)parsed;
    return true;
}

/**
 * @brief Reads a sysfs file containing one numeric value.
 *
 * @param path Sysfs file path.
 * @param out Output parsed value.
 * @return bool True on success, false on read or parse failure.
 */
static bool read_sysfs_u64(const char *path, uint64_t *out) {
    FILE *f = fopen(path, "r");
    char buf[64];
    bool ok;

    if (!f) {
        return false;
    }
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return false;
    }
    fclose(f);
    buf[strcspn(buf, "\n")] = '\0';
    ok = parse_u64(buf, out);
    return ok;
}

/**
 * @brief Parses a compact UIO endpoint string.
 *
 * Endpoint form:
 *
 * ```text
 * uio:/dev/uioX[:irq-control-offset]
 * ```
 *
 * @param endpoint Endpoint string from the backend device descriptor.
 * @param config Output runtime configuration.
 * @return bool True on success, false on malformed input.
 */
static bool parse_endpoint(const char *endpoint, struct uio_config *config) {
    const char *value = endpoint;
    const char *offset;
    size_t len;

    snprintf(config->path, sizeof(config->path), "/dev/uio0");
    config->irq_control_offset = UIO_IRQ_CONTROL_OFFSET;

    if (!value || !value[0]) {
        return false;
    }
    if (!strncmp(value, "uio:", 4)) {
        value += 4;
    }

    offset = strchr(value, ':');
    len = offset ? (size_t)(offset - value) : strlen(value);
    if (!len || len >= sizeof(config->path)) {
        return false;
    }
    memcpy(config->path, value, len);
    config->path[len] = '\0';

    if (offset && !parse_u64(offset + 1, &config->irq_control_offset)) {
        return false;
    }

    return true;
}

/**
 * @brief Extracts the UIO index from a `/dev/uioX` path.
 *
 * @param path Character device path.
 * @param index Output parsed UIO index.
 * @return bool True on success, false on unsupported path form.
 */
static bool uio_index_from_path(const char *path, unsigned *index) {
    const char *base = strrchr(path, '/');
    char *end = NULL;
    unsigned long parsed;

    base = base ? base + 1 : path;
    if (strncmp(base, "uio", 3)) {
        return false;
    }
    errno = 0;
    parsed = strtoul(base + 3, &end, 10);
    if (errno || !end || *end || parsed > UINT32_MAX) {
        return false;
    }
    *index = (unsigned)parsed;
    return true;
}

/**
 * @brief Maps one UIO resource from `/dev/uioX`.
 *
 * UIO resources are mapped by using the resource index as the mmap page offset.
 * The resource size is read from `/sys/class/uio/uioX/maps/mapN/size`.
 *
 * @param config Parsed UIO endpoint configuration.
 * @param resource Resource index to map.
 * @param mapping Output mapping state.
 * @return bool True on success, false on sysfs or mmap failure.
 */
static bool map_resource(const struct uio_config *config, unsigned resource,
                         struct uio_mapping *mapping) {
    unsigned index;
    long page_size = sysconf(_SC_PAGESIZE);
    char size_path[512];
    uint64_t size;

    if (page_size <= 0 || !uio_index_from_path(config->path, &index)) {
        return false;
    }

    snprintf(size_path, sizeof(size_path),
             "/sys/class/uio/uio%u/maps/map%u/size", index, resource);
    if (read_sysfs_u64(size_path, &size) && size) {
        mapping->addr = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, g_uio.fd, (off_t)resource * page_size);
    } else {
        FILE *f;
        int resource_fd;
        char resource_list_path[512];
        char resource_path[512];
        unsigned current = 0;
        uint64_t start = 0;
        uint64_t end = 0;
        uint64_t flags = 0;

        snprintf(resource_list_path, sizeof(resource_list_path),
                 "/sys/class/uio/uio%u/device/resource", index);
        f = fopen(resource_list_path, "r");
        if (!f) {
            return false;
        }
        while (current <= resource &&
               fscanf(f, "0x%" SCNx64 " 0x%" SCNx64 " 0x%" SCNx64, &start,
                      &end, &flags) == 3) {
            if (current == resource) {
                break;
            }
            current++;
        }
        fclose(f);
        if (current != resource || end < start) {
            return false;
        }
        size = end - start + 1;
        snprintf(resource_path, sizeof(resource_path),
                 "/sys/class/uio/uio%u/device/resource%u", index, resource);
        resource_fd = open(resource_path, O_RDWR | O_CLOEXEC);
        if (resource_fd < 0) {
            return false;
        }
        mapping->addr = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, resource_fd, 0);
        close(resource_fd);
    }
    if (mapping->addr == MAP_FAILED) {
        mapping->addr = NULL;
        return false;
    }
    mapping->size = size;
    return true;
}

/** @brief Releases all active UIO mappings and the device file descriptor. */
static void cleanup_uio(void) {
    if (g_uio.mmio.addr) {
        munmap(g_uio.mmio.addr, (size_t)g_uio.mmio.size);
        g_uio.mmio.addr = NULL;
        g_uio.mmio.size = 0;
    }
    if (g_uio.dma.addr) {
        munmap(g_uio.dma.addr, (size_t)g_uio.dma.size);
        g_uio.dma.addr = NULL;
        g_uio.dma.size = 0;
    }
    if (g_uio.fd >= 0) {
        close(g_uio.fd);
        g_uio.fd = -1;
    }
}

/**
 * @brief Writes guest-visible read registers from device callbacks.
 *
 * @param dev Registered backend device descriptor.
 */
static void refresh_read_registers(const struct fabric_device *dev) {
    static const uint64_t registers[] = {
        VIRTIO_MMIO_MAGIC_VALUE,       VIRTIO_MMIO_VERSION,
        VIRTIO_MMIO_DEVICE_ID,         VIRTIO_MMIO_VENDOR_ID,
        VIRTIO_MMIO_DEVICE_FEATURES,   VIRTIO_MMIO_QUEUE_NUM_MAX,
        VIRTIO_MMIO_INTERRUPT_STATUS,  VIRTIO_MMIO_STATUS,
        VIRTIO_MMIO_CONFIG_GENERATION,
    };

    for (size_t i = 0; i < sizeof(registers) / sizeof(registers[0]); i++) {
        uint64_t off = registers[i];
        if (off + 4 <= dev->size && off + 4 <= g_uio.mmio.size) {
            uint32_t value = (uint32_t)dev->ops->read(dev->opaque, off, 4);

            store_le32(g_uio.mmio.addr + off, value);
        }
    }

    for (uint64_t off = VIRTIO_MMIO_CONFIG;
         off + 4 <= dev->size && off + 4 <= g_uio.mmio.size; off += 4) {
        store_le32(g_uio.mmio.addr + off,
                   (uint32_t)dev->ops->read(dev->opaque, off, 4));
    }
}

/**
 * @brief Dispatches changed guest-writable virtio-mmio registers.
 *
 * The daemon wakes on a UIO interrupt and then scans the small set of writable
 * transport registers once. This avoids a polling loop while preserving the
 * existing virtio-mmio backend callback model.
 *
 * @param dev Registered backend device descriptor.
 * @param io Active fabric I/O context.
 * @param shadow Previous register values.
 * @param offsets Writable register offsets to scan.
 * @param count Number of offsets in `offsets` and `shadow`.
 * @return bool True on success, false on backend callback failure.
 */
static bool poll_write_registers(const struct fabric_device *dev,
                                 struct fabric_io *io, uint32_t *shadow,
                                 const uint64_t *offsets, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint64_t off = offsets[i];
        uint32_t value;

        if (off + 4 > dev->size || off + 4 > g_uio.mmio.size) {
            continue;
        }

        value = load_le32(g_uio.mmio.addr + off);
        if (value == shadow[i]) {
            continue;
        }
        shadow[i] = value;
        if (!dev->ops->write(dev->opaque, io, off, value, 4)) {
            return false;
        }

        if (off == VIRTIO_MMIO_QUEUE_NOTIFY) {
            store_le32(g_uio.mmio.addr + off, UIO_NOTIFY_IDLE);
            shadow[i] = UIO_NOTIFY_IDLE;
        } else if (off == VIRTIO_MMIO_INTERRUPT_ACK) {
            store_le32(g_uio.mmio.addr + off, 0);
            shadow[i] = 0;
        }
    }
    return true;
}

/**
 * @brief Blocks until Linux UIO delivers an interrupt and re-enables it.
 *
 * @return bool True when an interrupt was consumed and re-enabled.
 */
static bool wait_for_uio_interrupt(void) {
    uint32_t count;
    uint32_t enable = 1;
    ssize_t ret;

    do {
        ret = read(g_uio.fd, &count, sizeof(count));
    } while (ret < 0 && errno == EINTR);
    if (ret != sizeof(count)) {
        return false;
    }

    do {
        ret = write(g_uio.fd, &enable, sizeof(enable));
    } while (ret < 0 && errno == EINTR);
    return ret == sizeof(enable);
}

/**
 * @brief Initializes an empty UIO fabric instance.
 *
 * @param fabric Fabric instance to initialize.
 */
void fabric_init(struct fabric *fabric) { fabric->device_count = 0; }

/**
 * @brief Registers one backend device with the UIO fabric instance.
 *
 * @param fabric Fabric instance that receives the device registration.
 * @param device Device descriptor to copy into the fabric instance.
 * @return bool True on success, false if registration is invalid or full.
 */
bool fabric_register(struct fabric *fabric,
                     const struct fabric_device *device) {
    if (fabric->device_count >= FABRIC_MAX_DEVICES || !device->ops ||
        !device->ops->read || !device->ops->write) {
        return false;
    }
    fabric->devices[fabric->device_count++] = *device;
    return true;
}

/**
 * @brief Runs the UIO event loop for the registered backend device.
 *
 * @param fabric Fabric instance containing exactly one registered device.
 * @return bool True if the loop exits cleanly, false on setup or callback
 * failure.
 */
bool fabric_run(struct fabric *fabric) {
    static const uint64_t write_offsets[] = {
        VIRTIO_MMIO_DEVICE_FEATURES_SEL,
        VIRTIO_MMIO_DRIVER_FEATURES,
        VIRTIO_MMIO_DRIVER_FEATURES_SEL,
        VIRTIO_MMIO_QUEUE_SEL,
        VIRTIO_MMIO_QUEUE_NUM,
        VIRTIO_MMIO_QUEUE_READY,
        VIRTIO_MMIO_QUEUE_DESC_LOW,
        VIRTIO_MMIO_QUEUE_DESC_HIGH,
        VIRTIO_MMIO_QUEUE_AVAIL_LOW,
        VIRTIO_MMIO_QUEUE_AVAIL_HIGH,
        VIRTIO_MMIO_QUEUE_USED_LOW,
        VIRTIO_MMIO_QUEUE_USED_HIGH,
        VIRTIO_MMIO_QUEUE_NOTIFY,
        VIRTIO_MMIO_INTERRUPT_ACK,
        VIRTIO_MMIO_STATUS,
    };
    uint32_t shadow[sizeof(write_offsets) / sizeof(write_offsets[0])] = {0};
    struct fabric_device *dev;
    struct fabric_io io = {.fd = -1};

    if (fabric->device_count != 1) {
        fprintf(stderr, "uio: expected exactly one registered device, got %u\n",
                fabric->device_count);
        return false;
    }
    dev = &fabric->devices[0];
    if (!parse_endpoint(dev->socket_path, &g_uio.config)) {
        fprintf(stderr, "uio: invalid endpoint '%s'\n", dev->socket_path);
        return false;
    }

    g_uio.fd = open(g_uio.config.path, O_RDWR | O_CLOEXEC);
    if (g_uio.fd < 0) {
        perror("uio: open");
        return false;
    }
    if (!map_resource(&g_uio.config, 0, &g_uio.mmio) ||
        !map_resource(&g_uio.config, 1, &g_uio.dma)) {
        perror("uio: mmap resource");
        cleanup_uio();
        return false;
    }
    if (g_uio.mmio.size < dev->size) {
        fprintf(stderr,
                "uio: resource0 too small: 0x%" PRIx64 " < 0x%" PRIx64 "\n",
                g_uio.mmio.size, dev->size);
        cleanup_uio();
        return false;
    }

    if (dev->ops->connect) {
        dev->ops->connect(dev->opaque);
    }
    for (size_t i = 0; i < sizeof(write_offsets) / sizeof(write_offsets[0]);
         i++) {
        if (write_offsets[i] + 4 <= dev->size) {
            shadow[i] = load_le32(g_uio.mmio.addr + write_offsets[i]);
        }
    }
    store_le32(g_uio.mmio.addr + VIRTIO_MMIO_QUEUE_NOTIFY, UIO_NOTIFY_IDLE);
    for (size_t i = 0; i < sizeof(write_offsets) / sizeof(write_offsets[0]);
         i++) {
        if (write_offsets[i] == VIRTIO_MMIO_QUEUE_NOTIFY) {
            shadow[i] = UIO_NOTIFY_IDLE;
        }
    }
    refresh_read_registers(dev);

    fprintf(stderr,
            "%s: serving UIO device %s mmio=0x%" PRIx64 " dma=0x%" PRIx64
            " irq-control=0x%" PRIx64 "\n",
            dev->name, g_uio.config.path, g_uio.mmio.size, g_uio.dma.size,
            g_uio.config.irq_control_offset);

    for (;;) {
        if (!wait_for_uio_interrupt() ||
            !poll_write_registers(dev, &io, shadow, write_offsets,
                                  sizeof(write_offsets) /
                                      sizeof(write_offsets[0]))) {
            cleanup_uio();
            return false;
        }
        refresh_read_registers(dev);
    }
}

/**
 * @brief Reads bytes from the frontend RAM DMA resource.
 *
 * @param io Active fabric I/O context.
 * @param gpa Frontend guest physical address.
 * @param len Number of bytes to read.
 * @param data Output heap buffer containing copied bytes; caller frees it.
 * @return bool True on success, false on bounds or allocation failure.
 */
bool fabric_dma_read(struct fabric_io *io, uint64_t gpa, uint32_t len,
                     uint8_t **data) {
    (void)io;
    *data = NULL;
    if (!len) {
        return true;
    }
    if (gpa > g_uio.dma.size || len > g_uio.dma.size - gpa) {
        return false;
    }
    *data = malloc(len);
    if (!*data) {
        return false;
    }
    memcpy(*data, g_uio.dma.addr + gpa, len);
    return true;
}

/**
 * @brief Reads a 16-bit little-endian value from the DMA resource.
 *
 * @param io Active fabric I/O context.
 * @param gpa Frontend guest physical address.
 * @param value Output decoded host-endian value.
 * @return bool True on success, false on DMA read failure.
 */
bool fabric_dma_read_u16(struct fabric_io *io, uint64_t gpa, uint16_t *value) {
    uint8_t *data = NULL;

    if (!fabric_dma_read(io, gpa, 2, &data)) {
        return false;
    }
    *value = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    free(data);
    return true;
}

/**
 * @brief Writes bytes into the frontend RAM DMA resource.
 *
 * @param io Active fabric I/O context.
 * @param gpa Frontend guest physical address.
 * @param data Bytes to write.
 * @param len Number of bytes to write.
 * @return bool True on success, false when the write is out of bounds.
 */
bool fabric_dma_write(struct fabric_io *io, uint64_t gpa, const void *data,
                      uint32_t len) {
    (void)io;
    if (!len) {
        return true;
    }
    if (gpa > g_uio.dma.size || len > g_uio.dma.size - gpa) {
        return false;
    }
    memcpy(g_uio.dma.addr + gpa, data, len);
    return true;
}

/**
 * @brief Requests frontend virtio IRQ assertion through the control register.
 *
 * @param io Active fabric I/O context.
 * @return bool True on success, false when the control register is unavailable.
 */
bool fabric_raise_irq(struct fabric_io *io) {
    (void)io;
    if (g_uio.config.irq_control_offset + 4 > g_uio.mmio.size) {
        return false;
    }
    store_le32(g_uio.mmio.addr + g_uio.config.irq_control_offset, 1);
    return true;
}

/**
 * @brief Requests frontend virtio IRQ deassertion through the control register.
 *
 * @param io Active fabric I/O context.
 * @return bool True on success, false when the control register is unavailable.
 */
bool fabric_lower_irq(struct fabric_io *io) {
    (void)io;
    if (g_uio.config.irq_control_offset + 4 > g_uio.mmio.size) {
        return false;
    }
    store_le32(g_uio.mmio.addr + g_uio.config.irq_control_offset, 0);
    return true;
}
