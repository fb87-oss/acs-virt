#include "fabric.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define AXI_MSG_MMIO_READ 3
#define AXI_MSG_MMIO_READ_REPLY 4
#define AXI_MSG_MMIO_WRITE 5
#define AXI_MSG_IRQ_ASSERT 6
#define AXI_MSG_IRQ_DEASSERT 7
#define AXI_MSG_DMA_READ 8
#define AXI_MSG_DMA_READ_REPLY 9
#define AXI_MSG_DMA_WRITE 10
#define AXI_MSG_ERROR 0xffff

#define AXI_HEADER_LEN 24

/** @brief Wire-format AXI protocol header. */
struct axi_header {
    uint16_t kind;
    uint16_t flags;
    uint32_t window_id;
    uint64_t offset;
    uint32_t length;
};

/**
 * @brief Loads a 16-bit little-endian value from a protocol buffer.
 *
 * @param p Pointer to the first encoded byte.
 * @return uint16_t Decoded host-endian value.
 */
static uint16_t load_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/**
 * @brief Loads a 32-bit little-endian value from a protocol buffer.
 *
 * @param p Pointer to the first encoded byte.
 * @return uint32_t Decoded host-endian value.
 */
static uint32_t load_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/**
 * @brief Loads a 64-bit little-endian value from a protocol buffer.
 *
 * @param p Pointer to the first encoded byte.
 * @return uint64_t Decoded host-endian value.
 */
static uint64_t load_le64(const uint8_t *p) {
    return (uint64_t)load_le32(p) | ((uint64_t)load_le32(p + 4) << 32);
}

/**
 * @brief Stores a 16-bit little-endian value into a protocol buffer.
 *
 * @param p Pointer to the output buffer.
 * @param v Host-endian value to encode.
 */
static void store_le16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}

/**
 * @brief Stores a 32-bit little-endian value into a protocol buffer.
 *
 * @param p Pointer to the output buffer.
 * @param v Host-endian value to encode.
 */
static void store_le32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

/**
 * @brief Stores a 64-bit little-endian value into a protocol buffer.
 *
 * @param p Pointer to the output buffer.
 * @param v Host-endian value to encode.
 */
static void store_le64(uint8_t *p, uint64_t v) {
    store_le32(p, (uint32_t)v);
    store_le32(p + 4, (uint32_t)(v >> 32));
}

/**
 * @brief Reads or writes exactly len bytes unless the peer closes or an error
 * occurs.
 *
 * @param fd Socket file descriptor.
 * @param buf Buffer to read into or write from.
 * @param len Number of bytes to transfer.
 * @param write_op True for write, false for read.
 * @return bool True when all bytes are transferred.
 */
