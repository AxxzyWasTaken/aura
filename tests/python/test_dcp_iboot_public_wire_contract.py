# SPDX-License-Identifier: MIT
# Generated-by: LLM
# Assisted-by: LLM
"""Compare production command payloads to fixed public iBoot wire offsets."""

import os
from pathlib import Path
import shlex
import subprocess


def test_all_public_command_payloads_and_return_values(tmp_path):
    root = Path(__file__).resolve().parents[2]
    source = (root / "src/dcp_iboot.c").read_text()
    header = (root / "src/dcp_iboot.h").read_text()
    types = header[header.index("typedef struct {\n    u32 valid;"):
                   header.index("dcp_iboot_if_t *dcp_ib_init")]
    structs = source[source.index("#define TXBUF_LEN"):
                     source.index("void dcp_ib_service_init")]
    commands = source[source.index("static int dcp_ib_cmd"):]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef void dcp_dev_t;
typedef void afk_epic_ep_t;
typedef struct dcp_iboot_if dcp_iboot_if_t;
#define PACKED __attribute__((packed))
#define V13_3 133
static struct { int version; } os_firmware;
''' + types + structs + r'''
static bool v2;
static int transport_error;
static unsigned op, payload_len, response_len;
static u8 expected[256], response[64];
static bool afk_epic_is_v2(afk_epic_ep_t *epic) { (void)epic; return v2; }
static void word(u8 *bytes, unsigned offset, u32 value)
{
    bytes[offset] = value;
    bytes[offset + 1] = value >> 8;
    bytes[offset + 2] = value >> 16;
    bytes[offset + 3] = value >> 24;
}
static void prepare(unsigned command, unsigned len, unsigned reply_len)
{
    op = command;
    payload_len = len;
    response_len = reply_len;
    memset(expected, 0, sizeof(expected));
    memset(response, 0, sizeof(response));
}
static int afk_epic_command(afk_epic_ep_t *epic, int channel, int type,
                           void *tx, size_t txsize, void *rx, size_t *rxsize)
{
    (void)epic;
    const u8 *bytes = tx;
    assert(channel == 9 && type == 0xc0 && txsize == 16 + payload_len);
    u8 header[16] = {0};
    word(header, 0, op);
    word(header, 4, txsize);
    assert(!memcmp(bytes, header, 16));
    assert(!memcmp(bytes + 16, expected, payload_len));
    if (v2 && (op == 1 || op == 2 || op == 6)) {
        assert(!rx && !rxsize);
    } else {
        assert(rx && rxsize && *rxsize == 0x4000);
        memset(rx, 0, *rxsize);
        word(rx, 0, op);
        word(rx, 4, 8 + response_len);
        memcpy((u8 *)rx + 8, response, response_len);
        *rxsize = 8 + response_len;
    }
    return transport_error;
}
''' + commands + r'''
int main(void)
{
    dcp_iboot_if_t ib = {.channel = 9};
    _Static_assert(sizeof(struct txcmd) == 16, "request header");
    _Static_assert(sizeof(struct rxcmd) == 8, "reply header");
    _Static_assert(sizeof(dcp_timing_mode_t) == 24, "timing record");
    _Static_assert(sizeof(dcp_color_mode_t) == 24, "color record");
    _Static_assert(sizeof(dcp_layer_t) == 164, "layer record");
    _Static_assert(sizeof(struct swap_set_layer_cmd) == 208, "12.3 swap payload");
    _Static_assert(sizeof(struct swap_set_layer_cmd_v13_3) == 216, "13.3 swap payload");
    dcp_timing_mode_t timing = {.valid = 1, .width = 1920, .height = 1080, .fps = 60};
    dcp_color_mode_t color = {.valid = 1, .colorimetry = 1, .eotf = 1,
                              .encoding = 1, .bpp = 24};
    dcp_layer_t layer = {.plane_cnt = 1, .width = 1920, .height = 1080};
    u8 layer_bytes[164] = {0};
    word(layer_bytes, 136, 1);
    word(layer_bytes, 140, 1920);
    word(layer_bytes, 144, 1080);
    for (unsigned protocol = 0; protocol < 2; protocol++) {
        v2 = protocol;
        prepare(1, 164, 0);
        memcpy(expected, layer_bytes, 164);
        assert(!dcp_ib_set_surface(&ib, &layer));
        prepare(2, 1, 0);
        expected[0] = 1;
        assert(!dcp_ib_set_power(&ib, true));
        expected[0] = 0;
        assert(!dcp_ib_set_power(&ib, false));
        prepare(6, 48, 0);
        word(expected, 0, 1); word(expected, 4, 1920);
        word(expected, 8, 1080); word(expected, 12, 60);
        word(expected, 24, 1); word(expected, 28, 1); word(expected, 32, 1);
        word(expected, 36, 1); word(expected, 40, 24);
        assert(!dcp_ib_set_mode(&ib, &timing, &color));
        transport_error = -23;
        assert(dcp_ib_set_mode(&ib, &timing, &color) == -23);
        transport_error = 0;
        prepare(3, 0, 12);
        response[0] = 1; word(response, 4, 2); word(response, 8, 3);
        int timings = 0, colors = 0;
        assert(dcp_ib_get_hpd(&ib, &timings, &colors) == 1);
        assert(timings == 2 && colors == 3);
        assert(dcp_ib_get_hpd(&ib, NULL, NULL) == 1);
        transport_error = -23;
        timings = colors = 77;
        assert(dcp_ib_get_hpd(&ib, &timings, &colors) == -23);
        assert(timings == 77 && colors == 77);
        transport_error = 0;
        prepare(15, 0, 20);
        word(response, 12, 42);
        assert(dcp_ib_swap_begin(&ib) == 42);
        dcp_rect_t src = {.w = 1920, .h = 1080, .x = 5, .y = 6};
        dcp_rect_t dst = {.w = 960, .h = 540, .x = 7, .y = 8};
        for (unsigned newer = 0; newer < 2; newer++) {
            os_firmware.version = newer ? V13_3 : V13_3 - 1;
            prepare(16, newer ? 216 : 208, 0);
            word(expected, 4, 3);
            memcpy(expected + 8, layer_bytes, sizeof(layer_bytes));
            unsigned rect = newer ? 180 : 172;
            word(expected, rect, 1920); word(expected, rect + 4, 1080);
            word(expected, rect + 8, 5); word(expected, rect + 12, 6);
            word(expected, rect + 16, 960); word(expected, rect + 20, 540);
            word(expected, rect + 24, 7); word(expected, rect + 28, 8);
            assert(!dcp_ib_swap_set_layer(&ib, 3, &layer, &src, &dst));
        }
        prepare(18, 12, 0);
        memset(ib.txbuf + 16, 0xff, 12);
        assert(!dcp_ib_swap_end(&ib));
    }
    puts("PASS public golden wire offsets, V1/V2 setters, HPD values, error propagation and both swap layouts");
    return 0;
}
'''
    c = tmp_path / "wire-contract.c"
    c.write_text(harness)
    binary = tmp_path / "wire-contract"
    compiler = shlex.split(os.environ.get("CC", "cc"))
    subprocess.run(compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror",
                              str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
