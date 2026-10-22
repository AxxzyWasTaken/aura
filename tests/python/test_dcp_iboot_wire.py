# SPDX-License-Identifier: MIT
"""Compile production command handling against a fake EPIC transport."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def test_command_wire_and_response_bounds(tmp_path):
    source = (ROOT / "src/dcp_iboot.c").read_text()
    header = (ROOT / "src/dcp_iboot.h").read_text()
    types = header[header.index("typedef struct {\n    u32 valid;"):header.index("dcp_iboot_if_t *dcp_ib_init")]
    structures = source[source.index("#define TXBUF_LEN"):source.index("void dcp_ib_service_init")]
    commands = source[source.index("static int dcp_ib_cmd"):source.index("int dcp_ib_swap_begin")]
    prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef void dcp_dev_t;
typedef void afk_epic_ep_t;
typedef struct dcp_iboot_if dcp_iboot_if_t;
#define PACKED __attribute__((packed))
'''
    transport = r'''
static dcp_iboot_if_t instance;
static bool v2;
static unsigned expected_op, reply_count;
static size_t expected_size, reply_size=8, reply_len=8;
static int transport_error;
static bool afk_epic_is_v2(afk_epic_ep_t *epic) { (void)epic;return v2; }
static int afk_epic_command(afk_epic_ep_t *epic,int channel,int type,
                           void *tx,size_t txsize,void *rx,size_t *rxsize) {
 (void)epic;assert(channel==7 && type==0xc0);
 assert(tx==instance.txbuf && txsize==16+expected_size);
 assert(instance.txcmd.op==expected_op && instance.txcmd.len==txsize);
 assert(instance.txcmd.unk1==0 && instance.txcmd.unk2==0);
 bool status_only=v2 && (expected_op==IBOOT_SET_POWER || expected_op==IBOOT_SET_MODE || expected_op==IBOOT_SET_SURFACE);
 if(status_only) { assert(!rx && !rxsize);return transport_error; }
 assert(rx==instance.rxbuf && rxsize && *rxsize==RXBUF_LEN);
 if(transport_error) return transport_error;
 memset(rx,0,RXBUF_LEN);instance.rxcmd.op=expected_op;instance.rxcmd.len=reply_len;
 memcpy(instance.rxbuf+8,&reply_count,sizeof(reply_count));
 *rxsize=reply_size;return 0;
}
'''
    checks = r'''
int main(void) {
 _Static_assert(sizeof(struct txcmd)==16,"public request header ABI");
 _Static_assert(sizeof(struct rxcmd)==8,"public response header ABI");
 _Static_assert(offsetof(struct dcp_iboot_if,rxbuf)-offsetof(struct dcp_iboot_if,txbuf)==TXBUF_LEN,"separate transport buffers");
 instance.channel=7;expected_op=IBOOT_SWAP_END;expected_size=TXBUF_LEN-16;
 memset(instance.txbuf+16,0xa5,expected_size);
 assert(!dcp_ib_cmd(&instance,expected_op,expected_size));
 assert(instance.txbuf[16]==0xa5 && instance.txbuf[TXBUF_LEN-1]==0xa5);
 expected_size=0;reply_size=7;assert(dcp_ib_cmd(&instance,expected_op,0)<0);
 reply_size=8;reply_len=7;assert(dcp_ib_cmd(&instance,expected_op,0)<0);
 reply_len=9;assert(dcp_ib_cmd(&instance,expected_op,0)<0);
 reply_len=8;transport_error=-37;assert(dcp_ib_cmd(&instance,expected_op,0)==-37);transport_error=0;
 expected_op=IBOOT_GET_TIMING_MODES;reply_size=reply_len=12;reply_count=1;
 assert(dcp_ib_cmd(&instance,expected_op,0)<0);
 reply_size=reply_len=12+sizeof(dcp_timing_mode_t);dcp_timing_mode_t *modes=NULL;
 assert(dcp_ib_get_timing_modes(&instance,&modes)==1);
 assert((u8 *)modes==instance.rxbuf+12);
 reply_count=UINT32_MAX;assert(dcp_ib_cmd(&instance,expected_op,0)<0);
 expected_op=IBOOT_GET_COLOR_MODES;reply_count=1;reply_size=reply_len=12;
 assert(dcp_ib_cmd(&instance,expected_op,0)<0);
 reply_size=reply_len=12+sizeof(dcp_color_mode_t);dcp_color_mode_t *colors=NULL;
 assert(dcp_ib_get_color_modes(&instance,&colors)==1);
 assert((u8 *)colors==instance.rxbuf+12);
 expected_op=IBOOT_GET_HPD;reply_size=reply_len=8;assert(dcp_ib_cmd(&instance,expected_op,0)<0);
 reply_size=reply_len=8+sizeof(struct get_hpd_resp);assert(!dcp_ib_cmd(&instance,expected_op,0));
 v2=true;expected_op=IBOOT_SET_POWER;expected_size=1;assert(!dcp_ib_set_power(&instance,true));
 assert(instance.txbuf[16]==1);
 dcp_layer_t layer={.width=1234,.height=567};
 expected_op=IBOOT_SET_SURFACE;expected_size=sizeof(layer);assert(!dcp_ib_set_surface(&instance,&layer));
 assert(!memcmp(instance.txbuf+16,&layer,sizeof(layer)));
 puts("wire sizes, payload boundaries, mode pointers, response guards and V2 status-only setters PASS");
 return 0;
}
'''
    c = tmp_path / "wire.c"
    c.write_text(prefix + types + structures + transport + commands + checks)
    binary = tmp_path / "wire"
    subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
