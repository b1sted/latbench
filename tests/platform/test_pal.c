/* SPDX-License-Identifier: GPL-3.0 */

/* NOLINTBEGIN(bugprone-reserved-identifier, readability-identifier-naming) */
#define _POSIX_C_SOURCE 200809L
/* NOLINTEND(bugprone-reserved-identifier, readability-identifier-naming) */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <pthread.h>
#include <signal.h>

#include "pal/compiler.h"
#include "pal/pal.h"
#include "pal/pal_os.h"

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

static void test_thread_pin_status(void)
{
    pal_test_thread_arg_t ctx = {0u, 0u};
    lb_status_t pin_status = LB_STATUS_OK;
    pal_thread_t *thread = pal_thread_spawn(pal_test_thread_main, &ctx, INT32_MAX, false);

    check_true(thread != NULL, "pal_thread_spawn accepts an impossible pin request");
    if (thread == NULL) {
        return;
    }
    check_true(pal_thread_join_ex(thread, &pin_status) == LB_STATUS_OK,
               "pal_thread_join_ex completes");
    check_true(ctx.seen != 0u, "thread with a rejected pin still runs");
    check_true(pin_status != LB_STATUS_OK, "pal_thread_join_ex reports a rejected pin");
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

    pal_atomic_store_u32_relaxed(&value, 13u);
    check_true(pal_atomic_load_u32_relaxed(&value) == 13u,
               "relaxed atomic u32 store/load round-trip");
    check_true(pal_atomic_cas_u32_relaxed(&value, 13u, 17u),
               "relaxed atomic u32 CAS succeeds on a match");
    check_true(pal_atomic_load_u32_relaxed(&value) == 17u,
               "relaxed atomic u32 CAS commits the new value");
}

static void test_os_syscall(void)
{
    pal_os_syscall_result_t result = {0};

    check_true(pal_os_syscall_probe(64u, &result) == LB_STATUS_OK, "pal_os_syscall_probe succeeds");
    check_true(result.calls == 64u, "pal_os_syscall_probe reports all calls");
    check_true(result.timer_ns > 0u, "pal_os_syscall_probe times pal_now_ns");
    check_true(result.syscall_ns > 0u, "pal_os_syscall_probe times write");
    check_true(pal_os_syscall_probe(0u, &result) == LB_STATUS_INVALID_ARG,
               "pal_os_syscall_probe rejects zero calls");
}

static void test_os_ctx_thread(void)
{
    pal_os_ctx_result_t result = {0};

    check_true(pal_os_ctx_thread(8u, &result) == LB_STATUS_OK,
               "pal_os_ctx_thread completes a pipe ping-pong");
    check_true(result.switches == 16u, "pal_os_ctx_thread counts both handoffs per round trip");
    check_true(result.elapsed_ns > 0u, "pal_os_ctx_thread measures a nonzero duration");
    check_true(pal_os_ctx_thread(0u, &result) == LB_STATUS_INVALID_ARG,
               "pal_os_ctx_thread rejects zero round trips");
}

static void test_os_ctx_proc(void)
{
    pal_os_ctx_result_t result = {0};

    check_true(pal_os_ctx_proc(4u, &result) == LB_STATUS_OK,
               "pal_os_ctx_proc completes a forked pipe ping-pong");
    check_true(result.switches == 8u, "pal_os_ctx_proc counts both handoffs per round trip");
    check_true(result.elapsed_ns > 0u, "pal_os_ctx_proc measures a nonzero duration");
}

static void test_os_wakeup(void)
{
    pal_os_wakeup_result_t result = {0};

    check_true(pal_os_wakeup(8u, &result) == LB_STATUS_OK,
               "pal_os_wakeup completes a scheduler ping-pong");
    check_true(result.wakeups == 16u, "pal_os_wakeup counts both wakeups per round trip");
    check_true(result.elapsed_ns > 0u, "pal_os_wakeup measures a nonzero duration");
}

