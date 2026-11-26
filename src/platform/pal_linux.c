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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include "pal/compiler.h"
#include "pal/pal.h"
#include "pal/pal_os.h"
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
#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif

static bool pal_linux_apply_thp(void *base, size_t size, uint32_t flags)
{
    const size_t thp_align = (size_t)2u * 1024u * 1024u;
    const bool want_thp = ((flags & PAL_MEM_THP) != 0u) && ((flags & PAL_MEM_NO_THP) == 0u);
    int rc = 0;
    lb_thp_mode_t mode = LB_THP_UNSUPPORTED;

    if ((flags & PAL_MEM_NO_THP) != 0u) {
        (void)madvise(base, size, MADV_NOHUGEPAGE);
        return false;
    }
    if (!want_thp) {
        return false;
    }

    /*
     * Best-effort: old kernels and some Android configurations reject the
     * advice with EINVAL/ENOSYS. The mapping stays usable either way.
     */
    rc = madvise(base, size, MADV_HUGEPAGE);
    if (size < thp_align) {
        return false;
    }
    if (((uintptr_t)base & ((uintptr_t)thp_align - 1u)) != 0u) {
        return false;
    }

    mode = pal_thp_mode();
    if (mode == LB_THP_ALWAYS) {
        return true;
    }
    return mode == LB_THP_MADVISE && rc == 0;
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

lb_status_t pal_cpu_list(int32_t *cpus, size_t capacity, size_t *out_count)
{
    cpu_set_t set = {0};
    size_t count = 0u;

    if (out_count != NULL) {
        *out_count = 0u;
    }
    if (cpus == NULL || capacity == 0u || out_count == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        int32_t cpu = 0;

        for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
            if (CPU_ISSET((unsigned int)cpu, &set)) {
                if (count < capacity) {
                    cpus[count] = cpu;
                }
                count++;
            }
        }
        if (count > 0u) {
            *out_count = (count < capacity) ? count : capacity;
            return LB_STATUS_OK;
        }
    }

    /*
     * A denied or empty affinity mask (some sandboxes) still has to report a
     * usable CPU set, so fall back to a dense range of online CPU IDs.
     */
    count = (size_t)pal_cpu_count();
    if (count > capacity) {
        count = capacity;
    }
    {
        size_t i = 0u;

        for (i = 0u; i < count; i++) {
            cpus[i] = (int32_t)i;
        }
    }
    *out_count = count;
    return LB_STATUS_OK;
}

