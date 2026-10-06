/* SPDX-License-Identifier: MIT */

#include <stdarg.h>

#include "kboot_display_facts.h"
#include "adt.h"
#include "malloc.h"
#include "string.h"
#include "types.h"
#include "utils.h"
#include "vsprintf.h"

#include "libfdt/libfdt.h"

/*
 * T6030 (M3 Pro): the ADT's description of the external display blocks and the Type-C PHYs, for
 * Linux-side inspection, as /chosen/asahi,t6030-display-facts. It is not a device binding and no
 * driver reads it. adt,* properties are raw ADT bytes (little-endian cells); translated-reg (an
 * address and a size per reg entry, translated through the bus ranges) and adt-property-sizes are
 * big-endian cells.
 *
 *   atc-phy0 .. atc-phy3   compatible, reg, interrupts, every property name and size, and the
 *                          value of each property whose name mentions DP, HDMI or AUX
 *                          (up to 256 bytes; larger ones by name and size only)
 *   dcpext0, dcpext1, dispext0, dispext1, dart-dcpext0, dart-dcpext1, dart-dispext0,
 *   dart-dispext1          compatible, reg, interrupts
 *   any /arm-io node whose name contains xbar or crossbar
 *                          as the PHYs, with every value up to 64 bytes
 *   any /arm-io node whose name contains hdmi (dp2hdmi-gpio0)
 *                          every property name and size, and every value up to 256 bytes
 *   any /arm-io node whose name contains dptx
 *                          compatible, reg, interrupts, every property name and size
 *   skipped                what was absent or over a limit
 *
 * ADT memory only: no device register is read or written. Bounded: at most four levels below
 * /arm-io and 4096 nodes are visited, eight nodes per kind found by name, 512 property names per
 * node, 16 translated reg entries per node and 40 KiB of names and values in all. Every ADT value
 * is length-checked before it is copied. The node is built on a copy of the FDT and published
 * only if complete; an existing node is never replaced, and a failure leaves the FDT as it was
 * without stopping the boot.
 */

#define FACTS_NODE   "asahi,t6030-display-facts"
#define FACTS_SCHEMA 1
#define LOG          "ADT facts: T6030: "

#define BUDGET       (40 * 1024)
#define MAX_NAMES    512
#define MAX_REGS     16
#define MAX_FOUND    8
#define MAX_SKIPS    32
#define WALK_DEPTH   4
#define WALK_NODES   4096
#define BASIC_MAX    1024 /* compatible, reg, interrupts */
#define ADT_NAME_LEN 32   /* the ADT name field */
#define PATH_LEN     176  /* "/arm-io" and four names of up to 31 characters, with separators */
#define TRACE_LEN    9    /* adt_path_offset_trace() writes at most 8 entries */

/* Which property values a node exports, besides compatible, reg and interrupts */
enum values {
    V_NONE,   /* none */
    V_DP,     /* names mentioning DP, HDMI or AUX, up to 256 bytes */
    V_SMALL,  /* every value up to 64 bytes */
    V_ALL,    /* every value up to 256 bytes */
    V_NOLIST, /* none, and no property names */
};

/* Nodes found by name below /arm-io */
enum kind { K_XBAR, K_HDMI, K_DPTX, K_NR };

struct dfacts {
    void *dt;
    int chosen; /* /chosen; its offset does not move as its own subtree grows */
    int err;    /* the first libfdt error */
    u32 bytes;  /* names and values written, against BUDGET */
    bool over;  /* BUDGET reached (reported once) */
    int skips;
    int visited;
    int found[K_NR];
};

/* ---- FDT writes, into the copy; the first error sticks ---- */

static int f_node(struct dfacts *f, const char *sub)
{
    int node;

    if (f->err)
        return -1;
    node = fdt_subnode_offset(f->dt, f->chosen, FACTS_NODE);
    if (node >= 0 && sub)
        node = fdt_subnode_offset(f->dt, node, sub);
    if (node < 0)
        f->err = node;
    return node;
}

static void f_set(struct dfacts *f, const char *sub, const char *name, const void *val, int len)
{
    int node = f_node(f, sub);
    int ret;

    if (node < 0)
        return;
    ret = fdt_setprop(f->dt, node, name, val, len);
    if (ret)
        f->err = ret;
}

static void f_append(struct dfacts *f, const char *sub, const char *name, const void *val, int len)
{
    int node = f_node(f, sub);
    int ret;

    if (node < 0)
        return;
    ret = fdt_appendprop(f->dt, node, name, val, len);
    if (ret)
        f->err = ret;
}

static void f_str(struct dfacts *f, const char *sub, const char *name, const char *s)
{
    f_set(f, sub, name, s, strlen(s) + 1);
}

static void __attribute__((format(printf, 2, 3))) f_skip(struct dfacts *f, const char *fmt, ...)
{
    char buf[160];
    va_list ap;

    if (f->skips >= MAX_SKIPS)
        return;
    f->skips++;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf(LOG "%s\n", buf);
    f_append(f, NULL, "skipped", buf, strlen(buf) + 1);
}

