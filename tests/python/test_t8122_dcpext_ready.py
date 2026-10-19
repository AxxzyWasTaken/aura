# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise firmware-ready publication with production reservation code and real FDTs."""

from pathlib import Path
from test_siliconforest_handoff import run_c

ROOT = Path(__file__).resolve().parents[2]


def test_firmware_ready_requires_complete_reservations_and_keeps_suppliers_disabled(tmp_path):
    source = (ROOT / "src/kboot.c").read_text()

    def function(name):
        start = source.index("static int " + name + "(")
        return source[start:source.index("\n}", start) + 2] + "\n"

    start = source.index("static const char dcpext_aliases")
    aliases = source[start:source.index("\n};", start) + 3]
    header = (ROOT / "src/adt.h").read_text()
    start = header.index("struct adt_segment_ranges {")
    layout = header[start:header.index("} PACKED;", start) + len("} PACKED;")]
    run_c(tmp_path, "dcpext-ready", r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef uint64_t u64;
typedef uint32_t u32;
#define PACKED __attribute__((packed))
#define T8122 0x8122
#define T6030 0x6030
#define T6000 0x6000
#define T6001 0x6001
#define T6002 0x6002
#define SZ_16K 16384
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define T8122_DCPEXT_READY "apple,t8122-dcpext-firmware-ready"
#define bail(...) do { return -1; } while (0)
static void *dt, *adt;
static u32 chip_id = T8122;
static int fault, reservations;
static unsigned char blob[32768];
''' + layout + r'''
static struct adt_segment_ranges segments[2];
static u64 dram_base = 0x10000000000ULL, dram_size = 0x1000000;
static int adt_path_offset(void *tree, const char *path)
{
    if (!strcmp(path, "/chosen")) return 2;
    return !strcmp(path, "/arm-io/dcpext/iop-dcpext-nub") && fault != 5 ? 1 : -1;
}
static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    if (!strcmp(name, "segment-ranges")) {
        *len = fault == 6 ? 0 : fault == 7 ? sizeof(segments) - 1 : sizeof(segments);
        return fault == 8 ? NULL : segments;
    }
    *len = fault == 9 ? 4 : 8;
    if (!strcmp(name, "dram-base")) return &dram_base;
    if (!strcmp(name, "dram-size")) return &dram_size;
    return NULL;
}
static int get_value(int node, const char *name, void *out, size_t capacity)
{
    u32 len = 0;
    const void *p = adt_getprop(adt, node, name, &len);
    if (!p || len > capacity) return -1;
    memcpy(out, p, len);
    return len;
}
#define ADT_GETPROP(a, n, k, v) get_value(n, k, v, sizeof(*(v)))
static int dt_get_or_add_reserved_mem(const char *name, const char *compat,
                                     bool nomap, u64 physical, size_t size)
{
    reservations++;
    if ((fault == 1 && reservations == 1) || (fault == 2 && reservations == 2))
        return -FDT_ERR_NOSPACE;
    int parent = fdt_path_offset(dt, "/reserved-memory");
    int node = fdt_add_subnode(dt, parent, name);
    assert(node >= 0);
    assert(!fdt_setprop_u32(dt, node, "phandle", 100 + reservations));
    fdt64_t reg[2] = {cpu_to_fdt64(physical), cpu_to_fdt64(size)};
    assert(!fdt_setprop(dt, node, "reg", reg, sizeof(reg)));
    assert(!fdt_setprop_empty(dt, node, "no-map"));
    return node;
}
static int dt_set_dcp_firmware(const char *alias)
{
    int node = fdt_path_offset(dt, alias);
    fdt32_t ver[3] = {cpu_to_fdt32(14), cpu_to_fdt32(7), 0};
    assert(!fdt_setprop(dt, node, "apple,firmware-compat", ver, sizeof(ver)));
    return fault == 3 ? -1 : 0;
}
static int set_u32(void *tree, int node, const char *name, uint32_t value)
{
    if (fault == 4 && !strcmp(name, T8122_DCPEXT_READY)) return -FDT_ERR_NOSPACE;
    return fdt_setprop_u32(tree, node, name, value);
}
#define fdt_setprop_u32 set_u32
''' + function("dt_enable_dcp_with_typec_routes") +
        function("dt_device_set_reserved_mem") + function("dt_device_add_mem_region") +
        function("dt_reserve_asc_firmware") + aliases + "\n" +
        function("dt_display_transaction") + function("dt_reserve_t8122_dcpext_one") +
        function("dt_reserve_dcpext_firmware") + r'''
