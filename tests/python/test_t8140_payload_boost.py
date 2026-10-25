"""Check that payload loading restores T8140's inherited P-state."""
from pathlib import Path
import subprocess


def test_t8140_payload_boost(tmp_path):
    source = (Path(__file__).resolve().parents[2] / 'src/cpufreq.c').read_text()
    start = source.index('#define T8140_PAYLOAD_BOOST_PSTATE')
    end = source.index('int cpufreq_init(void)', start)
    helper = source[start:end]
    program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
#define T8140 0x8140
#define CLUSTER_PSTATE 0x20
struct cluster_t {const char *name; uint64_t base; bool performance; unsigned a,b,c;};
static unsigned chip_id, calls, last_state, inherited = 5;
static bool fail;
static uint64_t read64(uint64_t addr) {assert(addr == 0x211e00020ULL);return inherited;}
static unsigned pstate_reg_to_pstate(uint64_t value) {return value;}
static int set_pstate(const struct cluster_t *c, unsigned state) {
 assert(c->base == 0x211e00000ULL);calls++;last_state=state;return fail ? -1 : 0;
}
''' + helper + r'''
int main(void) {
 chip_id=0x8122;cpufreq_payload_boost(true);assert(!calls);
 chip_id=T8140;cpufreq_payload_boost(true);assert(calls==1 && last_state==8);
 cpufreq_payload_boost(true);assert(calls==1);
 cpufreq_payload_boost(false);assert(calls==2 && last_state==5);
 cpufreq_payload_boost(false);assert(calls==2);
 inherited=3;fail=true;cpufreq_payload_boost(true);assert(calls==3 && last_state==8);
 fail=false;cpufreq_payload_boost(false);assert(calls==4 && last_state==3);
 puts("T8140 payload boost and inherited-state restoration passed");
}
'''
    file = tmp_path / 'boost.c'
    file.write_text(program)
    binary = tmp_path / 'boost'
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(file), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