/* Takes n bytes of the budget, or reports (once) that it is used up */
static bool f_charge(struct dfacts *f, u32 n)
{
    if (n > BUDGET - min(f->bytes, (u32)BUDGET)) {
        if (!f->over)
            f_skip(f, "size budget of %u bytes reached; later names and values left out", BUDGET);
        f->over = true;
        return false;
    }
    f->bytes += n;
    return true;
}

/* ---- ADT names ---- */

/*
 * An ADT node or property name, NUL-terminated, if it is 1..31 printable characters terminated
 * within its field. Returns false otherwise.
 */
static bool name_ok(const char *s, size_t field, char *out)
{
    size_t n = strnlen(s, field);

    if (!n || n >= ADT_NAME_LEN || n == field)
        return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] < 0x21 || s[i] > 0x7e)
            return false;
    memcpy(out, s, n);
    out[n] = '\0';
    return true;
}

/* Whether a name is also usable in a device tree node name (`node`) or property name */
static bool fdt_name_ok(const char *s, bool node)
{
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == ',' || c == '.' || c == '_' || c == '+' || c == '-' || (!node && c == '#')))
            return false;
    }
    return true;
}

/* The name property of an ADT node, bounded by its length, if a device tree node may use it */
static bool node_name(int node, char *out)
{
    u32 len = 0;
    const char *s = adt_getprop(adt, node, "name", &len);

    return s && len && name_ok(s, min(len, (u32)ADT_NAME_LEN), out) && fdt_name_ok(out, true);
}

static bool contains(const char *s, const char *word)
{
    size_t n = strlen(word);

    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] && (s[i] | 0x20) == word[i])
            i++;
        if (i == n)
            return true;
    }
    return false;
}

static bool value_wanted(enum values v, const char *name, u32 size)
{
    if (!strcmp(name, "compatible") || !strcmp(name, "reg") || !strcmp(name, "interrupts"))
        return false; /* copied already */
    switch (v) {
        case V_DP:
            return size <= 256 &&
                   (contains(name, "dp") || contains(name, "hdmi") || contains(name, "aux"));
        case V_SMALL:
            return size <= 64;
        case V_ALL:
            return size <= 256;
        default:
            return false;
    }
}

/* ---- one node ---- */

/* adt,<name> = the ADT bytes, if the property exists and is at most max bytes */
static void copy_prop(struct dfacts *f, const char *sub, int node, const char *name, u32 max)
{
    char fname[ADT_NAME_LEN + 4];
    u32 len = 0;
    const void *p = adt_getprop(adt, node, name, &len);

    if (!p)
        return;
    if (len > max) {
        f_skip(f, "%s %s: %u bytes, over %u", sub, name, len, max);
        return;
    }
    if (!f_charge(f, len))
        return;
    snprintf(fname, sizeof(fname), "adt,%s", name);
    f_set(f, sub, fname, p, len);
}

static void export_node(struct dfacts *f, const char *sub, const char *path, enum values v)
{
    int trace[TRACE_LEN] = {0};
    int node = adt_path_offset_trace(adt, path, trace);
    int fdt;

    if (node < 0) {
        f_skip(f, "%s absent", path);
        return;
    }
    if (f->err)
        return;
    fdt = fdt_subnode_offset(f->dt, f_node(f, NULL), sub);
    if (fdt >= 0) {
        f_skip(f, "%s: a node named %s is exported already", path, sub);
        return;
    }
    fdt = fdt_add_subnode(f->dt, f_node(f, NULL), sub);
    if (fdt < 0) {
        if (!f->err)
            f->err = fdt;
        return;
    }
    f_str(f, sub, "adt-path", path);

    copy_prop(f, sub, node, "compatible", BASIC_MAX);
    copy_prop(f, sub, node, "reg", BASIC_MAX);
    copy_prop(f, sub, node, "interrupts", BASIC_MAX);
    if (adt_getprop(adt, node, "reg", NULL)) {
        for (int r = 0; r < MAX_REGS; r++) {
            u64 addr, size;
            fdt64_t cells[2];
            if (adt_get_reg(adt, trace, "reg", r, &addr, &size) || !f_charge(f, sizeof(cells)))
                break;
            cells[0] = cpu_to_fdt64(addr);
            cells[1] = cpu_to_fdt64(size);
            f_append(f, sub, "translated-reg", cells, sizeof(cells));
        }
    }
    if (v == V_NOLIST)
        return;

    int n = 0;
    ADT_FOREACH_PROPERTY(adt, node, prop)
    {
        char name[ADT_NAME_LEN];
        fdt32_t size = cpu_to_fdt32(prop->size);

        if (f->err || f->over)
            break;
        if (n++ >= MAX_NAMES) {
            f_skip(f, "%s: over %d properties, the rest left out", path, MAX_NAMES);
            break;
        }
        if (!name_ok(prop->name, sizeof(prop->name), name)) {
            f_skip(f, "%s: property %d has an unusable name", path, n - 1);
            continue;
        }
        if (!f_charge(f, strlen(name) + 1 + sizeof(size)))
            break;
        f_append(f, sub, "adt-property-names", name, strlen(name) + 1);
        f_append(f, sub, "adt-property-sizes", &size, sizeof(size));
        if (value_wanted(v, name, prop->size) && fdt_name_ok(name, false))
            copy_prop(f, sub, node, name, 256);
    }
}