static void world(void)
{
    fault = reservations = 0; chip_id = T8122;
    dram_base = 0x10000000000ULL; dram_size = 0x1000000;
    segments[0] = (struct adt_segment_ranges){.phys = dram_base + 0x4000,
        .remap = 0x10000000, .size = 0x4000};
    segments[1] = (struct adt_segment_ranges){.phys = dram_base + 0x10000,
        .remap = 0x20000000, .size = 0x8000};
    memset(blob, 0, sizeof(blob)); dt = blob;
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    assert(!fdt_setprop_string(dt, 0, "compatible", "apple,t8122"));
    assert(fdt_add_subnode(dt, 0, "reserved-memory") >= 0);
    assert(fdt_add_subnode(dt, 0, "soc") >= 0);
    const char *names[] = {"dcpext", "firmware-dart", "display-dart", "mailbox", "crossbar", "route"};
    for (unsigned i = 0; i < ARRAY_SIZE(names); i++) {
        int node = fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), names[i]);
        assert(node >= 0 && !fdt_setprop_string(dt, node, "status", "disabled"));
        if (!i) assert(!fdt_setprop_string(dt, node, "compatible", "apple,t8122-dcpext"));
    }
    int node = fdt_add_subnode(dt, 0, "aliases");
    assert(node >= 0 && !fdt_setprop_string(dt, node, "dcpext", "/soc/dcpext"));
}
static bool ready(void)
{
    int len = 0, node = fdt_path_offset(dt, "dcpext");
    const fdt32_t *value = fdt_getprop(dt, node, T8122_DCPEXT_READY, &len);
    return value && len == 4 && fdt32_ld(value) == 1;
}
static void still_disabled(void)
{
    const char *paths[] = {"/soc/dcpext", "/soc/firmware-dart", "/soc/display-dart",
        "/soc/mailbox", "/soc/crossbar", "/soc/route"};
    for (unsigned i = 0; i < ARRAY_SIZE(paths); i++) {
        int node = fdt_path_offset(dt, paths[i]);
        const char *status = fdt_getprop(dt, node, "status", NULL);
        assert(status && !strcmp(status, "disabled"));
    }
}
int main(void)
{
    world();
    assert(!dt_reserve_dcpext_firmware(false) && ready() && reservations == 2);
    int node = fdt_path_offset(dt, "dcpext");
    assert(fdt_stringlist_count(dt, node, "status") == 1);
    int len = 0;
    assert(fdt_getprop(dt, node, "memory-region", &len) && len == 8);
    still_disabled();
    for (int scenario = 1; scenario <= 9; scenario++) {
        world(); fault = scenario;
        unsigned char before[sizeof(blob)]; memcpy(before, blob, sizeof(blob));
        assert(!dt_reserve_dcpext_firmware(false));
        assert(!ready() && !memcmp(before, blob, sizeof(blob)));
        still_disabled();
    }
    for (int scenario = 0; scenario < 6; scenario++) {
        world();
        if (scenario == 0) segments[0].phys = dram_base - 0x4000;
        if (scenario == 1) segments[0].phys++;
        if (scenario == 2) segments[1].size = 0;
        if (scenario == 3) segments[1].remap = UINT64_MAX;
        if (scenario == 4) dram_size = 0;
        if (scenario == 5) dram_size = UINT64_MAX;
        assert(!dt_reserve_dcpext_firmware(false) && !ready() && !reservations);
        still_disabled();
    }
    world();
    assert(!fdt_setprop_u32(dt, fdt_path_offset(dt, "dcpext"), T8122_DCPEXT_READY, 1));
    fault = 5; /* A stale marker may not survive missing firmware metadata. */
    assert(!dt_reserve_dcpext_firmware(false) && !ready());
    still_disabled();
    world(); chip_id = 0x8103;
    assert(!dt_reserve_dcpext_firmware(false) && !ready() && !reservations);
    world(); assert(!fdt_setprop_string(dt, 0, "compatible", "apple,t8103"));
    assert(!dt_reserve_dcpext_firmware(false) && !ready() && !reservations);
    world();
    assert(!dt_reserve_dcpext_firmware(true) && !ready() && reservations == 2);
    node = fdt_path_offset(dt, "dcpext");
    assert(!strcmp(fdt_getprop(dt, node, "status", NULL), "okay"));
    return 0;
}
''')