static bool io_all(int fd, void *buf, size_t len, bool write_op) {
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

/**
 * @brief Reads and decodes one AXI protocol header from a socket.
 *
 * @param fd Socket file descriptor.
 * @param h Output decoded header.
 * @return bool True on success, false on socket failure.
 */
static bool read_header(int fd, struct axi_header *h) {
    uint8_t buf[AXI_HEADER_LEN];
    if (!io_all(fd, buf, sizeof(buf), false)) {
        return false;
    }

    h->kind = load_le16(buf);
    h->flags = load_le16(buf + 2);
    h->window_id = load_le32(buf + 4);
    h->offset = load_le64(buf + 8);
    h->length = load_le32(buf + 16);
    return true;
}

/**
 * @brief Encodes and writes one AXI protocol header to a socket.
 *
 * @param fd Socket file descriptor.
 * @param h Header to encode and write.
 * @return bool True on success, false on socket failure.
 */
static bool write_header(int fd, const struct axi_header *h) {
    uint8_t buf[AXI_HEADER_LEN] = {0};
    store_le16(buf, h->kind);
    store_le16(buf + 2, h->flags);
    store_le32(buf + 4, h->window_id);
    store_le64(buf + 8, h->offset);
    store_le32(buf + 16, h->length);
    return io_all(fd, buf, sizeof(buf), true);
}

/**
 * @brief Replies to an MMIO read request with a width-limited little-endian
 * value.
 *
 * @param fd Socket file descriptor.
 * @param request Original MMIO read request header.
 * @param value Device register value to return.
 * @return bool True on success, false on socket failure.
 */
static bool write_read_reply(int fd, const struct axi_header *request,
                             uint64_t value) {
    uint8_t bytes[8];
    uint32_t len = request->length < 8 ? request->length : 8;
    struct axi_header reply = {
        .kind = AXI_MSG_MMIO_READ_REPLY,
        .flags = request->flags,
        .window_id = request->window_id,
        .offset = request->offset,
        .length = len,
    };

    store_le64(bytes, value);
    return write_header(fd, &reply) && io_all(fd, bytes, len, true);
}

/**
 * @brief Reads an MMIO write payload and decodes it as a little-endian value.
 *
 * @param fd Socket file descriptor.
 * @param len Payload length in bytes.
 * @param value Output decoded host-endian value.
 * @return bool True on success, false on invalid length or socket failure.
 */
static bool read_value(int fd, uint32_t len, uint64_t *value) {
    uint8_t bytes[8] = {0};
    if (len > sizeof(bytes)) {
        return false;
    }
    if (!io_all(fd, bytes, len, false)) {
        return false;
    }
    *value = load_le64(bytes);
    return true;
}

/**
 * @brief Checks whether a request targets the registered device aperture.
 *
 * @param dev Registered device descriptor.
 * @param h Request header to validate.
 * @return bool True when the request is in range.
 */
static bool access_in_range(const struct fabric_device *dev,
                            const struct axi_header *h) {
    return h->window_id == 0 && h->length <= 8 && h->offset >= dev->addr &&
           h->offset <= dev->addr + dev->size &&
           h->length <= dev->addr + dev->size - h->offset;
}

/**
 * @brief Converts an absolute guest bus address into a device-relative offset.
 *
 * @param dev Registered device descriptor.
 * @param bus_addr Absolute guest bus address.
 * @return uint64_t Device-relative offset.
 */
static uint64_t device_offset(const struct fabric_device *dev,
                              uint64_t bus_addr) {
    return bus_addr - dev->addr;
}

/**
 * @brief Creates parent directories needed for a Unix socket path.
 *
 * @param path Unix socket path.
 * @return bool True on success, false on path or mkdir failure.
 */
static bool ensure_parent_dir(const char *path) {
    char tmp[4096];
    char partial[4096];
    bool absolute;
    char *slash;
    char *save = NULL;

    if (strlen(path) >= sizeof(tmp)) {
        return false;
    }
    strcpy(tmp, path);

    slash = strrchr(tmp, '/');
    if (!slash) {
        return true;
    }
    *slash = '\0';
    if (!tmp[0]) {
        return true;
    }

    absolute = tmp[0] == '/';
    snprintf(partial, sizeof(partial), "%s", absolute ? "/" : "");
    for (char *part = strtok_r(tmp, "/", &save); part;
         part = strtok_r(NULL, "/", &save)) {
        if (strlen(partial) + strlen(part) + 2 >= sizeof(partial)) {
            return false;
        }
        if (partial[0] && strcmp(partial, "/")) {
            strcat(partial, "/");
        }
        strcat(partial, part);
        if (mkdir(partial, 0777) < 0 && errno != EEXIST) {
            return false;
        }
    }

    return true;
}

/**
 * @brief Serves MMIO requests for one accepted QEMU socket connection.
 *
 * @param fd Connected QEMU socket.
 * @param dev Registered device descriptor.
 * @return bool True on clean service completion, false on protocol or socket
 * failure.
 */
static bool serve_connection(int fd, const struct fabric_device *dev) {
    struct fabric_io io = {.fd = fd};

    if (dev->ops->connect) {
        dev->ops->connect(dev->opaque);
    }

    for (;;) {
        struct axi_header h;
        if (!read_header(fd, &h)) {
            return false;
        }
        if (!access_in_range(dev, &h)) {
            fprintf(stderr,
                    "%s: invalid MMIO window=%u offset=0x%" PRIx64 " len=%u\n",
                    dev->name, h.window_id, h.offset, h.length);
            return false;
        }

        switch (h.kind) {
        case AXI_MSG_MMIO_READ: {
            uint64_t offset = device_offset(dev, h.offset);
            uint64_t value = dev->ops->read(dev->opaque, offset, h.length);
            fprintf(stderr,
                    "%s: read offset=0x%" PRIx64 " len=%u -> 0x%" PRIx64 "\n",
                    dev->name, offset, h.length, value);
            if (!write_read_reply(fd, &h, value)) {
                return false;
            }
            break;
        }
        case AXI_MSG_MMIO_WRITE: {
            uint64_t value;
            struct axi_header ack = {
                .kind = AXI_MSG_ERROR,
                .flags = 0,
                .window_id = 0,
                .offset = 0,
                .length = 0,
            };
            if (!read_value(fd, h.length, &value)) {
                return false;
            }
            uint64_t offset = device_offset(dev, h.offset);
            fprintf(stderr,
                    "%s: write offset=0x%" PRIx64 " len=%u value=0x%" PRIx64
                    "\n",
                    dev->name, offset, h.length, value);
            if (!dev->ops->write(dev->opaque, &io, offset, value, h.length)) {
                return false;
            }
            if (!write_header(fd, &ack)) {
                return false;
            }
            break;
        }
        default:
            fprintf(stderr, "%s: unsupported message kind=%u\n", dev->name,
                    h.kind);
            return false;
        }
    }
}

/**
 * @brief Binds a device socket and accepts QEMU connections forever.
 *
 * @param dev Registered device descriptor.
 * @return bool True only if the server exits cleanly, false on setup or accept
 * failure.
 */
static bool run_device(const struct fabric_device *dev) {
    int listen_fd;
    struct sockaddr_un addr;

    if (!ensure_parent_dir(dev->socket_path)) {
        fprintf(stderr, "%s: failed to create socket parent directory\n",
                dev->name);
        return false;
    }

    unlink(dev->socket_path);
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("axi: socket");
        return false;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(dev->socket_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "%s: socket path too long: %s\n", dev->name,
                dev->socket_path);
        close(listen_fd);
        return false;
    }
    strcpy(addr.sun_path, dev->socket_path);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(listen_fd, 8) < 0) {
        perror("axi: bind/listen");
        close(listen_fd);
        return false;
    }

    fprintf(stderr, "%s: serving AXI socket %s\n", dev->name, dev->socket_path);
    for (;;) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("axi: accept");
            close(listen_fd);
            return false;
        }

        fprintf(stderr, "%s: accepted QEMU connection\n", dev->name);
        if (!serve_connection(client_fd, dev)) {
            fprintf(stderr, "%s: connection closed\n", dev->name);
        }
        close(client_fd);
    }
}

