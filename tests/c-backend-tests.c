#define _GNU_SOURCE
#define CTEST_MAIN
#define CTEST_NO_COLORS

#include "ctest.h"

#include "virtio-blkd.h"
#include "virtio-consoled.h"
#include "virtio-mmio.h"

#include <fcntl.h>
#include <linux/virtio_config.h>
#include <linux/virtio_blk.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_mmio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, const char **argv)
{
    return ctest_main(argc, argv);
}

static void fill_file(int fd, size_t len)
{
    uint8_t zero[512] = {0};

    while (len) {
        size_t chunk = len < sizeof(zero) ? len : sizeof(zero);
        ASSERT_EQUAL((int)chunk, (int)write(fd, zero, chunk));
        len -= chunk;
    }
}

static void make_temp_path(char *path, size_t path_len, const char *prefix)
{
    int ret = snprintf(path, path_len, "run/%s-XXXXXX", prefix);
    ASSERT_TRUE(ret > 0 && (size_t)ret < path_len);
}

CTEST(endian_helpers, blkd_round_trips_little_endian_values)
{
    uint8_t buf[8] = {0};

    blkd_store_le16(buf, 0x1234);
    ASSERT_EQUAL_U(0x34, buf[0]);
    ASSERT_EQUAL_U(0x12, buf[1]);
    ASSERT_EQUAL_U(0x1234, blkd_load_le16(buf));

    blkd_store_le32(buf, 0x12345678);
    ASSERT_EQUAL_U(0x78, buf[0]);
    ASSERT_EQUAL_U(0x56, buf[1]);
    ASSERT_EQUAL_U(0x34, buf[2]);
    ASSERT_EQUAL_U(0x12, buf[3]);
    ASSERT_EQUAL_U(0x12345678, blkd_load_le32(buf));

    blkd_store_le64(buf, 0x0123456789abcdefULL);
    ASSERT_EQUAL_U(0x0123456789abcdefULL, blkd_load_le64(buf));
}

CTEST(endian_helpers, cond_round_trips_little_endian_values)
{
    uint8_t buf[8] = {0};

    cond_store_le16(buf, 0xabcd);
    ASSERT_EQUAL_U(0xcd, buf[0]);
    ASSERT_EQUAL_U(0xab, buf[1]);
    ASSERT_EQUAL_U(0xabcd, cond_load_le16(buf));

    cond_store_le32(buf, 0x89abcdef);
    ASSERT_EQUAL_U(0xef, buf[0]);
    ASSERT_EQUAL_U(0xcd, buf[1]);
    ASSERT_EQUAL_U(0xab, buf[2]);
    ASSERT_EQUAL_U(0x89, buf[3]);
    ASSERT_EQUAL_U(0x89abcdef, cond_load_le32(buf));

    cond_store_le64(buf, 0xfedcba9876543210ULL);
    ASSERT_EQUAL_U(0xfedcba9876543210ULL, cond_load_le64(buf));
}

CTEST(blkd_block, reads_writes_and_rejects_out_of_bounds)
{
    char path[128];
    int fd;
    struct blkd_block_backend backend;
    uint8_t write_buf[4] = {0xde, 0xad, 0xbe, 0xef};
    uint8_t read_buf[4] = {0};

    make_temp_path(path, sizeof(path), "virtio-blkd");
    fd = mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    fill_file(fd, 1024);
    close(fd);

    ASSERT_TRUE(blkd_block_open(&backend, path, false));
    ASSERT_EQUAL_U(1024, backend.image_len);
    ASSERT_FALSE(backend.readonly);
    ASSERT_TRUE(blkd_block_write(&backend, 512, write_buf, sizeof(write_buf)));
    ASSERT_TRUE(blkd_block_read(&backend, 512, read_buf, sizeof(read_buf)));
    ASSERT_DATA(write_buf, sizeof(write_buf), read_buf, sizeof(read_buf));
    ASSERT_FALSE(blkd_block_read(&backend, 1023, read_buf, sizeof(read_buf)));
    ASSERT_FALSE(blkd_block_write(&backend, 1024, write_buf, 1));
    blkd_block_close(&backend);

    ASSERT_TRUE(blkd_block_open(&backend, path, true));
    ASSERT_TRUE(backend.readonly);
    ASSERT_FALSE(blkd_block_write(&backend, 0, write_buf, sizeof(write_buf)));
    blkd_block_close(&backend);
    unlink(path);
}

