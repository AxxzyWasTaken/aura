/* SPDX-License-Identifier: MIT */
/*
 * T6030 (M3 Pro) internal display handoff.
 *
 * iBoot starts the internal DCP, lights the panel and leaves the coprocessor running. m1n1 does not
 * restart, reconfigure or stop it on T6030 (display.c refuses its own DCP modeset path there). This
 * file only describes the state iBoot left, for the next stage:
 *
 * Always, on every boot and for every kernel:
 *  - Reserve (no-map, by physical address only) the __OS_LOG buffers of dcp, dcpext0 and dcpext1.
 *    iBoot places them in RAM that is otherwise handed to the OS, and the running coprocessors keep
 *    writing to them. They are not DART addresses: their "remap" repeats the physical address, which
 *    falls inside the DCP's own framebuffer mapping, so they get no iommu-addresses.
 *  - Log the DCP CPU state and the dart-dcp and dart-disp0 PROTECT registers, then lock dart-dcp (as
 *    dt_set_display() already does for dart-disp0), so that an OS DART driver cannot reset the page
 *    tables the running DCP uses.
 *  - Walk the dart-dcp SID 5 and dart-disp0 SID 0 / SID 4 page tables, read only, and check every
 *    mapping against the ADT firmware segments. Unarmed this is a dry run: the result is only logged.
 *
 * Armed (boot.bin config line chosen.asahi,t6030-dcp=1), and only if every check below passes:
 *  - one /reserved-memory node (no-map) per iop-dcp-nub segment, with iommu-addresses for exactly
 *    the mappings found in the page tables (SID 5 for dcp, SID 0 for the display, SID 4 for the
 *    piodma child), in the 1 TiB alias form of the DARTs' apple,dma-range;
 *  - memory-region(-names) on dcp, display and piodma, apple,firmware-uuid/-version/-compat on dcp;
 *  - the marker apple,t6030-handoff = <1> on dcp, display and piodma.
 *  The display nodes stay "disabled": the shared boot.bin DTB also boots kernels that cannot adopt
 *  locked DARTs, so only a kernel that knows the marker may enable them.
 *
 * Fail-soft: dt_set_display_t6030() always returns 0; a failed check is logged and changes nothing.
 * Transactional: DT edits are made on a scratch copy that replaces the DT only if every step
 * succeeded. The always-on reservations and the armed handoff are separate transactions.
 */

#include "kboot.h"
#include "adt.h"
#include "exception.h"
#include "firmware.h"
#include "malloc.h"
#include "mcc.h"
#include "soc.h"
#include "string.h"
#include "utils.h"

#include "libfdt/libfdt.h"

#define bail(...)                                                                                  \
    do {                                                                                           \
        printf(__VA_ARGS__);                                                                       \
        return -1;                                                                                 \
    } while (0)

/* Firmware image the handoff is written for (iop-dcp-nub uuid, the 14.x DCP interface) */
#define DCP6_UUID "DDF38191-93B3-324A-BC8F-643006F5AC82"

/* J516S/J514S runtime ADT (arm-io ranges applied); checked against the ADT before any access */
#define DCP6_COPROC_BASE    0x28ec00000UL
#define DCP6_DART_DCP_BASE  0x28d30c000UL
#define DCP6_DART_DISP_BASE 0x28d304000UL
#define DCP6_PMGR_DISP_CPU  0x350710000UL
#define DCP6_DART_VM_BASE   0x10000000000UL /* DARTs' DVA alias: 1 TiB + 36-bit DVA */
#define DCP6_DART_VM_SIZE   0x1000000000UL

#define DCP6_SID_DCP    5
#define DCP6_SID_DISP   0
#define DCP6_SID_PIODMA 4

#define ASC_CPU_CONTROL     0x44
#define ASC_CPU_STATUS      0x48
#define ASC_CPU_CONTROL_RUN BIT(4)

#define PMGR_AUTO_ENABLE BIT(28)
#define PMGR_PS_ACTUAL   GENMASK(7, 4)
#define PMGR_PS_TARGET   GENMASK(3, 0)
#define PMGR_PS_ACTIVE   0xf

#define DART_T8110_PROTECT          0x200
#define DART_T8110_PROTECT_TTBR_TCR BIT(0)
#define DART_T8110_TCR(sid)         (0x1000 + 4 * (sid))
#define DART_T8110_TCR_TRANSLATE    BIT(0)
#define DART_T8110_TCR_BYPASS_DART  BIT(1)
#define DART_T8110_TCR_BYPASS_DAPF  BIT(2)
#define DART_T8110_TCR_FOUR_LEVEL   BIT(3)
#define DART_T8110_TTBR(sid)        (0x1400 + 4 * (sid))
#define DART_T8110_TTBR_VALID       BIT(0)
#define DART_T8110_TTBR_ADDR        GENMASK(29, 2)
#define DART_T8110_TTBR_SHIFT       14

/* Two-level tables of 16K pages cover the 36-bit DVA space: 2048 x 32 MiB, 2048 x 16 KiB */
#define DART_PTE_VALID   BIT(0)
#define DART_PTE_ADDR    GENMASK(39, 10)
#define DART_PTE_SHIFT   4
#define DART_PAGE        SZ_16K
#define DART_ENTRIES     2048
#define DART_L1_SHIFT    25
#define DART_L2_SHIFT    14
#define DART_DVA_MASK    ((1UL << 36) - 1)
#define DART_PT_MAX_SIZE SZ_1M

#define DCP6_SEGS         8
#define DCP6_SEG_MAX_SIZE (256 * SZ_1M)
#define DCP6_OSLOG_MAX    SZ_1M
#define DCP6_MAX_RUNS     16
#define DCP6_NAME_LEN     48

#define SEG_FLAGS_DATA  0
#define SEG_FLAGS_TEXT  1
#define SEG_FLAGS_HEAP  2
#define SEG_FLAGS_OSLOG 0xa

enum { SEG_TEXT, SEG_DATA, SEG_OSLOG, SEG_3, SEG_4, SEG_5, SEG_VRAM, SEG_7 };

/*
 * iop-dcp-nub segment-ranges, in ADT order. Every segment except __OS_LOG is also a carveout from
 * /chosen/carveout-memory-map (region-id-14 is /vram, the boot framebuffer).
 */
static const struct dcp6_seg_def {
    u32 flags;
    int region_id;
    const char *node;   /* /reserved-memory node name, before the unit address */
    const char *name;   /* memory-region-names entry */
    const char *compat; /* compatible of the reserved-memory node, or NULL */
} dcp6_seg_defs[DCP6_SEGS] = {
    {SEG_FLAGS_TEXT, 49, "dcp-text", "asc-firmware", "apple,asc-mem"},
    {SEG_FLAGS_DATA, 50, "dcp-data", "dcp_data", "apple,asc-mem"},
    {SEG_FLAGS_OSLOG, -1, "dcp-oslog", "os_log", NULL},
    {SEG_FLAGS_HEAP, 57, "dcp-seg3", "region57", "apple,asc-mem"},
    {SEG_FLAGS_HEAP, 94, "dcp-seg4", "region94", "apple,asc-mem"},
    {SEG_FLAGS_HEAP, 95, "dcp-seg5", "region95", "apple,asc-mem"},
    {SEG_FLAGS_HEAP, 14, "framebuffer", "framebuffer", "framebuffer"},
    {SEG_FLAGS_HEAP, 157, "dcp-seg7", "region157", "apple,asc-mem"},
};

#define DCP6_OSLOG_NODE "dcp-oslog"
#define DCP6_OSLOG_PROP "apple,dcp-os-log"
#define DCP6_MARKER     "apple,t6030-handoff"

static const struct {
    const char *owner;
    const char *nub;
} dcp6_nubs[] = {
    {"dcp", "/arm-io/dcp/iop-dcp-nub"},
    {"dcpext0", "/arm-io/dcpext0/iop-dcpext0-nub"},
    {"dcpext1", "/arm-io/dcpext1/iop-dcpext1-nub"},
};
#define DCP6_NUBS ARRAY_SIZE(dcp6_nubs)

struct dcp6_seg {
    u64 phys;
    u64 size; /* rounded up to 16K */
    u64 remap;
    u32 flags;
};

struct dcp6_run {
    u64 dva; /* 36-bit DART address */
    u64 phys;
    u64 size;
    int seg;
};

struct dcp6_sid {
    const char *what;
    int sid;
    bool ok;
    int nruns;
    struct dcp6_run runs[DCP6_MAX_RUNS];
};

/* Per-kboot state of the always-on part, used by the armed part. Reset on every call. */
static struct {
    bool mmio_ok;       /* display domain on and the register reads below did not fault */
    bool dcp_running;   /* ASC CPU_CONTROL RUN */
    bool dcp_locked;    /* dart-dcp PROTECT_TTBR_TCR set (by iBoot or by us) */
    bool disp_locked;   /* dart-disp0 PROTECT_TTBR_TCR set */
    bool segs_ok;       /* iop-dcp-nub segments parsed and checked */
    struct dcp6_seg segs[DCP6_SEGS];
    struct dcp6_sid sid[3]; /* dcp SID 5, display SID 0, piodma SID 4 */
} dcp6;

/* ---- small helpers ---- */

static bool dcp6_overlaps(u64 a, u64 a_size, u64 b, u64 b_size)
{
    u64 a_end = a + a_size < a ? ~0UL : a + a_size;
    u64 b_end = b + b_size < b ? ~0UL : b + b_size;

    return a_size && b_size && a < b_end && b < a_end;
}

static int dcp6_adt_u32(int node, const char *name, u32 *val)
{
    u32 len = 0;
    const void *p = adt_getprop(adt, node, name, &len);
    if (!p || len != sizeof(*val))
        return -1;
    memcpy(val, p, sizeof(*val));
    return 0;
}

