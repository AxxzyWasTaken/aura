/* SPDX-License-Identifier: MIT */
/*
 * Host test of the G15 GPU setup in kboot_gpu.c (T6030 and T8122), with a synthetic ADT, a
 * simulated bus and real libfdt. run-kboot-gpu-g15.sh copies kboot_gpu.c next to this file and
 * the stand-in utils.h/memory.h from tests/g15_host, so the setup runs unchanged on the host.
 *
 * Usage: kboot-gpu-g15 <scenario>   prints the resulting DT, bus and PMGR traces and the log.
 *        kboot-gpu-g15 list         prints the scenario names.
 * T6030 scenarios also build against an older kboot_gpu.c, so the run script can check that the
 * T6030 results did not change. T8122 scenarios need G15_NEW (this tree) and assert on results.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kboot_gpu.c"

/* ---------- m1n1 globals and stubs ---------- */

void *adt = (void *)1;
u32 board_id, chip_id;
volatile enum exc_guard_t exc_guard;
volatile int exc_count;
uint64_t ram_base;
u64 boot_flags, mem_size_actual;
size_t mcc_carveout_count;
struct mcc_carveout mcc_carveouts[4];

static char log_buf[1 << 18];
static size_t log_len;

int debug_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(log_buf + log_len, sizeof(log_buf) - log_len, fmt, ap);
    va_end(ap);
    if (n > 0)
        log_len = min(log_len + n, sizeof(log_buf) - 1);
    return n;
}

static char trace_buf[1 << 16];
static size_t trace_len;
static int pmgr_enables, pmgr_disables, mmio_reads, unguarded_faults;

static void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void trace(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(trace_buf + trace_len, sizeof(trace_buf) - trace_len, fmt, ap);
    va_end(ap);
    if (n > 0)
        trace_len = min(trace_len + n, sizeof(trace_buf) - 1);
}

static u64 ram_min = 0x10000000000, ram_size = 0x1e0000000;
u64 kboot_ram_base(void)
{
    return ram_min;
}
u64 kboot_ram_size(void)
{
    return ram_size;
}
u64 kboot_fb_base(void)
{
    return 0;
}
u64 top_of_memory_alloc(size_t size)
{
    (void)size;
    abort();
}
int32_t rust_gpu_initdata_size(uint32_t a, uint32_t b, size_t *c, size_t *d, size_t *e)
{
    (void)a, (void)b, (void)c, (void)d, (void)e;
    return -1;
}
int32_t rust_fill_gpu_initdata(struct initdata_inputs *a, void *b, void *c, void *d)
{
    (void)a, (void)b, (void)c, (void)d;
    return -1;
}

/* PMGR: the GPU power closure. pmgr_off is the state seen by pmgr_adt_power_is_off(). */
static int pmgr_enable_ret, pmgr_off = 1, pmgr_off_after_release = 1;
int pmgr_adt_power_enable(const char *path)
{
    trace("pmgr enable %s\n", path);
    pmgr_enables++;
    if (!pmgr_enable_ret)
        pmgr_off = 0;
    return pmgr_enable_ret;
}
int pmgr_adt_power_disable(const char *path)
{
    trace("pmgr disable %s\n", path);
    pmgr_disables++;
    return 0;
}
int pmgr_adt_power_disable_recursive(const char *path)
{
    trace("pmgr disable-recursive %s\n", path);
    pmgr_disables++;
    pmgr_off = pmgr_off_after_release;
    return 0;
}
int pmgr_adt_power_is_off(const char *path)
{
    trace("pmgr is-off %s = %d\n", path, pmgr_off);
    return pmgr_off;
}

/* ---------- synthetic ADT ---------- */

struct hprop {
    const char *name;
    u32 len;
    u8 data[1024] __attribute__((aligned(8)));
};

struct hnode {
    const char *path;
    int nprops;
    struct hprop props[48];
};

static struct hnode hnodes[16];
static int n_hnodes;

static struct hnode *hnode(const char *path)
{
    for (int i = 0; i < n_hnodes; i++)
        if (!strcmp(hnodes[i].path, path))
            return &hnodes[i];
    assert(n_hnodes < (int)ARRAY_SIZE(hnodes));
    hnodes[n_hnodes].path = path;
    return &hnodes[n_hnodes++];
}

static void hset(const char *path, const char *name, const void *data, u32 len)
{
    struct hnode *n = hnode(path);
    struct hprop *p = NULL;
    for (int i = 0; i < n->nprops; i++)
        if (!strcmp(n->props[i].name, name))
            p = &n->props[i];
    if (!p) {
        assert(n->nprops < (int)ARRAY_SIZE(n->props));
        p = &n->props[n->nprops++];
    }
    assert(len <= sizeof(p->data));
    p->name = name;
    p->len = len;
    memcpy(p->data, data, len);
}

static void hdel(const char *path, const char *name)
{
    struct hnode *n = hnode(path);
    for (int i = 0; i < n->nprops; i++)
        if (!strcmp(n->props[i].name, name)) {
            n->props[i] = n->props[--n->nprops];
            return;
        }
}

