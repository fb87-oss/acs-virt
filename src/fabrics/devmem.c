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

#define DEVMEM_NOTIFY_IDLE 0xffffffffu

/** @brief Runtime configuration for the Linux /dev/mem fabric. */
struct devmem_config {
    const char *path;    ///< Character device used for physical mappings.
    uint64_t mmio_base;  ///< Physical base address of the virtio-mmio aperture.
    uint64_t mmio_size;  ///< Mapped virtio-mmio aperture size.
    uint32_t poll_us;    ///< Poll interval for guest-written registers.
    bool have_irq;       ///< Whether an IRQ control register is configured.
    uint64_t irq_addr;   ///< Physical address of the IRQ control register.
    uint32_t irq_assert; ///< Value written to assert the interrupt line.
    uint32_t irq_deassert; ///< Value written to deassert the interrupt line.
};

/** @brief Active /dev/mem mapping state. */
struct devmem_state {
    int fd;                      ///< Open /dev/mem file descriptor.
    uint8_t *mmio_map;           ///< Page-aligned mapped MMIO base pointer.
    uint8_t *mmio;               ///< Mapped MMIO aperture pointer.
    size_t mmio_map_len;         ///< Length passed to munmap for the aperture.
    size_t mmio_page_delta;      ///< Offset from mapped page base to aperture.
    struct devmem_config config; ///< Parsed runtime configuration.
};

static struct devmem_state g_devmem = {.fd = -1};

/**
 * @brief Loads a little-endian 32-bit value from a memory-mapped register.
 *
 * @param p Register address.
 * @return uint32_t Loaded value.
 */
