/* SPDX-License-Identifier: GPL-3.0 */

/* NOLINTBEGIN(bugprone-reserved-identifier, readability-identifier-naming) */
#define _POSIX_C_SOURCE 200809L
/* NOLINTEND(bugprone-reserved-identifier, readability-identifier-naming) */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include <sys/mman.h>
#include <unistd.h>

#include "pal/pal.h"
#include "pal_internal.h"

bool pal_is_tty(bool use_stderr)
{
    const int fd = use_stderr ? STDERR_FILENO : STDOUT_FILENO;

    return isatty(fd) != 0;
}

size_t pal_page_size(void)
{
    const long page = sysconf(_SC_PAGESIZE);

    if (page <= 0) {
        return 4096u;
    }
    return (size_t)page;
}

void pal_sleep_ms(uint32_t milliseconds)
{
    struct timespec request = {0, 0};

    if (milliseconds == 0u) {
        return;
    }

    request.tv_sec = (time_t)(milliseconds / 1000u);
    request.tv_nsec = (long)((milliseconds % 1000u) * 1000000u);

    /* Restart after a signal so a requested pause is not cut short. */
    while (nanosleep(&request, &request) != 0) {
        if (errno != EINTR) {
            break;
        }
    }
}

const char *pal_arch_name(void)
{
#if defined(__x86_64__)
    return "x86_64";
#elif defined(__i386__)
    return "i386";
#elif defined(__aarch64__)
#if defined(__APPLE__)
    return "arm64";
#else
    return "aarch64";
#endif
#elif defined(__riscv) && (__riscv_xlen == 64)
    return "riscv64";
#elif defined(__arm__)
#if defined(__ARM_PCS_VFP)
    return "armhf";
#else
    return "armv7";
#endif
#else
    return "unknown";
#endif
}

uint64_t pal_safe_max_mem_bytes(uint64_t user_limit_mb)
{
    const uint64_t mib = UINT64_C(1024) * UINT64_C(1024);
    const uint64_t total = pal_total_ram_bytes();
    const uint64_t cap = total / 4u;
    uint64_t requested = 0u;

    if (user_limit_mb == 0u) {
        return cap;
    }
    if (user_limit_mb > (UINT64_MAX / mib)) {
        return cap;
    }

    requested = user_limit_mb * mib;
    if (cap == 0u) {
        return requested;
    }
    return (requested < cap) ? requested : cap;
}

void *pal_mem_data(const pal_mem_t *mem)
{
    if (mem == NULL) {
        return NULL;
    }
    return mem->base;
}

size_t pal_mem_size(const pal_mem_t *mem)
{
    if (mem == NULL) {
        return 0u;
    }
    return mem->size;
}

void pal_mem_free(pal_mem_t *mem)
{
    if (mem == NULL) {
        return;
    }

    if (mem->mapped_size > 0u) {
        if (munmap(mem->base, mem->mapped_size) != 0) {
            /* Nothing safe to do while tearing down; the OS owns the mapping. */
        }
    } else {
        free(mem->base);
    }
    free(mem);
}

/*
 * Platform-neutral fallbacks. Linux and macOS provide their own
 * implementations; this block keeps a plain POSIX target buildable.
 */
#if !defined(__linux__) && !defined(__APPLE__)

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
#if defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
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
#else
    return 0u;
#endif
}

uint32_t pal_cpu_count(void)
{
    long count = -1;

#if defined(_SC_NPROCESSORS_ONLN)
    count = sysconf(_SC_NPROCESSORS_ONLN);
#endif
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
    (void)cpu_id;
    return LB_STATUS_UNSUPPORTED;
}

lb_status_t pal_set_qos(bool high_perf)
{
    (void)high_perf;
    return LB_STATUS_UNSUPPORTED;
}

pal_mem_t *pal_mem_alloc(size_t size, uint32_t flags)
{
    pal_mem_t *mem = NULL;
    void *base = NULL;

    if (size == 0u) {
        return NULL;
    }
    (void)flags;

    mem = malloc(sizeof(*mem));
    if (mem == NULL) {
        goto cleanup;
    }

    base = malloc(size);
    if (base == NULL) {
        goto cleanup;
    }

    mem->base = base;
    mem->mapped_size = 0u;
    mem->size = size;
    return mem;

cleanup:
    free(base);
    free(mem);
    return NULL;
}

#endif /* !__linux__ && !__APPLE__ */