static void hset_u32(const char *path, const char *name, u32 v)
{
    hset(path, name, &v, sizeof(v));
}

static void hset_u64(const char *path, const char *name, u64 v)
{
    hset(path, name, &v, sizeof(v));
}

int adt_path_offset(const void *tree, const char *path)
{
    (void)tree;
    for (int i = 0; i < n_hnodes; i++)
        if (!strcmp(hnodes[i].path, path))
            return i + 1;
    return -1;
}

int adt_path_offset_trace(const void *tree, const char *path, int *offsets)
{
    int off = adt_path_offset(tree, path);
    if (off > 0 && offsets) {
        offsets[0] = off;
        offsets[1] = 0;
    }
    return off;
}

const void *adt_getprop(const void *tree, int node, const char *name, u32 *lenp)
{
    (void)tree;
    if (node < 1 || node > n_hnodes)
        return NULL;
    struct hnode *n = &hnodes[node - 1];
    for (int i = 0; i < n->nprops; i++)
        if (!strcmp(n->props[i].name, name)) {
            if (lenp)
                *lenp = n->props[i].len;
            return n->props[i].data;
        }
    return NULL;
}

int adt_getprop_copy(const void *tree, int node, const char *name, void *out, size_t len)
{
    u32 plen;
    const void *p = adt_getprop(tree, node, name, &plen);
    if (!p || plen != len)
        return -1;
    memcpy(out, p, len);
    return len;
}

int adt_get_reg(const void *tree, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    u32 len;
    const u64 *reg = adt_getprop(tree, path[0], prop, &len);
    if (!reg || len < (idx + 1) * 16u)
        return -1;
    if (addr)
        memcpy(addr, &reg[idx * 2], sizeof(*addr));
    if (size)
        memcpy(size, &reg[idx * 2 + 1], sizeof(*size));
    return 0;
}

/* ---------- simulated bus ---------- */

#define DRAM_BASE 0x10000000000UL
#define DRAM_SIZE 0x200000000UL
#define SGX_BASE  0x290000000UL
#define SGX_SIZE  0x1000000UL
#define ASC_BASE  0x292400000UL
#define ASC_SIZE  0x40000UL
#define FW_TEXT   0x101f8000000UL
#define FW_DATA   (FW_TEXT + 0x5c000)
#define FW_VA     0xfffffc0000000000UL
#define LEAK_FUSE 0x3642c81a8UL

static u32 fw_hdr[0x38 / 4];
static char fw_version_str[64] = "RTKit-test";

struct mmio {
    u64 addr;
    u32 val;
    int fault;
};
static struct mmio mmio[16];
static int n_mmio;

static void mmio_set(u64 addr, u32 val, int fault)
{
    for (int i = 0; i < n_mmio; i++)
        if (mmio[i].addr == addr) {
            mmio[i].val = val;
            mmio[i].fault = fault;
            return;
        }
    mmio[n_mmio++] = (struct mmio){addr, val, fault};
}

static u32 bus_fault(u64 addr)
{
    trace("fault 0x%lx%s\n", addr, (exc_guard & GUARD_TYPE_MASK) == GUARD_MARK ? "" : " UNGUARDED");
    if ((exc_guard & GUARD_TYPE_MASK) == GUARD_MARK)
        exc_count++;
    else
        unguarded_faults++;
    return 0xffffffff;
}

u32 host_read32(u64 addr)
{
    if (addr >= FW_TEXT + 0x4200 && addr < FW_TEXT + 0x4200 + sizeof(fw_hdr)) {
        trace("read32 fw+0x%lx\n", addr - FW_TEXT);
        return fw_hdr[(addr - FW_TEXT - 0x4200) / 4];
    }
    for (int i = 0; i < n_mmio; i++)
        if (mmio[i].addr == addr) {
            trace("read32 0x%lx%s\n", addr, mmio[i].fault ? " (faults)" : "");
            mmio_reads++;
            if (mmio[i].fault)
                return bus_fault(addr);
            return mmio[i].val;
        }
    trace("read32 0x%lx UNMAPPED\n", addr);
    return bus_fault(addr);
}

u8 host_read8(u64 addr)
{
    u64 off = fw_hdr[3];
    if (addr >= FW_TEXT + off && addr < FW_TEXT + off + sizeof(fw_version_str))
        return fw_version_str[addr - FW_TEXT - off];
    trace("read8 0x%lx UNMAPPED\n", addr);
    return bus_fault(addr);
}

void host_sysop(const char *op)
{
    (void)op;
}

/* ---------- fixtures ---------- */

/* The 14.8.3 "g15s build a0" image identity from src/firmware.c */
static const u8 g15s_a0_uuid[16] = {0x0c, 0xbb, 0x17, 0xaf, 0x78, 0x3d, 0x36, 0x7b,
                                    0x9c, 0xed, 0xd5, 0x91, 0x09, 0x44, 0x73, 0x1e};

