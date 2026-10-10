/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef BOOT_STORAGE_H
#define BOOT_STORAGE_H

struct kernel_header;

int boot_storage_load(const char *spec, struct kernel_header **kernel, void **fdt);

#endif
