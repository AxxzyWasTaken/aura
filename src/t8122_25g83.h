/* SPDX-License-Identifier: MIT */
#ifndef T8122_25G83_H
#define T8122_25G83_H

#include <stdbool.h>
#include "libfdt/libfdt.h"

/*
 * The J615 (M3 Air 15") runs the J613's 25G83 DCP, PMP and GPU images: Apple's
 * 26.6.2 restore image has one entry for each, for both boards, and the J615's
 * ADT display, PMP and GPU nodes equal the J613's but for phandles. It is
 * still EXPERIMENTAL, not booted on a J615, so the display and GPU handoffs
 * take a J615 only when its owner set chosen.asahi,j615-25g83-experimental=1
 * in m1n1.conf. Linux checks the same switch again.
 */
#define T8122_25G83_J615_OPT_IN "asahi,j615-25g83-experimental"

/* A J613, or a J615 with the opt-in. `dt` has /chosen set already. */
static inline bool t8122_25g83_board(const void *dt)
{
    if (!fdt_node_check_compatible(dt, 0, "apple,j613"))
        return true;
    if (fdt_node_check_compatible(dt, 0, "apple,j615"))
        return false;
    int chosen = fdt_path_offset(dt, "/chosen"), len = 0;
    const char *optin =
        chosen < 0 ? NULL : fdt_getprop(dt, chosen, T8122_25G83_J615_OPT_IN, &len);
    return optin && len == 2 && optin[0] == '1' && !optin[1];
}

#endif