struct fixture {
    u32 chip;
    const char *compatible;
    const char *optin;  /* chosen property set to "1", NULL for none */
    u32 clusters;       /* pstate tables, OPP microvolt cells */
    int mbox_disabled;  /* the gpu's mailbox node starts disabled */
    int dt_size;
};

#define U64PAIR(a, b) {(a), (b)}

static void setup_adt(const struct fixture *fx)
{
    n_hnodes = 0;
    memset(hnodes, 0, sizeof(hnodes));

    hset_u32("/arm-io", "chip-revision", 0x11);
    u64 sgx_reg[2] = U64PAIR(SGX_BASE, SGX_SIZE);
    hset("/arm-io/sgx", "reg", sgx_reg, sizeof(sgx_reg));

    /* 5 pstates (off + 4) per table; table 1 repeats table 0 with a zero to back-fill */
    struct perf_state ps[2][5] = {
        {{0, 125}, {338000000, 600}, {618000000, 700}, {796000000, 800}, {1380000000, 1000}},
        {{0, 125}, {338000000, 610}, {618000000, 0}, {796000000, 810}, {1380000000, 1010}},
    };
    hset_u32("/arm-io/sgx", "perf-state-count", 5);
    hset_u32("/arm-io/sgx", "gpu-num-perf-states", 4);
    hset_u32("/arm-io/sgx", "perf-state-table-count", fx->clusters);
    hset("/arm-io/sgx", "perf-states", ps, sizeof(ps[0]) * fx->clusters);
    hset("/arm-io/sgx", "perf-states-sram", ps, sizeof(ps[0]) * fx->clusters);

    static const char *const regions[] = {"gfx-handoff", "gfx-shared-region", "gpu-region",
                                          "gfx-shared-l2-region"};
    static char names[4][2][40];
    for (int i = 0; i < 4; i++) {
        snprintf(names[i][0], sizeof(names[i][0]), "%s-base", regions[i]);
        snprintf(names[i][1], sizeof(names[i][1]), "%s-size", regions[i]);
        hset_u64("/arm-io/sgx", names[i][0], 0x101f0000000UL + i * 0x1000000UL);
        hset_u64("/arm-io/sgx", names[i][1], 0x4000UL << i);
    }

    u64 asc_reg[2] = U64PAIR(ASC_BASE, ASC_SIZE);
    hset("/arm-io/gfx-asc", "reg", asc_reg, sizeof(asc_reg));
    hset("/arm-io/gfx-asc", "segment-names", "__TEXT;__DATA", sizeof("__TEXT;__DATA"));
    struct adt_segment_ranges seg[2] = {
        {FW_TEXT, FW_VA, FW_TEXT, 0x5c000, 1},
        {FW_DATA, FW_VA + 0x5c000, FW_DATA, 0x114000, 0},
    };
    hset("/arm-io/gfx-asc", "segment-ranges", seg, sizeof(seg));
    hset("/arm-io/gfx-asc/iop-gfx-nub", "segment-ranges", seg, sizeof(seg));
    hset_u32("/arm-io/gfx-asc/iop-gfx-nub", "pre-loaded", 1);

    hset_u64("/chosen", "dram-base", DRAM_BASE);
    hset_u64("/chosen", "dram-size", DRAM_SIZE);
}

static void setup_bus(const struct fixture *fx)
{
    n_mmio = 0;
    memset(fw_hdr, 0, sizeof(fw_hdr));
    fw_hdr[0] = 0x14000011;
    fw_hdr[1] = 0x64697575;
    fw_hdr[2] = 5;
    fw_hdr[3] = 0x4800;
    memcpy(&fw_hdr[5], g15s_a0_uuid, sizeof(g15s_a0_uuid));
    fw_hdr[11] = 0x58e88;
    fw_hdr[12] = 0x1e8;
    fw_hdr[13] = 0x5c000;

    if (fx->chip == T6030) {
        mmio_set(SGX_BASE + 0xd04000, 0x07031100, 0);
        mmio_set(SGX_BASE + 0xe01500, 0x000ffbff, 0); /* 9 + 10 cores */
        mmio_set(SGX_BASE + 0xe01504, 0, 0);
        mmio_set(SGX_BASE + 0xe01508, 0, 0);
        mmio_set(LEAK_FUSE, 0x12345678, 0);
    } else {
        mmio_set(SGX_BASE + 0xd04000, 0x07022000, 0);
        mmio_set(SGX_BASE + 0xd04010, 0x0001010a, 0); /* 1 die, 1 cluster, 10 cores */
        mmio_set(SGX_BASE + 0xe01500, 0x000000ff, 0); /* 8 active cores */
        mmio_set(SGX_BASE + 0xe01504, 0, 0);
        mmio_set(SGX_BASE + 0xe01508, 0, 0);
    }

    ram_base = DRAM_BASE;
    mem_size_actual = DRAM_SIZE;
    mcc_carveout_count = 1;
    mcc_carveouts[0].base = 0x101fc000000UL;
    mcc_carveouts[0].size = 0x100000;
}

