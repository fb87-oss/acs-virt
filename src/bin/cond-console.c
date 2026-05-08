#define _GNU_SOURCE

#include "cond.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static bool write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *pos = buf;

    while (len) {
        ssize_t ret = write(fd, pos, len);
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

bool cond_console_open(struct cond_console_backend *backend, const char *path)
{
    if (!path || !path[0] || !strcmp(path, "-")) {
        backend->fd = STDOUT_FILENO;
        return true;
    }

    backend->fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0666);
    if (backend->fd < 0) {
        perror("cond: open output");
        return false;
    }
    return true;
}

void cond_console_close(struct cond_console_backend *backend)
{
    if (backend->fd >= 0 && backend->fd != STDOUT_FILENO) {
        close(backend->fd);
    }
    backend->fd = -1;
}

bool cond_console_write(struct cond_console_backend *backend, const void *buf, size_t len)
{
    return write_all(backend->fd, buf, len);
}
