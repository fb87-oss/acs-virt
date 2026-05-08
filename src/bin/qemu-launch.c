#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define FASTOML_IMPLEMENTATION
#include "fastoml.h"

#define MAX_MMIO_WINDOWS 32

struct machine_config {
    char type[64];
    char memory[64];
    bool kvm;
    bool pcie;
};

struct qemu_config {
    char binary[4096];
    char bios_dir[4096];
};

struct transport_config {
    char ram_access[64];
};

struct mmio_window {
    char name[128];
    char base[64];
    char size[64];
    uint32_t irq;
    char socket[4096];
    char target[128];
    bool enabled;
};

struct vm_config {
    struct machine_config machine;
    struct qemu_config qemu;
    struct transport_config transport;
    struct mmio_window windows[MAX_MMIO_WINDOWS];
    uint32_t window_count;
};

struct args {
    const char *config;
    const char *kernel;
    const char *initrd;
    bool dry_run;
    int extra_argc;
    char **extra_argv;
};

struct argv_builder {
    char **items;
    size_t len;
    size_t cap;
};

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s --kernel <bzImage> --initrd <initrd> [--dry-run] <vm.toml> [-- <qemu-args>...]\n",
            program);
    exit(2);
}

static bool read_file(const char *path, char **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long len;
    char *buf;

    if (!f) {
        perror("qemu-launch: fopen config");
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

static void copy_optional_string(const fastoml_node *table, const char *key, char *out, size_t out_len)
{
    if (table && fastoml_table_get_cstr(table, key)) {
        if (!copy_toml_string(table, key, out, out_len)) {
            fprintf(stderr, "qemu-launch: invalid string value for %s\n", key);
            exit(1);
        }
    }
}

static void copy_optional_bool(const fastoml_node *table, const char *key, bool *out)
{
    const fastoml_node *node = table ? fastoml_table_get_cstr(table, key) : NULL;
    int value;

    if (!node) {
        return;
    }
    if (fastoml_node_as_bool(node, &value) != FASTOML_OK) {
        fprintf(stderr, "qemu-launch: invalid bool value for %s\n", key);
        exit(1);
    }
    *out = value != 0;
}

static uint32_t read_required_u32(const fastoml_node *table, const char *key)
{
    const fastoml_node *node = fastoml_table_get_cstr(table, key);
    int64_t value;

    if (!node || fastoml_node_as_int(node, &value) != FASTOML_OK || value < 0 || value > UINT32_MAX) {
        fprintf(stderr, "qemu-launch: invalid integer value for %s\n", key);
        exit(1);
    }
    return (uint32_t)value;
}

static uint64_t parse_u64_text(const char *value)
{
    char *end = NULL;
    int base = !strncmp(value, "0x", 2) ? 16 : 10;
    uint64_t parsed;

    errno = 0;
    parsed = strtoull(value, &end, base);
    if (errno || !end || *end) {
        fprintf(stderr, "qemu-launch: invalid integer literal: %s\n", value);
        exit(1);
    }
    return parsed;
}

static void parse_config(const char *path, struct vm_config *cfg)
{
    char *text = NULL;
    size_t text_len = 0;
    fastoml_options options;
    fastoml_parser *parser;
    const fastoml_document *doc = NULL;
    fastoml_error err;
    fastoml_status status;
    const fastoml_node *root;
    const fastoml_node *machine;
    const fastoml_node *qemu;
    const fastoml_node *transport;
    const fastoml_node *windows;

    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->machine.type, "microvm");
    strcpy(cfg->machine.memory, "512M");
    cfg->machine.kvm = true;
    cfg->machine.pcie = false;
    strcpy(cfg->qemu.binary, "out/qemu-x64-minimal/bin/qemu-system-x86_64");
    strcpy(cfg->qemu.bios_dir, "build/qemu-pc-bios");
    strcpy(cfg->transport.ram_access, "shared-mem");

    if (!read_file(path, &text, &text_len)) {
        exit(1);
    }

    fastoml_options_default(&options);
    parser = fastoml_parser_create(&options);
    if (!parser) {
        free(text);
        exit(1);
    }

    status = fastoml_parse(parser, text, text_len, &doc, &err);
    if (status != FASTOML_OK) {
        fprintf(stderr, "qemu-launch: TOML parse error %s at line %u column %u\n",
                fastoml_status_string(status), err.line, err.column);
        fastoml_parser_destroy(parser);
        free(text);
        exit(1);
    }

    root = fastoml_doc_root(doc);
    machine = fastoml_table_get_cstr(root, "machine");
    if (!machine) {
        fprintf(stderr, "qemu-launch: missing [machine]\n");
        exit(1);
    }
    copy_optional_string(machine, "type", cfg->machine.type, sizeof(cfg->machine.type));
    copy_optional_string(machine, "memory", cfg->machine.memory, sizeof(cfg->machine.memory));
    copy_optional_bool(machine, "kvm", &cfg->machine.kvm);
    copy_optional_bool(machine, "pcie", &cfg->machine.pcie);

    qemu = fastoml_table_get_cstr(root, "qemu");
    copy_optional_string(qemu, "binary", cfg->qemu.binary, sizeof(cfg->qemu.binary));
    copy_optional_string(qemu, "bios_dir", cfg->qemu.bios_dir, sizeof(cfg->qemu.bios_dir));

    transport = fastoml_table_get_cstr(root, "transport");
    copy_optional_string(transport, "ram_access", cfg->transport.ram_access, sizeof(cfg->transport.ram_access));

    windows = fastoml_table_get_cstr(root, "mmio_windows");
    if (windows) {
        cfg->window_count = fastoml_array_size(windows);
        if (cfg->window_count > MAX_MMIO_WINDOWS) {
            fprintf(stderr, "qemu-launch: too many mmio windows\n");
            exit(1);
        }
        for (uint32_t i = 0; i < cfg->window_count; i++) {
            const fastoml_node *window = fastoml_array_at(windows, i);
            struct mmio_window *out = &cfg->windows[i];
            out->enabled = true;
            copy_optional_bool(window, "enabled", &out->enabled);
            if (!copy_toml_string(window, "name", out->name, sizeof(out->name)) ||
                !copy_toml_string(window, "base", out->base, sizeof(out->base)) ||
                !copy_toml_string(window, "size", out->size, sizeof(out->size)) ||
                !copy_toml_string(window, "socket", out->socket, sizeof(out->socket))) {
                fprintf(stderr, "qemu-launch: invalid mmio window %u\n", i);
                exit(1);
            }
            out->irq = read_required_u32(window, "irq");
            if (fastoml_table_get_cstr(window, "target")) {
                copy_optional_string(window, "target", out->target, sizeof(out->target));
            } else {
                strcpy(out->target, out->name);
            }
        }
    }

    fastoml_parser_destroy(parser);
    free(text);
}

static void validate_config(const struct vm_config *cfg)
{
    if (strcmp(cfg->machine.type, "microvm")) {
        fprintf(stderr, "qemu-launch: only machine.type = \"microvm\" is supported\n");
        exit(1);
    }
    if (cfg->machine.pcie) {
        fprintf(stderr, "qemu-launch: PCIe is not supported for this MMIO-only platform\n");
        exit(1);
    }
    if (strcmp(cfg->transport.ram_access, "shared-mem") && strcmp(cfg->transport.ram_access, "qemu-mediated")) {
        fprintf(stderr, "qemu-launch: invalid ram_access: %s\n", cfg->transport.ram_access);
        exit(1);
    }
    for (uint32_t i = 0; i < cfg->window_count; i++) {
        parse_u64_text(cfg->windows[i].base);
        parse_u64_text(cfg->windows[i].size);
        if (cfg->windows[i].irq > 255) {
            fprintf(stderr, "qemu-launch: mmio window %s has invalid irq %u\n",
                    cfg->windows[i].name, cfg->windows[i].irq);
            exit(1);
        }
    }
}

static void parse_args(int argc, char **argv, struct args *out)
{
    memset(out, 0, sizeof(*out));
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--")) {
            out->extra_argc = argc - i - 1;
            out->extra_argv = &argv[i + 1];
            break;
        } else if (!strcmp(argv[i], "--kernel")) {
            if (++i >= argc) {
                usage(argv[0]);
            }
            out->kernel = argv[i];
        } else if (!strcmp(argv[i], "--initrd")) {
            if (++i >= argc) {
                usage(argv[0]);
            }
            out->initrd = argv[i];
        } else if (!strcmp(argv[i], "--dry-run")) {
            out->dry_run = true;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h") || argv[i][0] == '-') {
            usage(argv[0]);
        } else if (!out->config) {
            out->config = argv[i];
        } else {
            usage(argv[0]);
        }
    }
    if (!out->config || !out->kernel || !out->initrd) {
        usage(argv[0]);
    }
}

