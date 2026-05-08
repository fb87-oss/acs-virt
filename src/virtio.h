#ifndef VIRTIO_H
#define VIRTIO_H

#include <stdbool.h>
#include <stdint.h>

#include <linux/virtio_ring.h>

#define VIRTIO_VENDOR_ID_LOCAL 0x43484950u
#define VIRTIO_INTERRUPT_VRING 1u

/** @brief Split virtqueue runtime state tracked by backend drivers. */
struct virtio_queue {
    uint32_t num; ///< Negotiated queue size currently programmed by the guest.
    uint32_t ready; ///< Guest-provided queue ready flag.
    uint64_t desc;  ///< Guest physical address of the descriptor table.
    uint64_t avail; ///< Guest physical address of the available ring.
    uint64_t used;  ///< Guest physical address of the used ring.
    uint16_t last_avail_idx; ///< Backend cursor into the available ring.
};

/** @brief Decoded virtqueue split-ring descriptor. */
struct virtio_desc {
    uint64_t addr;  ///< Guest physical address of the descriptor payload.
    uint32_t len;   ///< Payload length in bytes.
    uint16_t flags; ///< Descriptor flags from linux/virtio_ring.h.
    uint16_t next;  ///< Next descriptor index when VRING_DESC_F_NEXT is set.
};

/** @brief Fabric I/O context supplied by the active backend transport. */
struct virt_axi_io;

/** @brief Return device feature bits for a virtio device instance. */
typedef uint64_t (*virtio_get_features_fn)(void *opaque);

/** @brief Return a value from device-specific virtio config space. */
typedef uint64_t (*virtio_get_config_fn)(void *opaque, uint64_t offset,
                                         uint32_t len);

/** @brief Handle a guest queue notification. */
typedef bool (*virtio_notify_queue_fn)(void *opaque, struct virt_axi_io *io,
                                       uint32_t queue);

/** @brief Device-specific hooks used by generic virtio and transport code. */
struct virtio_device_ops {
    virtio_get_features_fn get_features; ///< Optional feature callback.
    virtio_get_config_fn get_config; ///< Optional config-space read callback.
    virtio_notify_queue_fn
        notify_queue;            ///< Optional queue notification callback.
    void (*reset)(void *opaque); ///< Optional device-specific reset callback.
};

/** @brief Transport-independent virtio device state. */
struct virtio_device {
    uint32_t device_id;   ///< Linux virtio device id.
    uint32_t vendor_id;   ///< Vendor id exposed through transport registers.
    uint32_t queue_count; ///< Number of queues in the queues array.
    uint32_t queue_size;  ///< Default maximum queue size.
    struct virtio_queue *queues; ///< Driver-owned queue state array.
    uint64_t
        driver_features; ///< 64-bit feature set accepted by the guest driver.
    uint32_t status;     ///< Virtio device status register value.
    uint32_t interrupt_status; ///< Pending virtio interrupt status bits.
    uint32_t
        config_generation; ///< Config generation value exposed to the guest.
    void *opaque;          ///< Device-specific state passed to callbacks.
    const struct virtio_device_ops *ops; ///< Device-specific operation table.
};

/** @brief Fabric-provided DMA read callback type. */
typedef bool (*virtio_dma_read_fn)(struct virt_axi_io *io, uint64_t gpa,
                                   uint32_t len, uint8_t **data);

/** @brief Fabric-provided 16-bit DMA read callback type. */
typedef bool (*virtio_dma_read_u16_fn)(struct virt_axi_io *io, uint64_t gpa,
                                       uint16_t *value);

/** @brief Fabric-provided DMA write callback type. */
typedef bool (*virtio_dma_write_fn)(struct virt_axi_io *io, uint64_t gpa,
                                    const void *data, uint32_t len);

/**
 * @brief Loads a little-endian 16-bit value.
 *
 * @param p Pointer to the first byte of the encoded value.
 * @return uint16_t Decoded host-endian value.
 */
static inline uint16_t virtio_load_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/**
 * @brief Loads a little-endian 32-bit value.
 *
 * @param p Pointer to the first byte of the encoded value.
 * @return uint32_t Decoded host-endian value.
 */
