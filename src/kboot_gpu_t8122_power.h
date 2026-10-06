/* SPDX-License-Identifier: MIT */
#ifndef KBOOT_GPU_T8122_POWER_H
#define KBOOT_GPU_T8122_POWER_H

#include "types.h"

enum t8122_identity_result {
    T8122_ID_NOT_RUN,
    T8122_ID_PASS,
    T8122_ID_POWER_FAILED,
    T8122_ID_READ_FAULT,
    T8122_ID_MISMATCH,
    T8122_ID_POWER_OFF_FAILED,
};

struct t8122_powered_identity {
    enum t8122_identity_result result;
    u32 id_version;
    u32 core_mask[3];
    u32 id_counts_1; /* reported only; not part of the result */
};

struct t8122_power_probe_ops {
    int (*enable)(void *context);
    int (*read)(void *context, u32 *id_version, u32 core_mask[3], u32 *id_counts_1);
    int (*disable)(void *context);
};

struct t8122_powered_identity t8122_power_probe_run(const struct t8122_power_probe_ops *ops,
                                                     void *context);
bool t8122_power_probe_opted_in(const void *dt);

#endif