static int dcp6_adt_u64(int node, const char *name, u64 *val)
{
    u32 len = 0;
    const void *p = adt_getprop(adt, node, name, &len);
    if (!p || len != sizeof(*val))
        return -1;
    memcpy(val, p, sizeof(*val));
    return 0;
}

static int dcp6_adt_u64_pair(int node, const char *name, u64 *a, u64 *b)
{
    u64 v[2];
    u32 len = 0;
    const void *p = adt_getprop(adt, node, name, &len);
    if (!p || len != sizeof(v))
        return -1;
    memcpy(v, p, sizeof(v));
    *a = v[0];
    *b = v[1];
    return 0;
}

/* reg[idx] of an ADT node, with the /arm-io ranges applied */
static int dcp6_adt_reg(const char *path, int idx, u64 *addr)
{
    int adt_path[8];
    int node = adt_path_offset_trace(adt, path, adt_path);
    if (node < 0)
        return -1;
    return adt_get_reg(adt, adt_path, "reg", idx, addr, NULL);
}

static int dcp6_dram(u64 *base, u64 *size)
{
    int chosen = adt_path_offset(adt, "/chosen");
    if (chosen < 0 || dcp6_adt_u64(chosen, "dram-base", base) ||
        dcp6_adt_u64(chosen, "dram-size", size) || !*size || *base + *size <= *base)
        bail("DCP: T6030: no usable /chosen dram-base/dram-size\n");
    return 0;
}

/* Name number idx in a ';'-separated ADT segment-names list */
static bool dcp6_seg_name_is(const char *names, u32 len, int idx, const char *want)
{
    u32 i = 0;
    while (idx--) {
        while (i < len && names[i] && names[i] != ';')
            i++;
        if (i >= len || names[i] != ';')
            return false;
        i++;
    }
    size_t n = strlen(want);
    return i + n <= len && !memcmp(names + i, want, n) &&
           (i + n == len || names[i + n] == ';' || names[i + n] == 0);
}

static int dcp6_guard_begin(void)
{
    int before = exc_count;
    exc_guard = GUARD_MARK | GUARD_SILENT;
    return before;
}

/* Returns -1 if a synchronous abort or an SError was taken since dcp6_guard_begin() */
static int dcp6_guard_end(int before)
{
    sysop("dsb sy");
    sysop("isb");
    exc_guard = GUARD_OFF;
    return exc_count == before ? 0 : -1;
}

/* Whether [base, base+size) overlaps an enabled /reserved-memory node or a /memreserve/ entry */
static int dcp6_resv_conflict(void *dt, u64 base, u64 size, const char *what)
{
    int n = fdt_num_mem_rsv(dt);
    for (int i = 0; i < n; i++) {
        u64 a, s;
        if (!fdt_get_mem_rsv(dt, i, &a, &s) && dcp6_overlaps(base, size, a, s))
            bail("DCP: T6030: %s 0x%lx+0x%lx overlaps /memreserve/ 0x%lx+0x%lx\n", what, base, size,
                 a, s);
    }

    int resv = fdt_path_offset(dt, "/reserved-memory");
    if (resv < 0)
        return 0;
    int node;
    fdt_for_each_subnode(node, dt, resv)
    {
        const char *status = fdt_getprop(dt, node, "status", NULL);
        if (status && !strcmp(status, "disabled"))
            continue;
        int len;
        const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
        if (!reg || len <= 0)
            continue;
        for (int k = 0; (k + 2) * (int)sizeof(fdt64_t) <= len; k += 2) {
            u64 a = fdt64_ld(&reg[k]), s = fdt64_ld(&reg[k + 1]);
            if (dcp6_overlaps(base, size, a, s)) {
                const char *name = fdt_get_name(dt, node, NULL);
                bail("DCP: T6030: %s 0x%lx+0x%lx overlaps /reserved-memory/%s\n", what, base, size,
                     name ? name : "?");
            }
        }
    }
    return 0;
}

static int dcp6_carveout_conflict(u64 base, u64 size, const char *what)
{
    for (size_t j = 0; j < mcc_carveout_count; j++)
        if (dcp6_overlaps(base, size, mcc_carveouts[j].base, mcc_carveouts[j].size))
            bail("DCP: T6030: %s 0x%lx+0x%lx overlaps memory carveout 0x%lx+0x%lx\n", what, base,
                 size, mcc_carveouts[j].base, mcc_carveouts[j].size);
    return 0;
}

/*
 * Existing /reserved-memory/<name> with exactly this no-map reg: returns its offset (the
 * reservation is already there), -FDT_ERR_NOTFOUND if there is no such node, or -FDT_ERR_EXISTS
 * if a node of that name exists with other contents (or the lookup failed).
 */
static int dcp6_find_resv(void *dt, const char *name, u64 base, u64 size)
{
    int resv = fdt_path_offset(dt, "/reserved-memory");
    if (resv < 0)
        return -FDT_ERR_EXISTS;
    int node = fdt_subnode_offset(dt, resv, name);
    if (node == -FDT_ERR_NOTFOUND)
        return node;
    if (node < 0)
        return -FDT_ERR_EXISTS;
    int len;
    const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
    if (!reg || len != 2 * sizeof(fdt64_t) || fdt64_ld(&reg[0]) != base ||
        fdt64_ld(&reg[1]) != size || !fdt_getprop(dt, node, "no-map", NULL))
        return -FDT_ERR_EXISTS;
    return node;
}

/* Add /reserved-memory/<name> (reg, no-map, phandle). Returns its phandle, 0 on failure. */
static u32 dcp6_add_resv(void *dt, const char *name, u64 base, u64 size, const char *compat)
{
    fdt64_t reg[2];
    u32 phandle;

    int resv = fdt_path_offset(dt, "/reserved-memory");
    if (resv < 0)
        return 0;
    int node = fdt_add_subnode(dt, resv, name);
    if (node < 0)
        return 0;
    fdt64_st(&reg[0], base);
    fdt64_st(&reg[1], size);
    int err = fdt_generate_phandle(dt, &phandle);
    if (!err)
        err = fdt_setprop_u32(dt, node, "phandle", phandle);
    if (!err)
        err = fdt_setprop(dt, node, "reg", reg, sizeof(reg));
    if (!err && compat)
        err = fdt_setprop_string(dt, node, "compatible", compat);
    if (!err)
        err = fdt_setprop_empty(dt, node, "no-map");
    return err ? 0 : phandle;
}

/* A scratch copy of the DT, for an all-or-nothing edit */
static void *dcp6_scratch(void *dt)
{
    int size = fdt_totalsize(dt);
    void *scratch = malloc(size);
    if (!scratch) {
        printf("DCP: T6030: no memory for a DT copy (%d bytes)\n", size);
        return NULL;
    }
    memcpy(scratch, dt, size);
    return scratch;
}

/* Replace dt by scratch (same buffer size) if it is a valid DT; frees scratch either way */
static int dcp6_commit(void *dt, void *scratch)
{
    int ret = -1;
    if (fdt_check_header(scratch) == 0 && fdt_totalsize(scratch) == fdt_totalsize(dt)) {
        memcpy(dt, scratch, fdt_totalsize(scratch));
        ret = 0;
    } else {
        printf("DCP: T6030: DT copy is not valid, nothing changed\n");
    }
    free(scratch);
    return ret;
}

/* ---- always: the __OS_LOG reservations ---- */

/*
 * The __OS_LOG segment of one nub: flags 0xa, named __OS_LOG, remap == phys (a physical buffer),
 * 16K aligned, inside DRAM. Returns 0 and the reservation (size rounded up to 16K).
 */
static int dcp6_get_oslog(const char *owner, const char *nub_path, u64 *phys, u64 *size)
{
    int nub = adt_path_offset(adt, nub_path);
    if (nub < 0)
        bail("DCP: T6030: %s: no %s\n", owner, nub_path);

    u32 len = 0, names_len = 0;
    const struct adt_segment_ranges *seg = adt_getprop(adt, nub, "segment-ranges", &len);
    const char *names = adt_getprop(adt, nub, "segment-names", &names_len);
    if (!seg || !len || len % sizeof(*seg) || len > 16 * sizeof(*seg) || !names)
        bail("DCP: T6030: %s: missing or bad segment-ranges/-names (len %u)\n", owner, len);

    int found = -1;
    for (u32 i = 0; i < len / sizeof(*seg); i++) {
        struct adt_segment_ranges r;
        memcpy(&r, &seg[i], sizeof(r));
        if (r.unk != SEG_FLAGS_OSLOG)
            continue;
        if (found >= 0)
            bail("DCP: T6030: %s: more than one __OS_LOG segment\n", owner);
        found = i;
        if (!dcp6_seg_name_is(names, names_len, i, "__OS_LOG"))
            bail("DCP: T6030: %s: segment %u has __OS_LOG flags but another name\n", owner, i);
        if (r.remap != r.phys)
            bail("DCP: T6030: %s: __OS_LOG remap 0x%lx is not its physical address 0x%lx\n", owner,
                 r.remap, r.phys);
        *phys = r.phys;
        *size = ALIGN_UP((u64)r.size, SZ_16K);
    }
    if (found < 0)
        bail("DCP: T6030: %s: no __OS_LOG segment\n", owner);

    u64 dram_base, dram_size;
    if (dcp6_dram(&dram_base, &dram_size))
        return -1;
    if (!*phys || !*size || *size > DCP6_OSLOG_MAX || (*phys & (SZ_16K - 1)) ||
        *phys < dram_base || *phys + *size > dram_base + dram_size)
        bail("DCP: T6030: %s: bad __OS_LOG 0x%lx+0x%lx\n", owner, *phys, *size);
    return 0;
}