static void test_os_shootdown(void)
{
    pal_os_shootdown_result_t result = {0};

    check_true(pal_os_shootdown(16u, &result) == LB_STATUS_OK,
               "pal_os_shootdown completes a single-thread run");
    check_true(result.single_ops == 32u, "pal_os_shootdown counts both mprotect calls");
    check_true(result.single_ns > 0u, "pal_os_shootdown measures a nonzero duration");
    if (result.multi_skipped) {
        printf("SKIP: pal_os_shootdown multi-thread needs at least 2 CPUs\n");
    } else {
        check_true(result.background_threads >= 1u, "pal_os_shootdown reports background threads");
        check_true(result.multi_ops == 32u, "pal_os_shootdown multi-thread counts both calls");
    }
}

static void test_os_fork(void)
{
    pal_os_fork_result_t result = {0};

    check_true(pal_os_fork_once(2u, &result) == LB_STATUS_OK,
               "pal_os_fork_once completes a fork batch");
    check_true(result.forks == 2u, "pal_os_fork_once reports every reaped child");
    check_true(result.elapsed_ns > 0u, "pal_os_fork_once measures a nonzero duration");
}

static void test_os_sig(void)
{
    pal_os_sig_result_t result = {0};

    check_true(pal_os_sig_roundtrip(8u, &result) == LB_STATUS_OK,
               "pal_os_sig_roundtrip delivers SIGUSR1 to itself");
    check_true(result.signals == 8u, "pal_os_sig_roundtrip reports every delivery");
    check_true(result.elapsed_ns > 0u, "pal_os_sig_roundtrip measures a nonzero duration");
}

static void test_os_sig_blocked(void)
{
    pal_os_sig_result_t result = {0};
    sigset_t sigusr1 = {0};
    sigset_t previous = {0};

    if (sigemptyset(&sigusr1) != 0 || sigaddset(&sigusr1, SIGUSR1) != 0) {
        check_true(false, "cannot build a SIGUSR1 signal set");
        return;
    }
    if (pthread_sigmask(SIG_BLOCK, &sigusr1, &previous) != 0) {
        printf("SKIP: cannot block SIGUSR1 in the test thread\n");
        return;
    }

    check_true(pal_os_sig_roundtrip(8u, &result) == LB_STATUS_OK,
               "pal_os_sig_roundtrip unblocks SIGUSR1 internally");
    check_true(result.signals == 8u,
               "pal_os_sig_roundtrip delivers while the caller had SIGUSR1 blocked");
    check_true(result.elapsed_ns > 0u, "blocked-caller round trip is still timed");

    check_true(pthread_sigmask(SIG_SETMASK, &previous, NULL) == 0,
               "test restores its original signal mask");
}

static void test_os_sleep(void)
{
    uint64_t samples[3] = {0u, 0u, 0u};
    size_t count = 0u;
    size_t i = 0u;
    bool nonzero = true;

    check_true(pal_os_sleep_us(1u, 3u, samples, 3u, &count) == LB_STATUS_OK,
               "pal_os_sleep_us handles a 1 us target");
    check_true(count == 3u, "pal_os_sleep_us reports every sample");
    for (i = 0u; i < count; i++) {
        if (samples[i] == 0u) {
            nonzero = false;
        }
    }
    check_true(nonzero, "pal_os_sleep_us records nonzero elapsed time");

    check_true(pal_os_sleep_us(100u, 2u, samples, 3u, &count) == LB_STATUS_OK,
               "pal_os_sleep_us handles a 100 us target");
    check_true(count == 2u, "pal_os_sleep_us respects the requested sample count");
    check_true(pal_os_sleep_us(1u, 4u, samples, 3u, &count) == LB_STATUS_INVALID_ARG,
               "pal_os_sleep_us rejects a capacity smaller than the sample count");
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
    test_thread_pin_status();
    test_atomics();
    test_os_syscall();
    test_os_ctx_thread();
    test_os_ctx_proc();
    test_os_wakeup();
    test_os_shootdown();
    test_os_fork();
    test_os_sig();
    test_os_sig_blocked();
    test_os_sleep();
    test_compiler_primitives();

    printf("test_pal: %d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
