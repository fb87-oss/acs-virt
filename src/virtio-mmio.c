#include "virtio-mmio.h"

#include <linux/virtio_mmio.h>

#include <string.h>

/**
 * @brief Masks a register value to the width requested by the MMIO access.
 *
 * @param value Full register value.
 * @param len Access width in bytes.
 * @return uint64_t Masked value.
 */
static uint64_t mask_read(uint64_t value, uint32_t len) {
    switch (len) {
    case 1:
        return value & 0xff;
    case 2:
        return value & 0xffff;
    case 4:
        return value & 0xffffffff;
    default:
        return value;
    }
}

/**
 * @brief Resets transport selectors and the underlying virtio device.
 *
 * @param mmio Transport state to reset.
 */
void virtio_mmio_reset(struct virtio_mmio *mmio) {
    mmio->device_features_sel = 0;
    mmio->driver_features_sel = 0;
    mmio->queue_sel = 0;
    virtio_device_reset(mmio->vdev);
}

/**
 * @brief Initializes a virtio-mmio transport wrapper over a virtio device.
 *
 * @param mmio Transport state to initialize.
 * @param vdev Underlying virtio device state.
 */
void virtio_mmio_init(struct virtio_mmio *mmio, struct virtio_device *vdev) {
    memset(mmio, 0, sizeof(*mmio));
    mmio->vdev = vdev;
    virtio_mmio_reset(mmio);
}

/**
 * @brief Returns a mutable queue pointer by index.
 *
 * @param mmio Transport state containing the virtio device.
 * @param queue_index Queue index to look up.
 * @return struct virtio_queue* Queue pointer, or NULL when invalid.
 */
struct virtio_queue *virtio_mmio_queue(struct virtio_mmio *mmio,
                                       uint32_t queue_index) {
    if (queue_index >= mmio->vdev->queue_count) {
        return NULL;
    }
    return &mmio->vdev->queues[queue_index];
}

/**
 * @brief Returns a const queue pointer by index.
 *
 * @param mmio Transport state containing the virtio device.
 * @param queue_index Queue index to look up.
 * @return const struct virtio_queue* Queue pointer, or NULL when invalid.
 */
const struct virtio_queue *
virtio_mmio_queue_const(const struct virtio_mmio *mmio, uint32_t queue_index) {
    if (queue_index >= mmio->vdev->queue_count) {
        return NULL;
    }
    return &mmio->vdev->queues[queue_index];
}

/**
 * @brief Reads a virtio-mmio register or device-specific config-space value.
 *
 * @param mmio Transport state to read from.
 * @param offset Register offset within the virtio-mmio aperture.
 * @param len Access width in bytes.
 * @return uint64_t Register value masked to the requested access width.
 */
uint64_t virtio_mmio_read(const struct virtio_mmio *mmio, uint64_t offset,
                          uint32_t len) {
    struct virtio_device *vdev = mmio->vdev;
    const struct virtio_queue *queue =
        virtio_mmio_queue_const(mmio, mmio->queue_sel);
    uint64_t value;

    switch (offset) {
    case VIRTIO_MMIO_MAGIC_VALUE:
        value = VIRTIO_MMIO_MAGIC_NUMBER;
        break;
    case VIRTIO_MMIO_VERSION:
        value = VIRTIO_MMIO_VERSION_2;
        break;
    case VIRTIO_MMIO_DEVICE_ID:
        value = vdev->device_id;
        break;
    case VIRTIO_MMIO_VENDOR_ID:
        value = vdev->vendor_id;
        break;
    case VIRTIO_MMIO_DEVICE_FEATURES:
        value = vdev->ops && vdev->ops->get_features
                    ? vdev->ops->get_features(vdev->opaque)
                    : 0;
        value = mmio->device_features_sel == 0 ? (uint32_t)value
                                               : (uint32_t)(value >> 32);
        break;
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
        value = mmio->device_features_sel;
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES:
        value = mmio->driver_features_sel == 0
                    ? (uint32_t)vdev->driver_features
                    : (uint32_t)(vdev->driver_features >> 32);
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
        value = mmio->driver_features_sel;
        break;
    case VIRTIO_MMIO_QUEUE_SEL:
        value = mmio->queue_sel;
        break;
    case VIRTIO_MMIO_QUEUE_NUM_MAX:
        value = mmio->queue_sel < vdev->queue_count ? vdev->queue_size : 0;
        break;
    case VIRTIO_MMIO_QUEUE_NUM:
        value = queue ? queue->num : 0;
        break;
    case VIRTIO_MMIO_QUEUE_READY:
        value = queue ? queue->ready : 0;
        break;
    case VIRTIO_MMIO_INTERRUPT_STATUS:
        value = vdev->interrupt_status;
        break;
    case VIRTIO_MMIO_STATUS:
        value = vdev->status;
        break;
    case VIRTIO_MMIO_CONFIG_GENERATION:
        value = 0;
        break;
    default:
        value =
            offset >= VIRTIO_MMIO_CONFIG && vdev->ops && vdev->ops->get_config
                ? vdev->ops->get_config(vdev->opaque,
                                        offset - VIRTIO_MMIO_CONFIG, len)
                : 0;
        break;
    }

    return mask_read(value, len);
}