static void dcp6_reserve_oslogs(void *dt)
{
    struct {
        u64 phys, size;
        bool add;
        char name[DCP6_NAME_LEN];
    } logs[DCP6_NUBS];
    int nadd = 0, have = 0;

    for (size_t i = 0; i < DCP6_NUBS; i++) {
        logs[i].add = false;
        if (dcp6_get_oslog(dcp6_nubs[i].owner, dcp6_nubs[i].nub, &logs[i].phys, &logs[i].size))
            continue;
        snprintf(logs[i].name, sizeof(logs[i].name), DCP6_OSLOG_NODE "@%lx", logs[i].phys);

        int existing = dcp6_find_resv(dt, logs[i].name, logs[i].phys, logs[i].size);
        if (existing >= 0) {
            have++;
            continue;
        }
        if (existing != -FDT_ERR_NOTFOUND) {
            printf("DCP: T6030: %s: /reserved-memory/%s exists with other contents\n",
                   dcp6_nubs[i].owner, logs[i].name);
            continue;
        }

        bool bad = false;
        for (size_t j = 0; j < i; j++)
            if (logs[j].add && dcp6_overlaps(logs[i].phys, logs[i].size, logs[j].phys, logs[j].size)) {
                printf("DCP: T6030: %s: __OS_LOG overlaps %s's\n", dcp6_nubs[i].owner,
                       dcp6_nubs[j].owner);
                bad = true;
            }
        if (bad || dcp6_resv_conflict(dt, logs[i].phys, logs[i].size, "__OS_LOG") ||
            dcp6_carveout_conflict(logs[i].phys, logs[i].size, "__OS_LOG"))
            continue;
        logs[i].add = true;
        nadd++;
    }

    if (!nadd) {
        if (have)
            printf("DCP: T6030: %d __OS_LOG reservation(s) already present\n", have);
        return;
    }

    void *scratch = dcp6_scratch(dt);
    if (!scratch)
        return;
    for (size_t i = 0; i < DCP6_NUBS; i++) {
        if (!logs[i].add)
            continue;
        u32 ph = dcp6_add_resv(scratch, logs[i].name, logs[i].phys, logs[i].size, NULL);
        int resv = fdt_path_offset(scratch, "/reserved-memory");
        int node = resv < 0 ? resv : fdt_subnode_offset(scratch, resv, logs[i].name);
        if (!ph || node < 0 || fdt_setprop_empty(scratch, node, DCP6_OSLOG_PROP)) {
            printf("DCP: T6030: could not add the __OS_LOG reservations (DT full?), none added\n");
            free(scratch);
            return;
        }
    }
    if (dcp6_commit(dt, scratch))
        return;

    for (size_t i = 0; i < DCP6_NUBS; i++)
        if (logs[i].add)
            printf("DCP: T6030: %s __OS_LOG reserved: /reserved-memory/%s (0x%lx+0x%lx, no-map)\n",
                   dcp6_nubs[i].owner, logs[i].name, logs[i].phys, logs[i].size);
}

/* ---- always: DCP CPU state, DART lock state, dart-dcp lock ---- */

static void dcp6_check_hw(void)
{
    u64 coproc, dart_dcp, dart_disp;
    int node = adt_path_offset(adt, "/arm-io/dart-dcp");
    int node2 = adt_path_offset(adt, "/arm-io/dart-disp0");

    if (node < 0 || node2 < 0 || !adt_is_compatible(adt, node, "dart,t8110") ||
        !adt_is_compatible(adt, node2, "dart,t8110") || dcp6_adt_reg("/arm-io/dcp", 0, &coproc) ||
        dcp6_adt_reg("/arm-io/dart-dcp", 0, &dart_dcp) ||
        dcp6_adt_reg("/arm-io/dart-disp0", 0, &dart_disp) || coproc != DCP6_COPROC_BASE ||
        dart_dcp != DCP6_DART_DCP_BASE || dart_disp != DCP6_DART_DISP_BASE) {
        printf("DCP: T6030: ADT dcp/dart-dcp/dart-disp0 not as expected, not touching them\n");
        return;
    }

    /*
     * The DCP blocks are only accessed while the display power domain is on: actually on, or
     * targeted on with automatic power management (which ungates on access)
     */
    int g = dcp6_guard_begin();
    u32 ps = read32(DCP6_PMGR_DISP_CPU);
    bool on = FIELD_GET(PMGR_PS_ACTUAL, ps) == PMGR_PS_ACTIVE ||
              (FIELD_GET(PMGR_PS_TARGET, ps) == PMGR_PS_ACTIVE && (ps & PMGR_AUTO_ENABLE));
    if (dcp6_guard_end(g) || !on) {
        printf("DCP: T6030: display power domain not active (0x%x), not touching the DCP\n", ps);
        return;
    }

    g = dcp6_guard_begin();
    u32 ctl = read32(DCP6_COPROC_BASE + ASC_CPU_CONTROL);
    u32 status = read32(DCP6_COPROC_BASE + ASC_CPU_STATUS);
    u32 prot_dcp = read32(DCP6_DART_DCP_BASE + DART_T8110_PROTECT);
    u32 prot_disp = read32(DCP6_DART_DISP_BASE + DART_T8110_PROTECT);
    if (dcp6_guard_end(g)) {
        printf("DCP: T6030: reading the DCP/DART registers faulted, not touching them again\n");
        return;
    }

    dcp6.mmio_ok = true;
    dcp6.dcp_running = ctl & ASC_CPU_CONTROL_RUN;
    dcp6.disp_locked = prot_disp & DART_T8110_PROTECT_TTBR_TCR;
    printf("DCP: T6030: CPU_CONTROL 0x%x (%s) STATUS 0x%x; PROTECT dart-dcp 0x%x dart-disp0 0x%x\n",
           ctl, dcp6.dcp_running ? "running" : "NOT running", status, prot_dcp, prot_disp);

    if (prot_dcp & DART_T8110_PROTECT_TTBR_TCR) {
        dcp6.dcp_locked = true;
        return;
    }
    if (!dcp6.dcp_running) {
        printf("DCP: T6030: DCP not running, leaving dart-dcp unlocked\n");
        return;
    }

    g = dcp6_guard_begin();
    write32(DCP6_DART_DCP_BASE + DART_T8110_PROTECT, prot_dcp | DART_T8110_PROTECT_TTBR_TCR);
    u32 after = read32(DCP6_DART_DCP_BASE + DART_T8110_PROTECT);
    if (dcp6_guard_end(g) || !(after & DART_T8110_PROTECT_TTBR_TCR)) {
        printf("DCP: T6030: locking dart-dcp failed (PROTECT 0x%x)\n", after);
        return;
    }
    dcp6.dcp_locked = true;
    printf("DCP: T6030: dart-dcp locked (PROTECT 0x%x)\n", after);
}

/* ---- always: firmware segments of the internal DCP ---- */

static int dcp6_get_segs(void)
{
    int nub = adt_path_offset(adt, dcp6_nubs[0].nub);
    if (nub < 0)
        bail("DCP: T6030: no %s\n", dcp6_nubs[0].nub);

    u32 len = 0, names_len = 0;
    const struct adt_segment_ranges *seg = adt_getprop(adt, nub, "segment-ranges", &len);
    const char *names = adt_getprop(adt, nub, "segment-names", &names_len);
    if (!seg || len != DCP6_SEGS * sizeof(*seg) || !names)
        bail("DCP: T6030: dcp segment-ranges missing or not %d entries (len %u)\n", DCP6_SEGS, len);
    /* Segment 0 (text) is checked by its flags below */
    if (!dcp6_seg_name_is(names, names_len, SEG_DATA, "__DATA") ||
        !dcp6_seg_name_is(names, names_len, SEG_OSLOG, "__OS_LOG"))
        bail("DCP: T6030: dcp segments 1 and 2 are not named __DATA and __OS_LOG\n");

    u64 dram_base, dram_size;
    if (dcp6_dram(&dram_base, &dram_size))
        return -1;
    u64 ram_min = kboot_ram_base();
    u64 ram_max = kboot_ram_base() + kboot_ram_size();

    int cmap = adt_path_offset(adt, "/chosen/carveout-memory-map");
    if (cmap < 0)
        bail("DCP: T6030: no /chosen/carveout-memory-map\n");

    for (int i = 0; i < DCP6_SEGS; i++) {
        struct adt_segment_ranges r;
        struct dcp6_seg *s = &dcp6.segs[i];
        const struct dcp6_seg_def *d = &dcp6_seg_defs[i];

        memcpy(&r, &seg[i], sizeof(r));
        s->phys = r.phys;
        s->size = ALIGN_UP((u64)r.size, SZ_16K);
        s->remap = r.remap;
        s->flags = r.unk;

        if (s->flags != d->flags)
            bail("DCP: T6030: dcp segment %d flags 0x%x, expected 0x%x\n", i, s->flags, d->flags);
        if (!r.size || s->size > DCP6_SEG_MAX_SIZE || (s->phys & (SZ_16K - 1)) ||
            s->phys < dram_base || s->phys + s->size < s->phys ||
            s->phys + s->size > dram_base + dram_size)
            bail("DCP: T6030: bad dcp segment %d 0x%lx+0x%x\n", i, s->phys, r.size);

        if (i == SEG_OSLOG) {
            if (s->remap != s->phys)
                bail("DCP: T6030: dcp __OS_LOG remap 0x%lx is not physical\n", s->remap);
        } else {
            /* A DVA in the DARTs' 1 TiB alias, inside the 36-bit space */
            u64 dva = s->remap - DCP6_DART_VM_BASE;
            if (s->remap < DCP6_DART_VM_BASE || (s->remap & (SZ_16K - 1)) ||
                dva > DART_DVA_MASK || dva + s->size > DART_DVA_MASK + 1)
                bail("DCP: T6030: dcp segment %d remap 0x%lx is not a DART alias address\n", i,
                     s->remap);
            /* carveouts are outside the RAM the OS gets */
            if (!(s->phys + s->size <= ram_min || s->phys >= ram_max))
                bail("DCP: T6030: dcp segment %d 0x%lx+0x%lx is in usable RAM\n", i, s->phys,
                     s->size);
            if (dcp6_carveout_conflict(s->phys, s->size, "dcp segment"))
                return -1;

            /* The matching carveout: same base, same (16K rounded) size */
            char prop[32];
            u64 cbase, csize;
            snprintf(prop, sizeof(prop), "region-id-%d", d->region_id);
            if (dcp6_adt_u64_pair(cmap, prop, &cbase, &csize) || cbase != s->phys ||
                csize != s->size)
                bail("DCP: T6030: dcp segment %d does not match carveout %s\n", i, prop);
        }

        for (int j = 0; j < i; j++) {
            if (dcp6_overlaps(s->phys, s->size, dcp6.segs[j].phys, dcp6.segs[j].size))
                bail("DCP: T6030: dcp segments %d and %d overlap\n", j, i);
            if (i != SEG_OSLOG && j != SEG_OSLOG &&
                dcp6_overlaps(s->remap, s->size, dcp6.segs[j].remap, dcp6.segs[j].size))
                bail("DCP: T6030: dcp segments %d and %d overlap in DVA space\n", j, i);
        }
    }

    /* The framebuffer segment is /vram, where iBoot's boot framebuffer lives */
    u64 vram;
    if (dcp6_adt_reg("/vram", 0, &vram) || vram != dcp6.segs[SEG_VRAM].phys ||
        kboot_fb_base() < dcp6.segs[SEG_VRAM].phys ||
        kboot_fb_base() >= dcp6.segs[SEG_VRAM].phys + dcp6.segs[SEG_VRAM].size)
        bail("DCP: T6030: /vram or the boot framebuffer is not dcp segment %d\n", SEG_VRAM);

    return 0;
}