static char *xstrdup(const char *value)
{
    char *out = strdup(value);
    if (!out) {
        perror("qemu-launch: strdup");
        exit(1);
    }
    return out;
}

static char *xasprintf(const char *fmt, ...)
{
    va_list ap;
    char *out = NULL;

    va_start(ap, fmt);
    if (vasprintf(&out, fmt, ap) < 0) {
        out = NULL;
    }
    va_end(ap);
    if (!out) {
        perror("qemu-launch: asprintf");
        exit(1);
    }
    return out;
}

static char *resolve_path(const char *workspace, const char *path)
{
    if (path[0] == '/') {
        return xstrdup(path);
    }
    return xasprintf("%s/%s", workspace, path);
}

static void argv_push_owned(struct argv_builder *builder, char *value)
{
    if (builder->len + 1 >= builder->cap) {
        size_t next = builder->cap ? builder->cap * 2 : 32;
        char **items = realloc(builder->items, next * sizeof(*items));
        if (!items) {
            perror("qemu-launch: realloc");
            exit(1);
        }
        builder->items = items;
        builder->cap = next;
    }
    builder->items[builder->len++] = value;
    builder->items[builder->len] = NULL;
}

static void argv_push(struct argv_builder *builder, const char *value)
{
    argv_push_owned(builder, xstrdup(value));
}