#define CHK(x) assert((x) >= 0)

static void add_resv(void *dt, int resv, const char *name, u32 phandle)
{
    int n = fdt_add_subnode(dt, resv, name);
    CHK(n);
    fdt64_t reg[2] = {0, 0};
    CHK(fdt_setprop(dt, n, "reg", reg, sizeof(reg)));
    CHK(fdt_setprop_string(dt, n, "status", "disabled"));
    CHK(fdt_setprop_u32(dt, n, "phandle", phandle));
}

static void setup_dt(void *dt, const struct fixture *fx)
{
    CHK(fdt_create_empty_tree(dt, fx->dt_size));
    CHK(fdt_setprop_u32(dt, 0, "#address-cells", 2));
    CHK(fdt_setprop_u32(dt, 0, "#size-cells", 2));

    int aliases = fdt_add_subnode(dt, 0, "aliases");
    CHK(aliases);
    CHK(fdt_setprop_string(dt, aliases, "gpu", "/soc/gpu@292400000"));

    int chosen = fdt_add_subnode(dt, 0, "chosen");
    CHK(chosen);
    CHK(fdt_setprop_string(dt, chosen, "bootargs", "quiet"));
    if (fx->optin)
        CHK(fdt_setprop_string(dt, chosen, fx->optin, "1"));

    int resv = fdt_add_subnode(dt, 0, "reserved-memory");
    CHK(resv);
    CHK(fdt_setprop_u32(dt, resv, "#address-cells", 2));
    CHK(fdt_setprop_u32(dt, resv, "#size-cells", 2));
    CHK(fdt_setprop_empty(dt, resv, "ranges"));
    /* fdt_add_subnode() inserts at the head: add in reverse */
    add_resv(dt, resv, "globals", 16);
    add_resv(dt, resv, "hw-cal-b", 15);
    add_resv(dt, resv, "hw-cal-a", 14);
    add_resv(dt, resv, "uat-pagetables-l2", 13);
    add_resv(dt, resv, "uat-ttbs", 12);
    add_resv(dt, resv, "uat-pagetables", 11);
    add_resv(dt, resv, "uat-handoff", 10);

    int opp = fdt_add_subnode(dt, 0, "opp-table-gpu");
    CHK(opp);
    CHK(fdt_setprop_u32(dt, opp, "phandle", 30));
    for (int i = 1; i >= 0; i--) {
        char name[8];
        snprintf(name, sizeof(name), "opp%02d", i);
        int o = fdt_add_subnode(dt, opp, name);
        CHK(o);
        fdt32_t uv[2] = {cpu_to_fdt32(400000), cpu_to_fdt32(400000)};
        CHK(fdt_setprop_u64(dt, o, "opp-hz", 0));
        CHK(fdt_setprop(dt, o, "opp-microvolt", uv, sizeof(fdt32_t) * fx->clusters));
        CHK(fdt_setprop_u32(dt, o, "opp-microwatt", 0));
    }

    int soc = fdt_add_subnode(dt, 0, "soc");
    CHK(soc);
    CHK(fdt_setprop_u32(dt, soc, "#address-cells", 2));
    CHK(fdt_setprop_u32(dt, soc, "#size-cells", 2));
    CHK(fdt_setprop_empty(dt, soc, "ranges"));

    int mbox = fdt_add_subnode(dt, soc, "mbox@292408000");
    CHK(mbox);
    CHK(fdt_setprop_string(dt, mbox, "compatible", "apple,asc-mailbox-v4"));
    CHK(fdt_setprop_u32(dt, mbox, "phandle", 20));
    if (fx->mbox_disabled)
        CHK(fdt_setprop_string(dt, mbox, "status", "disabled"));

    int gpu = fdt_add_subnode(dt, soc, "gpu@292400000");
    CHK(gpu);
    CHK(fdt_setprop_string(dt, gpu, "compatible", fx->compatible));
    fdt64_t reg[4] = {cpu_to_fdt64(ASC_BASE), cpu_to_fdt64(ASC_SIZE), cpu_to_fdt64(SGX_BASE),
                      cpu_to_fdt64(SGX_SIZE)};
    CHK(fdt_setprop(dt, gpu, "reg", reg, sizeof(reg)));
    CHK(fdt_setprop(dt, gpu, "reg-names", "asc\0sgx", sizeof("asc\0sgx")));
    CHK(fdt_setprop_u32(dt, gpu, "mboxes", 20));
    fdt32_t mr[6] = {cpu_to_fdt32(12), cpu_to_fdt32(11), cpu_to_fdt32(10),
                     cpu_to_fdt32(14), cpu_to_fdt32(15), cpu_to_fdt32(16)};
    CHK(fdt_setprop(dt, gpu, "memory-region", mr, sizeof(mr)));
    static const char mrn[] = "ttbs\0pagetables\0handoff\0hw-cal-a\0hw-cal-b\0globals";
    CHK(fdt_setprop(dt, gpu, "memory-region-names", mrn, sizeof(mrn)));
    fdt32_t zero3[3] = {0, 0, 0};
    CHK(fdt_setprop(dt, gpu, "apple,firmware-abi", zero3, sizeof(zero3)));
    CHK(fdt_setprop(dt, gpu, "apple,firmware-version", zero3, sizeof(zero3)));
    CHK(fdt_setprop(dt, gpu, "apple,firmware-compat", zero3, sizeof(zero3)));
    CHK(fdt_setprop_u32(dt, gpu, "operating-points-v2", 30));
    fdt32_t one[2] = {cpu_to_fdt32(0x3f800000), cpu_to_fdt32(0x3f800000)};
    CHK(fdt_setprop(dt, gpu, "apple,core-leak-coef", one, sizeof(fdt32_t) * fx->clusters));
    CHK(fdt_setprop(dt, gpu, "apple,sram-leak-coef", zero3, sizeof(fdt32_t) * fx->clusters));
    CHK(fdt_setprop_string(dt, gpu, "status", "disabled"));
}

