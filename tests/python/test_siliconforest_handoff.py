# SPDX-License-Identifier: GPL-2.0-or-later
"""Fault injection into the production handoff transaction and Max opt-in."""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def definition(source, name):
    start = source.index("static int " + name + "(")
    end = source.index("\n}", start) + 2
    return source[start:end]


def run_c(tmp_path, name, source):
    binary = tmp_path / name
    lib = ROOT / "src/libfdt"
    subprocess.run(
        ["cc", "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-parameter", "-I", str(lib), "-x", "c", "-",
         *map(str, (lib / f for f in (
             "fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_sw.c", "fdt_wip.c",
             "fdt_empty_tree.c", "fdt_strerror.c"))), "-o", str(binary)],
        input=source, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)


def test_partial_display_publication_is_discarded(tmp_path):
    function = definition((ROOT / "src/kboot.c").read_text(), "dt_t6030_transaction")
    run_c(tmp_path, "transaction", r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
static void *dt;
''' + function + r'''
static int publish(unsigned int fail)
{
    int node = fdt_path_offset(dt, "/controller");
    assert(fdt_setprop_string(dt, node, "status", "okay") == 0);
    assert(fdt_add_subnode(dt, 0, "reserved-firmware") >= 0);
    return fail ? -1 : 0;
}
int main(void)
{
    unsigned char original[2048], before[2048];
    dt = original;
    assert(fdt_create_empty_tree(dt, sizeof(original)) == 0);
    int node = fdt_add_subnode(dt, 0, "controller");
    assert(fdt_setprop_string(dt, node, "status", "disabled") == 0);
    memcpy(before, original, sizeof(original));
    assert(dt_t6030_transaction(publish, 1) == -1);
    assert(dt == original && !memcmp(original, before, sizeof(original)));
    assert(dt_t6030_transaction(publish, 0) == 0);
    assert(dt == original && fdt_check_header(dt) == 0);
    node = fdt_path_offset(dt, "/controller");
    assert(!strcmp(fdt_getprop(dt, node, "status", NULL), "okay"));
    assert(fdt_path_offset(dt, "/reserved-firmware") >= 0);
    return 0;
}
''')


def test_max_probe_requires_exact_opt_in(tmp_path):
    source = (ROOT / "src/kboot_gpu.c").read_text()
    function = definition(source, "dt_set_gpu_t6031")
    start = source.index("static bool g15_optin(")
    optin = source[start:source.index("\n}", start) + 2]
    run_c(tmp_path, "max-intent", r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
enum { T8122_ID_NOT_RUN };
struct t8122_powered_identity { int result; };
struct fw_version_info { int unused; };
static const struct fw_version_info *g15_t6031_fw;
static struct t8122_powered_identity g15_t6031_id;
static const struct { const char *optin; } g15_soc_t6031 = {"asahi,t6031-gpu"};
static int board = 1, reads, probes, setups, disabled;
static bool t6031_board(void *dt) { return board; }
static void g15_disable(void *dt) { disabled++; }
static const struct fw_version_info *dt_t6031_gpu_image(void *dt)
{ reads++; return NULL; }
static void t6031_gpu_probe(void *dt) { probes++; }
static int dt_set_gpu_g15(void *dt, const void *soc) { setups++; return 0; }
''' + optin + function + r'''
int main(void)
{
    unsigned char dt[2048];
    assert(fdt_create_empty_tree(dt, sizeof(dt)) == 0);
    int chosen = fdt_add_subnode(dt, 0, "chosen");
    assert(chosen >= 0);
    assert(dt_set_gpu_t6031(dt) == 0);
    assert(!reads && !probes && !setups && disabled == 1);
    const char *refused[] = {"0", "", "true", "01", "1x"};
    for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        assert(!fdt_setprop_string(dt, chosen, "asahi,t6031-gpu", refused[i]));
        assert(!dt_set_gpu_t6031(dt));
        assert(!reads && !probes && !setups);
    }
    char malformed[] = {'1'};
    assert(!fdt_setprop(dt, chosen, "asahi,t6031-gpu", malformed, sizeof(malformed)));
    assert(!dt_set_gpu_t6031(dt) && !reads && !probes);
    assert(!fdt_setprop_string(dt, chosen, "asahi,t6031-gpu", "1"));
    assert(!dt_set_gpu_t6031(dt));
    assert(reads == 1 && probes == 1 && setups == 1);
    board = 0;
    assert(!dt_set_gpu_t6031(dt));
    assert(reads == 1 && probes == 1 && setups == 1);
    return 0;
}
''')
