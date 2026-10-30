/* SPDX-License-Identifier: GPL-2.0-only */

#include "stage1_proxy.h"

void stage1_proxy_window_init(struct stage1_proxy_window *window, u32 timeout_ms)
{
    window->timeout_ms = timeout_ms;
    window->kis_only = false;
}

enum stage1_proxy_result stage1_proxy_window_step(struct stage1_proxy_window *window,
                                                  u32 elapsed_ms, bool cdc_ready, bool cdc_failed,
                                                  bool carrier_taken, bool cdc_sync, bool kis_sync)
{
    if (cdc_failed) {
        if (carrier_taken)
            return STAGE1_PROXY_CDC_FAILED;
        window->kis_only = true;
    }

    if (!window->kis_only && cdc_ready && cdc_sync)
        return STAGE1_PROXY_CDC;

    if (!carrier_taken && kis_sync)
        return STAGE1_PROXY_KIS;

    if (elapsed_ms >= window->timeout_ms)
        return STAGE1_PROXY_TIMEOUT;

    return STAGE1_PROXY_WAIT;
}

enum stage1_payload_action stage1_proxy_payload_action(enum stage1_proxy_result window_result,
                                                       bool payload_valid, bool cdc_live,
                                                       bool kis_live)
{
    if (payload_valid)
        return STAGE1_PAYLOAD_NATIVE;

    if (window_result == STAGE1_PROXY_CDC_FAILED)
        return STAGE1_PAYLOAD_HALT;
    if (cdc_live)
        return STAGE1_PAYLOAD_CDC_FALLBACK;
    if (kis_live)
        return STAGE1_PAYLOAD_KIS_FALLBACK;
    return STAGE1_PAYLOAD_HALT;
}

bool stage1_proxy_action_needs_cleanup(enum stage1_payload_action action)
{
    return action == STAGE1_PAYLOAD_NATIVE;
}

/*
 * The previous boot ended without a clean shutdown before an OS started: a Stage 2 or a later
 * loader that hangs, faults or is forced off. Linux clears the counts once it runs, so this holds
 * for one boot after each such failure.
 */
bool stage1_previous_boot_stopped_early(u8 counts, u8 error_stage)
{
    return (counts & 0xf) && error_stage < STAGE1_BOOT_STAGE_KERNEL_STARTED;
}