/* ---- nodes found by name below /arm-io ---- */

static const struct {
    const char *words[2];
    const char *what;
    enum values v;
} kinds[K_NR] = {
    [K_XBAR] = {{"xbar", "crossbar"}, "crossbar", V_SMALL},
    [K_HDMI] = {{"hdmi", NULL}, "hdmi", V_ALL},
    [K_DPTX] = {{"dptx", NULL}, "dptx", V_NONE},
};

static void visit(struct dfacts *f, const char *path, const char *name)
{
    for (int k = 0; k < K_NR; k++) {
        bool match = false;
        for (int w = 0; w < 2; w++)
            match |= kinds[k].words[w] && contains(name, kinds[k].words[w]);
        if (!match)
            continue;
        if (f->found[k] >= MAX_FOUND) {
            if (f->found[k]++ == MAX_FOUND)
                f_skip(f, "over %d %s nodes; %s and later ones left out", MAX_FOUND, kinds[k].what,
                       path);
            return;
        }
        f->found[k]++;
        export_node(f, name, path, kinds[k].v);
        return;
    }
}

/*
 * Visits the children of `node` (at `path`) and their subtrees, up to WALK_DEPTH levels below
 * /arm-io. The next sibling is only looked up while one is left.
 */
static void walk(struct dfacts *f, int node, char *path, size_t len, int depth)
{
    int count = adt_get_child_count(adt, node);

    if (count <= 0 || depth > WALK_DEPTH)
        return;
    int child = adt_first_child_offset(adt, node);
    for (int i = 0; i < count && !f->err; i++) {
        char name[ADT_NAME_LEN];

        if (i)
            child = adt_next_sibling_offset(adt, child);
        if (child < 0)
            return;
        if (++f->visited > WALK_NODES) {
            if (f->visited == WALK_NODES + 1)
                f_skip(f, "over %d ADT nodes; the search stopped", WALK_NODES);
            return;
        }
        if (!node_name(child, name))
            continue;
        size_t n = strlen(name);
        if (len + 1 + n + 1 > PATH_LEN)
            continue;
        path[len] = '/';
        memcpy(path + len + 1, name, n + 1);
        visit(f, path, name);
        walk(f, child, path, len + 1 + n, depth + 1);
        path[len] = '\0';
    }
}

/* ---- the node ---- */

static const char *const phys[] = {"atc-phy0", "atc-phy1", "atc-phy2", "atc-phy3"};

static const char *const ext[] = {
    "dcpext0",      "dcpext1",      "dispext0",      "dispext1",
    "dart-dcpext0", "dart-dcpext1", "dart-dispext0", "dart-dispext1",
};

static void build(struct dfacts *f)
{
    fdt32_t schema = cpu_to_fdt32(FACTS_SCHEMA);
    char path[PATH_LEN];

    f_set(f, NULL, "schema-version", &schema, sizeof(schema));
    f_str(f, NULL, "encoding", "adt-raw-little-endian");

    for (size_t i = 0; i < ARRAY_SIZE(phys); i++) {
        snprintf(path, sizeof(path), "/arm-io/%s", phys[i]);
        export_node(f, phys[i], path, V_DP);
    }
    for (size_t i = 0; i < ARRAY_SIZE(ext); i++) {
        snprintf(path, sizeof(path), "/arm-io/%s", ext[i]);
        export_node(f, ext[i], path, V_NOLIST);
    }

    int arm_io = adt_path_offset(adt, "/arm-io");
    if (arm_io < 0) {
        f_skip(f, "/arm-io absent");
        return;
    }
    strcpy(path, "/arm-io");
    walk(f, arm_io, path, strlen(path), 1);
    for (int k = 0; k < K_NR; k++)
        if (!f->found[k])
            f_skip(f, "no /arm-io node named *%s*", kinds[k].what);
}

void dt_display_facts_t6030(void *dt)
{
    struct dfacts f = {0};
    int chosen = fdt_path_offset(dt, "/chosen");
    size_t size = fdt_totalsize(dt);
    void *copy;

    if (chosen < 0 || fdt_subnode_offset(dt, chosen, FACTS_NODE) >= 0)
        return;
    copy = malloc(size);
    if (!copy) {
        printf(LOG "/chosen/" FACTS_NODE " not published: no memory\n");
        return;
    }
    memcpy(copy, dt, size);

    f.dt = copy;
    f.chosen = chosen; /* the same offset in the copy */
    int node = fdt_add_subnode(copy, chosen, FACTS_NODE);
    if (node < 0)
        f.err = node;
    else
        build(&f);

    if (!f.err) {
        memcpy(dt, copy, size);
        printf(LOG "/chosen/" FACTS_NODE " published (%u bytes of names and values)\n", f.bytes);
    } else {
        printf(LOG "/chosen/" FACTS_NODE " not published: %s\n", fdt_strerror(f.err));
    }
    free(copy);
}