static inline uint32_t virtio_load_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/**
 * @brief Loads a little-endian 64-bit value.
 *
 * @param p Pointer to the first byte of the encoded value.
 * @return uint64_t Decoded host-endian value.
 */
static inline uint64_t virtio_load_le64(const uint8_t *p) {
    return (uint64_t)virtio_load_le32(p) |
           ((uint64_t)virtio_load_le32(p + 4) << 32);
}

/**
 * @brief Stores a 16-bit value in little-endian byte order.
 *
 * @param p Pointer to the output buffer.
 * @param v Host-endian value to encode.
 */
static inline void virtio_store_le16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}

/**
 * @brief Stores a 32-bit value in little-endian byte order.
 *
 * @param p Pointer to the output buffer.
 * @param v Host-endian value to encode.
 */
static inline void virtio_store_le32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

/**
 * @brief Stores a 64-bit value in little-endian byte order.
 *
 * @param p Pointer to the output buffer.
 * @param v Host-endian value to encode.
 */
static inline void virtio_store_le64(uint8_t *p, uint64_t v) {
    virtio_store_le32(p, (uint32_t)v);
    virtio_store_le32(p + 4, (uint32_t)(v >> 32));
}

/**
 * @brief Initializes transport-independent virtio device state.
 *
 * @param dev Device state to initialize.
 * @param device_id Linux virtio device id to expose.
 * @param vendor_id Vendor id to expose through the transport.
 * @param queue_count Number of queues in the queues array.
 * @param queue_size Default maximum size for each queue.
 * @param queues Driver-owned queue storage.
 * @param opaque Device-specific callback state.
 * @param ops Device-specific operation table.
 */
void virtio_device_init(struct virtio_device *dev, uint32_t device_id,
                        uint32_t vendor_id, uint32_t queue_count,
                        uint32_t queue_size, struct virtio_queue *queues,
                        void *opaque, const struct virtio_device_ops *ops);

/**
 * @brief Resets transport-independent virtio device state and all queues.
 *
 * @param dev Device state to reset.
 */
void virtio_device_reset(struct virtio_device *dev);

/**
 * @brief Reads and decodes one split-ring descriptor from guest memory.
 *
 * @param queue Queue containing the descriptor table address.
 * @param io Active fabric I/O context.
 * @param dma_read DMA read callback used to fetch descriptor bytes.
 * @param index Descriptor index to read.
 * @param desc Output descriptor structure.
 * @return bool True on success, false on DMA failure.
 */
bool virtio_read_desc(const struct virtio_queue *queue, struct virt_axi_io *io,
                      virtio_dma_read_fn dma_read, uint16_t index,
                      struct virtio_desc *desc);

/**
 * @brief Appends one completed descriptor head to a queue's used ring.
 *
 * @param queue Queue containing the used ring address.
 * @param io Active fabric I/O context.
 * @param dma_read_u16 DMA callback used to read the current used index.
 * @param dma_write DMA callback used to write the used element and index.
 * @param head Descriptor chain head index being completed.
 * @param len Number of bytes written by the device for this chain.
 * @return bool True on success, false on DMA failure.
 */
bool virtio_add_used(const struct virtio_queue *queue, struct virt_axi_io *io,
                     virtio_dma_read_u16_fn dma_read_u16,
                     virtio_dma_write_fn dma_write, uint16_t head,
                     uint32_t len);

/**
 * @brief Returns the next available descriptor head without advancing the queue
 * cursor.
 *
 * @param queue Queue containing the available ring address and backend cursor.
 * @param io Active fabric I/O context.
 * @param dma_read_u16 DMA callback used to read available ring fields.
 * @param head Output descriptor chain head index when work is available.
 * @param available Output flag indicating whether a descriptor is available.
 * @return bool True on success, false on DMA failure.
 */
bool virtio_next_avail(const struct virtio_queue *queue, struct virt_axi_io *io,
                       virtio_dma_read_u16_fn dma_read_u16, uint16_t *head,
                       bool *available);

#endif
