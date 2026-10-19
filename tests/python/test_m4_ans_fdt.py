# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Only enable ANS placeholder nodes once their live resources are complete."""
from pathlib import Path
import subprocess


def test_ans_fdt_admission(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    text = (repo / "src/kboot.c").read_text()
    def function(name):
        start = text.index("static int " + name + "(")
        opening = text.index("{", start)
        depth = 1
        end = opening + 1
        while depth:
            depth += (text[end] == "{") - (text[end] == "}")
            end += 1
        return text[start:end]
    production = "\n".join(function(name) for name in
                           ["dt_set_adt_regs", "dt_set_adt_irqs", "dt_set_ans_nvme"])
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
typedef uint64_t u64;
typedef uint32_t u32;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
static void *dt, *adt;
static int fault;
static const u32 irqs[] = {70, 71, 72, 73, 74};
static int adt_path_offset_trace(void *a, const char *p, int *path)
{ path[0] = strstr(p, "sart") ? 2 : 1; return path[0]; }
static void *adt_get_property(void *a, int node, const char *p) { return (void *)1; }
static int adt_get_reg(void *a, int *path, const char *p, int index, u64 *base, u64 *size)
{
 if (fault == 1 && path[0] == 2) return -1;
 *base = path[0] == 2 ? 0x485c50000ULL : index == 0 ? 0x481600000ULL :
         index == 3 ? 0x485cc0000ULL : 0x4c5cc0000ULL;
 *size = index == 9 ? 0x10000 : 0x60000;
 return 0;
}
static const void *adt_getprop(void *a, int n, const char *p, u32 *len)
{ *len = fault == 2 ? 16 : sizeof(irqs); return irqs; }
''' + production + r'''
static void setup(void *blob)
{
 dt = blob;
 assert(!fdt_create_empty_tree(dt, 8192));
 const char *aliases[] = {"ans", "ans-mbox", "sart-ans"};
 for (unsigned i = 0; i < 3; i++) {
  int n = fdt_add_subnode(dt, 0, aliases[i]);
  assert(n >= 0 && !fdt_setprop_string(dt, n, "status", "disabled"));
 }
 int n = fdt_add_subnode(dt, 0, "aliases");
 for (unsigned i = 0; i < 3; i++) {
  char path[32]; snprintf(path, sizeof(path), "/%s", aliases[i]);
  assert(!fdt_setprop_string(dt, n, aliases[i], path));
 }
}
int main(void)
{
 char blob[8192];
 for (fault = 0; fault < 3; fault++) {
  setup(blob);
  assert((dt_set_ans_nvme(false) < 0) == (fault != 0));
  const char *aliases[] = {"ans", "ans-mbox", "sart-ans"};
  for (unsigned i = 0; i < 3; i++) {
   int n = fdt_path_offset(dt, aliases[i]);
   const char *status = fdt_getprop(dt, n, "status", NULL);
   assert(status && !strcmp(status, fault ? "disabled" : "okay"));
  }
  if (!fault) {
   int len;
   const fdt32_t *v = fdt_getprop(dt, fdt_path_offset(dt, "ans-mbox"), "interrupts", &len);
   assert(v && len == 48);
   assert(fdt32_to_cpu(v[1]) == 71 && fdt32_to_cpu(v[4]) == 70);
   assert(fdt32_to_cpu(v[7]) == 73 && fdt32_to_cpu(v[10]) == 72);
   const fdt64_t *reg = fdt_getprop(dt, fdt_path_offset(dt, "ans"), "reg", &len);
   assert(reg && len == 48 && fdt64_to_cpu(reg[1]) == 0x40000);
  }
 }
 return 0;
}
'''
    source = tmp_path / "ans-fdt.c"
    source.write_text(harness)
    binary = tmp_path / "ans-fdt"
    libfdt = repo / "src/libfdt"
    sources = [libfdt / (n + ".c") for n in ("fdt", "fdt_ro", "fdt_rw", "fdt_sw",
               "fdt_wip", "fdt_empty_tree", "fdt_check", "fdt_strerror", "fdt_addresses")]
    subprocess.run(["cc", "-D_GNU_SOURCE", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-format", "-I", str(libfdt), str(source),
                    *map(str, sources), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
