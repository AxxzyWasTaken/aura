/* SPDX-License-Identifier: MIT */
/*
 * Host stand-in for src/utils.h, for tests/smbios_host.c: system registers come from the test
 * (host_sysreg), printf() goes to stdout.
 */
#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arm_cpu_regs.h"
#include "types.h"

#define ARRAY_SIZE(s) (sizeof(s) / sizeof((s)[0]))
#define min(a, b)     (((a) < (b)) ? (a) : (b))

u64 host_sysreg(const char *name);
#define mrs(reg) host_sysreg(#reg)

static inline int in_el2(void)
{
    return 1;
}

#endif
