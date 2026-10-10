"""Check inherited-session power messages and exact acknowledgements."""
from pathlib import Path
import re
import subprocess


def function(source, name):
    match = re.search(r"(?:static )?bool " + name + r"\(", source)
    brace = source.index("{", match.end())
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def test_inherited_quiesce(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/rtkit.c").read_text()
    defines = "\n".join(line for line in source.splitlines() if line.startswith("#define MGMT_") or line.startswith("#define RTKIT_EP_MGMT"))
    start = source.index("enum rtkit_power_state {")
    enum = source[start:source.index("};", start) + 2]
    bodies = "\n".join(function(source, name) for name in ("rtkit_inherited_quiesce_state", "rtkit_quiesce_inherited"))
    harness = r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32; typedef uint64_t u64;
#define BIT(n) (1ULL<<(n))
#define GENMASK(h,l) (((~0ULL)>>(63-(h)))&((~0ULL)<<(l)))
#define FIELD_PREP(mask,val) (((u64)(val)<<__builtin_ctzll(mask))&(mask))
typedef struct {int unused;} asc_dev_t;
struct asc_message {u64 msg0; u32 msg1;};
static asc_dev_t asc;
static int running,queued,sends,receives,fail_send,fail_receive,wrong_type,wrong_state,wrong_endpoint,reserved,stop_after_ack,extra_after_ack;
static bool asc_cpu_running(asc_dev_t *a){assert(a==&asc);return running;}
static bool asc_can_recv(asc_dev_t *a){assert(a==&asc);return queued;}
static bool asc_send(asc_dev_t *a,const struct asc_message *m){
 assert(a==&asc);sends++;
 assert(m->msg1==0 && m->msg0==(((u64)(sends==1?0xb:6)<<52)|0x10));
 return sends!=fail_send;
}
static bool asc_recv_timeout(asc_dev_t *a,struct asc_message *m,u32 us){
 assert(a==&asc && us==1000000);receives++;
 if(receives==fail_receive)return false;
 m->msg0=((u64)(receives==1?0xb:7)<<52)|0x10;
 m->msg1=0;
 if(receives==wrong_type)m->msg0^=1ULL<<52;
 if(receives==wrong_state)m->msg0^=1;
 if(receives==wrong_endpoint)m->msg1=2;
 if(receives==reserved)m->msg0|=1ULL<<20;
 if(receives==stop_after_ack)running=0;
 if(receives==extra_after_ack)queued=1;
 return true;
}
static void reset(void){running=1;queued=sends=receives=fail_send=fail_receive=wrong_type=wrong_state=wrong_endpoint=reserved=stop_after_ack=extra_after_ack=0;}
''' + defines + "\n" + enum + "\n" + bodies + r'''
int main(void){
 reset();assert(rtkit_quiesce_inherited(&asc));assert(sends==2&&receives==2&&running);puts("AP/IOP QUIESCED exact ACKs PASS; CPU RUN retained");
 reset();assert(!rtkit_quiesce_inherited(NULL));assert(sends==0);
 reset();running=0;assert(!rtkit_quiesce_inherited(&asc));assert(sends==0);
 reset();queued=1;assert(!rtkit_quiesce_inherited(&asc));assert(sends==0);
 for(int stage=1;stage<=2;stage++){
  reset();fail_send=stage;assert(!rtkit_quiesce_inherited(&asc));assert(sends==stage&&receives==stage-1);
  reset();fail_receive=stage;assert(!rtkit_quiesce_inherited(&asc));assert(sends==stage);
  reset();wrong_type=stage;assert(!rtkit_quiesce_inherited(&asc));assert(sends==stage);
  reset();wrong_state=stage;assert(!rtkit_quiesce_inherited(&asc));assert(sends==stage);
  reset();wrong_endpoint=stage;assert(!rtkit_quiesce_inherited(&asc));assert(sends==stage);
  reset();reserved=stage;assert(!rtkit_quiesce_inherited(&asc));assert(sends==stage);
  reset();stop_after_ack=stage;assert(!rtkit_quiesce_inherited(&asc));assert(sends==stage);
 }
 reset();extra_after_ack=2;assert(!rtkit_quiesce_inherited(&asc));assert(sends==2);
 puts("PASS: preconditions, send/ACK failure, wrong type/state/endpoint, extra bits, CPU stop, residual queue refused");return 0;
}
'''
    c = tmp_path / "inherited-quiesce.c"
    binary = tmp_path / "inherited-quiesce"
    c.write_text(harness)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
