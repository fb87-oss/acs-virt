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

#define FASTOML_IMPLEMENTATION
#include "fastoml.h"

static bool read_file(const char *path, char **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long len;
    char *buf;

    if (!f) {
        perror("cond: fopen config");
        return false;
    }
    if (fseek(f, 0, SEEK_END) < 0) {
        fclose(f);
        return false;
    }
    len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) < 0) {
        fclose(f);
        return false;
    }

    buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return false;
    }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return false;
    }
    fclose(f);
    buf[len] = '\0';
    *out = buf;
    *out_len = (size_t)len;
    return true;
}

static bool copy_toml_string(const fastoml_node *table, const char *key, char *out, size_t out_len)
{
    const fastoml_node *node = fastoml_table_get_cstr(table, key);
    fastoml_slice value;

    if (!node || fastoml_node_as_slice(node, &value) != FASTOML_OK || (size_t)value.len >= out_len) {
        return false;
    }
    memcpy(out, value.ptr, value.len);
    out[value.len] = '\0';
    return true;
}

static bool parse_config(const char *path, struct cond_config *cfg)
{
    char *text = NULL;
    size_t text_len = 0;
    fastoml_options options;
    fastoml_parser *parser;
    const fastoml_document *doc = NULL;
    fastoml_error err;
    fastoml_status status;
    const fastoml_node *root;
    const fastoml_node *console;
    const fastoml_node *transport;
    const fastoml_node *qemu_mmio;
    bool ok = false;

    cfg->socket[0] = '\0';
    strcpy(cfg->ram_access, "shared-mem");
    strcpy(cfg->output, "-");

    if (!read_file(path, &text, &text_len)) {
        return false;
    }

    fastoml_options_default(&options);
    parser = fastoml_parser_create(&options);
    if (!parser) {
        free(text);
        return false;
    }

    status = fastoml_parse(parser, text, text_len, &doc, &err);
    if (status != FASTOML_OK) {
        fprintf(stderr, "cond: TOML parse error %s at line %u column %u\n",
                fastoml_status_string(status), err.line, err.column);
        goto out;
    }

    root = fastoml_doc_root(doc);
    console = fastoml_table_get_cstr(root, "console");
    transport = fastoml_table_get_cstr(root, "transport");
    qemu_mmio = transport ? fastoml_table_get_cstr(transport, "qemu_mmio") : NULL;
    if (!qemu_mmio || !copy_toml_string(qemu_mmio, "socket", cfg->socket, sizeof(cfg->socket))) {
        goto out;
    }
    if (fastoml_table_get_cstr(qemu_mmio, "ram_access") &&
        !copy_toml_string(qemu_mmio, "ram_access", cfg->ram_access, sizeof(cfg->ram_access))) {
        goto out;
    }
    if (console && fastoml_table_get_cstr(console, "output") &&
        !copy_toml_string(console, "output", cfg->output, sizeof(cfg->output))) {
        goto out;
    }

    ok = true;

out:
    fastoml_parser_destroy(parser);
    free(text);
    return ok;
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
        fprintf(stderr, "usage: cond <backend.toml>\n");
        return 2;
    }
    if (!parse_config(argv[1], &cfg)) {
        fprintf(stderr, "cond: failed to parse config %s\n", argv[1]);
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
