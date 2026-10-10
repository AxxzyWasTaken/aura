/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef DAPF_H
#define DAPF_H

int dapf_init_all(void);
int dapf_init_fdt(const void *fdt);
int dapf_init(const char *path, int index);

#endif