/* ---- always: read-only page-table walk ---- */

static int dcp6_seg_of(u64 phys)
{
    for (int i = 0; i < DCP6_SEGS; i++)
        if (phys >= dcp6.segs[i].phys && phys < dcp6.segs[i].phys + dcp6.segs[i].size)
            return i;
    return -1;
}

/* Record one valid PTE. Runs are contiguous in DVA and PA, and never span two segments. */
static int dcp6_run_add(struct dcp6_sid *s, u64 dva, u64 pa)
{
    int seg = dcp6_seg_of(pa);
    if (seg < 0)
        bail("DCP: T6030: %s SID %d maps DVA 0x%lx to 0x%lx, outside the firmware segments\n",
             s->what, s->sid, dva, pa);

    if (s->nruns) {
        struct dcp6_run *r = &s->runs[s->nruns - 1];
        if (r->seg == seg && r->dva + r->size == dva && r->phys + r->size == pa) {
            r->size += DART_PAGE;
            return 0;
        }
    }
    if (s->nruns >= DCP6_MAX_RUNS)
        bail("DCP: T6030: %s SID %d has too many mappings\n", s->what, s->sid);
    s->runs[s->nruns++] = (struct dcp6_run){dva, pa, DART_PAGE, seg};
    return 0;
}

static int dcp6_walk_inner(struct dcp6_sid *s, u64 base, u64 pt_lo, u64 pt_hi)
{
    u32 tcr = read32(base + DART_T8110_TCR(s->sid));
    u32 ttbr = read32(base + DART_T8110_TTBR(s->sid));

    if (!(tcr & DART_T8110_TCR_TRANSLATE) ||
        (tcr & (DART_T8110_TCR_BYPASS_DART | DART_T8110_TCR_FOUR_LEVEL)) ||
        !(ttbr & DART_T8110_TTBR_VALID))
        bail("DCP: T6030: %s SID %d: TCR 0x%x TTBR 0x%x not a two-level translation\n", s->what,
             s->sid, tcr, ttbr);

    u64 l1 = (u64)FIELD_GET(DART_T8110_TTBR_ADDR, ttbr) << DART_T8110_TTBR_SHIFT;
    if (l1 < pt_lo || l1 + DART_PAGE > pt_hi)
        bail("DCP: T6030: %s SID %d: L1 table 0x%lx outside pt-region 0x%lx..0x%lx\n", s->what,
             s->sid, l1, pt_lo, pt_hi);

    for (u64 i = 0; i < DART_ENTRIES; i++) {
        u64 e1 = read64(l1 + 8 * i);
        if (!(e1 & DART_PTE_VALID))
            continue;
        u64 l2 = (e1 & DART_PTE_ADDR) << DART_PTE_SHIFT;
        if (l2 < pt_lo || l2 + DART_PAGE > pt_hi)
            bail("DCP: T6030: %s SID %d: L2 table 0x%lx outside pt-region\n", s->what, s->sid, l2);

        for (u64 j = 0; j < DART_ENTRIES; j++) {
            u64 e2 = read64(l2 + 8 * j);
            if (!(e2 & DART_PTE_VALID))
                continue;
            u64 dva = (i << DART_L1_SHIFT) | (j << DART_L2_SHIFT);
            if (dcp6_run_add(s, dva, (e2 & DART_PTE_ADDR) << DART_PTE_SHIFT))
                return -1;
        }
    }
    return 0;
}

/*
 * Walk one stream's page tables. The TTBR/TCR reads and the table reads are guarded, and a table
 * is only read after its address was found inside the ADT pt-region of that stream, which must be
 * a 16K-aligned DRAM range outside usable RAM and outside the protected carveouts.
 */
static int dcp6_walk(struct dcp6_sid *s, const char *adt_dart, u64 base)
{
    char prop[24];
    u64 pt_lo, pt_hi, vm_base, vm_size;

    s->nruns = 0;
    int node = adt_path_offset(adt, adt_dart);
    snprintf(prop, sizeof(prop), "pt-region-%d", s->sid);
    if (node < 0 || dcp6_adt_u64_pair(node, prop, &pt_lo, &pt_hi))
        bail("DCP: T6030: %s: no %s/%s\n", s->what, adt_dart, prop);
    if (dcp6_adt_u64(node, "vm-base", &vm_base) || dcp6_adt_u64(node, "vm-size", &vm_size) ||
        vm_base != DCP6_DART_VM_BASE || vm_size != DCP6_DART_VM_SIZE)
        bail("DCP: T6030: %s: %s vm-base/vm-size not 0x%lx/0x%lx\n", s->what, adt_dart,
             DCP6_DART_VM_BASE, DCP6_DART_VM_SIZE);

    u64 dram_base, dram_size;
    u64 ram_min = kboot_ram_base();
    u64 ram_max = kboot_ram_base() + kboot_ram_size();
    if (dcp6_dram(&dram_base, &dram_size))
        return -1;
    if (pt_hi <= pt_lo || pt_hi - pt_lo > DART_PT_MAX_SIZE || ((pt_lo | pt_hi) & (SZ_16K - 1)) ||
        pt_lo < dram_base || pt_hi > dram_base + dram_size || !(pt_hi <= ram_min || pt_lo >= ram_max))
        bail("DCP: T6030: %s: bad %s 0x%lx..0x%lx\n", s->what, prop, pt_lo, pt_hi);
    if (dcp6_carveout_conflict(pt_lo, pt_hi - pt_lo, prop))
        return -1;

    int g = dcp6_guard_begin();
    int ret = dcp6_walk_inner(s, base, pt_lo, pt_hi);
    if (dcp6_guard_end(g))
        bail("DCP: T6030: %s SID %d: reading the page tables faulted\n", s->what, s->sid);
    return ret;
}

/*
 * dcp (SID 5) must map exactly the non-log segments, each at its ADT remap address and in full.
 * display (SID 0) and piodma (SID 4) may map only the heap segments (flags 2), each at most once,
 * in full, starting at the segment base, so that one iommu-addresses entry per mapping describes
 * it; the display must map the framebuffer segment.
 */
static int dcp6_check_sid(struct dcp6_sid *s)
{
    bool seen[DCP6_SEGS] = {false};

    for (int i = 0; i < s->nruns; i++) {
        struct dcp6_run *r = &s->runs[i];
        struct dcp6_seg *g = &dcp6.segs[r->seg];

        if (r->seg == SEG_OSLOG)
            bail("DCP: T6030: %s SID %d maps the __OS_LOG buffer\n", s->what, s->sid);
        if (s->sid != DCP6_SID_DCP && g->flags != SEG_FLAGS_HEAP)
            bail("DCP: T6030: %s SID %d maps dcp segment %d\n", s->what, s->sid, r->seg);
        if (seen[r->seg])
            bail("DCP: T6030: %s SID %d maps dcp segment %d twice\n", s->what, s->sid, r->seg);
        seen[r->seg] = true;
        if (r->phys != g->phys || r->size != g->size)
            bail("DCP: T6030: %s SID %d maps 0x%lx+0x%lx of dcp segment %d (0x%lx+0x%lx)\n",
                 s->what, s->sid, r->phys, r->size, r->seg, g->phys, g->size);
        if (s->sid == DCP6_SID_DCP && r->dva != g->remap - DCP6_DART_VM_BASE)
            bail("DCP: T6030: dcp SID %d maps segment %d at DVA 0x%lx, ADT says 0x%lx\n", s->sid,
                 r->seg, r->dva, g->remap);
    }

    if (s->sid == DCP6_SID_DCP) {
        for (int i = 0; i < DCP6_SEGS; i++)
            if (i != SEG_OSLOG && !seen[i])
                bail("DCP: T6030: dcp SID %d does not map dcp segment %d\n", s->sid, i);
    } else if (s->sid == DCP6_SID_DISP && !seen[SEG_VRAM]) {
        bail("DCP: T6030: display SID %d does not map the framebuffer\n", s->sid);
    }
    return 0;
}

