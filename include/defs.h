/* SPDX-License-Identifier: GPL-3.0 */
#ifndef LB_DEFS_H
#define LB_DEFS_H

/*
 * Shared compile-time buffer capacities. Every consumer must pass the capacity
 * explicitly to bounded string and I/O helpers.
 */

#define LB_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

#define LB_NAME_MAX 64
#define LB_LINE_MAX 256
#define LB_CSV_MAX 512
#define LB_PATH_MAX 4096

#endif /* LB_DEFS_H */
