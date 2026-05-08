#define _GNU_SOURCE

#include "cond.h"

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

static bool copy_value(const char *value, char *out, size_t out_len)
{
    size_t len = strlen(value);

    if (len >= out_len) {
        return false;
    }
    memcpy(out, value, len + 1);
    return true;
}

static bool parse_config_arg(const char *arg, struct cond_config *cfg)
{
    char *copy;
    char *save = NULL;

    cfg->socket[0] = '\0';
    strcpy(cfg->ram_access, "shared-mem");
    strcpy(cfg->output, "-");

    copy = strdup(arg);
    if (!copy) {
        return false;
    }

    for (char *part = strtok_r(copy, ",", &save); part; part = strtok_r(NULL, ",", &save)) {
        char *value = strchr(part, '=');
        if (!value) {
            free(copy);
            return false;
        }
        *value++ = '\0';
        if (!strcmp(part, "socket")) {
            if (!copy_value(value, cfg->socket, sizeof(cfg->socket))) {
                free(copy);
                return false;
            }
        } else if (!strcmp(part, "output")) {
            if (!copy_value(value, cfg->output, sizeof(cfg->output))) {
                free(copy);
                return false;
            }
        } else if (!strcmp(part, "ram_access")) {
            if (!copy_value(value, cfg->ram_access, sizeof(cfg->ram_access))) {
                free(copy);
                return false;
            }
        } else if (strcmp(part, "name")) {
            free(copy);
            return false;
        }
    }

    free(copy);
    return cfg->socket[0];
}

static bool ensure_parent_dir(const char *path)
{
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
    for (char *part = strtok_r(tmp, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
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

int main(int argc, char **argv)
{
    struct cond_config cfg;
    struct cond_console_backend backend;
    int listen_fd;
    struct sockaddr_un addr;

    if (argc != 2) {
        fprintf(stderr, "usage: cond name=<name>,socket=<path>[,output=<path>][,ram_access=<mode>]\n");
        return 2;
    }
    if (!parse_config_arg(argv[1], &cfg)) {
        fprintf(stderr, "cond: failed to parse config args\n");
        return 1;
    }
    if (!cond_console_open(&backend, cfg.output)) {
        return 1;
    }
    if (!ensure_parent_dir(cfg.socket)) {
        fprintf(stderr, "cond: failed to create socket parent directory\n");
        cond_console_close(&backend);
        return 1;
    }

    unlink(cfg.socket);
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("cond: socket");
        cond_console_close(&backend);
        return 1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(cfg.socket) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "cond: socket path too long: %s\n", cfg.socket);
        close(listen_fd);
        cond_console_close(&backend);
        return 1;
    }
    strcpy(addr.sun_path, cfg.socket);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(listen_fd, 8) < 0) {
        perror("cond: bind/listen");
        close(listen_fd);
        cond_console_close(&backend);
        return 1;
    }

    fprintf(stderr, "cond: serving console on %s (%s), output=%s\n", cfg.socket, cfg.ram_access, cfg.output);
    for (;;) {
        struct cond_virtio_device dev;
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("cond: accept");
            break;
        }
        fprintf(stderr, "cond: accepted QEMU connection\n");
        cond_virtio_init(&dev, &backend);
        if (!cond_axi_serve(client_fd, &dev)) {
            fprintf(stderr, "cond: connection closed\n");
        }
        close(client_fd);
    }

    close(listen_fd);
    cond_console_close(&backend);
    return 1;
}
