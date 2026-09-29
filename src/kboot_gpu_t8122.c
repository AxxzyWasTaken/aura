/* SPDX-License-Identifier: MIT */

#include "adt.h"
#include "kboot_gpu_t8122.h"
#include "libfdt/libfdt.h"
#include <malloc.h>

/*
 * The Air has no grounded GPU DT or hardware configuration yet. Preserve a
 * small, explicit set of iBoot's ADT metadata for Linux-side inspection before
 * anyone guesses resources from the Pro. These are raw little-endian ADT
 * properties, not Linux device bindings. No firmware bytes or MMIO reads.
 *
 * No export unless chosen.asahi,t8122-gpu-diag=1. The caller selects T8122.
 * Build in a private FDT copy and publish only on success, including strings.
 * Failure is non-fatal to kboot. A pre-existing export is never overwritten.
 */
#define EXPORT_NAME "asahi,t8122-gpu-diagnostics"
#define EXPORT_PATH "/chosen/" EXPORT_NAME
#define MAX_PROPERTY_BYTES 32768u
#define MAX_TOTAL_BYTES 49152u

static const char *const nodes[] = {"arm-io", "sgx", "gfx-asc", "pmgr", "mapper-gfx-asc"};
static const char *const paths[] = {
    "/arm-io", "/arm-io/sgx", "/arm-io/gfx-asc", "/arm-io/pmgr",
    "/arm-io/mapper-gfx-asc",
};

/* Only named hardware-description fields, never arbitrary node contents. */
static const char *const properties[] = {
    "compatible", "chip-revision", "#address-cells", "#size-cells", "ranges",
    "reg", "interrupts", "interrupts-valid", "device", "devices", "ps-regs", "clock-gates",
    "AAPL,phandle", "iommu-parent", "gpu-num-perf-states", "gpu-perf-base-pstate",
    "gptbat-ready",
    "power-gates", "perf-state-count", "perf-state-table-count", "perf-states",
    "perf-states-sram", "gpu-core-count", "gpu-core-mask", "gpu-count",
    "gpu-num-mgpus", "gpu-num-cores", "gpu-num-frags",
    "gfx-handoff-base", "gfx-handoff-size", "gfx-shared-region-base",
    "gfx-shared-region-size", "gfx-shared-l2-region-base",
    "gfx-shared-l2-region-size", "gpu-region-base", "gpu-region-size",
};

static int export_metadata(void *dt, int chosen)
{
    int out = fdt_add_subnode(dt, chosen, EXPORT_NAME);
    if (out < 0)
        return -1;
    if (fdt_setprop_u32(dt, out, "schema-version", 1) ||
        fdt_setprop_string(dt, out, "encoding", "raw-adt-little-endian"))
        goto fail;

    u32 total = 0;
    for (unsigned int i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
        int in = adt_path_offset(adt, paths[i]);
        /* Missing optional nodes remain absent; sgx and gfx-asc are required. */
        if (in < 0) {
            if (i == 1 || i == 2)
                goto fail;
            continue;
        }
        int child = fdt_add_subnode(dt, out, nodes[i]);
        if (child < 0 || fdt_setprop_string(dt, child, "adt-path", paths[i]))
            goto fail;
        for (unsigned int j = 0; j < sizeof(properties) / sizeof(properties[0]); j++) {
            u32 size = 0;
            const void *value = adt_getprop(adt, in, properties[j], &size);
            if (!value)
                continue;
            if (size > MAX_PROPERTY_BYTES || size > MAX_TOTAL_BYTES - total)
                goto fail;
            total += size;
            /* Prefix distinguishes ADT bytes from normal FDT cells. */
            char name[64] = "adt,";
            size_t n = strlen(properties[j]);
            if (n >= sizeof(name) - 4)
                goto fail;
            memcpy(name + 4, properties[j], n + 1);
            if (fdt_setprop(dt, child, name, value, size))
                goto fail;
        }
    }
    if (fdt_setprop(dt, out, "complete", NULL, 0))
        goto fail;
    return 0;

fail:
    return -1;
}

int dt_gpu_t8122_diagnostics(void *dt)
{
    int chosen = fdt_path_offset(dt, "/chosen");
    int len = 0;
    const char *opt = chosen < 0 ? NULL :
        fdt_getprop(dt, chosen, "asahi,t8122-gpu-diag", &len);
    if (!opt || len != 2 || opt[0] != '1' || opt[1] != '\0')
        return 0;
    if (fdt_path_offset(dt, EXPORT_PATH) >= 0)
        return 0;

    size_t size = fdt_totalsize(dt);
    void *copy = malloc(size);
    if (!copy)
        return 0;
    memcpy(copy, dt, size);
    if (!export_metadata(copy, chosen))
        memcpy(dt, copy, size);
    free(copy);
    return 0;
}
