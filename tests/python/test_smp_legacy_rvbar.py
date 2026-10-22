# SPDX-License-Identifier: MIT
"""Preserve legacy reset-vector permission without relaxing T8140 ownership.

MMIO is modeled; the admission helper and both callers are production bodies.
"""
import re

from test_smp_timeouts import REPO, compile_and_run


def function(source, name):
    match = re.search(r"^(?:static )?(?:bool|int) " + name + r"\([^;]*?\)\s*\{", source, re.M | re.S)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def test_legacy_rvbar_lock_does_not_prune_cpus(tmp_path):
    source = (REPO / "src/smp.c").read_text()
    bodies = "\n".join(function(source, name) for name in (
        "smp_prepare_rvbar", "smp_start_cpu", "smp_start_secondaries"))
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned long u64;
typedef uint32_t u32;
typedef uint8_t u8;
_Static_assert(sizeof(u64) == 8, "production u64 ABI");
#define MAX_CPUS 32
#define MAX_EL3_CPUS 32
#define SECONDARY_STACK_SIZE 64
#define DUMMY_STACK_SIZE 64
#define BIT(n) (1ULL << (n))
#define RVBAR_LOCK 1
#define RVBAR_ADDR 0x0000fffffffff000ULL
#define PMGR_DIE_OFFSET 0x100000
#define T8103 0x8103
#define T8140 0x8140
#define T8142 0x8142
#define T8152 0x8152
#define MAIR_IDX_NORMAL 0
#define PERM_RW 0
#define PERM_RX 1
#define FIELD_GET(mask, x) (!!((x) & (mask)))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define sysop(x)                                                               \
  do {                                                                         \
  } while (0)
static int chip_id, boot_cpu_idx, target_cpu;
static bool smp_initialized = true;
static u64 installed_relay, relocated_from, smp_start_fail_mask,
    cpu_start_base = 0x1000;
static unsigned write64_count, write32_count;
static unsigned char _vectors_start[4096] __attribute__((aligned(4096)));
/* aura computes the relocated reset vector from the image base inline. */
static unsigned char _base[1];
static u64 rvbars[MAX_CPUS];
struct cpu_info {
  bool valid;
  u8 die, cluster, core;
  u64 impl_reg;
};
static struct cpu_info cpu_info[MAX_CPUS];
static int cpu_nodes[MAX_CPUS];
static struct spin_table {
  u64 mpidr, flag, target, args[4], retval;
} spin_table[MAX_CPUS];
static u8 secondary_stacks[MAX_CPUS][SECONDARY_STACK_SIZE],
    secondary_stacks_el3[MAX_CPUS][SECONDARY_STACK_SIZE];
static u8 dummy_stack[DUMMY_STACK_SIZE], dummy_stack_el1[DUMMY_STACK_SIZE];
static void *_reset_stack, *_reset_stack_el1;
static struct {
  bool apple_sysregs_unlocked, unsafe_wfi;
} feature_data, *cpu_features = &feature_data;
static u64 read64(u64 a) {
  assert(a < MAX_CPUS);
  return rvbars[a];
}
static void write64(u64 a, u64 v) {
  assert(a < MAX_CPUS);
  rvbars[a] = v;
  write64_count++;
}
static u32 read32(u64 a) {
  (void)a;
  return 0;
}
static void write32(u64 a, u32 v) {
  (void)v;
  write32_count++;
  if (a >= cpu_start_base + 8 && a <= cpu_start_base + 12)
    spin_table[target_cpu].flag = 1;
}
static bool has_el3(void) { return false; }
static bool smp_old_vector_owned(u64 a) {
  (void)a;
  return false;
}
static u64 old_vectors_start(void) { return 0; }
static u64 get_page_size(void) { return 4096; }
static bool mmu_active(void) { return false; }
static void mmu_add_mapping(u64 a, u64 b, u64 c, int d, int e) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  (void)e;
}
static void dc_cvac_range(void *a, u64 n) {
  (void)a;
  (void)n;
}
static void ic_ivau_range(void *a, u64 n) {
  (void)a;
  (void)n;
}
static bool t8142_prepare_reset_vector(u64 a) {
  (void)a;
  return false;
}
static u32 cpu_start_bit(int i, const struct cpu_info *cpu) {
  (void)i;
  return BIT(4 * cpu->cluster + cpu->core);
}
static void udelay(int t) { (void)t; }
static void smp_start_t8152(void) {}
static void smp_set_wfe_mode(bool a) { (void)a; }
''' + bodies + r'''
static void setup(int chip, bool unlocked) {
  chip_id = chip;
  cpu_features->apple_sysregs_unlocked = unlocked;
  installed_relay = relocated_from = 0;
  write64_count = write32_count = 0;
  memset(spin_table, 0, sizeof(spin_table));
  memset(cpu_info, 0, sizeof(cpu_info));
  for (int i = 0; i < 8; i++) {
    cpu_info[i] = (struct cpu_info){true, 0, i / 4, i % 4, i};
    rvbars[i] = 0x800000001ULL;
  }
}
int main(void) {
  const int chips[] = {T8103, 0x8112, 0x6030};
  const int boot_indices[] = {0, 4};
  for (unsigned c = 0; c < sizeof(chips) / sizeof(chips[0]); c++) {
    for (unsigned b = 0; b < sizeof(boot_indices) / sizeof(boot_indices[0]);
         b++) {
      for (unsigned lock = 0; lock <= 1; lock++) {
        setup(chips[c], true);
        boot_cpu_idx = boot_indices[b];
        spin_table[boot_cpu_idx].flag = 1;
        for (int i = 0; i < 8; i++)
          rvbars[i] = 0x800000000ULL | lock;
        /* Air failure shape: CPU0's vector is current; loader is CPU4. */
        rvbars[0] = (u64)_vectors_start;
        assert(smp_start_secondaries() == 0);
        unsigned alive = 0;
        for (int i = 0; i < 8; i++) {
          if (spin_table[i].flag)
            alive |= 1u << i;
          if (i != boot_cpu_idx || !lock)
            assert((rvbars[i] & RVBAR_ADDR) == (u64)_vectors_start);
        }
        assert(alive == 0xff);
        /* Only established caller-side writes: seven secondary RVBARs,
         * plus boot RVBAR when its hardware lock bit is clear. */
        assert(write64_count == (lock && boot_cpu_idx == 4 ? 7 : 8));
        assert(write32_count == 14);
      }
    }
  }
  for (unsigned c = 0; c < sizeof(chips) / sizeof(chips[0]); c++) {
    for (unsigned lock = 0; lock <= 1; lock++) {
      setup(chips[c], false);
      rvbars[1] = 0x800000000ULL | lock;
      assert(smp_prepare_rvbar(&cpu_info[1]) == -1);
      assert(!write64_count && !write32_count);
      rvbars[1] = (u64)_vectors_start | lock;
      assert(smp_prepare_rvbar(&cpu_info[1]) == 0);
      assert(!write64_count && !write32_count);
    }
  }
  /* Absent architectural permission, failed T8140 ownership still refuses
   * any stale vector without performing a relay or CPU-start write. */
  for (unsigned lock = 0; lock <= 1; lock++) {
    setup(T8140, false);
    rvbars[1] = 0x800000000ULL | lock;
    assert(smp_prepare_rvbar(&cpu_info[1]) == -1);
    assert(!write64_count && !write32_count);
  }
  puts("legacy RVBAR matrix PASS");
  return 0;
}
'''
    compile_and_run(tmp_path, "smp-legacy-rvbar", harness)
