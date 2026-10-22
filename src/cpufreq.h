/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef CPUFREQ_H
#define CPUFREQ_H

#include "types.h"

int cpufreq_init(void);
void cpufreq_fixup(void);
void cpufreq_payload_boost(bool enable);

/* Explicit T8152 firmware requests; no voltage/PLL initialization. */
u64 cpufreq_get_cluster_hz(unsigned int cluster);
int cpufreq_set_cluster_pstate(unsigned int cluster, unsigned int pstate);

#endif