/* ---------- output ---------- */

static void dump_node(const void *dt, int node, int depth)
{
    int prop;
    fprintf(stdout, "%*s%s {\n", depth * 2, "", depth ? fdt_get_name(dt, node, NULL) : "/");
    fdt_for_each_property_offset(prop, dt, node)
    {
        const char *name;
        int len;
        const u8 *v = fdt_getprop_by_offset(dt, prop, &name, &len);
        fprintf(stdout, "%*s%s =", depth * 2 + 2, "", name);
        for (int i = 0; i < len; i++)
            fprintf(stdout, " %02x", v[i]);
        fprintf(stdout, ";\n");
    }
    int child;
    fdt_for_each_subnode(child, dt, node) dump_node(dt, child, depth + 1);
    fprintf(stdout, "%*s}\n", depth * 2, "");
}

static void report(const void *dt)
{
    fprintf(stdout, "== dt\n");
    dump_node(dt, 0, 0);
    fprintf(stdout, "== trace\n%s", trace_buf);
    fprintf(stdout, "== log\n%s", log_buf);
}

/* ---------- helpers for assertions ---------- */

static const char *status_of(const void *dt, const char *path)
{
    int n = fdt_path_offset(dt, path);
    assert(n >= 0);
    const char *s = fdt_getprop(dt, n, "status", NULL);
    return s ? s : "(none)";
}

static int prop_len(const void *dt, const char *path, const char *name)
{
    int n = fdt_path_offset(dt, path), len = -1;
    assert(n >= 0);
    return fdt_getprop(dt, n, name, &len) ? len : -1;
}

static int log_has(const char *s)
{
    return strstr(log_buf, s) != NULL;
}

static void expect_gpu_off(const void *dt)
{
    assert(!strcmp(status_of(dt, "/soc/gpu@292400000"), "disabled"));
    static const char *const uat[] = {"uat-handoff", "uat-pagetables", "uat-ttbs",
                                      "uat-pagetables-l2"};
    for (int i = 0; i < 4; i++) {
        char p[64];
        snprintf(p, sizeof(p), "/reserved-memory/%s", uat[i]);
        assert(!strcmp(status_of(dt, p), "disabled"));
    }
    assert(prop_len(dt, "/soc/gpu@292400000", "apple,m3-handoff-version") < 0);
    assert(unguarded_faults == 0);
}

/* ---------- scenarios ---------- */

static const struct fixture t6030_fx = {T6030, "apple,agx-t6030", "asahi,t6030-gpu", 2, 0, 65536};
#ifdef G15_NEW
static const struct fixture t8122_fx = {T8122, "apple,agx-t8122", "asahi,t8122-gpu", 1, 1, 65536};
#endif

static struct fixture fx;
static u8 dtbuf[1 << 17];

static void prepare(const struct fixture *f)
{
    fx = *f;
    chip_id = fx.chip;
    log_len = trace_len = 0;
    log_buf[0] = trace_buf[0] = 0;
    pmgr_enables = pmgr_disables = mmio_reads = unguarded_faults = 0;
    pmgr_enable_ret = 0;
    pmgr_off = pmgr_off_after_release = 1;
    exc_guard = GUARD_OFF;
    exc_count = 0;
    os_firmware = fw_versions[V14_7];
    setup_adt(&fx);
    setup_bus(&fx);
    setup_dt(dtbuf, &fx);
}

static int run(void)
{
    return dt_set_gpu(dtbuf);
}

