# SPDX-License-Identifier: MIT
import pathlib, subprocess, tempfile, unittest

class ExternalMetadata(unittest.TestCase):
    def test_actual_observer_privacy_and_bounds(self):
        source = (pathlib.Path(__file__).parents[2] / "src/kboot.c").read_text()
        code = source.split("static void dt_t8140_external_metadata(void)", 1)[1].split("static int dt_set_display_t8140(void)", 1)[0]
        prefix = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef uint64_t u64; typedef uint32_t u32; typedef uint8_t u8; typedef uint32_t fdt32_t;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define SZ_1M 1048576u
#define cpu_to_fdt32(x) __builtin_bswap32((u32)(x))
static void *adt = (void *)1, *dt = (void *)2;
static unsigned blob_size = 4096, mode, writes, reads;
static fdt32_t captured[1024]; static size_t captured_size;
static u32 numeric[] = {0x20, 0x30};
static u32 adt_get_size(void) { return blob_size; }
static int fdt_path_offset(void *p, const char *name) { assert(p==dt); assert(!strcmp(name,"/chosen")); return 0; }
static int adt_path_offset(void *p,const char *name) { assert(p==adt); assert(strstr(name,"/arm-io/")==name); return mode==1 ? -1 : 1; }
static int adt_path_offset_trace(void *p,const char *name,int *trace) { trace[0]=1; trace[1]=0; return adt_path_offset(p,name); }
static int adt_get_reg(void *p,int *trace,const char *name,unsigned index,u64 *address,u64 *size) {
 assert(p==adt && trace[0]==1 && !strcmp(name,"reg") && index<4);
 if(mode>=2 || index>=2) return -1;
 *address=0x416300000ULL+index*0x4000; *size=0xc000; return 0;
}
static int adt_first_child_offset(void *p,int node) { assert(p==adt && node==1); return mode==0 ? 2 : -1; }
static int adt_get_child_count(void *p,int node) { assert(p==adt && node==1); return mode==0 ? 70 : 0; }
static int adt_next_sibling_offset(void *p,int node) { assert(p==adt && node>=2 && node<=17); return node+1; }
static int adt_getprop_copy(void *p,int node,const char *name,void *out,size_t len) { assert(p==adt && node>=2 && node<=17 && len==4); assert(!strcmp(name,"reg") || !strcmp(name,"AAPL,phandle")); *(u32 *)out=node; return 0; }
static const void *adt_getprop(void *p,int node,const char *name,u32 *length) {
 assert(p==adt && node==1); reads++;
 assert(strcmp(name,"serial-number") && strcmp(name,"calibration") && strcmp(name,"firmware"));
 if(!strcmp(name,"reg")) { *length=mode==2 ? 129 : mode==3 ? 3 : 8; return mode>=2 ? (void *)1 : numeric; }
 if(!strcmp(name,"segment-ranges")) { *length=128; return (void *)1; }
 *length=0; return NULL;
}
static int fdt_setprop(void *p,int node,const char *name,const void *data,size_t len) {
 assert(p==dt && node==0); assert(len<=sizeof(captured));
 if(!strcmp(name,"asahi,external-display-mappers")) { assert(len==(mode==0 ? 408 : 24)); writes++; return 0; }
 if(!strcmp(name,"asahi,external-display-registers")) {
  const fdt32_t *v=data; assert(__builtin_bswap32(v[0])==1 && __builtin_bswap32(v[1])==7);
  size_t pos=2;
  for(unsigned i=0;i<7;i++) {
   assert(__builtin_bswap32(v[pos++])==i);
   unsigned count=__builtin_bswap32(v[pos++]); assert(count==(mode==0 ? 2 : 0));
   for(unsigned j=0;j<count;j++) { assert(__builtin_bswap32(v[pos])==4); assert(__builtin_bswap32(v[pos+1])==0x16300000+j*0x4000); pos+=4; }
  }
  assert(pos*4==len); writes++; return 0;
 }
 assert(!strcmp(name,"asahi,external-display-metadata"));
 memcpy(captured,data,len); captured_size=len; writes++; return 0;
}
"""
        suffix = r"""
int main(void) {
 for(mode=0;mode<4;mode++) {
  writes=reads=0; dt_t8140_external_metadata(); assert(writes==3 && reads<=112);
  assert(__builtin_bswap32(captured[0])==1 && captured_size<=4096);
  size_t n=2;
  for(unsigned i=0;i<7;i++) {
   assert(__builtin_bswap32(captured[n++])==i);
   unsigned present=__builtin_bswap32(captured[n++]); unsigned props=__builtin_bswap32(captured[n++]);
   assert(present==(mode!=1) && props==16);
   for(unsigned j=0;j<props;j++) {
    assert(__builtin_bswap32(captured[n++])==j); n++; unsigned len=__builtin_bswap32(captured[n++]); unsigned count=__builtin_bswap32(captured[n++]);
    if(j==0 && mode==0) { assert(len==8 && count==2); assert(__builtin_bswap32(captured[n])==0x20); }
    else assert(count==0);
    n+=count;
   }
  }
  assert(n*4==captured_size);
 }
 writes=reads=0; blob_size=17*SZ_1M; dt_t8140_external_metadata(); assert(!writes && !reads);
 puts("selected numeric facts, absent nodes, poison firmware, oversized/misaligned payload and blob bounds PASS");
}
"""
        with tempfile.TemporaryDirectory() as d:
            f=pathlib.Path(d)/"test.c"; f.write_text(prefix+"static void dt_t8140_external_metadata(void)"+code+suffix)
            binary=pathlib.Path(d)/"test"
            subprocess.run(["clang","-std=c11","-Wall","-Wextra","-Werror",str(f),"-o",str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

if __name__=="__main__": unittest.main()
