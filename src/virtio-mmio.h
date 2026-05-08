#ifndef VIRTIO_MMIO_H
#define VIRTIO_MMIO_H

#include <stdbool.h>
#include <stdint.h>

#include "virtio.h"

#define VIRTIO_MMIO_MAGIC_NUMBER 0x74726976u
#define VIRTIO_MMIO_VERSION_2 2u

/** @brief virtio-mmio transport state layered over a transport-independent
 * device. */
struct virtio_mmio {
    struct virtio_device *vdev; ///< Device state and callbacks exposed through
                                ///< the MMIO transport.
    uint32_t
        device_features_sel; ///< Selected 32-bit bank for device feature reads.
    uint32_t driver_features_sel; ///< Selected 32-bit bank for driver feature
                                  ///< writes/reads.
    uint32_t queue_sel; ///< Currently selected virtqueue for queue register
                        ///< accesses.
};

/**
 * @brief Initializes virtio-mmio transport state for a virtio device.
 *
 * @param mmio Transport state to initialize.
 * @param vdev Underlying virtio device state.
 */
void virtio_mmio_init(struct virtio_mmio *mmio, struct virtio_device *vdev);

/**
 * @brief Resets virtio-mmio transport selectors and the underlying virtio
 * device.
 *
 * @param mmio Transport state to reset.
 */
void virtio_mmio_reset(struct virtio_mmio *mmio);

/**
 * @brief Reads a virtio-mmio register or device config-space field.
 *
 * @param mmio Transport state to read from.
 * @param offset Register offset within the virtio-mmio aperture.
 * @param len Access width in bytes.
 * @return uint64_t Register value masked to the requested access width.
 */
uint64_t virtio_mmio_read(const struct virtio_mmio *mmio, uint64_t offset,
                          uint32_t len);

/**
 * @brief Writes a virtio-mmio register value.
 *
 * @param mmio Transport state to update.
 * @param offset Register offset within the virtio-mmio aperture.
 * @param raw_value Value provided by the guest write.
 */
void virtio_mmio_write(struct virtio_mmio *mmio, uint64_t offset,
                       uint64_t raw_value);

/**
 * @brief Returns a mutable queue by index.
 *
 * @param mmio Transport state containing the virtio device.
 * @param queue_index Queue index to look up.
 * @return struct virtio_queue* Queue pointer, or NULL if the index is invalid.
 */
struct virtio_queue *virtio_mmio_queue(struct virtio_mmio *mmio,
                                       uint32_t queue_index);

/**
 * @brief Returns a const queue by index.
 *
 * @param mmio Transport state containing the virtio device.
 * @param queue_index Queue index to look up.
 * @return const struct virtio_queue* Queue pointer, or NULL if the index is
 * invalid.
 */
const struct virtio_queue *
virtio_mmio_queue_const(const struct virtio_mmio *mmio, uint32_t queue_index);

/**
 * @brief Marks the device as having a used-buffer interrupt pending.
 *
 * @param mmio Transport state containing the virtio device.
 */
void virtio_mmio_used_buffer(struct virtio_mmio *mmio);

#endif
