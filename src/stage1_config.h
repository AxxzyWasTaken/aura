/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef STAGE1_CONFIG_H
#define STAGE1_CONFIG_H

#include "types.h"

u32 stage1_config_window_ms(void);

/*
 * A proxy request (the aura-proxy boot argument, or an aura-proxy file at the root of the ESP
 * Stage 1 boots from) holds Stage 1 for a host this long, then boots as normal.
 */
#define STAGE1_PROXY_REQUEST_MS 15000
bool stage1_proxy_request_window(void);
/*
 * After a boot that stopped before an OS started (iBoot's PMU record), Stage 1 holds this long
 * for a host before it boots its Stage 2 again, so a bad Stage 2 can be replaced over USB.
 */
#define STAGE1_RECOVERY_WINDOW_MS 10000
bool stage1_recovery_window(void);
/* True when an earlier aura stage started this one (it handed its NVMe session over). */
extern bool stage1_chained;
const char *stage1_config_target(void);
const char *stage1_config_esp_uuid(void);

#endif