CTEST(cond_console, writes_to_output_file)
{
    char path[128];
    struct cond_console_backend backend;
    int fd;
    char buf[16] = {0};
    const char msg[] = "hello console";

    make_temp_path(path, sizeof(path), "virtio-consoled");
    fd = mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    close(fd);

    ASSERT_TRUE(cond_console_open(&backend, path));
    ASSERT_TRUE(cond_console_write(&backend, msg, strlen(msg)));
    cond_console_close(&backend);

    fd = open(path, O_RDONLY);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQUAL((int)strlen(msg), (int)read(fd, buf, strlen(msg)));
    close(fd);
    ASSERT_STR(msg, buf);
    unlink(path);
}

CTEST(blkd_virtio, status_zero_resets_runtime_state)
{
    struct blkd_block_backend backend = {.fd = -1, .image_len = 64 * 1024 * 1024, .readonly = false};
    struct blkd_virtio_device dev;
    struct virtio_mmio mmio;

    blkd_virtio_init(&dev, &backend);
    virtio_mmio_init(&mmio, &dev.vdev);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_DRIVER_FEATURES, 1);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_QUEUE_SEL, 0);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_QUEUE_NUM, 128);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_QUEUE_READY, 1);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_QUEUE_DESC_LOW, 0x1000);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_QUEUE_AVAIL_LOW, 0x2000);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_QUEUE_USED_LOW, 0x3000);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_STATUS,
                           VIRTIO_CONFIG_S_ACKNOWLEDGE |
                           VIRTIO_CONFIG_S_DRIVER |
                           VIRTIO_CONFIG_S_DRIVER_OK |
                           VIRTIO_CONFIG_S_FEATURES_OK);

    dev.vdev.interrupt_status = 1;
    dev.queue.last_avail_idx = 7;
    virtio_mmio_write(&mmio, VIRTIO_MMIO_STATUS, 0);

    ASSERT_EQUAL_U(0, mmio.device_features_sel);
    ASSERT_EQUAL_U(0, mmio.driver_features_sel);
    ASSERT_EQUAL_U(0, dev.vdev.driver_features);
    ASSERT_EQUAL_U(BLKD_QUEUE_SIZE, dev.queue.num);
    ASSERT_EQUAL_U(0, dev.queue.ready);
    ASSERT_EQUAL_U(0, dev.queue.desc);
    ASSERT_EQUAL_U(0, dev.queue.avail);
    ASSERT_EQUAL_U(0, dev.queue.used);
    ASSERT_EQUAL_U(0, dev.queue.last_avail_idx);
    ASSERT_EQUAL_U(0, dev.vdev.status);
    ASSERT_EQUAL_U(0, dev.vdev.interrupt_status);
}

CTEST(blkd_virtio, exposes_linux_virtio_mmio_probe_registers)
{
    struct blkd_block_backend backend = {.fd = -1, .image_len = 64 * 1024 * 1024, .readonly = false};
    struct blkd_virtio_device dev;
    struct virtio_mmio mmio;

    blkd_virtio_init(&dev, &backend);
    virtio_mmio_init(&mmio, &dev.vdev);

    ASSERT_EQUAL_U(VIRTIO_MMIO_MAGIC_NUMBER, virtio_mmio_read(&mmio, VIRTIO_MMIO_MAGIC_VALUE, 4));
    ASSERT_EQUAL_U(VIRTIO_MMIO_VERSION_2, virtio_mmio_read(&mmio, VIRTIO_MMIO_VERSION, 4));
    ASSERT_EQUAL_U(VIRTIO_ID_BLOCK, virtio_mmio_read(&mmio, VIRTIO_MMIO_DEVICE_ID, 4));
    ASSERT_EQUAL_U(VIRTIO_VENDOR_ID_LOCAL, virtio_mmio_read(&mmio, VIRTIO_MMIO_VENDOR_ID, 4));

    virtio_mmio_write(&mmio, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    ASSERT_EQUAL_U((1u << VIRTIO_BLK_F_BLK_SIZE) | (1u << VIRTIO_BLK_F_FLUSH),
                   virtio_mmio_read(&mmio, VIRTIO_MMIO_DEVICE_FEATURES, 4));
    virtio_mmio_write(&mmio, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    ASSERT_EQUAL_U(1u << (VIRTIO_F_VERSION_1 - 32),
                   virtio_mmio_read(&mmio, VIRTIO_MMIO_DEVICE_FEATURES, 4));

    ASSERT_EQUAL_U((uint32_t)(backend.image_len / BLKD_SECTOR_SIZE),
                   virtio_mmio_read(&mmio, VIRTIO_MMIO_CONFIG + offsetof(struct virtio_blk_config, capacity), 4));
    ASSERT_EQUAL_U(BLKD_SECTOR_SIZE,
                   virtio_mmio_read(&mmio, VIRTIO_MMIO_CONFIG + offsetof(struct virtio_blk_config, blk_size), 4));
}

CTEST(blkd_virtio, stores_linux_virtio_mmio_queue_setup)
{
    struct blkd_block_backend backend = {.fd = -1, .image_len = 64 * 1024 * 1024, .readonly = false};
    struct blkd_virtio_device dev;
    struct virtio_mmio mmio;

    blkd_virtio_init(&dev, &backend);
    virtio_mmio_init(&mmio, &dev.vdev);
    ASSERT_EQUAL_U(BLKD_QUEUE_SIZE, virtio_mmio_read(&mmio, VIRTIO_MMIO_QUEUE_NUM_MAX, 4));

    virtio_mmio_write(&mmio, VIRTIO_MMIO_QUEUE_NUM, 128);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_QUEUE_READY, 1);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_QUEUE_DESC_LOW, 0x89abcdef);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_QUEUE_DESC_HIGH, 0x01234567);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_QUEUE_AVAIL_LOW, 0x76543210);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_QUEUE_AVAIL_HIGH, 0xfedcba98);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_QUEUE_USED_LOW, 0x13572468);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_QUEUE_USED_HIGH, 0x24681357);

    ASSERT_EQUAL_U(128, dev.queue.num);
    ASSERT_EQUAL_U(1, dev.queue.ready);
    ASSERT_EQUAL_U(0x0123456789abcdefULL, dev.queue.desc);
    ASSERT_EQUAL_U(0xfedcba9876543210ULL, dev.queue.avail);
    ASSERT_EQUAL_U(0x2468135713572468ULL, dev.queue.used);
}