static void shell_quote(FILE *f, const char *value)
{
    bool bare = value[0] != '\0';
    for (const char *p = value; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              strchr("/._-:=,", *p))) {
            bare = false;
            break;
        }
    }
    if (bare) {
        fputs(value, f);
        return;
    }
    fputc('\'', f);
    for (const char *p = value; *p; p++) {
        if (*p == '\'') {
            fputs("'\\''", f);
        } else {
            fputc(*p, f);
        }
    }
    fputc('\'', f);
}

static int run_qemu(char **argv)
{
    pid_t pid = fork();
    int status;

    if (pid < 0) {
        perror("qemu-launch: fork");
        return 1;
    }
    if (pid == 0) {
        execv(argv[0], argv);
        perror("qemu-launch: execv");
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) {
        perror("qemu-launch: waitpid");
        return 1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

int main(int argc, char **argv)
{
    struct args args;
    struct vm_config config;
    struct argv_builder qemu_argv = {0};
    char workspace[4096];
    char *qemu_bin;
    char *bios_dir;
    char *machine_arg;

    parse_args(argc, argv, &args);
    if (!getcwd(workspace, sizeof(workspace))) {
        perror("qemu-launch: getcwd");
        return 1;
    }

    parse_config(args.config, &config);
    validate_config(&config);

    qemu_bin = resolve_path(workspace, config.qemu.binary);
    bios_dir = resolve_path(workspace, config.qemu.bios_dir);
    if (!args.dry_run && access(qemu_bin, X_OK) < 0) {
        fprintf(stderr, "qemu-launch: QEMU binary not found: %s. Run scripts/build-qemu-x64.sh first.\n", qemu_bin);
        return 1;
    }

    machine_arg = xasprintf("microvm,pcie=%s,ioapic2=on,virtio-mmio-transports=0,memory-backend=guestmem",
                            config.machine.pcie ? "on" : "off");

    argv_push_owned(&qemu_argv, qemu_bin);
    argv_push(&qemu_argv, "-L");
    argv_push_owned(&qemu_argv, bios_dir);
    argv_push(&qemu_argv, "-object");
    argv_push_owned(&qemu_argv, xasprintf("memory-backend-memfd,id=guestmem,size=%s,share=on", config.machine.memory));
    argv_push(&qemu_argv, "-machine");
    argv_push_owned(&qemu_argv, machine_arg);
    argv_push(&qemu_argv, "-m");
    argv_push(&qemu_argv, config.machine.memory);
    argv_push(&qemu_argv, "-nographic");
    argv_push(&qemu_argv, "-kernel");
    argv_push(&qemu_argv, args.kernel);
    argv_push(&qemu_argv, "-initrd");
    argv_push(&qemu_argv, args.initrd);
    argv_push(&qemu_argv, "-append");
    argv_push(&qemu_argv, "console=ttyS0 root=/dev/ram0 rdinit=/linuxrc loglevel=8");
    if (config.machine.kvm) {
        argv_push(&qemu_argv, "-enable-kvm");
    }
    for (uint32_t i = 0; i < config.window_count; i++) {
        struct mmio_window *window = &config.windows[i];
        char *socket_path;
        if (!window->enabled) {
            continue;
        }
        socket_path = resolve_path(workspace, window->socket);
        argv_push(&qemu_argv, "-device");
        argv_push_owned(&qemu_argv,
                        xasprintf("axi-bus,id=%s,base=%s,size=%s,irq=%u,socket=%s,ram-access=%s,target=%s",
                                  window->name, window->base, window->size, window->irq, socket_path,
                                  config.transport.ram_access, window->target));
        free(socket_path);
    }
    for (int i = 0; i < args.extra_argc; i++) {
        argv_push(&qemu_argv, args.extra_argv[i]);
    }

    if (args.dry_run) {
        for (size_t i = 0; i < qemu_argv.len; i++) {
            if (i) {
                putchar(' ');
            }
            shell_quote(stdout, qemu_argv.items[i]);
        }
        putchar('\n');
        return 0;
    }

    return run_qemu(qemu_argv.items);
}