/* T6030: results must match the pre-refactor setup. */
static void t6030_dry(void)
{
    struct fixture f = t6030_fx;
    f.optin = NULL;
    prepare(&f);
    run();
}
static void t6030_armed(void)
{
    prepare(&t6030_fx);
    run();
}
static void t6030_unknown_fw(void)
{
    prepare(&t6030_fx);
    fw_hdr[5] ^= 1;
    run();
}
static void t6030_wrong_id(void)
{
    prepare(&t6030_fx);
    mmio_set(SGX_BASE + 0xd04000, 0x07021100, 0);
    run();
}
static void t6030_core_mask(void)
{
    prepare(&t6030_fx);
    mmio_set(SGX_BASE + 0xe01504, 0x10, 0);
    run();
}
static void t6030_fuse_fault(void)
{
    prepare(&t6030_fx);
    mmio_set(LEAK_FUSE, 0, 1);
    run();
}
static void t6030_id_fault(void)
{
    prepare(&t6030_fx);
    mmio_set(SGX_BASE + 0xd04000, 0, 1);
    run();
}
static void t6030_region_in_ram(void)
{
    prepare(&t6030_fx);
    hset_u64("/arm-io/sgx", "gpu-region-base", ram_min + 0x100000);
    run();
}
static void t6030_nub_mismatch(void)
{
    prepare(&t6030_fx);
    hset_u32("/arm-io/gfx-asc/iop-gfx-nub", "pre-loaded", 0);
    run();
}
static void t6030_dt_sgx_mismatch(void)
{
    prepare(&t6030_fx);
    u64 sgx_reg[2] = U64PAIR(SGX_BASE + 0x10000, SGX_SIZE);
    hset("/arm-io/sgx", "reg", sgx_reg, sizeof(sgx_reg));
    run();
}
static void t6030_bad_pstate(void)
{
    prepare(&t6030_fx);
    hset_u32("/arm-io/sgx", "gpu-num-perf-states", 5);
    run();
}
static void t6030_no_table_count(void)
{
    prepare(&t6030_fx);
    hdel("/arm-io/sgx", "perf-state-table-count");
    run();
}
static void t6030_power_fail(void)
{
    prepare(&t6030_fx);
    pmgr_enable_ret = -1;
    run();
}
static void t6030_small_dt(void)
{
    prepare(&t6030_fx);
    /* Leave a little room, so the OPP rebuild runs out of space part way */
    fdt_pack(dtbuf);
    CHK(fdt_open_into(dtbuf, dtbuf, fdt_totalsize(dtbuf) + 0x200));
    run();
}
static void t6030_tiny_dt(void)
{
    prepare(&t6030_fx);
    /* Too little room to rebuild the OPP table */
    fdt_pack(dtbuf);
    CHK(fdt_open_into(dtbuf, dtbuf, fdt_totalsize(dtbuf) + 0x40));
    run();
}
static void t6030_no_alias(void)
{
    prepare(&t6030_fx);
    fdt_delprop(dtbuf, fdt_path_offset(dtbuf, "/aliases"), "gpu");
    run();
}
static void t6030_wrong_compat(void)
{
    struct fixture f = t6030_fx;
    f.compatible = "apple,agx-t6031";
    prepare(&f);
    run();
}

#ifdef G15_NEW
/*
 * T8122. With this tree's tables the setup must refuse before any GPU access: the G15G image,
 * leakage fuse and power model are not known yet.
 */
static void t8122_default(void)
{
    struct fixture f = t8122_fx;
    f.optin = NULL;
    prepare(&f);
    static u8 before[sizeof(dtbuf)];
    memcpy(before, dtbuf, sizeof(dtbuf));
    run();
    assert(!memcmp(before, dtbuf, sizeof(dtbuf)));
    assert(!trace_len && !pmgr_enables && !mmio_reads);
}

static void t8122_armed_refuses(void)
{
    prepare(&t8122_fx);
    run();
    expect_gpu_off(dtbuf);
    assert(!strcmp(status_of(dtbuf, "/soc/mbox@292408000"), "disabled"));
    assert(!pmgr_enables && !mmio_reads);
    assert(log_has("FW: GFX: image uuid"));
    assert(log_has("T8122 GPU setup failed"));
}

/*
 * The T8122 pipeline with the data that is still missing supplied by the test: the G15S image
 * stands in for the G15G one, and the leakage and power model are fixed test values.
 */
static int test_read_leak(const struct g15_soc *soc, float *leak)
{
    (void)soc;
    *leak = 2.0f;
    return 0;
}

static void test_calc_power(u32 count, const struct perf_state *core, float leak, u32 cores,
                            u32 *max_pwr)
{
    (void)leak;
    (void)cores;
    max_pwr[0] = 0;
    for (u32 i = 1; i < count; i++)
        max_pwr[i] = core[i].volt * (core[i].freq / 1000000);
}

static struct g15_soc test_soc;

static int run_t8122_test_soc(void)
{
    test_soc = g15_soc_t8122;
    test_soc.chip = T6030; /* the G15S images stand in for the G15G ones */
    test_soc.read_leak = test_read_leak;
    test_soc.calc_power = test_calc_power;
    return dt_set_gpu_g15(dtbuf, &test_soc);
}