static void dcp6_dry_run(void)
{
    static const struct {
        const char *what;
        int sid;
        const char *dart;
        u64 base;
    } sids[3] = {
        {"dcp", DCP6_SID_DCP, "/arm-io/dart-dcp", DCP6_DART_DCP_BASE},
        {"display", DCP6_SID_DISP, "/arm-io/dart-disp0", DCP6_DART_DISP_BASE},
        {"piodma", DCP6_SID_PIODMA, "/arm-io/dart-disp0", DCP6_DART_DISP_BASE},
    };

    dcp6.segs_ok = !dcp6_get_segs();
    if (!dcp6.segs_ok || !dcp6.mmio_ok)
        return;

    for (int i = 0; i < 3; i++) {
        struct dcp6_sid *s = &dcp6.sid[i];
        s->what = sids[i].what;
        s->sid = sids[i].sid;
        s->ok = !dcp6_walk(s, sids[i].dart, sids[i].base) && !dcp6_check_sid(s);
        if (!s->ok)
            continue;
        u64 pages = 0;
        for (int j = 0; j < s->nruns; j++)
            pages += s->runs[j].size / DART_PAGE;
        printf("DCP: T6030: %s SID %d: %d mappings, %lu pages, all in firmware segments (", s->what,
               s->sid, s->nruns, pages);
        for (int j = 0; j < s->nruns; j++)
            printf("%s%d@0x%lx", j ? " " : "", s->runs[j].seg, s->runs[j].dva);
        printf(")\n");
    }
}

/* ---- armed: the handoff ---- */

struct dcp6_dt {
    char dcp[DCP6_NAME_LEN * 2];
    char display[DCP6_NAME_LEN * 2];
    char piodma[DCP6_NAME_LEN * 2];
};

static int dcp6_status_disabled(void *dt, int node, const char *what)
{
    const char *status = fdt_getprop(dt, node, "status", NULL);
    if (!status || strcmp(status, "disabled"))
        bail("DCP: T6030: %s is not status \"disabled\"\n", what);
    return 0;
}

/* Parent of `node` has 2 address and 2 size cells */
static int dcp6_cells_2_2(void *dt, int node, const char *what)
{
    int parent = fdt_parent_offset(dt, node);
    if (parent < 0 || fdt_address_cells(dt, parent) != 2 || fdt_size_cells(dt, parent) != 2)
        bail("DCP: T6030: %s: parent does not have 2 address and 2 size cells\n", what);
    return 0;
}

/*
 * iommus = <&dart sid> with a disabled t8110 DART at `base` whose apple,dma-range is the ADT
 * DVA alias. Returns the DART's offset.
 */
static int dcp6_check_iommu(void *dt, int node, const char *what, u32 sid, u64 base)
{
    int len;
    const fdt32_t *iommus = fdt_getprop(dt, node, "iommus", &len);
    if (!iommus || len != 2 * sizeof(fdt32_t) || fdt32_ld(&iommus[1]) != sid)
        bail("DCP: T6030: %s iommus is not <&dart %u>\n", what, sid);

    int dart = fdt_node_offset_by_phandle(dt, fdt32_ld(&iommus[0]));
    if (dart < 0 || fdt_node_check_compatible(dt, dart, "apple,t8110-dart"))
        bail("DCP: T6030: %s: iommus does not point to a t8110 DART\n", what);

    const fdt32_t *cells = fdt_getprop(dt, dart, "#iommu-cells", &len);
    if (!cells || len != sizeof(fdt32_t) || fdt32_ld(cells) != 1)
        bail("DCP: T6030: %s: DART #iommu-cells is not 1\n", what);

    const fdt64_t *reg = fdt_getprop(dt, dart, "reg", &len);
    if (dcp6_cells_2_2(dt, dart, what) || !reg || len < (int)(2 * sizeof(fdt64_t)) ||
        fdt64_ld(&reg[0]) != base)
        bail("DCP: T6030: %s: DART reg is not 0x%lx\n", what, base);

    const fdt32_t *range = fdt_getprop(dt, dart, "apple,dma-range", &len);
    if (!range || len != 4 * sizeof(fdt32_t) ||
        (((u64)fdt32_ld(&range[0]) << 32) | fdt32_ld(&range[1])) != DCP6_DART_VM_BASE ||
        (((u64)fdt32_ld(&range[2]) << 32) | fdt32_ld(&range[3])) != DCP6_DART_VM_SIZE)
        bail("DCP: T6030: %s: DART apple,dma-range is not <0x%lx 0x%lx>\n", what,
             DCP6_DART_VM_BASE, DCP6_DART_VM_SIZE);

    if (dcp6_status_disabled(dt, dart, "the DART of the display nodes"))
        return -1;
    return dart;
}

/* Node for alias `name`, and its path. Returns the offset or a negative error. */
static int dcp6_alias(void *dt, const char *name, char *path, size_t len)
{
    const char *p = fdt_get_alias(dt, name);
    if (!p)
        return -FDT_ERR_NOTFOUND;
    int node = fdt_path_offset(dt, p);
    if (node < 0 || fdt_get_path(dt, node, path, len))
        return -FDT_ERR_BADPATH;
    return node;
}

static const char *const dcp6_new_props[] = {
    "memory-region", "memory-region-names", DCP6_MARKER, "apple,firmware-uuid",
};

/* The DT side of the gates: nodes, aliases, cells, iommus, DARTs, status, nothing published yet */
static int dcp6_check_dt(void *dt, struct dcp6_dt *n)
{
    if (fdt_node_check_compatible(dt, 0, "apple,j516s") &&
        fdt_node_check_compatible(dt, 0, "apple,j514s"))
        bail("DCP: T6030: not a J516S/J514S device tree\n");

    char p1[sizeof(n->piodma)], p2[sizeof(n->piodma)];
    int dcp = dcp6_alias(dt, "dcp", n->dcp, sizeof(n->dcp));
    int disp = dcp6_alias(dt, "disp0", n->display, sizeof(n->display));
    int pio1 = dcp6_alias(dt, "disp0-piodma", p1, sizeof(p1));
    int pio2 = dcp6_alias(dt, "disp0_piodma", p2, sizeof(p2));
    if (dcp < 0 || disp < 0 || (pio1 < 0 && pio2 < 0))
        bail("DCP: T6030: device tree has no dcp/disp0/disp0-piodma aliases\n");
    if (pio1 >= 0 && pio2 >= 0 && pio1 != pio2)
        bail("DCP: T6030: disp0-piodma and disp0_piodma aliases differ\n");
    int pio = pio1 >= 0 ? pio1 : pio2;
    memcpy(n->piodma, pio1 >= 0 ? p1 : p2, sizeof(n->piodma));

    if (fdt_node_check_compatible(dt, dcp, "apple,t6030-dcp") ||
        fdt_node_check_compatible(dt, disp, "apple,t6030-display-subsystem"))
        bail("DCP: T6030: dcp/disp0 are not apple,t6030-dcp/apple,t6030-display-subsystem\n");
    if (fdt_parent_offset(dt, pio) != dcp)
        bail("DCP: T6030: piodma is not a child of dcp\n");

    if (dcp6_cells_2_2(dt, dcp, "dcp") || dcp6_cells_2_2(dt, disp, "display") ||
        dcp6_cells_2_2(dt, pio, "piodma"))
        return -1;
    int resv = fdt_path_offset(dt, "/reserved-memory");
    if (resv < 0 || fdt_address_cells(dt, resv) != 2 || fdt_size_cells(dt, resv) != 2)
        bail("DCP: T6030: /reserved-memory missing or not 2+2 cells\n");

    if (dcp6_status_disabled(dt, dcp, "dcp") || dcp6_status_disabled(dt, disp, "display"))
        return -1;
    int dart_dcp = dcp6_check_iommu(dt, dcp, "dcp", DCP6_SID_DCP, DCP6_DART_DCP_BASE);
    int dart_disp = dcp6_check_iommu(dt, disp, "display", DCP6_SID_DISP, DCP6_DART_DISP_BASE);
    int dart_pio = dcp6_check_iommu(dt, pio, "piodma", DCP6_SID_PIODMA, DCP6_DART_DISP_BASE);
    if (dart_dcp < 0 || dart_disp < 0 || dart_pio < 0)
        return -1;
    if (dart_disp != dart_pio || dart_dcp == dart_disp)
        bail("DCP: T6030: display and piodma must share one DART, dcp another\n");

    int len;
    const fdt32_t *mbox = fdt_getprop(dt, dcp, "mboxes", &len);
    int mb = mbox && len == sizeof(fdt32_t) ? fdt_node_offset_by_phandle(dt, fdt32_ld(mbox)) : -1;
    if (mb < 0 || dcp6_status_disabled(dt, mb, "the dcp mailbox"))
        bail("DCP: T6030: dcp mailbox missing or enabled\n");

    const int nodes[3] = {dcp, disp, pio};
    for (int i = 0; i < 3; i++)
        for (size_t j = 0; j < ARRAY_SIZE(dcp6_new_props); j++)
            if (fdt_getprop(dt, nodes[i], dcp6_new_props[j], NULL))
                bail("DCP: T6030: %s already has %s\n", i == 0 ? "dcp" : i == 1 ? "display" : "piodma",
                     dcp6_new_props[j]);
    return 0;
}

