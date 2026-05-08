#define _GNU_SOURCE

#include "blkd.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
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
        perror("blkd: fopen config");
        return false;
    }
    if (fseek(f, 0, SEEK_END) < 0) {
        fclose(f);
        return false;
    }
    len = ftell(f);
    if (len < 0) {
        fclose(f);
        return false;
    }
    if (fseek(f, 0, SEEK_SET) < 0) {
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

    if (!node || fastoml_node_as_slice(node, &value) != FASTOML_OK) {
        return false;
    }
    if ((size_t)value.len >= out_len) {
        return false;
    }

    memcpy(out, value.ptr, value.len);
    out[value.len] = '\0';
    return true;
}

static bool parse_config(const char *path, struct blkd_config *cfg)
{
    char *text = NULL;
    size_t text_len = 0;
    fastoml_options options;
    fastoml_parser *parser;
    const fastoml_document *doc = NULL;
    fastoml_error err;
    fastoml_status status;
    const fastoml_node *root;
    const fastoml_node *block;
    const fastoml_node *transport;
    const fastoml_node *qemu_mmio;
    const fastoml_node *readonly;
    bool ok = false;

    cfg->image[0] = '\0';
    cfg->socket[0] = '\0';
    strcpy(cfg->ram_access, "shared-mem");
    cfg->readonly = false;

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
        fprintf(stderr, "blkd: TOML parse error %s at line %u column %u\n",
                fastoml_status_string(status), err.line, err.column);
        goto out;
    }

    root = fastoml_doc_root(doc);
    block = fastoml_table_get_cstr(root, "block");
    transport = fastoml_table_get_cstr(root, "transport");
    qemu_mmio = transport ? fastoml_table_get_cstr(transport, "qemu_mmio") : NULL;
    if (!block || !qemu_mmio) {
        goto out;
    }

    if (!copy_toml_string(block, "image", cfg->image, sizeof(cfg->image)) ||
        !copy_toml_string(qemu_mmio, "socket", cfg->socket, sizeof(cfg->socket))) {
        goto out;
    }

    readonly = fastoml_table_get_cstr(block, "readonly");
    if (readonly) {
        int value = 0;
        if (fastoml_node_as_bool(readonly, &value) != FASTOML_OK) {
            goto out;
        }
        cfg->readonly = value != 0;
    }

    if (fastoml_table_get_cstr(qemu_mmio, "ram_access") &&
        !copy_toml_string(qemu_mmio, "ram_access", cfg->ram_access, sizeof(cfg->ram_access))) {
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
    struct blkd_config cfg;
    struct blkd_block_backend backend;
    int listen_fd;
    struct sockaddr_un addr;

    if (argc != 2) {
        fprintf(stderr, "usage: blkd <backend.toml>\n");
        return 2;
    }

    if (!parse_config(argv[1], &cfg)) {
        fprintf(stderr, "blkd: failed to parse config %s\n", argv[1]);
        return 1;
    }

    if (!blkd_block_open(&backend, cfg.image, cfg.readonly)) {
        return 1;
    }

    if (!ensure_parent_dir(cfg.socket)) {
        fprintf(stderr, "blkd: failed to create socket parent directory\n");
        blkd_block_close(&backend);
        return 1;
    }

    unlink(cfg.socket);
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("blkd: socket");
        blkd_block_close(&backend);
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(cfg.socket) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "blkd: socket path too long: %s\n", cfg.socket);
        close(listen_fd);
        blkd_block_close(&backend);
        return 1;
    }
    strcpy(addr.sun_path, cfg.socket);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(listen_fd, 8) < 0) {
        perror("blkd: bind/listen");
        close(listen_fd);
        blkd_block_close(&backend);
        return 1;
    }

    fprintf(stderr, "blkd: serving %" PRIu64 " sectors on %s (%s)\n",
            backend.image_len / BLKD_SECTOR_SIZE, cfg.socket, cfg.ram_access);

    for (;;) {
        struct blkd_virtio_device dev;
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("blkd: accept");
            break;
        }

        fprintf(stderr, "blkd: accepted QEMU connection\n");
        blkd_virtio_init(&dev, &backend);
        if (!blkd_axi_serve(client_fd, &dev)) {
            fprintf(stderr, "blkd: connection closed\n");
        }
        close(client_fd);
    }

    close(listen_fd);
    blkd_block_close(&backend);
    return 1;
}
