/* SPDX-License-Identifier: GPL-3.0 */
#ifndef PAL_INTERNAL_H
#define PAL_INTERNAL_H

/*
 * Private backing layout shared by the platform allocators and the POSIX
 * accessors. This header is not part of the public include tree.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include "pal/pal.h"

struct pal_mem {
    void *base;
    size_t mapped_size;
    size_t size;
    bool thp_active;
};

struct pal_thread {
    pthread_t handle;
    pal_thread_fn_t fn;
    void *arg;
    int32_t pin_cpu;
    lb_status_t pin_status;
    bool qos_perf;
};

#endif /* PAL_INTERNAL_H */