/* The ADT and hardware side of the gates */
static int dcp6_check_adt(void)
{
    if (chip_id != T6030)
        bail("DCP: T6030: chip 0x%x is not T6030\n", chip_id);
    if (os_firmware.version != V14_7)
        bail("DCP: T6030: OS firmware %s is not 14.7\n", os_firmware.string);

    int nub = adt_path_offset(adt, dcp6_nubs[0].nub);
    u32 len = 0, preloaded, running;
    const char *uuid = nub < 0 ? NULL : adt_getprop(adt, nub, "uuid", &len);
    if (!uuid || len != sizeof(DCP6_UUID) || memcmp(uuid, DCP6_UUID, sizeof(DCP6_UUID)))
        bail("DCP: T6030: dcp firmware is not image " DCP6_UUID "\n");
    if (dcp6_adt_u32(nub, "pre-loaded", &preloaded) || preloaded != 1 ||
        dcp6_adt_u32(nub, "running", &running) || running != 1)
        bail("DCP: T6030: iBoot did not leave the dcp firmware loaded and running\n");

    if (!dcp6.mmio_ok || !dcp6.dcp_running || !dcp6.dcp_locked || !dcp6.disp_locked)
        bail("DCP: T6030: DCP not running or its DARTs not locked\n");
    if (!dcp6.segs_ok)
        bail("DCP: T6030: dcp firmware segments not usable\n");
    for (int i = 0; i < 3; i++)
        if (!dcp6.sid[i].ok)
            bail("DCP: T6030: %s page tables not validated\n", dcp6.sid[i].what);
    return 0;
}

/* Phandle of the node at `path` in dt, adding one if it has none; 0 on failure */
static u32 dcp6_phandle(void *dt, const char *path)
{
    int node = fdt_path_offset(dt, path);
    if (node < 0)
        return 0;
    u32 ph = fdt_get_phandle(dt, node);
    if (ph)
        return ph;
    if (fdt_generate_phandle(dt, &ph))
        return 0;
    node = fdt_path_offset(dt, path);
    if (node < 0 || fdt_setprop_u32(dt, node, "phandle", ph))
        return 0;
    return ph;
}

static int dcp6_append_iommu_addr(void *dt, int node, u32 phandle, u64 dva, u64 size)
{
    fdt32_t tuple[5];
    tuple[0] = cpu_to_fdt32(phandle);
    fdt64_st(&tuple[1], DCP6_DART_VM_BASE | dva);
    fdt64_st(&tuple[3], size);
    return fdt_appendprop(dt, node, "iommu-addresses", tuple, sizeof(tuple));
}

/* The run of stream `sid_idx` that maps segment `seg`, or NULL */
static const struct dcp6_run *dcp6_run_of(int sid_idx, int seg)
{
    for (int i = 0; i < dcp6.sid[sid_idx].nruns; i++)
        if (dcp6.sid[sid_idx].runs[i].seg == seg)
            return &dcp6.sid[sid_idx].runs[i];
    return NULL;
}

static int dcp6_set_list(void *dt, const char *path, const fdt32_t *ph, int count,
                         const char *names, int names_len)
{
    int node = fdt_path_offset(dt, path);
    if (node < 0)
        return -1;
    if (count && fdt_setprop(dt, node, "memory-region", ph, count * sizeof(*ph)))
        return -1;
    node = fdt_path_offset(dt, path);
    if (count && (node < 0 || fdt_setprop(dt, node, "memory-region-names", names, names_len)))
        return -1;
    node = fdt_path_offset(dt, path);
    if (node < 0 || fdt_setprop_u32(dt, node, DCP6_MARKER, 1))
        return -1;
    return 0;
}

/* Write the whole handoff into `dt` (a scratch copy). Any failure leaves it half-written. */
static int dcp6_publish(void *dt, const struct dcp6_dt *n)
{
    u32 ph_dcp = dcp6_phandle(dt, n->dcp);
    u32 ph_disp = dcp6_phandle(dt, n->display);
    u32 ph_pio = dcp6_phandle(dt, n->piodma);
    if (!ph_dcp || !ph_disp || !ph_pio)
        return -1;

    fdt32_t mr[3][DCP6_SEGS];
    char names[3][DCP6_SEGS * 16];
    int count[3] = {0}, names_len[3] = {0};

    for (int i = 0; i < DCP6_SEGS; i++) {
        const struct dcp6_seg_def *d = &dcp6_seg_defs[i];
        const struct dcp6_seg *s = &dcp6.segs[i];
        char name[DCP6_NAME_LEN];
        u32 ph;

        snprintf(name, sizeof(name), "%s@%lx", d->node, s->phys);
        if (i == SEG_OSLOG) {
            /* Added by the always-on part of this kboot */
            int resv = fdt_path_offset(dt, "/reserved-memory");
            int node = resv < 0 ? resv : fdt_subnode_offset(dt, resv, name);
            ph = node < 0 ? 0 : fdt_get_phandle(dt, node);
        } else {
            ph = dcp6_add_resv(dt, name, s->phys, s->size, d->compat);
        }
        if (!ph)
            return -1;

        int resv = fdt_path_offset(dt, "/reserved-memory");
        int node = resv < 0 ? resv : fdt_subnode_offset(dt, resv, name);
        if (node < 0)
            return -1;

        const struct dcp6_run *r;
        const u32 owner_ph[3] = {ph_dcp, ph_disp, ph_pio};
        for (int k = 0; k < 3; k++) {
            if (k == 0 && i == SEG_OSLOG)
                r = NULL; /* physical buffer: memory-region entry only, no DART address */
            else if (!(r = dcp6_run_of(k, i)))
                continue;
            if (r) {
                node = fdt_subnode_offset(dt, fdt_path_offset(dt, "/reserved-memory"), name);
                if (node < 0 || dcp6_append_iommu_addr(dt, node, owner_ph[k], r->dva, r->size))
                    return -1;
            }
            size_t nl = strlen(d->name) + 1;
            mr[k][count[k]++] = cpu_to_fdt32(ph);
            memcpy(names[k] + names_len[k], d->name, nl);
            names_len[k] += nl;
        }
    }

    if (dcp6_set_list(dt, n->dcp, mr[0], count[0], names[0], names_len[0]) ||
        dcp6_set_list(dt, n->display, mr[1], count[1], names[1], names_len[1]) ||
        dcp6_set_list(dt, n->piodma, mr[2], count[2], names[2], names_len[2]))
        return -1;

    int dcp = fdt_path_offset(dt, n->dcp);
    if (dcp < 0 || fdt_setprop_string(dt, dcp, "apple,firmware-uuid", DCP6_UUID))
        return -1;
    dcp = fdt_path_offset(dt, n->dcp);
    if (dcp < 0 || firmware_set_fdt(dt, dcp, "apple,firmware-version", &os_firmware))
        return -1;
    dcp = fdt_path_offset(dt, n->dcp);
    if (dcp < 0 || firmware_set_fdt(dt, dcp, "apple,firmware-compat", &os_firmware))
        return -1;
    return 0;
}

static bool dcp6_armed(void *dt)
{
    int chosen = fdt_path_offset(dt, "/chosen");
    int len = 0;
    const char *optin = chosen >= 0 ? fdt_getprop(dt, chosen, "asahi,t6030-dcp", &len) : NULL;
    return optin && len == 2 && !strcmp(optin, "1");
}

/* The segments' reservations: __OS_LOG's is in place, the others do not collide with anything */
static int dcp6_check_resv(void *dt)
{
    for (int i = 0; i < DCP6_SEGS; i++) {
        const struct dcp6_seg *s = &dcp6.segs[i];
        char name[DCP6_NAME_LEN];

        snprintf(name, sizeof(name), "%s@%lx", dcp6_seg_defs[i].node, s->phys);
        int node = dcp6_find_resv(dt, name, s->phys, s->size);
        if (i == SEG_OSLOG) {
            if (node < 0 || !fdt_get_phandle(dt, node) || !fdt_getprop(dt, node, DCP6_OSLOG_PROP, NULL))
                bail("DCP: T6030: dcp __OS_LOG is not reserved as /reserved-memory/%s\n", name);
        } else if (node != -FDT_ERR_NOTFOUND) {
            bail("DCP: T6030: /reserved-memory/%s already exists\n", name);
        } else if (dcp6_resv_conflict(dt, s->phys, s->size, "dcp segment")) {
            return -1;
        }
    }
    return 0;
}

static void dcp6_handoff(void *dt)
{
    struct dcp6_dt n;

    int dcp = fdt_path_offset(dt, "dcp");
    if (dcp >= 0 && fdt_getprop(dt, dcp, DCP6_MARKER, NULL)) {
        printf("DCP: T6030: handoff already published\n");
        return;
    }
    if (dcp6_check_adt() || dcp6_check_dt(dt, &n) || dcp6_check_resv(dt)) {
        printf("DCP: T6030: armed, but handoff NOT published; display nodes untouched\n");
        return;
    }

    void *scratch = dcp6_scratch(dt);
    if (!scratch)
        return;
    if (dcp6_publish(scratch, &n)) {
        printf("DCP: T6030: DT write failed (full?); handoff NOT published, nothing changed\n");
        free(scratch);
        return;
    }
    if (dcp6_commit(dt, scratch))
        return;
    printf("DCP: T6030: handoff published (firmware " DCP6_UUID ", %s); display nodes left "
           "disabled, marker " DCP6_MARKER " set\n",
           os_firmware.string);
}

/*
 * External firmware handoff is independent of the running panel transaction.
 * Only describe preloaded memory; do not access the external ASC or DART here.
 * The external node stays disabled until a kernel explicitly opts into it.
 */
#define DCPEXT6_MARKER "apple,t6030-dcpext-memory-ready"
#define DCPEXT6_SEGS 4

