/* SPDX-License-Identifier: GPL-3.0 */
#ifndef PAL_INTERNAL_H
#define PAL_INTERNAL_H

/*
 * Private backing layout shared by the platform allocators and the POSIX
 * accessors. This header is not part of the public include tree.
 */

#include <stddef.h>

struct pal_mem {
    void *base;
    size_t mapped_size;
    size_t size;
};

#endif /* PAL_INTERNAL_H */