static uint32_t load_le32(const volatile uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/**
 * @brief Stores a little-endian 32-bit value to a memory-mapped register.
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
 * @brief Parses a 64-bit integer from an environment value.
 *
 * @param value String value to parse.
 * @param out Output parsed integer.
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
 * @brief Parses a 32-bit integer from an environment value.
 *
 * @param value String value to parse.
 * @param out Output parsed integer.
 * @return bool True on success, false on malformed input or overflow.
 */
static bool parse_u32(const char *value, uint32_t *out) {
    uint64_t parsed;

    if (!parse_u64(value, &parsed) || parsed > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)parsed;
    return true;
}

/**
 * @brief Reads an optional 64-bit environment value.
 *
 * @param name Environment variable name.
 * @param out Output parsed value when present.
 * @return bool True when unset or valid, false when malformed.
 */
static bool env_u64(const char *name, uint64_t *out) {
    const char *value = getenv(name);

    return !value || parse_u64(value, out);
}

/**
 * @brief Reads an optional 32-bit environment value.
 *
 * @param name Environment variable name.
 * @param out Output parsed value when present.
 * @return bool True when unset or valid, false when malformed.
 */
static bool env_u32(const char *name, uint32_t *out) {
    const char *value = getenv(name);

    return !value || parse_u32(value, out);
}

/**
 * @brief Parses a compact devmem endpoint string.
 *
 * @param endpoint Endpoint string from the backend device descriptor.
 * @param config Configuration object to update.
 * @return bool True on success, false on malformed input.
 */
static bool parse_endpoint(const char *endpoint, struct devmem_config *config) {
    char copy[256];
    char *save = NULL;
    char *part;
    unsigned index = 0;

    if (!endpoint || strncmp(endpoint, "devmem:", 7)) {
        return true;
    }
    if (strlen(endpoint + 7) >= sizeof(copy)) {
        return false;
    }
    strcpy(copy, endpoint + 7);

    for (part = strtok_r(copy, ":", &save); part;
         part = strtok_r(NULL, ":", &save), index++) {
        switch (index) {
        case 0:
            if (!parse_u64(part, &config->mmio_base)) {
                return false;
            }
            break;
        case 1:
            if (!parse_u64(part, &config->mmio_size)) {
                return false;
            }
            break;
        case 2:
            if (!parse_u32(part, &config->poll_us)) {
                return false;
            }
            break;
        default:
            return false;
        }
    }

    return index >= 1;
}

/**
 * @brief Builds the devmem fabric configuration from endpoint and environment.
 *
 * @param device Registered backend device descriptor.
 * @param config Output configuration.
 * @return bool True on success, false on missing or malformed config.
 */
static bool load_config(const struct fabric_device *device,
                        struct devmem_config *config) {
    const char *path = getenv("CHIPLETS_DEVMEM_PATH");
    const char *base = getenv("CHIPLETS_DEVMEM_MMIO_BASE");

    *config = (struct devmem_config){
        .path = path && path[0] ? path : "/dev/mem",
        .mmio_base = 0,
        .mmio_size = device->size,
        .poll_us = 1000,
        .have_irq = false,
        .irq_addr = 0,
        .irq_assert = 1,
        .irq_deassert = 0,
    };

    if (base && !parse_u64(base, &config->mmio_base)) {
        fprintf(stderr, "devmem: invalid CHIPLETS_DEVMEM_MMIO_BASE=%s\n", base);
        return false;
    }
    if (!env_u64("CHIPLETS_DEVMEM_MMIO_SIZE", &config->mmio_size) ||
        !env_u32("CHIPLETS_DEVMEM_POLL_US", &config->poll_us) ||
        !env_u32("CHIPLETS_DEVMEM_IRQ_ASSERT", &config->irq_assert) ||
        !env_u32("CHIPLETS_DEVMEM_IRQ_DEASSERT", &config->irq_deassert)) {
        fprintf(stderr, "devmem: invalid numeric environment value\n");
        return false;
    }
    if (!parse_endpoint(device->socket_path, config)) {
        fprintf(stderr, "devmem: invalid endpoint '%s'\n", device->socket_path);
        return false;
    }

    if (getenv("CHIPLETS_DEVMEM_IRQ_ADDR")) {
        if (!parse_u64(getenv("CHIPLETS_DEVMEM_IRQ_ADDR"), &config->irq_addr)) {
            fprintf(stderr, "devmem: invalid CHIPLETS_DEVMEM_IRQ_ADDR\n");
            return false;
        }
        config->have_irq = true;
    }

    if (!config->mmio_base || config->mmio_size < device->size) {
        fprintf(stderr, "devmem: mmio base/size not configured\n");
        return false;
    }
    return true;
}

/**
 * @brief Maps a physical address range from the active /dev/mem descriptor.
 *
 * @param paddr Physical address to map.
 * @param len Number of bytes needed from paddr.
 * @param map_len Output full mapped length for munmap.
 * @param page_delta Output offset from mapped page base to paddr.
 * @return void* Mapped page base, or MAP_FAILED on failure.
 */
static void *map_phys(uint64_t paddr, size_t len, size_t *map_len,
                      size_t *page_delta) {
    long page_size = sysconf(_SC_PAGESIZE);
    uint64_t page_mask = (uint64_t)page_size - 1;
    uint64_t base = paddr & ~page_mask;

    *page_delta = (size_t)(paddr - base);
    *map_len = *page_delta + len;
    return mmap(NULL, *map_len, PROT_READ | PROT_WRITE, MAP_SHARED, g_devmem.fd,
                (off_t)base);
}

/** @brief Releases the active devmem file descriptor and MMIO mapping. */
static void cleanup_devmem(void) {
    if (g_devmem.mmio_map) {
        munmap(g_devmem.mmio_map, g_devmem.mmio_map_len);
        g_devmem.mmio_map = NULL;
        g_devmem.mmio = NULL;
        g_devmem.mmio_map_len = 0;
        g_devmem.mmio_page_delta = 0;
    }
    if (g_devmem.fd >= 0) {
        close(g_devmem.fd);
        g_devmem.fd = -1;
    }
}

/**
 * @brief Writes all guest-visible read registers from device callbacks.
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
        if (off + 4 <= dev->size) {
            store_le32(g_devmem.mmio + off,
                       (uint32_t)dev->ops->read(dev->opaque, off, 4));
        }
    }

    for (uint64_t off = VIRTIO_MMIO_CONFIG; off + 4 <= dev->size; off += 4) {
        store_le32(g_devmem.mmio + off,
                   (uint32_t)dev->ops->read(dev->opaque, off, 4));
    }
}

/**
 * @brief Polls guest-writable virtio-mmio registers and dispatches changes.
 *
 * @param dev Registered backend device descriptor.
 * @param io Active devmem fabric I/O context.
 * @param shadow Shadow copy of the writeable registers.
 * @param offsets Register offsets tracked by shadow.
 * @param count Number of tracked offsets.
 * @return bool True on success, false on device callback failure.
 */
static bool poll_write_registers(const struct fabric_device *dev,
                                 struct fabric_io *io, uint32_t *shadow,
                                 const uint64_t *offsets, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint64_t off = offsets[i];
        uint32_t value;

        if (off + 4 > dev->size) {
            continue;
        }

        value = load_le32(g_devmem.mmio + off);
        if (value == shadow[i]) {
            continue;
        }
        shadow[i] = value;
        if (!dev->ops->write(dev->opaque, io, off, value, 4)) {
            return false;
        }

        if (off == VIRTIO_MMIO_QUEUE_NOTIFY) {
            store_le32(g_devmem.mmio + off, DEVMEM_NOTIFY_IDLE);
            shadow[i] = DEVMEM_NOTIFY_IDLE;
        } else if (off == VIRTIO_MMIO_INTERRUPT_ACK) {
            store_le32(g_devmem.mmio + off, 0);
            shadow[i] = 0;
        }
    }
    return true;
}

