#define _GNU_SOURCE

#include "blkd.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static bool pread_all(int fd, void *buf, size_t len, off_t offset)
{
    uint8_t *pos = buf;

    while (len) {
        ssize_t ret = pread(fd, pos, len, offset);
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
        offset += ret;
    }

    return true;
}

static bool pwrite_all(int fd, const void *buf, size_t len, off_t offset)
{
    const uint8_t *pos = buf;

    while (len) {
        ssize_t ret = pwrite(fd, pos, len, offset);
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
        offset += ret;
    }

    return true;
}

bool blkd_block_open(struct blkd_block_backend *backend, const char *path, bool readonly)
{
    struct stat st;

    backend->fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (backend->fd < 0) {
        perror("blkd: open image");
        return false;
    }

    if (fstat(backend->fd, &st) < 0) {
        perror("blkd: fstat image");
        close(backend->fd);
        backend->fd = -1;
        return false;
    }

    backend->image_len = (uint64_t)st.st_size;
    backend->readonly = readonly;
    return true;
}

void blkd_block_close(struct blkd_block_backend *backend)
{
    if (backend->fd >= 0) {
        close(backend->fd);
        backend->fd = -1;
    }
}

bool blkd_block_read(struct blkd_block_backend *backend, uint64_t offset, void *buf, size_t len)
{
    if (offset > backend->image_len || len > backend->image_len - offset || offset > (uint64_t)INT64_MAX) {
        return false;
    }
    return pread_all(backend->fd, buf, len, (off_t)offset);
}

bool blkd_block_write(struct blkd_block_backend *backend, uint64_t offset, const void *buf, size_t len)
{
    if (backend->readonly || offset > backend->image_len || len > backend->image_len - offset ||
        offset > (uint64_t)INT64_MAX) {
        return false;
    }
    return pwrite_all(backend->fd, buf, len, (off_t)offset);
}

bool blkd_block_flush(struct blkd_block_backend *backend)
{
    return fsync(backend->fd) == 0;
}