/**
 * @brief Initializes an empty AXI fabric instance.
 *
 * @param bus Fabric instance to initialize.
 */
void fabric_init(struct fabric *bus) { bus->device_count = 0; }

/**
 * @brief Registers a backend device with an AXI fabric instance.
 *
 * @param bus Fabric instance that receives the device registration.
 * @param device Device descriptor to copy into the fabric instance.
 * @return bool True on success, false if registration is invalid or full.
 */
bool fabric_register(struct fabric *bus, const struct fabric_device *device) {
    if (bus->device_count >= FABRIC_MAX_DEVICES || !device->ops ||
        !device->ops->read || !device->ops->write) {
        return false;
    }
    bus->devices[bus->device_count++] = *device;
    return true;
}

/**
 * @brief Runs the AXI socket loop for the registered device.
 *
 * @param bus Fabric instance containing exactly one registered device.
 * @return bool True if the server exits cleanly, false on setup or protocol
 * failure.
 */
bool fabric_run(struct fabric *bus) {
    if (bus->device_count != 1) {
        fprintf(stderr, "axi: expected exactly one registered device, got %u\n",
                bus->device_count);
        return false;
    }
    return run_device(&bus->devices[0]);
}

/**
 * @brief Requests a guest memory read from QEMU over the active socket.
 *
 * @param io Active fabric I/O context.
 * @param gpa Guest physical address to read from.
 * @param len Number of bytes to read.
 * @param data Output heap buffer containing the read bytes; caller frees it.
 * @return bool True on success, false on socket or protocol failure.
 */
