/* SPDX-License-Identifier: MIT */

#include "kboot_gpu_t8122_power.h"
#include "libfdt/libfdt.h"

#define T8122_ID_GEN       7
#define T8122_ID_VARIANT   2
#define T8122_ID_CORE_MASK 0x3ffu

bool t8122_power_probe_opted_in(const void *dt)
{
    int chosen = fdt_path_offset(dt, "/chosen");
    int len = 0;
    const char *value = chosen < 0 ? NULL :
        fdt_getprop(dt, chosen, "asahi,t8122-gpu-power-diag", &len);
    return value && len == 2 && value[0] == '1' && value[1] == '\0';
}

struct t8122_powered_identity t8122_power_probe_run(const struct t8122_power_probe_ops *ops,
                                                     void *context)
{
    struct t8122_powered_identity identity = {.result = T8122_ID_POWER_FAILED};

    /* Disable even after a failed enable: PMGR can have made a partial vote. */
    if (!ops->enable(context)) {
        if (ops->read(context, &identity.id_version, identity.core_mask, &identity.id_counts_1,
                      &identity.id_counts_2)) {
            identity.result = T8122_ID_READ_FAULT;
        } else if ((identity.id_version >> 24) != T8122_ID_GEN ||
                   ((identity.id_version >> 16) & 0xff) != T8122_ID_VARIANT ||
                   !(identity.core_mask[0] & T8122_ID_CORE_MASK) ||
                   (identity.core_mask[0] & ~T8122_ID_CORE_MASK) ||
                   identity.core_mask[1] || identity.core_mask[2]) {
            identity.result = T8122_ID_MISMATCH;
        } else {
            identity.result = T8122_ID_PASS;
        }
    }
    if (ops->disable(context))
        identity.result = T8122_ID_POWER_OFF_FAILED;
    return identity;
}
