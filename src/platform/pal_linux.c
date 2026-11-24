/* SPDX-License-Identifier: GPL-3.0 */

/*
 * _GNU_SOURCE exposes CPU_ZERO/CPU_SET and sched_setaffinity on glibc;
 * _POSIX_C_SOURCE keeps clock_gettime and sysconf visible as well. Both are
 * scoped to this platform translation unit only.
 */
/* NOLINTBEGIN(bugprone-reserved-identifier, readability-identifier-naming) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
/* NOLINTEND(bugprone-reserved-identifier, readability-identifier-naming) */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pal/pal.h"
#include "pal_internal.h"

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

/*
 * Bionic and older kernel headers may omit the transparent-hugepage advice
 * constants even though the running kernel can still reject them at runtime.
 */
#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif
#ifndef MADV_NOHUGEPAGE
#define MADV_NOHUGEPAGE 15
#endif

static void pal_linux_apply_thp(void *base, size_t size, uint32_t flags)
{
    int advice = 0;

    if ((flags & PAL_MEM_NO_THP) != 0u) {
        advice = MADV_NOHUGEPAGE;
    } else if ((flags & PAL_MEM_THP) != 0u) {
        advice = MADV_HUGEPAGE;
    }

    if (advice != 0) {
        if (madvise(base, size, advice) != 0) {
            /*
             * Best-effort: old kernels and some Android configurations reject
             * the advice with EINVAL/ENOSYS. The mapping stays usable.
             */
        }
    }
}

uint64_t pal_now_ns(void)
{
    struct timespec ts = {0, 0};

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

uint64_t pal_total_ram_bytes(void)
{
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page = sysconf(_SC_PAGESIZE);
    uint64_t count = 0u;
    uint64_t bytes_per_page = 0u;

    if (pages <= 0 || page <= 0) {
        return 0u;
    }

    count = (uint64_t)pages;
    bytes_per_page = (uint64_t)page;
    if (count > (UINT64_MAX / bytes_per_page)) {
        return UINT64_MAX;
    }
    return count * bytes_per_page;
}

uint32_t pal_cpu_count(void)
{
    const long count = sysconf(_SC_NPROCESSORS_ONLN);

    if (count <= 0) {
        return 1u;
    }
    if ((uint64_t)count > (uint64_t)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)count;
}

lb_status_t pal_pin_cpu(int32_t cpu_id)
{
    cpu_set_t set = {0};

    if (cpu_id < 0) {
        return LB_STATUS_INVALID_ARG;
    }
    if ((uint64_t)cpu_id >= (uint64_t)pal_cpu_count()) {
        return LB_STATUS_INVALID_ARG;
    }
    if ((uint32_t)cpu_id >= (uint32_t)CPU_SETSIZE) {
        return LB_STATUS_INVALID_ARG;
    }

    CPU_ZERO(&set);
    CPU_SET((unsigned int)cpu_id, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
#if defined(__ANDROID__)
        /* Bionic surfaces cpuset and SELinux denials through several errno values. */
        return LB_STATUS_UNSUPPORTED;
#else
        if (errno == EPERM || errno == EINVAL || errno == ENOSYS) {
            return LB_STATUS_UNSUPPORTED;
        }
        return LB_STATUS_ERROR;
#endif
    }
    return LB_STATUS_OK;
}

lb_status_t pal_set_qos(bool high_perf)
{
    (void)high_perf;
    return LB_STATUS_UNSUPPORTED;
}

bool pal_pin_supported(void)
{
    return true;
}

pal_mem_t *pal_mem_alloc(size_t size, uint32_t flags)
{
    pal_mem_t *mem = NULL;
    void *base = NULL;
    size_t mapped_size = 0u;
    const size_t page = pal_page_size();

    if (size == 0u) {
        return NULL;
    }
    if (page == 0u || size > (SIZE_MAX - (page - 1u))) {
        return NULL;
    }
    mapped_size = ((size + page - 1u) / page) * page;

    base = mmap(NULL, mapped_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        mapped_size = 0u;
        base = malloc(size);
        if (base == NULL) {
            goto cleanup;
        }
    }

    mem = malloc(sizeof(*mem));
    if (mem == NULL) {
        goto cleanup;
    }

    mem->base = base;
    mem->mapped_size = mapped_size;
    mem->size = size;

    if (mapped_size > 0u) {
        pal_linux_apply_thp(base, mapped_size, flags);
    }

    return mem;

cleanup:
    if (mapped_size > 0u) {
        (void)munmap(base, mapped_size);
    } else {
        free(base);
    }
    return NULL;
}