lb_status_t pal_pin_cpu(int32_t cpu_id)
{
    cpu_set_t set = {0};

    if (cpu_id < 0) {
        return LB_STATUS_INVALID_ARG;
    }
    if ((uint32_t)cpu_id >= (uint32_t)CPU_SETSIZE) {
        return LB_STATUS_INVALID_ARG;
    }

    /*
     * Do not compare against pal_cpu_count(): _SC_NPROCESSORS_ONLN reports the
     * size of the online cpuset, which can be smaller than the highest valid
     * CPU ID when cores are offline or the cpuset is sparse. sched_setaffinity()
     * is the authority on whether this CPU exists and is allowed.
     */
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

bool pal_ilp_scaling_expected(void)
{
#if defined(__arm__) && !defined(__aarch64__)
    /* Cortex-A7 and other in-order ARMv7 cores expose one integer pipeline. */
    return false;
#else
    return true;
#endif
}

lb_thp_mode_t pal_thp_mode(void)
{
    FILE *file = fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
    char line[256];
    lb_thp_mode_t mode = LB_THP_UNSUPPORTED;

    if (file == NULL) {
        return LB_THP_UNSUPPORTED;
    }
    if (fgets(line, (int)sizeof(line), file) != NULL) {
        if (strstr(line, "[always]") != NULL) {
            mode = LB_THP_ALWAYS;
        } else if (strstr(line, "[madvise]") != NULL) {
            mode = LB_THP_MADVISE;
        } else if (strstr(line, "[never]") != NULL) {
            mode = LB_THP_NEVER;
        }
    }
    (void)fclose(file);
    return mode;
}

pal_mem_t *pal_mem_alloc(size_t size, uint32_t flags)
{
    pal_mem_t *mem = NULL;
    void *base = NULL;
    size_t mapped_size = 0u;
    const size_t page = pal_page_size();
    const size_t thp_align = (size_t)2u * 1024u * 1024u;
    const bool want_thp = ((flags & PAL_MEM_THP) != 0u) && ((flags & PAL_MEM_NO_THP) == 0u);

    if (size == 0u) {
        return NULL;
    }
    if (page == 0u || size > (SIZE_MAX - (page - 1u))) {
        return NULL;
    }
    mapped_size = ((size + page - 1u) / page) * page;

    /*
     * Transparent huge pages only apply to aligned ranges. Over-map by one
     * huge-page-sized window, trim the unaligned prefix and suffix, and keep
     * the interior mapping so base is 2 MiB aligned.
     */
    if (want_thp && mapped_size >= thp_align && mapped_size <= (SIZE_MAX - thp_align)) {
        const size_t over_size = mapped_size + thp_align;
        void *raw =
            mmap(NULL, over_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (raw != MAP_FAILED) {
            const uintptr_t align_mask = (uintptr_t)(thp_align - 1u);
            const uintptr_t aligned_addr = ((uintptr_t)raw + align_mask) & ~align_mask;
            const size_t prefix = (size_t)(aligned_addr - (uintptr_t)raw);
            const size_t suffix = over_size - prefix - mapped_size;

            if (prefix > 0u) {
                (void)munmap(raw, prefix);
            }
            if (suffix > 0u) {
                (void)munmap((void *)(aligned_addr + (uintptr_t)mapped_size), suffix);
            }
            base = (void *)aligned_addr;
        }
    }

    if (base == NULL) {
        base = mmap(NULL, mapped_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED) {
            mapped_size = 0u;
            base = malloc(size);
            if (base == NULL) {
                goto cleanup;
            }
        }
    }

    mem = malloc(sizeof(*mem));
    if (mem == NULL) {
        goto cleanup;
    }

    mem->base = base;
    mem->mapped_size = mapped_size;
    mem->size = size;
    mem->thp_active = false;

    if (mapped_size > 0u) {
        mem->thp_active = pal_linux_apply_thp(base, mapped_size, flags);
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

/* --------------------------------------------------------- swap-in probe */

static bool pal_linux_is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f';
}

/*
 * Extract the Size and Used columns from one /proc/swaps row. The first two
 * columns are the filename and type, so only fields 3 and 4 are numeric.
 */
static bool pal_linux_swap_fields(const char *line, uint64_t *size_kib, uint64_t *used_kib)
{
    const char *cursor = line;
    uint32_t field = 0u;
    bool have_size = false;
    bool have_used = false;

    while (*cursor != '\0') {
        char *end = NULL;
        unsigned long long value = 0u;

        while (*cursor != '\0' && pal_linux_is_space(*cursor)) {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        field++;
        value = strtoull(cursor, &end, 10);
        if (end == cursor) {
            while (*cursor != '\0' && !pal_linux_is_space(*cursor)) {
                cursor++;
            }
            continue;
        }
        if (field == 3u) {
            *size_kib = (uint64_t)value;
            have_size = true;
        } else if (field == 4u) {
            *used_kib = (uint64_t)value;
            have_used = true;
        }
        cursor = end;
    }
    return have_size && have_used;
}

static lb_status_t pal_linux_swap_free_kib(uint64_t *out_free_kib)
{
    FILE *file = fopen("/proc/swaps", "r");
    char line[512];
    uint64_t free_kib = 0u;
    bool saw_entry = false;

    if (file == NULL) {
        return LB_STATUS_UNSUPPORTED;
    }
    if (fgets(line, (int)sizeof(line), file) == NULL) {
        (void)fclose(file);
        return LB_STATUS_UNSUPPORTED;
    }

    while (fgets(line, (int)sizeof(line), file) != NULL) {
        uint64_t size_kib = 0u;
        uint64_t used_kib = 0u;

        if (!pal_linux_swap_fields(line, &size_kib, &used_kib)) {
            continue;
        }
        saw_entry = true;
        if (size_kib > used_kib) {
            const uint64_t delta = size_kib - used_kib;

            if (free_kib > (UINT64_MAX - delta)) {
                free_kib = UINT64_MAX;
            } else {
                free_kib += delta;
            }
        }
    }
    (void)fclose(file);

    if (!saw_entry || free_kib == 0u) {
        return LB_STATUS_UNSUPPORTED;
    }
    *out_free_kib = free_kib;
    return LB_STATUS_OK;
}

static uint64_t pal_linux_xorshift64(uint64_t *state)
{
    uint64_t value = *state;

    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *state = value;
    return value * UINT64_C(2685821657736338717);
}

static void pal_linux_swap_seed(uint64_t *state, const void *base)
{
    *state = pal_now_ns() ^ (uint64_t)(uintptr_t)base ^ UINT64_C(0x9e3779b97f4a7c15);
    if (*state == 0u) {
        *state = UINT64_C(0x9e3779b97f4a7c15);
    }
}

static void pal_linux_swap_fill(void *base, size_t actual_pages, size_t page_size, uint64_t *state)
{
    uint64_t *words = base;
    const size_t words_per_page = page_size / sizeof(*words);
    const size_t word_count = actual_pages * words_per_page;
    const size_t tail_bytes = page_size % sizeof(uint64_t);
    size_t i = 0u;

    /*
     * Zero pages are not paged out and zram compresses repeated words, so every
     * word and every trailing byte gets a nonzero pseudo-random value.
     */
    for (i = 0u; i < word_count; i++) {
        uint64_t value = pal_linux_xorshift64(state);

        if (value == 0u) {
            value = UINT64_C(0x9e3779b97f4a7c15);
        }
        words[i] = value;
    }

    if (tail_bytes > 0u) {
        unsigned char *bytes = base;
        const size_t tail_offset = words_per_page * sizeof(uint64_t);
        size_t page = 0u;

        for (page = 0u; page < actual_pages; page++) {
            size_t tail = 0u;

            for (tail = 0u; tail < tail_bytes; tail++) {
                const uint64_t value = pal_linux_xorshift64(state);

                bytes[page * page_size + tail_offset + tail] = (unsigned char)(value | 1u);
            }
        }
    }
}

lb_status_t pal_os_swap_probe(size_t max_pages, uint64_t *samples_ns, size_t capacity,
                              size_t *out_count, pal_os_swap_result_t *out)
{
    const size_t page_size = pal_page_size();
    void *base = MAP_FAILED;
    size_t *order = NULL;
    struct rusage ru_before = {0};
    struct rusage ru_after = {0};
    uint64_t swap_free_kib = 0u;
    uint64_t limit_pages = 0u;
    uint64_t state = 0u;
    uint64_t acc = 0u;
    uint64_t total_start = 0u;
    uint64_t total_end = 0u;
    size_t actual_pages = 0u;
    size_t base_size = 0u;
    size_t i = 0u;
    lb_status_t status = LB_STATUS_ERROR;

    if (out_count != NULL) {
        *out_count = 0u;
    }
    if (out != NULL) {
        out->elapsed_ns = 0u;
        out->pages = 0u;
        out->major_faults = 0u;
        out->minor_faults = 0u;
        out->swap_free_kib = 0u;
    }
    if (max_pages == 0u || samples_ns == NULL || out_count == NULL || out == NULL ||
        capacity < max_pages) {
        return LB_STATUS_INVALID_ARG;
    }

    status = pal_linux_swap_free_kib(&swap_free_kib);
    if (status != LB_STATUS_OK) {
        return status;
    }
    if (page_size < 1024u) {
        return LB_STATUS_UNSUPPORTED;
    }

    /* Never use more than 25% of free swap or the automatic memory budget. */
    if (swap_free_kib > (UINT64_MAX / 1024u)) {
        limit_pages = UINT64_MAX / (uint64_t)page_size;
    } else {
        const uint64_t swap_bytes = (swap_free_kib / 4u) * 1024u;

        limit_pages = swap_bytes / (uint64_t)page_size;
    }
    if (limit_pages > (pal_safe_max_mem_bytes(0u) / (uint64_t)page_size)) {
        limit_pages = pal_safe_max_mem_bytes(0u) / (uint64_t)page_size;
    }
    if ((uint64_t)max_pages < limit_pages) {
        limit_pages = (uint64_t)max_pages;
    }
    if (limit_pages == 0u) {
        return LB_STATUS_UNSUPPORTED;
    }
    if (limit_pages > (uint64_t)(SIZE_MAX / page_size)) {
        return LB_STATUS_UNSUPPORTED;
    }
    actual_pages = (size_t)limit_pages;
    base_size = actual_pages * page_size;

    base = mmap(NULL, base_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        goto cleanup;
    }
    order = malloc(actual_pages * sizeof(*order));
    if (order == NULL) {
        goto cleanup;
    }

    /* Build an identity order, then shuffle it with a single-cycle Sattolo pass. */
    for (i = 0u; i < actual_pages; i++) {
        order[i] = i;
    }
    pal_linux_swap_seed(&state, base);
    for (i = actual_pages; i > 1u; i--) {
        const size_t target = (size_t)(pal_linux_xorshift64(&state) % (uint64_t)i);
        const size_t saved = order[i - 1u];

        order[i - 1u] = order[target];
        order[target] = saved;
    }

    pal_linux_swap_fill(base, actual_pages, page_size, &state);

    if (madvise(base, base_size, MADV_PAGEOUT) != 0) {
        if (errno == EINVAL || errno == ENOSYS || errno == EPERM) {
            status = LB_STATUS_UNSUPPORTED;
        } else {
            status = LB_STATUS_ERROR;
        }
        goto cleanup;
    }
    if (getrusage(RUSAGE_SELF, &ru_before) != 0) {
        goto cleanup;
    }

    {
        const volatile unsigned char *view = base;

        total_start = pal_now_ns();
        for (i = 0u; i < actual_pages; i++) {
            const size_t idx = order[i];
            const size_t offset = idx * page_size + ((idx * 64u) % page_size);
            uint64_t start = pal_now_ns();
            uint64_t end = 0u;

            acc += (uint64_t)view[offset];
            end = pal_now_ns();
            samples_ns[i] = (end >= start) ? (end - start) : 0u;
            PAL_KEEP_INT(acc);
        }
        total_end = pal_now_ns();
    }

    if (getrusage(RUSAGE_SELF, &ru_after) != 0) {
        goto cleanup;
    }

    out->elapsed_ns = (total_end >= total_start) ? (total_end - total_start) : 0u;
    out->pages = (uint64_t)actual_pages;
    out->major_faults = (ru_after.ru_majflt >= ru_before.ru_majflt)
                            ? (uint64_t)(ru_after.ru_majflt - ru_before.ru_majflt)
                            : 0u;
    out->minor_faults = (ru_after.ru_minflt >= ru_before.ru_minflt)
                            ? (uint64_t)(ru_after.ru_minflt - ru_before.ru_minflt)
                            : 0u;
    out->swap_free_kib = swap_free_kib;
    *out_count = actual_pages;
    status = LB_STATUS_OK;

cleanup:
    free(order);
    if (base != MAP_FAILED) {
        (void)munmap(base, base_size);
    }
    return status;
}