CTEST(blkd_virtio, acknowledges_vring_interrupt_with_uapi_bit)
{
    struct blkd_block_backend backend = {.fd = -1, .image_len = 64 * 1024 * 1024, .readonly = false};
    struct blkd_virtio_device dev;
    struct virtio_mmio mmio;

    blkd_virtio_init(&dev, &backend);
    virtio_mmio_init(&mmio, &dev.vdev);
    virtio_mmio_used_buffer(&mmio);

    ASSERT_EQUAL_U(VIRTIO_MMIO_INT_VRING, virtio_mmio_read(&mmio, VIRTIO_MMIO_INTERRUPT_STATUS, 4));
    virtio_mmio_write(&mmio, VIRTIO_MMIO_INTERRUPT_ACK, VIRTIO_MMIO_INT_VRING);
    ASSERT_EQUAL_U(0, virtio_mmio_read(&mmio, VIRTIO_MMIO_INTERRUPT_STATUS, 4));
}

CTEST(cond_virtio, status_zero_resets_all_queue_state)
{
    struct cond_console_backend backend = {.fd = -1};
    struct cond_virtio_device dev;
    struct virtio_mmio mmio;

    cond_virtio_init(&dev, &backend);
    virtio_mmio_init(&mmio, &dev.vdev);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_QUEUE_SEL, 1);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_QUEUE_NUM, 128);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_QUEUE_READY, 1);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_QUEUE_DESC_LOW, 0x1000);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_QUEUE_AVAIL_LOW, 0x2000);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_QUEUE_USED_LOW, 0x3000);
    virtio_mmio_write(&mmio, VIRTIO_MMIO_STATUS,
                           VIRTIO_CONFIG_S_ACKNOWLEDGE |
                           VIRTIO_CONFIG_S_DRIVER |
                           VIRTIO_CONFIG_S_DRIVER_OK |
                           VIRTIO_CONFIG_S_FEATURES_OK);

    dev.vdev.interrupt_status = 1;
    dev.queues[0].ready = 1;
    dev.queues[0].last_avail_idx = 5;
    dev.queues[1].last_avail_idx = 7;
    virtio_mmio_write(&mmio, VIRTIO_MMIO_STATUS, 0);

    ASSERT_EQUAL_U(0, mmio.queue_sel);
    ASSERT_EQUAL_U(0, dev.vdev.status);
    ASSERT_EQUAL_U(0, dev.vdev.interrupt_status);
    for (uint32_t i = 0; i < COND_QUEUE_COUNT; i++) {
        ASSERT_EQUAL_U(COND_QUEUE_SIZE, dev.queues[i].num);
        ASSERT_EQUAL_U(0, dev.queues[i].ready);
        ASSERT_EQUAL_U(0, dev.queues[i].desc);
        ASSERT_EQUAL_U(0, dev.queues[i].avail);
        ASSERT_EQUAL_U(0, dev.queues[i].used);
        ASSERT_EQUAL_U(0, dev.queues[i].last_avail_idx);
    }
}