/**
 * @brief Initializes an empty devmem fabric instance.
 *
 * @param fabric Fabric instance to initialize.
 */
void fabric_init(struct fabric *fabric) { fabric->device_count = 0; }

/**
 * @brief Registers one backend device with the devmem fabric instance.
 *
 * @param fabric Fabric instance that receives the device registration.
 * @param device Device descriptor to copy into the fabric instance.
 * @return bool True on success, false if the registration is invalid or full.
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
 * @brief Runs the devmem fabric polling loop.
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
        fprintf(stderr,
                "devmem: expected exactly one registered device, got %u\n",
                fabric->device_count);
        return false;
    }
    dev = &fabric->devices[0];
    if (!load_config(dev, &g_devmem.config)) {
        return false;
    }

    g_devmem.fd = open(g_devmem.config.path, O_RDWR | O_SYNC);
    if (g_devmem.fd < 0) {
        perror("devmem: open");
        return false;
    }
    g_devmem.mmio_map =
        map_phys(g_devmem.config.mmio_base, (size_t)g_devmem.config.mmio_size,
                 &g_devmem.mmio_map_len, &g_devmem.mmio_page_delta);
    if (g_devmem.mmio_map == MAP_FAILED) {
        perror("devmem: mmap mmio");
        g_devmem.mmio_map = NULL;
        cleanup_devmem();
        return false;
    }
    g_devmem.mmio = g_devmem.mmio_map + g_devmem.mmio_page_delta;

    if (dev->ops->connect) {
        dev->ops->connect(dev->opaque);
    }
    for (size_t i = 0; i < sizeof(write_offsets) / sizeof(write_offsets[0]);
         i++) {
        if (write_offsets[i] + 4 <= dev->size) {
            shadow[i] = load_le32(g_devmem.mmio + write_offsets[i]);
        }
    }
    store_le32(g_devmem.mmio + VIRTIO_MMIO_QUEUE_NOTIFY, DEVMEM_NOTIFY_IDLE);
    for (size_t i = 0; i < sizeof(write_offsets) / sizeof(write_offsets[0]);
         i++) {
        if (write_offsets[i] == VIRTIO_MMIO_QUEUE_NOTIFY) {
            shadow[i] = DEVMEM_NOTIFY_IDLE;
        }
    }
    refresh_read_registers(dev);

    fprintf(stderr,
            "%s: serving devmem aperture phys=0x%" PRIx64 " size=0x%" PRIx64
            " path=%s\n",
            dev->name, g_devmem.config.mmio_base, g_devmem.config.mmio_size,
            g_devmem.config.path);
    for (;;) {
        if (!poll_write_registers(dev, &io, shadow, write_offsets,
                                  sizeof(write_offsets) /
                                      sizeof(write_offsets[0]))) {
            cleanup_devmem();
            return false;
        }
        refresh_read_registers(dev);
        usleep(g_devmem.config.poll_us);
    }
}

/**
 * @brief Requests a guest physical memory read through devmem.
 *
 * @param io Active fabric I/O context.
 * @param gpa Guest physical address to read from.
 * @param len Number of bytes to read.
 * @param data Output buffer pointer.
 * @return bool True on success, false on mmap or allocation failure.
 */
