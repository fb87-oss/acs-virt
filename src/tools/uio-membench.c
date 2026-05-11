#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static bool parse_u64(const char *text, uint64_t *out) {
    char *end = NULL;
    unsigned long long value;

    errno = 0;
    value = strtoull(text, &end, 0);
    if (errno || end == text || *end) {
        return false;
    }
    *out = (uint64_t)value;
    return true;
}

static bool read_sysfs_u64(const char *path, uint64_t *out) {
    char buf[64];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0) {
        return false;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return false;
    }
    buf[n] = '\0';
    buf[strcspn(buf, "\n")] = '\0';
    return parse_u64(buf, out);
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double mib_per_s(uint64_t bytes, uint64_t ns) {
    if (!ns) {
        return 0.0;
    }
    return ((double)bytes / (1024.0 * 1024.0)) / ((double)ns / 1000000000.0);
}

static void bench_region(const char *label, const char *device, uint8_t *base,
                         uint64_t offset, uint64_t size, uint64_t repeat) {
    uint8_t *copy_buf = malloc(size);
    uint8_t *saved = malloc(size);
    volatile uint8_t sink = 0;
    uint64_t read_ns = 0;
    uint64_t write_ns = 0;

    if (!copy_buf || !saved) {
        perror("uio-membench: malloc");
        free(copy_buf);
        free(saved);
        return;
    }

    memcpy(saved, base, size);
    for (uint64_t i = 0; i < repeat; i++) {
        uint64_t start = monotonic_ns();
        memcpy(copy_buf, base, size);
        read_ns += monotonic_ns() - start;
        sink ^= copy_buf[(i * 4099) % size];
    }
    for (uint64_t i = 0; i < repeat; i++) {
        uint64_t start = monotonic_ns();
        memset(base, (int)(0xa5u ^ (unsigned)i), size);
        write_ns += monotonic_ns() - start;
        sink ^= base[(i * 4099) % size];
    }
    memcpy(base, saved, size);

    printf("%s device=%s offset=0x%" PRIx64 " bytes=%" PRIu64
           " repeat=%" PRIu64 " read_avg=%.1fMiB/s write_avg=%.1fMiB/s"
           " sink=%u\n",
           label, device, offset, size, repeat, mib_per_s(size * repeat, read_ns),
           mib_per_s(size * repeat, write_ns), sink);

    free(copy_buf);
    free(saved);
}

int main(int argc, char **argv) {
    const char *uio = argc > 1 ? argv[1] : "/dev/uio0";
    uint64_t size = argc > 2 ? strtoull(argv[2], NULL, 0) : 64ull * 1024 * 1024;
    uint64_t repeat = argc > 3 ? strtoull(argv[3], NULL, 0) : 3;
    const char *name = strrchr(uio, '/');
    char sysfs_size[128];
    uint64_t map_size;
    uint64_t offset;
    long page_size;
    int fd;
    uint8_t *map;

    if (!name || !name[1]) {
        fprintf(stderr, "usage: uio-membench /dev/uioN [bytes] [repeat]\n");
        return 2;
    }
    name++;
    snprintf(sysfs_size, sizeof(sysfs_size), "/sys/class/uio/%s/maps/map1/size", name);
    if (!read_sysfs_u64(sysfs_size, &map_size)) {
        perror("uio-membench: read map1 size");
        return 1;
    }
    if (!size || !repeat || size > map_size) {
        fprintf(stderr, "uio-membench: invalid bytes=%" PRIu64 " repeat=%" PRIu64
                        " map_size=%" PRIu64 "\n",
                size, repeat, map_size);
        return 2;
    }

    fd = open(uio, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("uio-membench: open uio");
        return 1;
    }
    page_size = sysconf(_SC_PAGESIZE);
    map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
               (off_t)page_size);
    close(fd);
    if (map == MAP_FAILED) {
        perror("uio-membench: mmap map1");
        return 1;
    }

    offset = map_size - size;
    bench_region("UIO_MEMBENCH", uio, map + offset, offset, size, repeat);

    munmap(map, map_size);
    return 0;
}
