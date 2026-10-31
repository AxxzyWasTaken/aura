/* SPDX-License-Identifier: MIT */

#ifndef PMU_COUNTER_H
#define PMU_COUNTER_H

#include "types.h"

int pmu_clear_panic_counter(void);
int pmu_read_boot_record(u8 *counts, u8 *error_stage);

#endif
