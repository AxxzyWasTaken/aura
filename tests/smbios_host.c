/* SPDX-License-Identifier: MIT */
/*
 * Host test of kboot.c's SMBIOS node (dt_set_cpus() to dt_set_smbios()) on one Mac's ADT and
 * device tree. tests/run-smbios.sh builds it with the functions copied out of src/kboot.c.
 *
 *   smbios-host <mac.adt> <mac.dtb> [sme]
 *
 * Prints every property of /smbios/smbios, then checks that the tables U-Boot builds from it
 * would carry the values Linux tools read.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adt.h"
#include "types.h"
#include "utils.h"

#include "libfdt/libfdt.h"

void *adt;
static size_t adt_size;
static int host_sme;

u32 adt_get_size(void)
{
    return adt_size;
}

int host_set_smbios(void *dt);

u64 host_sysreg(const char *name)
{
    if (strstr(name, "ID_AA64PFR1"))
        return host_sme ? (1UL << 24) : 0;
    fprintf(stderr, "unexpected system register %s\n", name);
    exit(2);
}

static void *slurp(const char *path, size_t extra, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    void *buf;

    if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
        perror(path);
        exit(2);
    }
    buf = calloc(1, n + extra);
    if (!buf || fread(buf, 1, n, f) != (size_t)n) {
        perror(path);
        exit(2);
    }
    fclose(f);
    *size = n;
    return buf;
}

static void dump(void *dt, int node, const char *path)
{
    int prop, child;

    fdt_for_each_property_offset(prop, dt, node)
    {
        const char *name;
        int len;
        const unsigned char *val = fdt_getprop_by_offset(dt, prop, &name, &len);
        int printable = len > 0 && val[len - 1] == 0;

        for (int i = 0; printable && i < len - 1; i++)
            if (val[i] < 0x20 || val[i] > 0x7e)
                printable = 0;
        printf("%s %s = ", path, name);
        if (printable) {
            printf("\"%s\"\n", val);
        } else if (len == 4) {
            printf("<0x%x>\n", fdt32_ld((const fdt32_t *)val));
        } else if (len == 8) {
            printf("<0x%llx>\n", (unsigned long long)fdt64_ld((const fdt64_t *)val));
        } else {
            printf("[%d bytes]\n", len);
        }
    }
    fdt_for_each_subnode(child, dt, node)
    {
        char sub[256];

        snprintf(sub, sizeof(sub), "%s/%s", path, fdt_get_name(dt, child, NULL));
        dump(dt, child, sub);
    }
}

static int fails;

static void expect_str(void *dt, const char *path, const char *prop, const char *want)
{
    int node = fdt_path_offset(dt, path);
    const char *got = node >= 0 ? fdt_getprop(dt, node, prop, NULL) : NULL;

    if (!got || (want && strcmp(got, want))) {
        printf("FAIL %s %s: %s, want %s\n", path, prop, got ? got : "(absent)",
               want ? want : "(any)");
        fails++;
    } else {
        printf("ok %s %s\n", path, prop);
    }
}

static void expect_nonzero(void *dt, const char *path, const char *prop)
{
    int node = fdt_path_offset(dt, path), len;
    const fdt32_t *val = node >= 0 ? fdt_getprop(dt, node, prop, &len) : NULL;

    if (!val || (len == 4 && !fdt32_ld(val)) || (len == 8 && !fdt64_ld((const fdt64_t *)val))) {
        printf("FAIL %s %s: absent or zero\n", path, prop);
        fails++;
    } else {
        printf("ok %s %s\n", path, prop);
    }
}

int main(int argc, char **argv)
{
    size_t dt_size;
    void *dt;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <mac.adt> <mac.dtb> [sme]\n", argv[0]);
        return 2;
    }
    host_sme = argc > 3 && !strcmp(argv[3], "sme");
    adt = slurp(argv[1], 0, &adt_size);
    dt = slurp(argv[2], 0, &dt_size);
    dt = realloc(dt, dt_size + 65536);
    if (!dt || fdt_open_into(dt, dt, dt_size + 65536))
        return 2;

    if (host_set_smbios(dt)) {
        printf("FAIL dt_set_smbios\n");
        return 1;
    }
    /* A second pass, as a reloaded aura would make, must leave one node of each */
    if (host_set_smbios(dt)) {
        printf("FAIL second dt_set_smbios\n");
        return 1;
    }

    dump(dt, fdt_path_offset(dt, "/smbios"), "/smbios");

    expect_str(dt, "/smbios/smbios/system", "product", NULL);
    expect_str(dt, "/smbios/smbios/system", "sku", NULL);
    expect_str(dt, "/smbios/smbios/system", "family", NULL);
    expect_str(dt, "/smbios/smbios/processor", "version", NULL);
    expect_nonzero(dt, "/smbios/smbios/processor", "core-count");
    expect_nonzero(dt, "/smbios/smbios/processor", "max-speed");
    expect_nonzero(dt, "/smbios/smbios/memory-device/dram", "size");
    expect_nonzero(dt, "/smbios/smbios/memory-array/dram", "maximum-capacity");
    expect_nonzero(dt, "/smbios/smbios/memory-array-mapped-address/dram",
                   "extended-ending-address");
    if (fdt_subnode_offset(dt, fdt_path_offset(dt, "/smbios/smbios"), "system") < 0 ||
        fdt_path_offset(dt, "/smbios/smbios/memory-device/dram") < 0) {
        printf("FAIL node layout\n");
        fails++;
    }
    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return !!fails;
}
