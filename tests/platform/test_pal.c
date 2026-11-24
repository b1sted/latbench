/* SPDX-License-Identifier: GPL-3.0 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "pal/compiler.h"
#include "pal/pal.h"

#define PAL_TEST_THREAD_VALUE 0x5a5a5a5au

typedef struct pal_test_thread_arg_t {
    uint32_t value;
    uint32_t seen;
} pal_test_thread_arg_t;

static int g_checks = 0;
static int g_failures = 0;

PAL_NO_VECTORIZE PAL_NOINLINE static uint32_t pal_test_scalar_sum(const uint32_t *values,
                                                                  size_t count)
{
    uint32_t sum = 0u;
    size_t i = 0u;

    PAL_NO_VECTORIZE_LOOP
    for (i = 0u; i < count; i++) {
        sum += values[i];
    }
    return sum;
}

static void check_true(bool condition, const char *name)
{
    g_checks++;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        g_failures++;
    }
}

static void test_now_ns(void)
{
    uint64_t previous = pal_now_ns();
    bool monotonic = true;
    int i = 0;

    check_true(previous != 0u, "pal_now_ns returns a nonzero value");
    for (i = 0; i < 1000; i++) {
        const uint64_t now = pal_now_ns();
        if (now < previous) {
            monotonic = false;
        }
        previous = now;
    }
    check_true(monotonic, "pal_now_ns is monotonic");
}

static void test_page_size(void)
{
    const size_t page = pal_page_size();

    check_true(page >= 4096u, "pal_page_size is at least 4096 bytes");
    check_true((page & (page - 1u)) == 0u, "pal_page_size is a power of two");
}

static void test_total_ram(void)
{
    const uint64_t total = pal_total_ram_bytes();

    check_true(total > 0u, "pal_total_ram_bytes reports physical RAM");
    check_true(total >= (UINT64_C(1024) * UINT64_C(1024)), "pal_total_ram_bytes is at least 1 MiB");
}

static void test_safe_max_mem(void)
{
    const uint64_t total = pal_total_ram_bytes();
    const uint64_t cap = total / 4u;
    const uint64_t automatic = pal_safe_max_mem_bytes(0u);
    const uint64_t one_mib = pal_safe_max_mem_bytes(1u);
    const uint64_t huge = pal_safe_max_mem_bytes(UINT64_MAX);

    if (cap > 0u) {
        check_true(automatic == cap, "automatic budget equals 25% of RAM");
        check_true(one_mib <= (UINT64_C(1024) * UINT64_C(1024)),
                   "explicit 1 MiB budget is honored");
        check_true(one_mib <= cap, "explicit budget is clipped to the 25% cap");
        check_true(huge <= cap, "overflowing limit is clipped to the 25% cap");
    }
}

static void test_mem_invalid(void)
{
    check_true(pal_mem_alloc(0u, 0u) == NULL, "pal_mem_alloc(0) returns NULL");
    check_true(pal_mem_data(NULL) == NULL, "pal_mem_data(NULL) returns NULL");
    check_true(pal_mem_size(NULL) == 0u, "pal_mem_size(NULL) returns zero");
    pal_mem_free(NULL);
}

static void test_mem_alloc_ok(uint32_t flags, const char *name)
{
    const size_t size = 8192u;
    pal_mem_t *mem = pal_mem_alloc(size, flags);
    unsigned char *data = NULL;

    check_true(mem != NULL, name);
    if (mem == NULL) {
        return;
    }

    data = pal_mem_data(mem);
    check_true(data != NULL, "pal_mem_data returns storage");
    check_true(pal_mem_size(mem) == size, "pal_mem_size matches the request");
    if (data != NULL) {
        data[0] = (unsigned char)0x5au;
        data[size - 1u] = (unsigned char)0xa5u;
        check_true(data[0] == (unsigned char)0x5au && data[size - 1u] == (unsigned char)0xa5u,
                   "arena storage is writable");
    }
    pal_mem_free(mem);
}

static void test_mem_odd_size(void)
{
    const size_t size = 12345u;
    pal_mem_t *mem = pal_mem_alloc(size, 0u);

    check_true(mem != NULL, "pal_mem_alloc accepts an unaligned size");
    if (mem != NULL) {
        check_true(pal_mem_size(mem) == size, "pal_mem_size preserves the requested size");
        pal_mem_free(mem);
    }
}

static void test_arch_name(void)
{
    const char *arch = pal_arch_name();

    check_true(arch != NULL, "pal_arch_name returns non-NULL");
    check_true(arch != NULL && arch[0] != '\0', "pal_arch_name returns a non-empty string");
}

static void test_cpu_and_pinning(void)
{
    const uint32_t count = pal_cpu_count();
    lb_status_t status = LB_STATUS_ERROR;

    check_true(count >= 1u, "pal_cpu_count is at least 1");

    status = pal_pin_cpu(0);
    check_true(status == LB_STATUS_OK || status == LB_STATUS_UNSUPPORTED,
               "pal_pin_cpu(0) succeeds or reports unsupported");
    if (status == LB_STATUS_UNSUPPORTED) {
        printf("SKIP: CPU pinning is not permitted on this host\n");
    }

    status = pal_pin_cpu(-1);
    check_true(status == LB_STATUS_INVALID_ARG || status == LB_STATUS_UNSUPPORTED,
               "pal_pin_cpu(-1) is rejected");
}

static void test_qos(void)
{
    const lb_status_t high = pal_set_qos(true);
    const lb_status_t low = pal_set_qos(false);

    check_true(high == LB_STATUS_OK || high == LB_STATUS_UNSUPPORTED,
               "pal_set_qos(true) succeeds or reports unsupported");
    check_true(low == LB_STATUS_OK || low == LB_STATUS_UNSUPPORTED,
               "pal_set_qos(false) succeeds or reports unsupported");
}

static void pal_test_thread_main(void *arg)
{
    pal_test_thread_arg_t *ctx = arg;

    if (ctx != NULL) {
        ctx->value = PAL_TEST_THREAD_VALUE;
        ctx->seen = 1u;
    }
}

static void test_thread_lifecycle(void)
{
    pal_test_thread_arg_t ctx = {0u, 0u};
    pal_thread_t *thread = pal_thread_spawn(pal_test_thread_main, &ctx, -1, false);

    check_true(thread != NULL, "pal_thread_spawn returns a handle");
    if (thread != NULL) {
        check_true(pal_thread_join(thread) == LB_STATUS_OK, "pal_thread_join completes");
        check_true(ctx.seen != 0u, "spawned thread runs its entry point");
        check_true(ctx.value == PAL_TEST_THREAD_VALUE, "spawned thread receives its argument");
    }

    check_true(pal_thread_join(NULL) == LB_STATUS_INVALID_ARG, "pal_thread_join rejects NULL");
    check_true(pal_thread_spawn(NULL, NULL, -1, false) == NULL,
               "pal_thread_spawn rejects a NULL entry point");
}

static void test_thread_pinned(void)
{
    pal_test_thread_arg_t ctx = {0u, 0u};
    pal_thread_t *thread = pal_thread_spawn(pal_test_thread_main, &ctx, 0, true);

    check_true(thread != NULL, "pinned pal_thread_spawn returns a handle");
    if (thread != NULL) {
        check_true(pal_thread_join(thread) == LB_STATUS_OK, "pinned pal_thread_join completes");
        check_true(ctx.seen != 0u, "pinned thread runs its entry point");
    }
}

static void test_atomics(void)
{
    uint32_t value = 0u;
    uint64_t counter = 0u;

    pal_atomic_store_u32(&value, 7u);
    check_true(pal_atomic_load_u32(&value) == 7u, "atomic u32 store/load round-trip");

    check_true(pal_atomic_cas_u32(&value, 7u, 9u), "atomic u32 CAS succeeds on a match");
    check_true(pal_atomic_load_u32(&value) == 9u, "atomic u32 CAS commits the new value");
    check_true(!pal_atomic_cas_u32(&value, 7u, 11u), "atomic u32 CAS fails on a mismatch");

    check_true(pal_atomic_fetch_add_u64(&counter, 5u) == 0u,
               "atomic u64 fetch_add returns the old value");
    check_true(counter == 5u, "atomic u64 fetch_add updates the counter");
    pal_atomic_fence_seq();
}

static void test_compiler_primitives(void)
{
    const uint32_t values[4] = {1u, 2u, 3u, 4u};
    uint64_t ticks = pal_now_ns();
    double scale = 1.5;
    uint32_t sum = 0u;

    PAL_BARRIER();
    PAL_KEEP_INT(ticks);
    PAL_KEEP_FP(scale);
    PAL_BLOCK_IFCONV();
    sum = pal_test_scalar_sum(values, 4u);

    check_true(ticks != 0u, "PAL_KEEP_INT preserves a live value");
    check_true(scale > 0.0, "PAL_KEEP_FP preserves a live value");
    check_true(sum == 10u, "scalar sum helper stays correct");
}

int main(void)
{
    test_now_ns();
    test_page_size();
    test_total_ram();
    test_safe_max_mem();
    test_mem_invalid();
    test_mem_alloc_ok(0u, "pal_mem_alloc(flags=0) returns a handle");
    test_mem_alloc_ok(PAL_MEM_NO_THP, "pal_mem_alloc(NO_THP) returns a handle");
    test_mem_alloc_ok(PAL_MEM_THP, "pal_mem_alloc(THP) returns a handle");
    test_mem_odd_size();
    test_arch_name();
    test_cpu_and_pinning();
    test_qos();
    test_thread_lifecycle();
    test_thread_pinned();
    test_atomics();
    test_compiler_primitives();

    printf("test_pal: %d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
