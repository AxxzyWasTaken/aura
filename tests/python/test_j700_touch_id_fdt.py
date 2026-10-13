"""Exercise the production hardware-presence fixup against real libfdt trees."""
from pathlib import Path
import subprocess


def test_j700_touch_id_presence(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/kboot.c").read_text()
    start = source.index("static int dt_disable_missing_j700_touch_id(")
    end = source.index("static int dt_disable_missing_devs(", start)
    harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
static void *dt, *adt;
static int arm_io_present = 1, spi2_present;
static int adt_path_offset(void *blob, const char *path)
{
    (void)blob;
    if (!strcmp(path, "/arm-io")) return arm_io_present ? 1 : -1;
    assert(!strcmp(path, "/arm-io/spi2"));
    return spi2_present ? 2 : -1;
}
''' + source[start:end] + r'''
static void setup(unsigned char *blob, int size, const char *machine)
{
    dt = blob;
    assert(!fdt_create_empty_tree(dt, size));
    assert(!fdt_setprop_string(dt, 0, "compatible", machine));
    int node = fdt_add_subnode(dt, 0, "spi@385108000");
    assert(node >= 0);
    assert(!fdt_setprop_string(dt, node, "compatible", "apple,t8140-spi"));
    assert(!fdt_setprop_string(dt, node, "status", "okay"));
    int child = fdt_add_subnode(dt, node, "fingerprint@0");
    assert(child >= 0);
    assert(!fdt_setprop_string(dt, child, "compatible", "apple,mesa-fingerprint"));
    node = fdt_add_subnode(dt, 0, "spi@other");
    assert(node >= 0);
    assert(!fdt_setprop_string(dt, node, "status", "okay"));
}
static void expect(const char *path, const char *status)
{
    int node = fdt_path_offset(dt, path);
    const char *value = fdt_getprop(dt, node, "status", NULL);
    assert(value && !strcmp(value, status));
}
int main(void)
{
    unsigned char blob[4096];
    setup(blob, sizeof(blob), "apple,j700");
    assert(!dt_disable_missing_j700_touch_id());
    expect("/spi@385108000", "disabled");
    expect("/spi@other", "okay");
    spi2_present = 1;
    setup(blob, sizeof(blob), "apple,j700");
    assert(!dt_disable_missing_j700_touch_id());
    expect("/spi@385108000", "okay");
    spi2_present = 0;
    setup(blob, sizeof(blob), "apple,j414s");
    assert(!dt_disable_missing_j700_touch_id());
    expect("/spi@385108000", "okay");
    setup(blob, sizeof(blob), "apple,j700");
    arm_io_present = 0;
    assert(dt_disable_missing_j700_touch_id() == -1);
    expect("/spi@385108000", "okay");
    arm_io_present = 1;
    assert(!fdt_del_node(dt, fdt_path_offset(dt, "/spi@385108000")));
    assert(!dt_disable_missing_j700_touch_id());
    assert(!fdt_check_header(dt));
    return 0;
}
'''
    libfdt = repo / "src/libfdt"
    sources = [libfdt / n for n in (
        "fdt.c", "fdt_addresses.c", "fdt_check.c", "fdt_empty_tree.c",
        "fdt_ro.c", "fdt_rw.c", "fdt_strerror.c", "fdt_sw.c", "fdt_wip.c")]
    binary = tmp_path / "j700-touch-id"
    subprocess.run(["cc", "-D_GNU_SOURCE", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(libfdt), "-x", "c", "-", *map(str, sources), "-o", str(binary)],
                   input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)
