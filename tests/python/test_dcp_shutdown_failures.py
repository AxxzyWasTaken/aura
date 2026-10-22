# SPDX-License-Identifier: MIT
"""A DCP whose firmware does not stop keeps every buffer and mapping it may use.

These compile the real afk.c and dcp.c functions against small stand-ins.
"""
from pathlib import Path
import os
import re
import subprocess

REPO = Path(os.environ.get("M1N1_DCP_SOURCE", Path(__file__).resolve().parents[2]))


def function(source, name):
    match = re.search(r"^(?:static )?(?:bool|int|void) " + name + r"\([^;]*?\)\s*\{",
                      source, re.M | re.S)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def compile_and_run(tmp_path, name, source):
    binary = tmp_path / name
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
         "-Wno-unused-function", "-x", "c", "-", "-o", str(binary)],
        input=source, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)


def test_epic_endpoint_that_does_not_stop_keeps_its_rings(tmp_path):
    source = (REPO / "src/afk.c").read_text()
    bodies = "\n".join(function(source, name)
                       for name in ("afk_epic_shutdown_ep", "afk_epic_shutdown"))
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
typedef uint32_t u32;
#define FIELD_PREP(mask, value) 0
#define RBEP_TYPE 0
#define RBEP_SHUTDOWN 0
typedef struct { int unused; } rtkit_dev_t;
struct rtkit_message { u32 ep; u64 msg; };
struct rtkit_buffer { bool freed; };
typedef struct afk_epic afk_epic_t;
typedef struct afk_epic_ep afk_epic_ep_t;
struct afk_epic { rtkit_dev_t *rtk; afk_epic_ep_t *endpoint[0x10]; };
struct afk_epic_ep {
    afk_epic_t *afk;
    u32 ep;
    bool started, stuck, released;
    struct rtkit_buffer buf, rxbuf, txbuf;
};
static int contexts_freed;
static afk_epic_t contexts[2];
static bool rtkit_send(rtkit_dev_t *rtk, const struct rtkit_message *msg) { return true; }
static int afk_epic_poll(afk_epic_t *afk, u32 ep, bool block)
{
    afk_epic_ep_t *epic = afk->endpoint[ep - 0x20];
    if (epic->stuck)
        return -1;
    epic->started = false;
    return 0;
}
static void rtkit_free_buffer(rtkit_dev_t *rtk, struct rtkit_buffer *bfr) { bfr->freed = true; }
/* Record frees without releasing anything, so the test can inspect it. */
static void record_free(void *p)
{
    if ((char *)p >= (char *)contexts && (char *)p < (char *)(contexts + 2))
        contexts_freed++;
    else
        ((afk_epic_ep_t *)p)->released = true;
}
#define free(p) record_free(p)
''' + bodies + r'''
#undef free
static bool freed(afk_epic_ep_t *e) { return e->buf.freed || e->rxbuf.freed || e->txbuf.freed; }

int main(void)
{
    afk_epic_t *afk = &contexts[0];
    afk_epic_ep_t stuck = {.afk = afk, .ep = 0x20, .started = true, .stuck = true};
    afk->endpoint[0] = &stuck;
    assert(afk_epic_shutdown_ep(&stuck) == -1);
    assert(!freed(&stuck) && !stuck.released && afk->endpoint[0] == &stuck);

    afk_epic_ep_t good = {.afk = afk, .ep = 0x21, .started = true};
    afk->endpoint[1] = &good;
    assert(afk_epic_shutdown(afk) == -1);
    assert(good.buf.freed && good.rxbuf.freed && good.txbuf.freed && good.released);
    assert(afk->endpoint[1] == NULL);
    assert(!freed(&stuck) && !stuck.released && afk->endpoint[0] == &stuck);
    assert(contexts_freed == 0);

    afk_epic_t *clean = &contexts[1];
    afk_epic_ep_t one = {.afk = clean, .ep = 0x22, .started = true};
    clean->endpoint[2] = &one;
    assert(afk_epic_shutdown(clean) == 0);
    assert(one.buf.freed && one.released && contexts_freed == 1);
    return 0;
}
'''
    compile_and_run(tmp_path, "epic-shutdown", harness)


def test_dcp_that_does_not_stop_keeps_buffers_and_mappings(tmp_path):
    source = (REPO / "src/dcp.c").read_text()
    bodies = "\n".join(function(source, name)
                       for name in ("dcp_mark_running", "dcp_left_running", "dcp_shutdown"))
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef struct {
    void *asc, *system_ep, *dptx_ep, *dpav_ep, *phy, *afk, *rtkit;
    void *dart_disp, *iovad_dcp, *dart_dcp;
} dcp_dev_t;
static char dcp_pmgr_dev[16] = "DISP0_CPU0";
static u32 dcp_die;
static bool dcp_running;
static int endpoint_result, afk_result, resets, released, darts, iovads, freed;
static bool stops, slept, quiesced;
static bool asc_is_v8(void *asc) { return false; }
static int dcp_system_shutdown(void *ep) { return endpoint_result; }
static int dcp_dptx_shutdown(void *ep) { return 0; }
static int dcp_dpav_shutdown(void *ep) { return 0; }
static int afk_epic_shutdown(void *afk) { return afk_result; }
static bool rtkit_sleep(void *rtk) { slept = true; return stops; }
static bool rtkit_quiesce(void *rtk) { quiesced = true; return stops; }
static void pmgr_reset(u32 die, const char *name) { resets++; }
static void rtkit_free(void *rtk) { released++; }
static void dart_shutdown(void *dart) { darts++; }
static void iovad_shutdown(void *iovad, void *dart) { iovads++; }
static void record_free(void *p) { freed++; }
#define free(p) record_free(p)
''' + bodies + r'''
#undef free
static void reset(int endpoint, int afk, bool stop)
{
    endpoint_result = endpoint, afk_result = afk, stops = stop;
    resets = released = darts = iovads = freed = 0;
    slept = quiesced = dcp_running = false;
}

static void kept(void)
{
    assert(released == 0 && darts == 0 && iovads == 0 && resets == 0);
    assert(dcp_left_running());
}

int main(void)
{
    dcp_dev_t dcp = {0};

    reset(0, 0, false);
    assert(dcp_shutdown(&dcp, false) == -1 && quiesced);
    kept();
    assert(freed == 1); /* only the PHY description; the DCP stays */

    reset(0, 0, false);
    assert(dcp_shutdown(&dcp, true) == -1 && slept);
    kept();

    reset(-1, 0, true);
    assert(dcp_shutdown(&dcp, false) == -1 && !quiesced && !slept);
    kept();

    reset(0, -1, true);
    assert(dcp_shutdown(&dcp, false) == -1 && !quiesced && !slept);
    kept();

    reset(0, 0, true);
    assert(dcp_shutdown(&dcp, true) == 0 && slept && resets == 1);
    assert(released == 1 && darts == 2 && iovads == 1 && freed == 2);
    assert(!dcp_left_running());
    return 0;
}
'''
    compile_and_run(tmp_path, "dcp-shutdown", harness)