static int dcpext6_publish(void *dt)
{
    const char *alias = "dcpext0";
    const char *nub_path = "/arm-io/dcpext0/iop-dcpext0-nub";
    const u32 flags[DCPEXT6_SEGS] = {SEG_FLAGS_TEXT, SEG_FLAGS_DATA,
                                    SEG_FLAGS_OSLOG, SEG_FLAGS_HEAP};
    char path[96];
    int node = dcp6_alias(dt, alias, path, sizeof(path));
    if (node < 0 || dcp6_status_disabled(dt, node, alias) ||
        dcp6_cells_2_2(dt, node, alias))
        return -1;
    int panel = fdt_path_offset(dt, "dcp");
    if (panel < 0 || !fdt_getprop(dt, panel, DCP6_MARKER, NULL))
        bail("DCP: T6030: dcpext needs the validated panel handoff first\n");
    if (chip_id != T6030 || os_firmware.version != V14_7)
        return -1;
    if (fdt_getprop(dt, node, "memory-region", NULL) ||
        fdt_getprop(dt, node, DCPEXT6_MARKER, NULL))
        bail("DCP: T6030: dcpext already has memory metadata\n");

    u64 base;
    if (dcp6_adt_reg("/arm-io/dart-dcpext0", 0, &base) ||
        base != 0x2d130c000UL ||
        dcp6_check_iommu(dt, node, alias, DCP6_SID_DCP, base) < 0)
        return -1;
    const fdt64_t *reg;
    int len;
    reg = fdt_getprop(dt, node, "reg", &len);
    if (dcp6_adt_reg("/arm-io/dcpext0", 0, &base) ||
        base != 0x2d2c00000UL || !reg || len < 16 || fdt64_ld(reg) != base)
        bail("DCP: T6030: dcpext ASC address mismatch\n");

    int nub = adt_path_offset(adt, nub_path);
    if (nub < 0)
        return -1;
    u32 seglen = 0, nameslen = 0, uuidlen = 0, loaded;
    const struct adt_segment_ranges *ranges = adt_getprop(adt, nub, "segment-ranges", &seglen);
    const char *names = adt_getprop(adt, nub, "segment-names", &nameslen);
    const char *uuid = adt_getprop(adt, nub, "uuid", &uuidlen);
    if (nub < 0 || !ranges || seglen != DCPEXT6_SEGS * sizeof(*ranges) || !names ||
        !uuid || uuidlen != sizeof(DCP6_UUID) || memcmp(uuid, DCP6_UUID, sizeof(DCP6_UUID)) ||
        dcp6_adt_u32(nub, "pre-loaded", &loaded) || loaded != 1)
        bail("DCP: T6030: dcpext firmware metadata invalid\n");
    if (!dcp6_seg_name_is(names, nameslen, 1, "__DATA") ||
        !dcp6_seg_name_is(names, nameslen, 2, "__OS_LOG"))
        return -1;

    struct dcp6_seg segs[DCPEXT6_SEGS];
    u64 dram_base, dram_size;
    if (dcp6_dram(&dram_base, &dram_size))
        return -1;
    for (int i = 0; i < DCPEXT6_SEGS; i++) {
        struct adt_segment_ranges r;
        memcpy(&r, &ranges[i], sizeof(r));
        struct dcp6_seg *s = &segs[i];
        *s = (struct dcp6_seg){r.phys, ALIGN_UP((u64)r.size, SZ_16K), r.remap, r.unk};
        if (s->flags != flags[i] || !r.size || s->size > DCP6_SEG_MAX_SIZE ||
            (s->phys & (SZ_16K - 1)) || s->phys < dram_base ||
            s->phys + s->size < s->phys || s->phys + s->size > dram_base + dram_size)
            return -1;
        if (i == SEG_OSLOG) {
            if (s->remap != s->phys || s->size > DCP6_OSLOG_MAX)
                return -1;
        } else {
            if (s->remap < DCP6_DART_VM_BASE || (s->remap & (SZ_16K - 1)) ||
                s->remap - DCP6_DART_VM_BASE + s->size > DCP6_DART_VM_SIZE)
                return -1;
            /* Firmware is already excluded from the usable RAM window. */
            u64 ram_end = kboot_ram_base() + kboot_ram_size();
            if (!(s->phys + s->size <= kboot_ram_base() || s->phys >= ram_end) ||
                dcp6_carveout_conflict(s->phys, s->size, alias))
                return -1;
        }
        for (int j = 0; j < i; j++) {
            if (dcp6_overlaps(s->phys, s->size, segs[j].phys, segs[j].size) ||
                (i != SEG_OSLOG && j != SEG_OSLOG &&
                 dcp6_overlaps(s->remap, s->size, segs[j].remap, segs[j].size)))
                return -1;
        }
    }
    /* Only the immutable text may alias the panel's firmware allocation. */
    if (segs[0].phys != dcp6.segs[0].phys || segs[0].size != dcp6.segs[0].size ||
        segs[0].remap != dcp6.segs[0].remap)
        return -1;

    u32 owner = dcp6_phandle(dt, path);
    if (!owner)
        return -1;
    fdt32_t mr[DCPEXT6_SEGS];
    for (int i = 0; i < DCPEXT6_SEGS; i++) {
        struct dcp6_seg *s = &segs[i];
        char name[64];
        if (i == SEG_TEXT || i == SEG_OSLOG) {
            snprintf(name, sizeof(name), "%s@%lx", i == SEG_TEXT ? "dcp-text" : DCP6_OSLOG_NODE,
                     s->phys);
            node = dcp6_find_resv(dt, name, s->phys, s->size);
            if (node < 0 || (i == SEG_TEXT && fdt_node_check_compatible(dt, node, "apple,asc-mem")) ||
                (i == SEG_OSLOG && (!fdt_getprop(dt, node, DCP6_OSLOG_PROP, NULL) ||
                                    fdt_getprop(dt, node, "iommu-addresses", NULL))))
                return -1;
            mr[i] = cpu_to_fdt32(fdt_get_phandle(dt, node));
        } else {
            if (dcp6_resv_conflict(dt, s->phys, s->size, alias))
                return -1;
            snprintf(name, sizeof(name), "dcpext0-%s@%lx", i == SEG_DATA ? "data" : "heap", s->phys);
            mr[i] = cpu_to_fdt32(dcp6_add_resv(dt, name, s->phys, s->size, "apple,asc-mem"));
            node = fdt_node_offset_by_phandle(dt, fdt32_to_cpu(mr[i]));
        }
        if (!mr[i] || node < 0)
            return -1;
        if (i != SEG_OSLOG && dcp6_append_iommu_addr(dt, node, owner,
                                                   s->remap - DCP6_DART_VM_BASE, s->size))
            return -1;
    }
    static const char mr_names[] = "asc-firmware\0dcp_data\0os_log\0heap";
    node = fdt_path_offset(dt, path);
    if (node < 0 || fdt_setprop(dt, node, "memory-region", mr, sizeof(mr)) ||
        fdt_setprop(dt, node, "memory-region-names", mr_names, sizeof(mr_names)) ||
        fdt_setprop_string(dt, node, "apple,firmware-uuid", DCP6_UUID) ||
        firmware_set_fdt(dt, node, "apple,firmware-version", &os_firmware) ||
        firmware_set_fdt(dt, node, "apple,firmware-compat", &os_firmware) ||
        fdt_setprop_u32(dt, node, DCPEXT6_MARKER, 1))
        return -1;
    return 0;
}

static void dcpext6_handoff(void *dt)
{
    int chosen = fdt_path_offset(dt, "/chosen"), len = 0;
    const char *opt = chosen < 0 ? NULL : fdt_getprop(dt, chosen, "asahi,t6030-dcpext", &len);
    if (!opt || len != 2 || memcmp(opt, "1", 2))
        return;
    void *scratch = dcp6_scratch(dt);
    if (!scratch)
        return;
    if (dcpext6_publish(scratch)) {
        printf("DCP: T6030: dcpext memory handoff rejected; panel handoff unchanged\n");
        free(scratch);
        return;
    }
    if (!dcp6_commit(dt, scratch))
        printf("DCP: T6030: dcpext memory handoff published; CPU untouched, node disabled\n");
}

#define DISPEXT6_BASE 0x2d1304000UL
#define DISPEXT6_MARKER "apple,t6030-dispext-handoff"
#define DISPEXT6_DART_PATH "/soc/iommu@2d1304000"
#define DISPEXT6_SCANOUT_PATH "/soc/dcp@2d2c00000/scanout"

struct dispext6_tables {
    u64 lo[2], hi[2];
    u32 tcr[2], ttbr[2];
};

/* Read only, including on refusal. Never restart or lock the display DART. */
static int dispext6_check_inner(struct dispext6_tables *t)
{
    const u32 sid[2] = {0, 4};
    /* Do not access the external display aperture unless its CPU domain is on. */
    if ((read32(0x3507003e0UL) & 0xf0) != 0xf0)
        return -1;
    if (!(read32(DISPEXT6_BASE + DART_T8110_PROTECT) & DART_T8110_PROTECT_TTBR_TCR))
        return -1;
    for (int i = 0; i < 2; i++) {
        t->tcr[i] = read32(DISPEXT6_BASE + DART_T8110_TCR(sid[i]));
        t->ttbr[i] = read32(DISPEXT6_BASE + DART_T8110_TTBR(sid[i]));
        if (!(t->tcr[i] & DART_T8110_TCR_TRANSLATE) ||
            (t->tcr[i] & (DART_T8110_TCR_BYPASS_DART | DART_T8110_TCR_FOUR_LEVEL)) ||
            !(t->ttbr[i] & DART_T8110_TTBR_VALID))
            return -1;
        u64 root = (u64)FIELD_GET(DART_T8110_TTBR_ADDR, t->ttbr[i]) << DART_T8110_TTBR_SHIFT;
        if (root < t->lo[i] || root + DART_PAGE > t->hi[i])
            return -1;
        /* Refuse even invalid-but-nonzero entries: ownership must be unambiguous. */
        for (u32 n = 0; n < DART_ENTRIES; n++)
            if (read64(root + 8 * n))
                return -1;
    }
    return 0;
}