bool fabric_dma_read(struct fabric_io *io, uint64_t gpa, uint32_t len,
                     uint8_t **data) {
    size_t map_len;
    size_t page_delta;
    uint8_t *map;

    (void)io;
    *data = NULL;
    if (!len) {
        return true;
    }
    *data = malloc(len);
    if (!*data) {
        return false;
    }
    map = map_phys(gpa, len, &map_len, &page_delta);
    if (map == MAP_FAILED) {
        free(*data);
        *data = NULL;
        return false;
    }
    memcpy(*data, map + page_delta, len);
    munmap(map, map_len);
    return true;
}

/**
 * @brief Requests a 16-bit little-endian guest physical memory read through
 * devmem.
 *
 * @param io Active fabric I/O context.
 * @param gpa Guest physical address to read from.
 * @param value Output decoded host-endian value.
 * @return bool True on success, false on mmap or allocation failure.
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
 * @brief Requests a guest physical memory write through devmem.
 *
 * @param io Active fabric I/O context.
 * @param gpa Guest physical address to write to.
 * @param data Bytes to write.
 * @param len Number of bytes to write.
 * @return bool True on success, false on mmap failure.
 */
bool fabric_dma_write(struct fabric_io *io, uint64_t gpa, const void *data,
                      uint32_t len) {
    size_t map_len;
    size_t page_delta;
    uint8_t *map;

    (void)io;
    if (!len) {
        return true;
    }
    map = map_phys(gpa, len, &map_len, &page_delta);
    if (map == MAP_FAILED) {
        return false;
    }
    memcpy(map + page_delta, data, len);
    munmap(map, map_len);
    return true;
}

/**
 * @brief Raises a devmem fabric interrupt.
 *
 * @param io Active fabric I/O context.
 * @return bool True on success, false on mmap failure.
 */
bool fabric_raise_irq(struct fabric_io *io) {
    size_t map_len;
    size_t page_delta;
    uint8_t *map;

    (void)io;
    if (!g_devmem.config.have_irq) {
        return true;
    }
    if (!fabric_lower_irq(io)) {
        return false;
    }
    map = map_phys(g_devmem.config.irq_addr, 4, &map_len, &page_delta);
    if (map == MAP_FAILED) {
        return false;
    }
    store_le32(map + page_delta, g_devmem.config.irq_assert);
    munmap(map, map_len);
    return true;
}

/**
 * @brief Lowers a devmem fabric interrupt.
 *
 * @param io Active fabric I/O context.
 * @return bool True on success, false on mmap failure.
 */
bool fabric_lower_irq(struct fabric_io *io) {
    size_t map_len;
    size_t page_delta;
    uint8_t *map;

    (void)io;
    if (!g_devmem.config.have_irq) {
        return true;
    }
    map = map_phys(g_devmem.config.irq_addr, 4, &map_len, &page_delta);
    if (map == MAP_FAILED) {
        return false;
    }
    store_le32(map + page_delta, g_devmem.config.irq_deassert);
    munmap(map, map_len);
    return true;
}
