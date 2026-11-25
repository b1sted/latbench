/* SPDX-License-Identifier: GPL-3.0 */

/* NOLINTBEGIN(bugprone-reserved-identifier, readability-identifier-naming) */
#define _DARWIN_C_SOURCE 1
/* NOLINTEND(bugprone-reserved-identifier, readability-identifier-naming) */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <mach/kern_return.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/mman.h>
#include <sys/sysctl.h>

#include "pal/pal.h"
#include "pal_internal.h"

static mach_timebase_info_data_t g_pal_macos_timebase = {1u, 1u};
static pthread_once_t g_pal_macos_timebase_once = PTHREAD_ONCE_INIT;

static void pal_macos_timebase_init(void)
{
    if (mach_timebase_info(&g_pal_macos_timebase) != KERN_SUCCESS ||
        g_pal_macos_timebase.denom == 0u) {
        g_pal_macos_timebase.numer = 1u;
        g_pal_macos_timebase.denom = 1u;
    }
}

uint64_t pal_now_ns(void)
{
    const uint64_t ticks = mach_absolute_time();
    uint64_t numer = 0u;
    uint64_t denom = 0u;
    uint64_t quotient = 0u;
    uint64_t remainder = 0u;

    /*
     * mach_timebase_info is immutable after the first successful fill, but the
     * lazy initialization must be thread-safe: "topo" and "os shootdown" call
     * pal_now_ns() from several freshly spawned threads at once.
     */
    (void)pthread_once(&g_pal_macos_timebase_once, pal_macos_timebase_init);

    numer = (uint64_t)g_pal_macos_timebase.numer;
    denom = (uint64_t)g_pal_macos_timebase.denom;

    /* Split the conversion to avoid overflowing ticks * numer. */
    quotient = ticks / denom;
    remainder = ticks % denom;
    return quotient * numer + (remainder * numer) / denom;
}

uint64_t pal_total_ram_bytes(void)
{
    uint64_t value = 0u;
    size_t len = sizeof(value);

    if (sysctlbyname("hw.memsize", &value, &len, NULL, 0) != 0) {
        return 0u;
    }
    return value;
}

uint32_t pal_cpu_count(void)
{
    int32_t count = 0;
    size_t len = sizeof(count);

    if (sysctlbyname("hw.ncpu", &count, &len, NULL, 0) != 0) {
        return 1u;
    }
    if (count <= 0) {
        return 1u;
    }
    return (uint32_t)count;
}

lb_status_t pal_pin_cpu(int32_t cpu_id)
{
    /* macOS does not expose a stable per-thread hard affinity API. */
    (void)cpu_id;
    return LB_STATUS_UNSUPPORTED;
}

bool pal_pin_supported(void)
{
    return false;
}

bool pal_ilp_scaling_expected(void)
{
    return true;
}

lb_status_t pal_set_qos(bool high_perf)
{
    qos_class_t qos = QOS_CLASS_BACKGROUND;
    int rc = 0;

    if (high_perf) {
        qos = QOS_CLASS_USER_INTERACTIVE;
    }

    rc = pthread_set_qos_class_self_np(qos, 0);
    if (rc != 0) {
        return LB_STATUS_UNSUPPORTED;
    }
    return LB_STATUS_OK;
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
    (void)flags;
    if (page == 0u || size > (SIZE_MAX - (page - 1u))) {
        return NULL;
    }
    mapped_size = ((size + page - 1u) / page) * page;

    base = mmap(NULL, mapped_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
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
    return mem;

cleanup:
    if (mapped_size > 0u) {
        (void)munmap(base, mapped_size);
    } else {
        free(base);
    }
    return NULL;
}
