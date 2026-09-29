/* SPDX-License-Identifier: MIT */
/* Synthetic ADT inputs + real libfdt. No device or firmware access. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/adt.h"
#include "../src/kboot_gpu_t8122.h"
#include "../src/libfdt/libfdt.h"

void *adt;
static int mode, reads, fail_alloc;
void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size)
{
    return fail_alloc ? NULL : __real_malloc(size);
}
static unsigned char data[32769];
static const char *const paths[] = {
    "/arm-io", "/arm-io/sgx", "/arm-io/gfx-asc", "/arm-io/pmgr",
    "/arm-io/mapper-gfx-asc",
};

int adt_path_offset(const void *tree, const char *path)
{
    (void)tree;
    reads++;
    for (int i = 0; i < 5; i++)
        if (!strcmp(paths[i], path))
            return mode == 1 && i == 2 ? -1 : i;
    return -1;
}

const void *adt_getprop(const void *tree, int node, const char *name, u32 *size)
{
    (void)tree;
    if (strcmp(name, "reg"))
        return NULL;
    *size = mode == 2 ? sizeof(data) : mode == 3 ? 32768 : 16;
    data[0] = (unsigned char)(node + 1);
    return data;
}

static void prepare(void *dt, int size, const void *opt, int len)
{
    assert(!fdt_create_empty_tree(dt, size));
    int node = fdt_add_subnode(dt, 0, "chosen");
    assert(node >= 0);
    assert(!fdt_setprop_string(dt, node, "bootargs", "unchanged"));
    if (opt)
        assert(!fdt_setprop(dt, node, "asahi,t8122-gpu-diag", opt, len));
    mode = reads = 0;
}

int main(void)
{
    static unsigned char dt[131072], before[sizeof(dt)];
    const char *export_path = "/chosen/asahi,t8122-gpu-diagnostics";

    assert(!fdt_create_empty_tree(dt, sizeof(dt)));
    memcpy(before, dt, sizeof(dt));
    assert(!dt_gpu_t8122_diagnostics(dt));
    assert(!reads && !memcmp(before, dt, sizeof(dt)));

    prepare(dt, sizeof(dt), "1", 2);
    memcpy(before, dt, sizeof(dt));
    fail_alloc = 1;
    assert(!dt_gpu_t8122_diagnostics(dt));
    fail_alloc = 0;
    assert(!reads && !memcmp(before, dt, sizeof(dt)));

    /* Exact string opt-in only, including its NUL terminator. */
    const char *opts[] = {NULL, "0", "1", "1xx", "1"};
    int lengths[] = {0, 2, 1, 3, 2};
    for (int i = 0; i < 4; i++) {
        prepare(dt, sizeof(dt), opts[i], lengths[i]);
        memcpy(before, dt, sizeof(dt));
        assert(!dt_gpu_t8122_diagnostics(dt));
        assert(!reads && !memcmp(before, dt, sizeof(dt)));
    }

    prepare(dt, sizeof(dt), "1", 2);
    assert(!dt_gpu_t8122_diagnostics(dt));
    int root = fdt_path_offset(dt, export_path), len;
    assert(root >= 0);
    assert(fdt_getprop(dt, root, "complete", &len) && len == 0);
    const fdt32_t *version = fdt_getprop(dt, root, "schema-version", &len);
    assert(version && len == 4 && fdt32_to_cpu(*version) == 1);
    for (int i = 0; i < 5; i++) {
        char path[128];
        const char *name = strrchr(paths[i], '/') + 1;
        snprintf(path, sizeof(path), "%s/%s", export_path, name);
        int node = fdt_path_offset(dt, path);
        const unsigned char *p = fdt_getprop(dt, node, "adt,reg", &len);
        assert(p && len == 16 && p[0] == i + 1);
        assert(!fdt_getprop(dt, node, "reg", &len));
        assert(!fdt_getprop(dt, node, "compatible", &len));
    }
    int chosen = fdt_path_offset(dt, "/chosen");
    assert(!strcmp(fdt_getprop(dt, chosen, "bootargs", NULL), "unchanged"));
    /* Repeated calls do not overwrite earlier evidence. */
    memcpy(before, dt, sizeof(dt));
    reads = 0;
    assert(!dt_gpu_t8122_diagnostics(dt));
    assert(!reads && !memcmp(before, dt, sizeof(dt)));

    /* Missing ASC, oversized property and aggregate overflow roll back bytes. */
    for (int failure = 1; failure <= 3; failure++) {
        prepare(dt, sizeof(dt), "1", 2);
        mode = failure;
        memcpy(before, dt, sizeof(dt));
        assert(!dt_gpu_t8122_diagnostics(dt));
        assert(!memcmp(before, dt, sizeof(dt)));
    }

    /* Exercise every out-of-space boundary, including partial child writes. */
    for (int size = 256; size <= 2048; size += 8) {
        prepare(dt, size, "1", 2);
        memcpy(before, dt, size);
        assert(!dt_gpu_t8122_diagnostics(dt));
        root = fdt_path_offset(dt, export_path);
        if (root < 0)
            assert(!memcmp(before, dt, size));
        else
            assert(fdt_getprop(dt, root, "complete", &len) && len == 0);
    }
    puts("PASS: opt-in, metadata, idempotency, allocation failure, bounds and 225 capacity boundaries");
}