static void t8122_pipeline(void)
{
    prepare(&t8122_fx);
    run_t8122_test_soc();
    const char *gpu = "/soc/gpu@292400000";
    assert(!strcmp(status_of(dtbuf, gpu), "okay"));
    assert(!strcmp(status_of(dtbuf, "/soc/mbox@292408000"), "okay"));
    assert(!strcmp(status_of(dtbuf, "/reserved-memory/uat-pagetables-l2"), "okay"));
    assert(prop_len(dtbuf, gpu, "apple,core-leak-coef") == 4);
    assert(prop_len(dtbuf, gpu, "apple,sram-leak-coef") == 4);
    assert(prop_len(dtbuf, gpu, "apple,m3-handoff-version") == 4);
    assert(prop_len(dtbuf, "/opp-table-gpu/opp04", "opp-microvolt") == 4);
    assert(prop_len(dtbuf, "/opp-table-gpu/opp00", "opp-microvolt") == 4);
    int gpu_node = fdt_path_offset(dtbuf, gpu);
    const fdt32_t *compat = fdt_getprop(dtbuf, gpu_node, "apple,firmware-compat", NULL);
    assert(fdt32_to_cpu(compat[0]) == 14 && fdt32_to_cpu(compat[1]) == 8 &&
           fdt32_to_cpu(compat[2]) == 3);
    assert(pmgr_enables == 1 && pmgr_disables == 0 && unguarded_faults == 0);
    assert(log_has("1 cluster(s) x 10 core slots"));
    assert(log_has("T8122 GPU enabled"));
}

static void t8122_closure_on(void)
{
    prepare(&t8122_fx);
    pmgr_off = 0;
    run_t8122_test_soc();
    expect_gpu_off(dtbuf);
    assert(!pmgr_enables && !mmio_reads);
    assert(log_has("power closure is not off"));
}

static void t8122_wrong_id(void)
{
    prepare(&t8122_fx);
    mmio_set(SGX_BASE + 0xd04000, 0x07032000, 0);
    run_t8122_test_soc();
    expect_gpu_off(dtbuf);
    assert(!strcmp(status_of(dtbuf, "/soc/mbox@292408000"), "disabled"));
    assert(pmgr_enables == 1 && pmgr_disables == 1 && pmgr_off == 1);
    assert(strstr(trace_buf, "pmgr disable-recursive /arm-io/sgx"));
}

static void t8122_two_dies(void)
{
    prepare(&t8122_fx);
    mmio_set(SGX_BASE + 0xd04010, 0x0002010a, 0);
    run_t8122_test_soc();
    expect_gpu_off(dtbuf);
    assert(pmgr_disables == 1 && pmgr_off == 1);
    assert(log_has("dies"));
}

static void t8122_mask_beyond_slots(void)
{
    prepare(&t8122_fx);
    mmio_set(SGX_BASE + 0xe01500, 0x000004ff, 0);
    run_t8122_test_soc();
    expect_gpu_off(dtbuf);
    assert(pmgr_disables == 1);
}

static void t8122_tables_exceed_clusters(void)
{
    struct fixture f = t8122_fx;
    f.clusters = 2;
    prepare(&f);
    run_t8122_test_soc();
    expect_gpu_off(dtbuf);
    assert(pmgr_disables == 1);
    assert(log_has("perf-state tables"));
}

static void t8122_release_fails(void)
{
    prepare(&t8122_fx);
    mmio_set(SGX_BASE + 0xd04000, 0, 1);
    pmgr_off_after_release = 0;
    run_t8122_test_soc();
    expect_gpu_off(dtbuf);
    assert(log_has("failed to power the GPU back off"));
}

static void t8122_small_dt(void)
{
    /* Too little room to rebuild the OPP table: everything is undone and the power released */
    prepare(&t8122_fx);
    fdt_pack(dtbuf);
    CHK(fdt_open_into(dtbuf, dtbuf, fdt_totalsize(dtbuf) + 0x40));
    run_t8122_test_soc();
    expect_gpu_off(dtbuf);
    assert(!strcmp(status_of(dtbuf, "/soc/mbox@292408000"), "disabled"));
    assert(pmgr_disables == 1 && pmgr_off == 1);
}

/*
 * The reservation-only diagnostic with the powered identity probe: the probe reads ID_VERSION,
 * ID_COUNTS_1 and the core masks under the guard and publishes them, and releases the power.
 */
static u32 identity_u32(const char *name)
{
    int n = fdt_path_offset(dtbuf, "/chosen/asahi,t8122-gpu-powered-identity"), len = 0;
    assert(n >= 0);
    const fdt32_t *v = fdt_getprop(dtbuf, n, name, &len);
    assert(v && len == 4);
    return fdt32_to_cpu(*v);
}

static void prepare_powered_identity(void)
{
    struct fixture f = t8122_fx;
    f.optin = NULL;
    prepare(&f);
    CHK(fdt_setprop_string(dtbuf, fdt_path_offset(dtbuf, "/chosen"),
                           "asahi,t8122-gpu-handoff-diag", "1"));
    CHK(fdt_setprop_string(dtbuf, fdt_path_offset(dtbuf, "/chosen"), "asahi,t8122-gpu-power-diag",
                           "1"));
}

