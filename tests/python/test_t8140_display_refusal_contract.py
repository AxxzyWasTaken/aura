# SPDX-License-Identifier: MIT
# Generated-by: LLM
# Assisted-by: LLM
"""Check complete DP supplier withdrawal with J700's alias-free external DT."""

import os
from pathlib import Path
import shlex
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture
def refusal_binary(tmp_path):
    source = (ROOT / "src/kboot.c").read_text()
    iommu = source[source.index("static int dt_get_iommu_node("):
                   source.index("static dart_dev_t *dt_init_dart_by_node(")]
    helpers = source[source.index("static int dt_t8140_disable_refs("):
                     source.index("/* A bounded observation of register descriptions")]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef uint32_t u32;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
static unsigned char tree[16384];
static void *dt = tree;
static size_t dt_bufsize = sizeof(tree);
static const char *failed_path;
static const char *lookup_fault;
static int guarded_subnode_offset(const void *blob, int parent, const char *name)
{
    if (lookup_fault && !strcmp(lookup_fault, "route-lookup") &&
        !strcmp(name, "typec-routes"))
        return -FDT_ERR_BADSTRUCTURE;
    return fdt_subnode_offset(blob, parent, name);
}
static int guarded_next_subnode(const void *blob, int offset)
{
    if (lookup_fault && !strcmp(lookup_fault, "route-iteration"))
        return -FDT_ERR_BADSTRUCTURE;
    return fdt_next_subnode(blob, offset);
}
static int guarded_compatible(const void *blob, int start, const char *compatible)
{
    if (lookup_fault &&
        ((!strcmp(lookup_fault, "external-lookup") &&
          !strcmp(compatible, "apple,t8140-dcpext")) ||
         (!strcmp(lookup_fault, "whole-iteration") &&
          !strcmp(compatible, "apple,t8140-dcp"))))
        return -FDT_ERR_BADSTRUCTURE;
    return fdt_node_offset_by_compatible(blob, start, compatible);
}
static int guarded_path_offset(const void *blob, const char *path)
{
    if (lookup_fault && !strcmp(lookup_fault, "alias-lookup") &&
        !strcmp(path, "dcpext"))
        return -FDT_ERR_BADSTRUCTURE;
    return fdt_path_offset(blob, path);
}
static int guarded_setprop_string(void *blob, int offset, const char *name,
                                  const char *value)
{
    char path[256];
    if (failed_path && !strcmp(name, "status") && !strcmp(value, "disabled") &&
        !fdt_get_path(blob, offset, path, sizeof(path)) && !strcmp(path, failed_path))
        return -FDT_ERR_NOSPACE;
    return fdt_setprop_string(blob, offset, name, value);
}
#undef fdt_setprop_string
#define fdt_setprop_string guarded_setprop_string
#define fdt_subnode_offset guarded_subnode_offset
#define fdt_next_subnode guarded_next_subnode
#define fdt_node_offset_by_compatible guarded_compatible
#define fdt_path_offset guarded_path_offset
''' + iommu + helpers + r'''
static int node(const char *path)
{
    int offset = fdt_path_offset(dt, path);
    assert(offset >= 0);
    return offset;
}
static void add(const char *parent, const char *name, u32 phandle)
{
    int offset = fdt_add_subnode(dt, node(parent), name);
    assert(offset >= 0);
    if (phandle)
        assert(!fdt_setprop_u32(dt, offset, "phandle", phandle));
}
static void string(const char *path, const char *key, const char *value)
{
    assert(!fdt_setprop_string(dt, node(path), key, value));
}
static void cell(const char *path, const char *key, u32 value)
{
    assert(!fdt_setprop_u32(dt, node(path), key, value));
}
static void iommu(const char *path, u32 provider, u32 stream)
{
    fdt32_t tuple[] = {cpu_to_fdt32(provider), cpu_to_fdt32(stream)};
    assert(!fdt_setprop(dt, node(path), "iommus", tuple, sizeof(tuple)));
}
static void status(const char *path, const char *expected)
{
    const char *actual = fdt_getprop(dt, node(path), "status", NULL);
    if (!actual || strcmp(actual, expected)) {
        fprintf(stderr, "%s status: expected %s, got %s\n", path, expected,
                actual ? actual : "absent");
        exit(1);
    }
}
static void setup(void)
{
    memset(tree, 0, sizeof(tree));
    assert(!fdt_create_empty_tree(dt, sizeof(tree)));
    add("/", "aliases", 0);
    add("/", "chosen", 0);
    add("/chosen", "framebuffer", 0);
    cell("/chosen", "apple,dcp-rtkit-quiesced", 1);
    add("/", "soc", 0);
    add("/soc", "dcp", 1);
    string("/soc/dcp", "compatible", "apple,t8140-dcp");
    string("/soc/dcp", "status", "okay");
    add("/soc", "display", 2);
    string("/soc/display", "compatible", "apple,display-subsystem");
    string("/soc/display", "status", "okay");
    string("/aliases", "dcp", "/soc/dcp");
    string("/aliases", "disp0", "/soc/display");
    /* Public J700 uses these two aliases, with no external resource aliases. */
    add("/soc", "dcp-dart", 3);
    add("/soc", "disp-dart", 4);
    add("/soc", "external-dcp-dart", 5);
    add("/soc", "external-disp-dart", 6);
    const char *darts[] = {"/soc/dcp-dart", "/soc/disp-dart",
                          "/soc/external-dcp-dart", "/soc/external-disp-dart"};
    for (unsigned i = 0; i < ARRAY_SIZE(darts); i++) {
        cell(darts[i], "#iommu-cells", 1);
        string(darts[i], "status", i < 2 ? "okay" : "disabled");
    }
    iommu("/soc/dcp", 3, 23);
    fdt32_t masters[] = {cpu_to_fdt32(4), cpu_to_fdt32(0),
                         cpu_to_fdt32(6), cpu_to_fdt32(0)};
    assert(!fdt_setprop(dt, node("/soc/display"), "iommus", masters, sizeof(masters)));
    add("/soc", "external-mbox", 7);
    cell("/soc/external-mbox", "#mbox-cells", 0);
    string("/soc/external-mbox", "status", "disabled");
    add("/soc", "external", 8);
    string("/soc/external", "compatible", "apple,t8140-dcpext");
    string("/soc/external", "status", "disabled");
    iommu("/soc/external", 5, 23);
    cell("/soc/external", "mboxes", 7);
    add("/soc/external", "piodma", 0);
    iommu("/soc/external/piodma", 6, 16);
    add("/soc/external", "typec-routes", 0);
    add("/soc/external/typec-routes", "route@0", 0);
    string("/soc/external/typec-routes/route@0", "status", "okay");
    const char *powers[] = {"power-sys", "power-fe", "power-cpu"};
    for (unsigned i = 0; i < ARRAY_SIZE(powers); i++) {
        add("/soc", powers[i], 10 + i);
        char path[64];
        snprintf(path, sizeof(path), "/soc/%s", powers[i]);
        cell(path, "#power-domain-cells", 0);
        string(path, "status", "disabled");
        if (i)
            cell(path, "power-domains", 9 + i);
    }
    cell("/soc/external", "power-domains", 12);
    cell("/soc/external-mbox", "power-domains", 12);
    assert(!fdt_setprop(dt, node("/soc/power-cpu"), "apple,always-on", NULL, 0));
    assert(!dt_t8140_enable_external("/soc/external"));
    status("/soc/external-disp-dart", "okay");
    status("/soc/power-cpu", "okay");
    status("/soc/power-fe", "okay");
    status("/soc/power-sys", "okay");
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    setup();
    if (!strcmp(argv[1], "external")) {
        assert(!dt_t8140_disable_dcpext());
        status("/soc/dcp", "okay");
        status("/soc/display", "okay");
        status("/soc/external", "disabled");
        status("/soc/external-dcp-dart", "disabled");
        status("/soc/external-mbox", "disabled");
        status("/soc/external/typec-routes/route@0", "disabled");
        status("/soc/power-cpu", "disabled");
        int length;
        const fdt32_t *refs = fdt_getprop(dt, node("/soc/display"), "iommus", &length);
        assert(refs && length == 8 && fdt32_to_cpu(refs[0]) == 4);
        assert(fdt_getprop(dt, node("/chosen"), "apple,dcp-rtkit-quiesced", NULL));
        status("/soc/external-disp-dart", "disabled");
    } else if (!strcmp(argv[1], "scanout-write-failure")) {
        failed_path = "/soc/external-disp-dart";
        assert(dt_t8140_disable_dcpext() < 0);
        status("/soc/external-disp-dart", "okay");
        assert(node("/chosen/framebuffer") >= 0);
    } else if (!strcmp(argv[1], "whole-power-write-failure")) {
        failed_path = "/soc/power-cpu";
        assert(dt_t8140_refuse_display() < 0);
        status("/soc/power-cpu", "okay");
        assert(node("/chosen/framebuffer") >= 0);
    } else if (!strcmp(argv[1], "malformed-piodma")) {
        cell("/soc/external-disp-dart", "#iommu-cells", 2);
        unsigned char before[sizeof(tree)];
        memcpy(before, tree, sizeof(tree));
        assert(dt_t8140_disable_dcpext() < 0);
        assert(!memcmp(before, tree, sizeof(tree)));
    } else if (!strncmp(argv[1], "write:", 6)) {
        failed_path = argv[1] + 6;
        assert(dt_t8140_disable_dcpext() < 0);
        assert(node("/chosen/framebuffer") >= 0);
    } else if (!strcmp(argv[1], "long-route")) {
        /* Both names are legal FDT names. The 31-byte name is the prefix of
         * the longer one, so silent truncation can disable the wrong route. */
        const char *short_name = "route01234567890123456789012345";
        const char *long_name = "route01234567890123456789012345x";
        assert(strlen(short_name) == 31);
        add("/soc/external/typec-routes", short_name, 0);
        add("/soc/external/typec-routes", long_name, 0);
        string("/soc/external/typec-routes/route01234567890123456789012345", "status", "okay");
        string("/soc/external/typec-routes/route01234567890123456789012345x", "status", "okay");
        assert(dt_t8140_disable_dcpext() < 0);
        status("/soc/external/typec-routes/route01234567890123456789012345x", "okay");
    } else if (!strcmp(argv[1], "route-lookup") ||
               !strcmp(argv[1], "route-iteration") ||
               !strcmp(argv[1], "external-lookup") ||
               !strcmp(argv[1], "alias-lookup")) {
        lookup_fault = argv[1];
        assert(dt_t8140_disable_dcpext() < 0);
        assert(node("/chosen/framebuffer") >= 0);
    } else if (!strcmp(argv[1], "whole-iteration")) {
        lookup_fault = argv[1];
        assert(dt_t8140_refuse_display() < 0);
        assert(node("/chosen/framebuffer") >= 0);
    } else {
        assert(!strcmp(argv[1], "whole"));
        assert(!dt_t8140_refuse_display());
        status("/soc/dcp", "disabled");
        status("/soc/display", "disabled");
        status("/soc/external", "disabled");
        status("/soc/external-dcp-dart", "disabled");
        status("/soc/external-disp-dart", "disabled");
        status("/soc/external-mbox", "disabled");
        status("/soc/external/typec-routes/route@0", "disabled");
        assert(!fdt_getprop(dt, node("/chosen"), "apple,dcp-rtkit-quiesced", NULL));
        assert(node("/chosen/framebuffer") >= 0);
        status("/soc/power-cpu", "disabled");
        status("/soc/power-fe", "disabled");
        status("/soc/power-sys", "disabled");
    }
    assert(!fdt_check_header(dt));
    return 0;
}
'''
    c = tmp_path / "refusal.c"
    c.write_text(harness)
    lib = ROOT / "src/libfdt"
    sources = [lib / name for name in (
        "fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_sw.c", "fdt_wip.c",
        "fdt_empty_tree.c", "fdt_check.c",
    )]
    binary = tmp_path / "refusal"
    compiler = shlex.split(os.environ.get("CC", "cc"))
    subprocess.run(compiler + ["-D_GNU_SOURCE", "-std=c11", "-Wall", "-Wextra", "-Werror",
                              "-I", str(lib), str(c), *map(str, sources),
                              "-o", str(binary)], check=True)
    return binary


def test_external_refusal_withdraws_scanout_provider_without_alias(refusal_binary):
    subprocess.run([str(refusal_binary), "external"], check=True)


def test_whole_refusal_withdraws_published_external_power_chain(refusal_binary):
    subprocess.run([str(refusal_binary), "whole"], check=True)


def test_external_refusal_rejects_scanout_provider_write_failure(refusal_binary):
    subprocess.run([str(refusal_binary), "scanout-write-failure"], check=True)


def test_external_refusal_rejects_malformed_piodma_tuple_without_edits(refusal_binary):
    subprocess.run([str(refusal_binary), "malformed-piodma"], check=True)


def test_whole_refusal_rejects_power_supplier_write_failure(refusal_binary):
    subprocess.run([str(refusal_binary), "whole-power-write-failure"], check=True)


@pytest.mark.parametrize("scenario", [
    "long-route", "route-lookup", "route-iteration", "external-lookup",
    "alias-lookup", "whole-iteration",
])
def test_refusal_rejects_incomplete_topology(refusal_binary, scenario):
    subprocess.run([str(refusal_binary), scenario], check=True)


@pytest.mark.parametrize("path", [
    "/soc/external-dcp-dart", "/soc/external-disp-dart", "/soc/external-mbox",
    "/soc/external", "/soc/external/typec-routes/route@0",
    "/soc/power-cpu", "/soc/power-fe", "/soc/power-sys",
])
def test_external_refusal_rejects_every_supplier_and_route_write_failure(refusal_binary, path):
    subprocess.run([str(refusal_binary), "write:" + path], check=True)
