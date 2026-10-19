# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the production T600x route handoff against real, relocating FDTs."""

from pathlib import Path
from test_siliconforest_handoff import definition, run_c

ROOT = Path(__file__).resolve().parents[2]


def test_routes_follow_the_reserved_owner_atomically(tmp_path):
    source = (ROOT / "src/kboot.c").read_text()
    run_c(tmp_path, "dcp-routes", r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
#define T6000 0x6000
#define T6001 0x6001
#define T6002 0x6002
static unsigned char blob[16384];
static void *dt = blob;
static unsigned chip_id;
static int writes, fail_write, fail_alloc;
static void *checked_malloc(size_t size)
{
    return fail_alloc ? NULL : malloc(size);
}
static int checked_set_string(void *tree, int node, const char *name, const char *value)
{
    if (++writes == fail_write)
        return -FDT_ERR_NOSPACE;
    return fdt_setprop(tree, node, name, value, strlen(value) + 1);
}
#undef fdt_setprop_string
#define fdt_setprop_string checked_set_string
#define malloc checked_malloc
''' + definition(source, "dt_enable_dcp_with_typec_routes") + r'''
#undef malloc
static int path(const char *name)
{
    int node = fdt_path_offset(dt, name);
    assert(node >= 0);
    return node;
}
static void status(const char *name, const char *expected)
{
    int len;
    const char *value = fdt_getprop(dt, path(name), "status", &len);
    if (!expected) {
        assert(!value && len == -FDT_ERR_NOTFOUND);
        return;
    }
    assert(value && len == (int)strlen(expected) + 1 && !strcmp(value, expected));
}
static void world(bool routes)
{
    memset(blob, 0, sizeof(blob)); dt = blob; chip_id = T6000;
    writes = fail_write = fail_alloc = 0;
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    assert(fdt_add_subnode(dt, 0, "soc") >= 0);
    int node = fdt_add_subnode(dt, path("/soc"), "dcpext0");
    assert(node >= 0);
    assert(!fdt_setprop_string(dt, node, "status", "disabled"));
    assert(!fdt_setprop_u32(dt, node, "memory-region", 99));
    if (routes) {
        assert(fdt_add_subnode(dt, node, "typec-routes") >= 0);
        /* Mixed lengths/order force later sibling offsets to relocate. */
        const char *names[] = {"route@0", "route@1", "route@2", "legacy",
                               "reserved", "failed", "malformed", "enabled"};
        const char *values[] = {"disabled", "disabled", "disabled", NULL,
                                "reserved", "fail", NULL, "okay"};
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            node = fdt_add_subnode(dt, path("/soc/dcpext0/typec-routes"), names[i]);
            assert(node >= 0);
            if (values[i])
                assert(!fdt_setprop_string(dt, node, "status", values[i]));
            if (i == 6) /* Not a terminated string. */
                assert(!fdt_setprop(dt, node, "status", "disabled", 8));
        }
    }
    /* The aliases follow reservation/controller edits in the structure. */
    node = fdt_add_subnode(dt, 0, "aliases");
    assert(node >= 0);
    assert(!fdt_setprop_string(dt, node, "dcpext0", "/soc/dcpext0"));
    writes = 0;
}
static void routes_status(const char *expected)
{
    status("/soc/dcpext0/typec-routes/route@0", expected);
    status("/soc/dcpext0/typec-routes/route@1", expected);
    status("/soc/dcpext0/typec-routes/route@2", expected);
    status("/soc/dcpext0/typec-routes/legacy", NULL);
    status("/soc/dcpext0/typec-routes/reserved", "reserved");
    status("/soc/dcpext0/typec-routes/failed", "fail");
    status("/soc/dcpext0/typec-routes/enabled", "okay");
    int len;
    const char *p = fdt_getprop(dt, path("/soc/dcpext0/typec-routes/malformed"),
                                "status", &len);
    assert(p && len == 8 && !memcmp(p, "disabled", 8));
}
int main(void)
{
    const unsigned chips[] = {T6000, T6001, T6002, 0x8103, 0x8112, 0x6020, 0x6030, 0x8122};
    for (unsigned i = 0; i < sizeof(chips) / sizeof(chips[0]); i++) {
        world(true); chip_id = chips[i];
        void *original = dt;
        assert(!dt_enable_dcp_with_typec_routes("dcpext0"));
        assert(dt == original && !fdt_check_header(dt));
        status("/soc/dcpext0", "okay");
        routes_status(i < 3 ? "okay" : "disabled");
        int len;
        const fdt32_t *p = fdt_getprop(dt, path("dcpext0"), "memory-region", &len);
        assert(p && len == 4 && fdt32_ld(p) == 99);
    }
    for (int failure = 0; failure <= 4; failure++) {
        world(true);
        unsigned char before[sizeof(blob)]; memcpy(before, blob, sizeof(blob));
        fail_alloc = failure == 0; fail_write = failure;
        assert(dt_enable_dcp_with_typec_routes("dcpext0") < 0);
        assert(dt == blob && !memcmp(before, blob, sizeof(blob)));
    }
    world(true);
    unsigned char before[sizeof(blob)]; memcpy(before, blob, sizeof(blob));
    assert(dt_enable_dcp_with_typec_routes("absent") < 0);
    assert(dt == blob && !memcmp(before, blob, sizeof(blob)));
    world(false);
    assert(!dt_enable_dcp_with_typec_routes("dcpext0"));
    status("/soc/dcpext0", "okay");
    puts("PASS T600x route ownership, relocation, legacy routes and atomic refusal");
    return 0;
}
''')


def test_reservation_caller_never_publishes_routes_after_failure(tmp_path):
    source = (ROOT / "src/kboot.c").read_text()
    start = source.index("struct disp_mapping {")
    layout = source[start:source.index("static int dt_add_reserved_regions(", start)]
    run_c(tmp_path, "dcp-route-reservations", r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef uint32_t u32;
typedef uint64_t u64;
typedef int dart_dev_t;
#define T6000 0x6000
#define T6001 0x6001
#define T6002 0x6002
#define bail_cleanup(...) do { ret = -1; goto err; } while (0)
static unsigned char blob[8192];
static void *dt = blob;
static unsigned chip_id = T6000;
static int fail_reserve, fail_dart, shutdowns, dart;
static dart_dev_t *dt_init_dart_by_node(int node, int index)
{ return fail_dart ? NULL : &dart; }
static void dart_shutdown(dart_dev_t *d) { shutdowns++; }
static int dt_get_iommu_node(int node, int index) { return -1; }
static int dt_get_or_add_reserved_mem(const char *name, const char *compat,
                                     bool nomap, u64 address, size_t size)
{ return fail_reserve ? -FDT_ERR_NOSPACE : fdt_path_offset(dt, "/reserved-memory"); }
static int dt_device_set_reserved_mem_from_dart(int n, dart_dev_t *d, const char *name,
                                               uint32_t phandle, u64 address, size_t size)
{ return 0; }
static int dt_device_add_mem_region(const char *alias, uint32_t phandle, const char *name)
{ return 0; }
''' + layout + definition(source, "dt_enable_dcp_with_typec_routes") +
        definition(source, "dt_add_reserved_regions") + r'''
static void world(void)
{
    memset(blob, 0, sizeof(blob)); dt = blob;
    fail_reserve = fail_dart = shutdowns = 0;
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    assert(fdt_add_subnode(dt, 0, "reserved-memory") >= 0);
    int node = fdt_add_subnode(dt, 0, "controller"); assert(node >= 0);
    assert(!fdt_setprop_string(dt, node, "status", "disabled"));
    node = fdt_add_subnode(dt, node, "typec-routes"); assert(node >= 0);
    node = fdt_add_subnode(dt, node, "route@0"); assert(node >= 0);
    assert(!fdt_setprop_string(dt, node, "status", "disabled"));
}
static void state(const char *expected)
{
    const char *paths[] = {"/controller", "/controller/typec-routes/route@0"};
    for (unsigned i = 0; i < 2; i++) {
        int node = fdt_path_offset(dt, paths[i]); assert(node >= 0);
        assert(!strcmp(fdt_getprop(dt, node, "status", NULL), expected));
    }
}
int main(void)
{
    struct disp_mapping map = { .mem_fdt = "firmware" };
    struct mem_region region = { .paddr = 0x100000, .size = 0x4000 };
    world(); fail_reserve = 1;
    assert(dt_add_reserved_regions("/controller", NULL, NULL, "apple,asc-mem",
                                   &map, &region, 1) < 0);
    state("disabled"); assert(shutdowns == 1);
    world(); fail_dart = 1;
    assert(dt_add_reserved_regions("/controller", NULL, NULL, "apple,asc-mem",
                                   &map, &region, 1) < 0);
    state("disabled"); assert(shutdowns == 0);
    world();
    assert(!dt_add_reserved_regions("/controller", NULL, NULL, "apple,asc-mem",
                                    &map, &region, 1));
    state("okay"); assert(shutdowns == 1);
    puts("PASS production reservation caller: no route publication after DART/reservation failure");
}
''')