/**
 * @brief Writes a virtio-mmio register value into transport or device state.
 *
 * @param mmio Transport state to update.
 * @param offset Register offset within the virtio-mmio aperture.
 * @param raw_value Value provided by the guest write.
 */
void virtio_mmio_write(struct virtio_mmio *mmio, uint64_t offset,
                       uint64_t raw_value) {
    struct virtio_device *vdev = mmio->vdev;
    struct virtio_queue *queue = virtio_mmio_queue(mmio, mmio->queue_sel);
    uint32_t value = (uint32_t)raw_value;

    switch (offset) {
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
        mmio->device_features_sel = value;
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES:
        if (mmio->driver_features_sel == 0) {
            vdev->driver_features =
                (vdev->driver_features & ~0xffffffffull) | value;
        } else if (mmio->driver_features_sel == 1) {
            vdev->driver_features = (vdev->driver_features & 0xffffffffull) |
                                    ((uint64_t)value << 32);
        }
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
        mmio->driver_features_sel = value;
        break;
    case VIRTIO_MMIO_QUEUE_SEL:
        mmio->queue_sel = value;
        break;
    case VIRTIO_MMIO_QUEUE_NUM:
        if (queue) {
            queue->num = value;
        }
        break;
    case VIRTIO_MMIO_QUEUE_READY:
        if (queue) {
            queue->ready = value;
        }
        break;
    case VIRTIO_MMIO_QUEUE_DESC_LOW:
        if (queue) {
            queue->desc = (queue->desc & ~0xffffffffull) | value;
        }
        break;
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
        if (queue) {
            queue->desc =
                (queue->desc & 0xffffffffull) | ((uint64_t)value << 32);
        }
        break;
    case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
        if (queue) {
            queue->avail = (queue->avail & ~0xffffffffull) | value;
        }
        break;
    case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
        if (queue) {
            queue->avail =
                (queue->avail & 0xffffffffull) | ((uint64_t)value << 32);
        }
        break;
    case VIRTIO_MMIO_QUEUE_USED_LOW:
        if (queue) {
            queue->used = (queue->used & ~0xffffffffull) | value;
        }
        break;
    case VIRTIO_MMIO_QUEUE_USED_HIGH:
        if (queue) {
            queue->used =
                (queue->used & 0xffffffffull) | ((uint64_t)value << 32);
        }
        break;
    case VIRTIO_MMIO_INTERRUPT_ACK:
        vdev->interrupt_status &= ~value;
        break;
    case VIRTIO_MMIO_STATUS:
        if (value == 0) {
            virtio_mmio_reset(mmio);
        } else {
            vdev->status = value;
        }
        break;
    default:
        break;
    }
}

/**
 * @brief Marks the virtqueue interrupt bit as pending.
 *
 * @param mmio Transport state containing the virtio device.
 */
void virtio_mmio_used_buffer(struct virtio_mmio *mmio) {
    mmio->vdev->interrupt_status |= VIRTIO_MMIO_INT_VRING;
}
