#ifndef VIRT_AXI_H
#define VIRT_AXI_H

#include <stdbool.h>
#include <stdint.h>

#define VIRT_AXI_MSG_MMIO_READ 3
#define VIRT_AXI_MSG_MMIO_READ_REPLY 4
#define VIRT_AXI_MSG_MMIO_WRITE 5
#define VIRT_AXI_MSG_IRQ_ASSERT 6
#define VIRT_AXI_MSG_IRQ_DEASSERT 7
#define VIRT_AXI_MSG_DMA_READ 8
#define VIRT_AXI_MSG_DMA_READ_REPLY 9
#define VIRT_AXI_MSG_DMA_WRITE 10
#define VIRT_AXI_MSG_ERROR 0xffff

#define VIRT_AXI_HEADER_LEN 24
#define VIRT_AXI_MAX_DEVICES 8

/** @brief Per-connection fabric I/O context passed to device callbacks. */
struct virt_axi_io {
    int fd; ///< Connected Unix socket file descriptor for the active QEMU
            ///< session.
};

/** @brief Device callbacks used by the virt-axi socket fabric. */
struct virt_axi_device_ops {
    void (*connect)(
        void *opaque); ///< Optional hook invoked after QEMU connects.
    uint64_t (*read)(
        void *opaque, uint64_t offset,
        uint32_t len); ///< Read a device-relative MMIO register value.
    bool (*write)(
        void *opaque, struct virt_axi_io *io, uint64_t offset, uint64_t value,
        uint32_t len); ///< Write a device-relative MMIO register value.
};

/** @brief Registered MMIO device exposed through a virt-axi socket. */
struct virt_axi_device {
    const char *name;        ///< Short diagnostic name used in backend logs.
    const char *socket_path; ///< Unix socket path that QEMU connects to for
                             ///< this device.
    uint64_t addr; ///< Absolute guest bus base address for the MMIO aperture.
    uint64_t size; ///< Size in bytes of the MMIO aperture.
    void *opaque;  ///< Driver-owned callback state.
    const struct virt_axi_device_ops *ops; ///< Device MMIO operation table.
};

/** @brief virt-axi fabric instance containing registered backend devices. */
struct virt_axi {
    struct virt_axi_device
        devices[VIRT_AXI_MAX_DEVICES]; ///< Fixed-size registration table for
                                       ///< backend devices.
    uint32_t device_count; ///< Number of populated entries in devices.
};

/**
 * @brief Initializes an empty virt-axi fabric instance.
 *
 * @param bus Fabric instance to initialize.
 */
void virt_axi_init(struct virt_axi *bus);

/**
 * @brief Registers one backend device with a virt-axi fabric instance.
 *
 * @param bus Fabric instance that receives the device registration.
 * @param device Device descriptor to copy into the fabric instance.
 * @return bool True on success, false if the registration is invalid or full.
 */
bool virt_axi_register(struct virt_axi *bus,
                       const struct virt_axi_device *device);

/**
 * @brief Runs the socket server for the registered virt-axi device.
 *
 * @param bus Fabric instance containing exactly one registered device.
 * @return bool True if the server exits cleanly, false on setup or protocol
 * failure.
 */
bool virt_axi_run(struct virt_axi *bus);

/**
 * @brief Requests a guest physical memory read through QEMU-mediated DMA.
 *
 * @param io Active fabric I/O context.
 * @param gpa Guest physical address to read from.
 * @param len Number of bytes to read.
 * @param data Output heap buffer containing the read bytes; caller frees it.
 * @return bool True on success, false on socket or protocol failure.
 */
bool virt_axi_dma_read(struct virt_axi_io *io, uint64_t gpa, uint32_t len,
                       uint8_t **data);

/**
 * @brief Requests a 16-bit little-endian guest physical memory read.
 *
 * @param io Active fabric I/O context.
 * @param gpa Guest physical address to read from.
 * @param value Output decoded host-endian value.
 * @return bool True on success, false on socket or protocol failure.
 */
bool virt_axi_dma_read_u16(struct virt_axi_io *io, uint64_t gpa,
                           uint16_t *value);

/**
 * @brief Requests a guest physical memory write through QEMU-mediated DMA.
 *
 * @param io Active fabric I/O context.
 * @param gpa Guest physical address to write to.
 * @param data Bytes to write.
 * @param len Number of bytes to write.
 * @return bool True on success, false on socket or protocol failure.
 */
bool virt_axi_dma_write(struct virt_axi_io *io, uint64_t gpa, const void *data,
                        uint32_t len);

/**
 * @brief Pulses the configured interrupt line high for the active device.
 *
 * @param io Active fabric I/O context.
 * @return bool True on success, false on socket failure.
 */
bool virt_axi_raise_irq(struct virt_axi_io *io);

/**
 * @brief Deasserts the configured interrupt line for the active device.
 *
 * @param io Active fabric I/O context.
 * @return bool True on success, false on socket failure.
 */
bool virt_axi_lower_irq(struct virt_axi_io *io);

#endif
