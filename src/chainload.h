/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef __CHAINLOAD_H__
#define __CHAINLOAD_H__

#include "types.h"

int chainload_image(void *base, size_t size, char **vars, size_t var_cnt);
int chainload_load(const char *spec, char **vars, size_t *var_cnt, size_t var_capacity);

#endif