static int dispext6_publish(void *dt)
{
    struct dispext6_tables t;
    const char *adt_path = "/arm-io/dart-dispext0";
    const char *pt_names[2] = {"pt-region-0", "pt-region-4"};
    const char *ext_path = "/soc/dcp@2d2c00000";
    u64 base, vm_base, vm_size, dram_base, dram_size;
    u32 irq;
    int a = adt_path_offset(adt, adt_path);
    int ext = fdt_path_offset(dt, ext_path), len;
    const fdt32_t *ready = ext < 0 ? NULL : fdt_getprop(dt, ext, DCPEXT6_MARKER, &len);
    if (chip_id != T6030 || os_firmware.version != V14_7 || a < 0 || ext < 0 ||
        !ready || len != 4 || fdt32_ld(ready) != 1 ||
        dcp6_status_disabled(dt, ext, "dcpext0") ||
        fdt_path_offset(dt, DISPEXT6_DART_PATH) != -FDT_ERR_NOTFOUND ||
        fdt_path_offset(dt, DISPEXT6_SCANOUT_PATH) != -FDT_ERR_NOTFOUND)
        return -1;
    if (dcp6_adt_reg(adt_path, 0, &base) || base != DISPEXT6_BASE ||
        dcp6_adt_reg(adt_path, 1, &base) || base != 0x2d1300000UL ||
        dcp6_adt_u32(a, "interrupts", &irq) || irq != 651 ||
        dcp6_adt_u64(a, "vm-base", &vm_base) || vm_base != DCP6_DART_VM_BASE ||
        dcp6_adt_u64(a, "vm-size", &vm_size) || vm_size != DCP6_DART_VM_SIZE ||
        dcp6_dram(&dram_base, &dram_size))
        return -1;
    for (int i = 0; i < 2; i++) {
        if (dcp6_adt_u64_pair(a, pt_names[i], &t.lo[i], &t.hi[i]) || t.hi[i] <= t.lo[i] ||
            t.hi[i] - t.lo[i] > DART_PT_MAX_SIZE || ((t.lo[i] | t.hi[i]) & (DART_PAGE - 1)) ||
            t.lo[i] < dram_base || t.hi[i] > dram_base + dram_size ||
            dcp6_overlaps(t.lo[i], t.hi[i] - t.lo[i], kboot_ram_base(), kboot_ram_size()) ||
            dcp6_carveout_conflict(t.lo[i], t.hi[i] - t.lo[i], pt_names[i]) ||
            dcp6_resv_conflict(dt, t.lo[i], t.hi[i] - t.lo[i], pt_names[i]))
            return -1;
    }
    if (dcp6_overlaps(t.lo[0], t.hi[0] - t.lo[0], t.lo[1], t.hi[1] - t.lo[1]))
        return -1;

    int fw = dcp6_check_iommu(dt, ext, "dcpext0", 5, 0x2d130c000UL);
    if (fw < 0)
        return -1;
    /* Copy only validated local interrupt-controller and power-domain references. */
    const fdt32_t *prop = fdt_getprop(dt, fw, "interrupt-parent", &len);
    if (!prop || len != 4)
        return -1;
    u32 aic = fdt32_ld(prop);
    int parent = fdt_node_offset_by_phandle(dt, aic);
    if (parent < 0 || !fdt_getprop(dt, parent, "interrupt-controller", NULL))
        return -1;
    prop = fdt_getprop(dt, parent, "#interrupt-cells", &len);
    if (!prop || len != 4 || fdt32_ld(prop) != 3)
        return -1;
    prop = fdt_getprop(dt, fw, "power-domains", &len);
    if (!prop || len != 4)
        return -1;
    u32 pd = fdt32_ld(prop);
    parent = fdt_node_offset_by_phandle(dt, pd);
    const char *label = parent < 0 ? NULL : fdt_getprop(dt, parent, "label", &len);
    if (!label || len != sizeof("dispext0_cpu") || memcmp(label, "dispext0_cpu", len))
        return -1;
    if (parent != fdt_path_offset(dt, "/soc/power-management@350700000/power-controller@3e0") ||
        fdt_node_check_compatible(dt, parent, "apple,t6030-pmgr-pwrstate"))
        return -1;
    prop = fdt_getprop(dt, parent, "#power-domain-cells", &len);
    if (!prop || len != 4 || fdt32_ld(prop))
        return -1;
    prop = fdt_getprop(dt, parent, "reg", &len);
    if (!prop || len != 8 || fdt32_ld(prop) != 0x3e0 || fdt32_ld(prop + 1) != 4)
        return -1;

    int guard = dcp6_guard_begin();
    int ret = dispext6_check_inner(&t);
    if (dcp6_guard_end(guard) || ret)
        return -1;

    fdt32_t mr[2], state[6];
    for (int i = 0; i < 2; i++) {
        char name[80];
        snprintf(name, sizeof(name), "dispext0-pt%d@%lx", i ? 4 : 0, t.lo[i]);
        u32 ph = dcp6_add_resv(dt, name, t.lo[i], t.hi[i] - t.lo[i], NULL);
        if (!ph)
            return -1;
        mr[i] = cpu_to_fdt32(ph);
        state[3 * i] = cpu_to_fdt32(i ? 4 : 0);
        state[3 * i + 1] = cpu_to_fdt32(t.tcr[i]);
        state[3 * i + 2] = cpu_to_fdt32(t.ttbr[i]);
    }
    int soc = fdt_path_offset(dt, "/soc");
    int dart = fdt_add_subnode(dt, soc, "iommu@2d1304000");
    const char compat[] = "apple,t6030-dart\0apple,t8110-dart";
    const char names[] = "sid0-page-tables\0sid4-page-tables";
    fdt64_t reg[] = {cpu_to_fdt64(DISPEXT6_BASE), cpu_to_fdt64(0x4000)};
    fdt64_t range[] = {cpu_to_fdt64(vm_base), cpu_to_fdt64(vm_size)};
    fdt32_t interrupts[] = {cpu_to_fdt32(0), cpu_to_fdt32(irq), cpu_to_fdt32(4)};
    if (dart < 0 || fdt_setprop(dt, dart, "compatible", compat, sizeof(compat)) ||
        fdt_setprop_string(dt, dart, "status", "disabled") ||
        fdt_setprop_u32(dt, dart, "#iommu-cells", 1) ||
        fdt_setprop(dt, dart, "reg", reg, sizeof(reg)) ||
        fdt_setprop(dt, dart, "apple,dma-range", range, sizeof(range)) ||
        fdt_setprop_u32(dt, dart, "interrupt-parent", aic) ||
        fdt_setprop(dt, dart, "interrupts", interrupts, sizeof(interrupts)) ||
        fdt_setprop_u32(dt, dart, "power-domains", pd) ||
        fdt_setprop(dt, dart, "memory-region", mr, sizeof(mr)) ||
        fdt_setprop(dt, dart, "memory-region-names", names, sizeof(names)) ||
        fdt_setprop(dt, dart, "apple,inherited-dart-state", state, sizeof(state)) ||
        fdt_setprop_u32(dt, dart, DISPEXT6_MARKER, 1))
        return -1;
    u32 ph = dcp6_phandle(dt, DISPEXT6_DART_PATH);
    ext = fdt_path_offset(dt, ext_path);
    int scanout = fdt_add_subnode(dt, ext, "scanout");
    fdt32_t iommu[] = {cpu_to_fdt32(ph), cpu_to_fdt32(0)};
    if (!ph || scanout < 0 ||
        fdt_setprop_string(dt, scanout, "compatible", "apple,t6030-dispext-scanout") ||
        fdt_setprop_string(dt, scanout, "status", "disabled") ||
        fdt_setprop_u32(dt, scanout, "#address-cells", 2) ||
        fdt_setprop_u32(dt, scanout, "#size-cells", 2) ||
        fdt_setprop(dt, scanout, "iommus", iommu, sizeof(iommu)) ||
        fdt_setprop_u32(dt, scanout, DISPEXT6_MARKER, 1))
        return -1;
    return 0;
}

static void dispext6_handoff(void *dt)
{
    int chosen = fdt_path_offset(dt, "/chosen"), len;
    const char *opt = chosen < 0 ? NULL : fdt_getprop(dt, chosen, "asahi,t6030-dispext", &len);
    if (!opt || len != 2 || memcmp(opt, "1", 2))
        return;
    void *scratch = dcp6_scratch(dt);
    if (!scratch)
        return;
    if (dispext6_publish(scratch)) {
        printf("DCP: T6030: display DART handoff rejected; existing handoffs unchanged\n");
        free(scratch);
        return;
    }
    if (!dcp6_commit(dt, scratch))
        printf("DCP: T6030: empty external display DART handoff published; hardware untouched\n");
}

/* Always returns 0: nothing here may stop the boot */
int dt_set_display_t6030(void *dt)
{
    memset(&dcp6, 0, sizeof(dcp6));

    dcp6_reserve_oslogs(dt);
    dcp6_check_hw();
    dcp6_dry_run();

    if (dcp6_armed(dt))
        dcp6_handoff(dt);
    else
        printf("DCP: T6030: not armed (no chosen.asahi,t6030-dcp=1): no display handoff\n");
    dcpext6_handoff(dt);
    dispext6_handoff(dt);
    return 0;
}