static void t8122_powered_identity(void)
{
    prepare_powered_identity();
    run();
    assert(log_has("T8122 UAT and firmware reservations supplied from ADT"));
    assert(identity_u32("schema-version") == 1);
    assert(identity_u32("probe-result") == T8122_ID_PASS);
    assert(identity_u32("id-version") == 0x07022000);
    assert(identity_u32("id-counts-1") == 0x0001010a);
    assert(identity_u32("core-mask-0") == 0xff);
    assert(identity_u32("core-mask-1") == 0 && identity_u32("core-mask-2") == 0);
    assert(log_has("ID_VERSION 0x07022000 ID_COUNTS_1 0x0001010a core masks 0xff 0x0 0x0"));
    assert(strstr(trace_buf, "pmgr enable /arm-io/sgx\nread32 0x290d04000\nread32 0x290d04010\n"));
    assert(pmgr_enables == 1 && pmgr_disables == 1 && pmgr_off == 1);
    assert(!strcmp(status_of(dtbuf, "/soc/gpu@292400000"), "disabled"));
    assert(!strcmp(status_of(dtbuf, "/soc/mbox@292408000"), "disabled"));
    assert(unguarded_faults == 0);
}

static void t8122_powered_identity_counts(void)
{
    /* Any ID_COUNTS_1 is published as read; it does not change the result */
    prepare_powered_identity();
    mmio_set(SGX_BASE + 0xd04010, 0x00020203, 0);
    run();
    assert(identity_u32("probe-result") == T8122_ID_PASS);
    assert(identity_u32("id-counts-1") == 0x00020203);
    assert(pmgr_disables == 1 && pmgr_off == 1 && unguarded_faults == 0);
}

static void t8122_powered_identity_counts_fault(void)
{
    /* A fault on ID_COUNTS_1 is a read fault, caught by the guard; the power is released */
    prepare_powered_identity();
    mmio_set(SGX_BASE + 0xd04010, 0, 1);
    run();
    assert(identity_u32("probe-result") == T8122_ID_READ_FAULT);
    assert(pmgr_enables == 1 && pmgr_disables == 1 && pmgr_off == 1);
    assert(unguarded_faults == 0);
}

static void t8122_with_diag_optin(void)
{
    /* The full setup takes precedence over the reservation-only diagnostic */
    prepare(&t8122_fx);
    fdt_setprop_string(dtbuf, fdt_path_offset(dtbuf, "/chosen"), "asahi,t8122-gpu-handoff-diag",
                       "1");
    run();
    expect_gpu_off(dtbuf);
    assert(log_has("handoff-diag ignored"));
}
#endif

static const struct {
    const char *name;
    void (*fn)(void);
} scenarios[] = {
    {"t6030-dry", t6030_dry},
    {"t6030-armed", t6030_armed},
    {"t6030-unknown-fw", t6030_unknown_fw},
    {"t6030-wrong-id", t6030_wrong_id},
    {"t6030-core-mask", t6030_core_mask},
    {"t6030-fuse-fault", t6030_fuse_fault},
    {"t6030-id-fault", t6030_id_fault},
    {"t6030-region-in-ram", t6030_region_in_ram},
    {"t6030-nub-mismatch", t6030_nub_mismatch},
    {"t6030-dt-sgx-mismatch", t6030_dt_sgx_mismatch},
    {"t6030-bad-pstate", t6030_bad_pstate},
    {"t6030-no-table-count", t6030_no_table_count},
    {"t6030-power-fail", t6030_power_fail},
    {"t6030-small-dt", t6030_small_dt},
    {"t6030-tiny-dt", t6030_tiny_dt},
    {"t6030-no-alias", t6030_no_alias},
    {"t6030-wrong-compat", t6030_wrong_compat},
#ifdef G15_NEW
    {"t8122-default", t8122_default},
    {"t8122-armed-refuses", t8122_armed_refuses},
    {"t8122-pipeline", t8122_pipeline},
    {"t8122-closure-on", t8122_closure_on},
    {"t8122-wrong-id", t8122_wrong_id},
    {"t8122-two-dies", t8122_two_dies},
    {"t8122-mask-beyond-slots", t8122_mask_beyond_slots},
    {"t8122-tables-exceed-clusters", t8122_tables_exceed_clusters},
    {"t8122-release-fails", t8122_release_fails},
    {"t8122-small-dt", t8122_small_dt},
    {"t8122-with-diag-optin", t8122_with_diag_optin},
    {"t8122-powered-identity", t8122_powered_identity},
    {"t8122-powered-identity-counts", t8122_powered_identity_counts},
    {"t8122-powered-identity-counts-fault", t8122_powered_identity_counts_fault},
#endif
};

#undef printf
int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    for (size_t i = 0; i < ARRAY_SIZE(scenarios); i++) {
        if (!strcmp(argv[1], "list")) {
            puts(scenarios[i].name);
            continue;
        }
        if (strcmp(argv[1], scenarios[i].name))
            continue;
        scenarios[i].fn();
        report(dtbuf);
        return 0;
    }
    return strcmp(argv[1], "list") ? 2 : 0;
}