bool fabric_dma_read(struct fabric_io *io, uint64_t gpa, uint32_t len,
                     uint8_t **data) {
    uint8_t *buf = NULL;

    if (len) {
        buf = malloc(len);
        if (!buf) {
            return false;
        }
    }
    if (!fabric_dma_read_into(io, gpa, len, buf)) {
        free(buf);
        return false;
    }

    *data = buf;
    return true;
}

/**
 * @brief Requests a guest memory read from QEMU into an existing buffer.
 *
 * @param io Active fabric I/O context.
 * @param gpa Guest physical address to read from.
 * @param len Number of bytes to read.
 * @param data Destination buffer.
 * @return bool True on success, false on socket or protocol failure.
 */
bool fabric_dma_read_into(struct fabric_io *io, uint64_t gpa, uint32_t len,
                          void *data) {
    struct axi_header request = {
        .kind = AXI_MSG_DMA_READ,
        .flags = 0,
        .window_id = 0,
        .offset = gpa,
        .length = len,
    };
    struct axi_header reply;

    if (!write_header(io->fd, &request) || !read_header(io->fd, &reply)) {
        return false;
    }
    if (reply.kind != AXI_MSG_DMA_READ_REPLY || reply.length != len) {
        return false;
    }

    if (len && !io_all(io->fd, data, len, false)) {
        return false;
    }
    return true;
}

/** @brief Socket-backed AXI DMA cannot expose a direct local memory mapping. */
bool fabric_dma_map(struct fabric_io *io, uint64_t gpa, uint32_t len,
                    void **data) {
    (void)io;
    (void)gpa;
    (void)len;
    *data = NULL;
    return false;
}

/** @brief Socket-backed AXI DMA mappings are never acquired. */
void fabric_dma_unmap(struct fabric_io *io, void *data, uint32_t len) {
    (void)io;
    (void)data;
    (void)len;
}

/**
 * @brief Requests and decodes a 16-bit little-endian guest memory read.
 *
 * @param io Active fabric I/O context.
 * @param gpa Guest physical address to read from.
 * @param value Output decoded host-endian value.
 * @return bool True on success, false on socket or protocol failure.
 */
bool fabric_dma_read_u16(struct fabric_io *io, uint64_t gpa, uint16_t *value) {
    uint8_t data[2];

    if (!fabric_dma_read_into(io, gpa, sizeof(data), data)) {
        return false;
    }
    *value = load_le16(data);
    return true;
}

/**
 * @brief Requests a guest memory write through QEMU over the active socket.
 *
 * @param io Active fabric I/O context.
 * @param gpa Guest physical address to write to.
 * @param data Bytes to write.
 * @param len Number of bytes to write.
 * @return bool True on success, false on socket or protocol failure.
 */
bool fabric_dma_write(struct fabric_io *io, uint64_t gpa, const void *data,
                      uint32_t len) {
    struct axi_header request = {
        .kind = AXI_MSG_DMA_WRITE,
        .flags = 0,
        .window_id = 0,
        .offset = gpa,
        .length = len,
    };
    struct axi_header reply;

    if (!write_header(io->fd, &request)) {
        return false;
    }
    if (len && !io_all(io->fd, (void *)data, len, true)) {
        return false;
    }
    if (!read_header(io->fd, &reply)) {
        return false;
    }
    return reply.kind == AXI_MSG_ERROR;
}

/**
 * @brief Deasserts then asserts the configured IRQ line to create an edge.
 *
 * @param io Active fabric I/O context.
 * @return bool True on success, false on socket failure.
 */
bool fabric_raise_irq(struct fabric_io *io) {
    struct axi_header irq = {
        .kind = AXI_MSG_IRQ_ASSERT,
        .flags = 0,
        .window_id = 0,
        .offset = 0,
        .length = 0,
    };

    return fabric_lower_irq(io) && write_header(io->fd, &irq);
}

/**
 * @brief Deasserts the configured IRQ line.
 *
 * @param io Active fabric I/O context.
 * @return bool True on success, false on socket failure.
 */
bool fabric_lower_irq(struct fabric_io *io) {
    struct axi_header irq = {
        .kind = AXI_MSG_IRQ_DEASSERT,
        .flags = 0,
        .window_id = 0,
        .offset = 0,
        .length = 0,
    };

    return write_header(io->fd, &irq);
}
