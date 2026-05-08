#ifndef FABRIC_H
#define FABRIC_H

#include <stdbool.h>
#include <stdint.h>

#define FABRIC_MAX_DEVICES 8

/** @brief Per-connection fabric I/O context passed to device callbacks. */
struct fabric_io {
    int fd; ///< Fabric-specific connection descriptor.
};

/** @brief Device callbacks used by backend fabric implementations. */
struct fabric_device_ops {
    void (*connect)(
        void *opaque); ///< Optional hook invoked after fabric connect.
    uint64_t (*read)(void *opaque, uint64_t offset,
                     uint32_t len); ///< Read a device-relative MMIO value.
    bool (*write)(void *opaque, struct fabric_io *io, uint64_t offset,
                  uint64_t value,
                  uint32_t len); ///< Write a device-relative MMIO value.
};

/** @brief Registered MMIO device exposed through the selected backend fabric.
 */
struct fabric_device {
    const char *name;        ///< Short diagnostic name used in backend logs.
    const char *socket_path; ///< Fabric endpoint path or identifier.
    uint64_t addr; ///< Absolute guest bus base address for the MMIO aperture.
    uint64_t size; ///< Size in bytes of the MMIO aperture.
    void *opaque;  ///< Driver-owned callback state.
    const struct fabric_device_ops *ops; ///< Device MMIO operation table.
};

/** @brief Backend fabric instance containing registered devices. */
struct fabric {
    struct fabric_device devices[FABRIC_MAX_DEVICES]; ///< Registered devices.
    uint32_t device_count; ///< Number of populated entries in devices.
};

/**
 * @brief Initializes an empty backend fabric instance.
 *
 * @param fabric Backend fabric instance to initialize.
 */
void fabric_init(struct fabric *fabric);

/**
 * @brief Registers one backend device with a fabric instance.
 *
 * @param fabric Fabric instance that receives the device registration.
 * @param device Device descriptor to copy into the fabric instance.
 * @return bool True on success, false if the registration is invalid or full.
 */
bool fabric_register(struct fabric *fabric, const struct fabric_device *device);

/**
 * @brief Runs the selected backend fabric server.
 *
 * @param fabric Fabric instance containing registered devices.
 * @return bool True if the server exits cleanly, false on setup or protocol
 * failure.
 */
bool fabric_run(struct fabric *fabric);

/**
 * @brief Requests a guest physical memory read through the active fabric.
 *
 * @param io Active fabric I/O context.
 * @param gpa Guest physical address to read from.
 * @param len Number of bytes to read.
 * @param data Output heap buffer containing the read bytes; caller frees it.
 * @return bool True on success, false on fabric or protocol failure.
 */
bool fabric_dma_read(struct fabric_io *io, uint64_t gpa, uint32_t len,
                     uint8_t **data);

/**
 * @brief Requests a 16-bit little-endian guest physical memory read.
 *
 * @param io Active fabric I/O context.
 * @param gpa Guest physical address to read from.
 * @param value Output decoded host-endian value.
 * @return bool True on success, false on fabric or protocol failure.
 */
bool fabric_dma_read_u16(struct fabric_io *io, uint64_t gpa, uint16_t *value);

/**
 * @brief Requests a guest physical memory write through the active fabric.
 *
 * @param io Active fabric I/O context.
 * @param gpa Guest physical address to write to.
 * @param data Bytes to write.
 * @param len Number of bytes to write.
 * @return bool True on success, false on fabric or protocol failure.
 */
bool fabric_dma_write(struct fabric_io *io, uint64_t gpa, const void *data,
                      uint32_t len);

/**
 * @brief Pulses the configured interrupt line high for the active device.
 *
 * @param io Active fabric I/O context.
 * @return bool True on success, false on fabric failure.
 */
bool fabric_raise_irq(struct fabric_io *io);

/**
 * @brief Deasserts the configured interrupt line for the active device.
 *
 * @param io Active fabric I/O context.
 * @return bool True on success, false on fabric failure.
 */
bool fabric_lower_irq(struct fabric_io *io);

#endif
