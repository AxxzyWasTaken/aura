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
const char *stage1_config_target(void);
const char *stage1_config_esp_uuid(void);

#endif
