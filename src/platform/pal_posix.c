/* SPDX-License-Identifier: GPL-3.0 */

/*
 * _DEFAULT_SOURCE keeps MAP_ANONYMOUS visible on glibc/uClibc/Bionic, and
 * _DARWIN_C_SOURCE keeps MAP_ANON visible on macOS when _POSIX_C_SOURCE is
 * set. Both stay scoped to this platform translation unit only.
 */
/* NOLINTBEGIN(bugprone-reserved-identifier, readability-identifier-naming) */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
/* NOLINTEND(bugprone-reserved-identifier, readability-identifier-naming) */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pal/compiler.h"
#include "pal/pal.h"
#include "pal/pal_os.h"
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
 * Apply the requested pin and QoS from inside the new thread. Both calls are
 * best-effort: a cpuset denial must not prevent the thread from running, and
 * macOS silently ignores the pin because it has no hard-affinity API. No
 * pthread_cancel() is used anywhere: Bionic does not provide it.
 */
static void *pal_thread_trampoline(void *opaque)
{
    pal_thread_t *thread = opaque;

    if (thread == NULL) {
        return NULL;
    }

    if (thread->pin_cpu >= 0) {
        thread->pin_status = pal_pin_cpu(thread->pin_cpu);
    }
    if (thread->qos_perf) {
        (void)pal_set_qos(true);
    }
    if (thread->fn != NULL) {
        thread->fn(thread->arg);
    }
    return NULL;
}

pal_thread_t *pal_thread_spawn(pal_thread_fn_t fn, void *arg, int32_t pin_cpu, bool qos_perf)
{
    pal_thread_t *thread = NULL;
    int rc = 0;

    if (fn == NULL) {
        return NULL;
    }

    thread = calloc(1u, sizeof(*thread));
    if (thread == NULL) {
        return NULL;
    }

    thread->fn = fn;
    thread->arg = arg;
    thread->pin_cpu = pin_cpu;
    thread->pin_status = (pin_cpu >= 0) ? LB_STATUS_ERROR : LB_STATUS_OK;
    thread->qos_perf = qos_perf;

    rc = pthread_create(&thread->handle, NULL, pal_thread_trampoline, thread);
    if (rc != 0) {
        free(thread);
        return NULL;
    }
    return thread;
}

lb_status_t pal_thread_join_ex(pal_thread_t *t, lb_status_t *pin_status)
{
    int rc = 0;

    if (t == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    rc = pthread_join(t->handle, NULL);
    if (rc != 0) {
        return LB_STATUS_ERROR;
    }

    if (pin_status != NULL) {
        *pin_status = t->pin_status;
    }
    free(t);
    return LB_STATUS_OK;
}

lb_status_t pal_thread_join(pal_thread_t *t)
{
    return pal_thread_join_ex(t, NULL);
}

/* ------------------------------------------------------ operating-system probes */

#define PAL_OS_SYSCALL_WARMUP 1000u
#define PAL_OS_SD_MAX_THREADS 7u
#define PAL_OS_SD_READY_TIMEOUT_NS UINT64_C(1000000000)

#if defined(__APPLE__)
#define PAL_OS_MAP_ANON MAP_ANON
#else
#define PAL_OS_MAP_ANON MAP_ANONYMOUS
#endif

static volatile sig_atomic_t g_pal_os_sig_count = 0;

static void pal_os_sig_handler(int signo)
{
    (void)signo;
    g_pal_os_sig_count++;
}

lb_status_t pal_os_syscall_probe(uint32_t calls, pal_os_syscall_result_t *out)
{
    int fd = -1;
    uint8_t byte = 0x5au;
    uint64_t last = 0u;
    uint64_t start = 0u;
    uint64_t end = 0u;
    uint32_t i = 0u;
    lb_status_t status = LB_STATUS_ERROR;

    if (out == NULL || calls == 0u) {
        return LB_STATUS_INVALID_ARG;
    }
    out->timer_ns = 0u;
    out->syscall_ns = 0u;
    out->calls = 0u;

    fd = open("/dev/null", O_WRONLY);
    if (fd < 0) {
        goto cleanup;
    }

    for (i = 0u; i < PAL_OS_SYSCALL_WARMUP; i++) {
        last = pal_now_ns();
        if (write(fd, &byte, 1u) != 1) {
            goto cleanup;
        }
    }
    PAL_KEEP_INT(last);

    start = pal_now_ns();
    for (i = 0u; i < calls; i++) {
        last = pal_now_ns();
        PAL_BARRIER();
    }
    end = pal_now_ns();
    PAL_KEEP_INT(last);
    out->timer_ns = (end >= start) ? (end - start) : 0u;

    start = pal_now_ns();
    for (i = 0u; i < calls; i++) {
        if (write(fd, &byte, 1u) != 1) {
            goto cleanup;
        }
    }
    end = pal_now_ns();
    out->syscall_ns = (end >= start) ? (end - start) : 0u;
    out->calls = (uint64_t)calls;
    status = LB_STATUS_OK;

cleanup:
    if (fd >= 0) {
        close(fd);
    }
    return status;
}

typedef struct pal_os_pipe_ctx_t {
    int request_fd;
    int response_fd;
    uint32_t rounds;
    uint32_t completed;
} pal_os_pipe_ctx_t;

static void pal_os_pipe_worker(pal_os_pipe_ctx_t *ctx)
{
    uint8_t token = 0u;
    uint32_t i = 0u;

    if (ctx == NULL) {
        return;
    }

    for (i = 0u; i < ctx->rounds; i++) {
        ssize_t count = read(ctx->request_fd, &token, 1u);

        if (count != 1) {
            break;
        }
        token = (uint8_t)(token + 1u);
        count = write(ctx->response_fd, &token, 1u);
        if (count != 1) {
            break;
        }
        ctx->completed++;
    }
}

static void pal_os_pipe_thread_main(void *arg)
{
    pal_os_pipe_worker(arg);
}

lb_status_t pal_os_ctx_thread(uint32_t round_trips, pal_os_ctx_result_t *out)
{
    int request[2] = {-1, -1};
    int response[2] = {-1, -1};
    pal_os_pipe_ctx_t ctx = {0};
    pal_thread_t *thread = NULL;
    uint8_t token = 0u;
    uint64_t start = 0u;
    uint64_t end = 0u;
    uint32_t i = 0u;
    uint32_t completed = 0u;
    lb_status_t status = LB_STATUS_ERROR;

    if (out == NULL || round_trips == 0u) {
        return LB_STATUS_INVALID_ARG;
    }
    out->elapsed_ns = 0u;
    out->switches = 0u;

    if (pipe(request) != 0) {
        goto cleanup;
    }
    if (pipe(response) != 0) {
        goto cleanup;
    }

    ctx.request_fd = request[0];
    ctx.response_fd = response[1];
    ctx.rounds = round_trips;
    ctx.completed = 0u;

    thread = pal_thread_spawn(pal_os_pipe_thread_main, &ctx, -1, false);
    if (thread == NULL) {
        goto cleanup;
    }

    start = pal_now_ns();
    for (i = 0u; i < round_trips; i++) {
        if (write(request[1], &token, 1u) != 1) {
            break;
        }
        if (read(response[0], &token, 1u) != 1) {
            break;
        }
        completed++;
    }
    end = pal_now_ns();

    close(request[1]);
    request[1] = -1;
    if (pal_thread_join(thread) == LB_STATUS_OK) {
        thread = NULL;
    } else {
        goto cleanup;
    }

    if (completed == 0u) {
        goto cleanup;
    }
    out->elapsed_ns = (end >= start) ? (end - start) : 0u;
    out->switches = (uint64_t)completed * 2u;
    status = LB_STATUS_OK;

cleanup:
    if (request[1] >= 0) {
        close(request[1]);
        request[1] = -1;
    }
    if (thread != NULL) {
        (void)pal_thread_join(thread);
    }
    if (request[0] >= 0) {
        close(request[0]);
    }
    if (response[0] >= 0) {
        close(response[0]);
    }
    if (response[1] >= 0) {
        close(response[1]);
    }
    return status;
}

lb_status_t pal_os_ctx_proc(uint32_t round_trips, pal_os_ctx_result_t *out)
{
    int request[2] = {-1, -1};
    int response[2] = {-1, -1};
    pid_t pid = -1;
    uint8_t token = 0u;
    uint64_t start = 0u;
    uint64_t end = 0u;
    uint32_t i = 0u;
    uint32_t completed = 0u;
    int wait_status = 0;
    lb_status_t status = LB_STATUS_ERROR;

    if (out == NULL || round_trips == 0u) {
        return LB_STATUS_INVALID_ARG;
    }
    out->elapsed_ns = 0u;
    out->switches = 0u;

    if (pipe(request) != 0) {
        goto cleanup;
    }
    if (pipe(response) != 0) {
        goto cleanup;
    }

    pid = fork();
    if (pid < 0) {
        goto cleanup;
    }
    if (pid == 0) {
        pal_os_pipe_ctx_t child_ctx = {0};

        close(request[1]);
        close(response[0]);
        child_ctx.request_fd = request[0];
        child_ctx.response_fd = response[1];
        child_ctx.rounds = round_trips;
        pal_os_pipe_worker(&child_ctx);
        _exit(0);
    }

    close(request[0]);
    request[0] = -1;
    close(response[1]);
    response[1] = -1;

    start = pal_now_ns();
    for (i = 0u; i < round_trips; i++) {
        if (write(request[1], &token, 1u) != 1) {
            break;
        }
        if (read(response[0], &token, 1u) != 1) {
            break;
        }
        completed++;
    }
    end = pal_now_ns();

    close(request[1]);
    request[1] = -1;

    while (waitpid(pid, &wait_status, 0) < 0) {
        if (errno != EINTR) {
            goto cleanup;
        }
    }
    pid = -1;

    if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
        goto cleanup;
    }
    if (completed == 0u) {
        goto cleanup;
    }

    out->elapsed_ns = (end >= start) ? (end - start) : 0u;
    out->switches = (uint64_t)completed * 2u;
    status = LB_STATUS_OK;

cleanup:
    if (request[1] >= 0) {
        close(request[1]);
        request[1] = -1;
    }
    if (pid > 0) {
        int child_status = 0;

        while (waitpid(pid, &child_status, 0) < 0) {
            if (errno != EINTR) {
                break;
            }
        }
    }
    if (request[0] >= 0) {
        close(request[0]);
    }
    if (response[0] >= 0) {
        close(response[0]);
    }
    if (response[1] >= 0) {
        close(response[1]);
    }
    return status;
}

typedef struct pal_os_wakeup_ctx_t {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    uint32_t rounds;
    uint32_t completed;
    uint32_t ready;
    uint32_t turn;
    uint32_t stop;
} pal_os_wakeup_ctx_t;

static void pal_os_wakeup_main(void *arg)
{
    pal_os_wakeup_ctx_t *ctx = arg;
    uint32_t i = 0u;

    if (ctx == NULL) {
        return;
    }
    if (pthread_mutex_lock(&ctx->mutex) != 0) {
        return;
    }

    ctx->ready = 1u;
    (void)pthread_cond_signal(&ctx->cond);

    for (i = 0u; i < ctx->rounds; i++) {
        while (ctx->turn != 1u && ctx->stop == 0u) {
            if (pthread_cond_wait(&ctx->cond, &ctx->mutex) != 0) {
                ctx->stop = 1u;
                break;
            }
        }
        if (ctx->stop != 0u) {
            break;
        }
        ctx->turn = 0u;
        ctx->completed++;
        (void)pthread_cond_signal(&ctx->cond);
    }

    (void)pthread_mutex_unlock(&ctx->mutex);
}

lb_status_t pal_os_wakeup(uint32_t round_trips, pal_os_wakeup_result_t *out)
{
    pal_os_wakeup_ctx_t ctx = {0};
    pal_thread_t *thread = NULL;
    bool mutex_ready = false;
    bool cond_ready = false;
    uint64_t start = 0u;
    uint64_t end = 0u;
    uint32_t i = 0u;
    uint32_t completed = 0u;
    lb_status_t status = LB_STATUS_ERROR;

    if (out == NULL || round_trips == 0u) {
        return LB_STATUS_INVALID_ARG;
    }
    out->elapsed_ns = 0u;
    out->wakeups = 0u;

    ctx.rounds = round_trips;
    if (pthread_mutex_init(&ctx.mutex, NULL) != 0) {
        goto cleanup;
    }
    mutex_ready = true;
    if (pthread_cond_init(&ctx.cond, NULL) != 0) {
        goto cleanup;
    }
    cond_ready = true;

    thread = pal_thread_spawn(pal_os_wakeup_main, &ctx, -1, false);
    if (thread == NULL) {
        goto cleanup;
    }

    if (pthread_mutex_lock(&ctx.mutex) != 0) {
        goto cleanup;
    }
    while (ctx.ready == 0u && ctx.stop == 0u) {
        if (pthread_cond_wait(&ctx.cond, &ctx.mutex) != 0) {
            ctx.stop = 1u;
            break;
        }
    }

    start = pal_now_ns();
    for (i = 0u; i < round_trips; i++) {
        ctx.turn = 1u;
        (void)pthread_cond_signal(&ctx.cond);
        while (ctx.turn != 0u && ctx.stop == 0u) {
            if (pthread_cond_wait(&ctx.cond, &ctx.mutex) != 0) {
                ctx.stop = 1u;
                break;
            }
        }
        if (ctx.stop != 0u || ctx.turn != 0u) {
            break;
        }
        completed++;
    }
    end = pal_now_ns();

    ctx.stop = 1u;
    (void)pthread_cond_signal(&ctx.cond);
    (void)pthread_mutex_unlock(&ctx.mutex);

    if (pal_thread_join(thread) == LB_STATUS_OK) {
        thread = NULL;
    } else {
        goto cleanup;
    }

    if (completed == 0u) {
        goto cleanup;
    }
    out->elapsed_ns = (end >= start) ? (end - start) : 0u;
    out->wakeups = (uint64_t)completed * 2u;
    status = LB_STATUS_OK;

cleanup:
    if (thread != NULL) {
        if (pthread_mutex_lock(&ctx.mutex) == 0) {
            ctx.stop = 1u;
            (void)pthread_cond_signal(&ctx.cond);
            (void)pthread_mutex_unlock(&ctx.mutex);
        }
        (void)pal_thread_join(thread);
    }
    if (cond_ready) {
        (void)pthread_cond_destroy(&ctx.cond);
    }
    if (mutex_ready) {
        (void)pthread_mutex_destroy(&ctx.mutex);
    }
    return status;
}

typedef struct pal_os_sd_ctx_t {
    const volatile uint8_t *page;
    uint32_t *stop;
    uint32_t *ready;
    uint32_t index;
    uint64_t sink;
} pal_os_sd_ctx_t;

static void pal_os_sd_worker(void *arg)
{
    pal_os_sd_ctx_t *ctx = arg;
    uint64_t sum = 0u;

    if (ctx == NULL) {
        return;
    }

    pal_atomic_store_u32(&ctx->ready[ctx->index], 1u);
    while (pal_atomic_load_u32_relaxed(ctx->stop) == 0u) {
        sum += (uint64_t)ctx->page[1];
    }
    ctx->sink = sum;
}

lb_status_t pal_os_shootdown(uint32_t iterations, pal_os_shootdown_result_t *out)
{
    const size_t page_size = pal_page_size();
    const uint32_t cpu_count = pal_cpu_count();
    void *mapping = MAP_FAILED;
    size_t mapping_size = 0u;
    uint8_t *page = NULL;
    volatile uint8_t *vpage = NULL;
    uint32_t stop = 0u;
    uint32_t ready[PAL_OS_SD_MAX_THREADS] = {0};
    pal_os_sd_ctx_t contexts[PAL_OS_SD_MAX_THREADS] = {0};
    pal_thread_t *threads[PAL_OS_SD_MAX_THREADS] = {0};
    uint32_t background = 0u;
    uint32_t i = 0u;
    uint32_t completed = 0u;
    uint64_t start = 0u;
    uint64_t end = 0u;
    uint64_t deadline = 0u;
    lb_status_t status = LB_STATUS_ERROR;

    if (out == NULL || iterations == 0u) {
        return LB_STATUS_INVALID_ARG;
    }
    out->single_ns = 0u;
    out->single_ops = 0u;
    out->multi_ns = 0u;
    out->multi_ops = 0u;
    out->background_threads = 0u;
    out->multi_skipped = false;

    if (page_size < 2u || (page_size & (page_size - 1u)) != 0u || page_size > (SIZE_MAX / 2u)) {
        return LB_STATUS_ERROR;
    }

    /*
     * mprotect() only accepts whole pages. Allocate the probe page with mmap()
     * directly instead of pal_mem_alloc(), whose malloc() fallback could place
     * it inside a heap arena and make mprotect() revoke access to unrelated
     * allocations that share the page.
     */
    mapping_size = page_size * 2u;
    mapping =
        mmap(NULL, mapping_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | PAL_OS_MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) {
        goto cleanup;
    }
    page = (uint8_t *)(((uintptr_t)mapping + (uintptr_t)page_size - 1u) &
                       ~((uintptr_t)page_size - 1u));
    vpage = page;
    vpage[0] = 0x5au;
    vpage[1] = 0xa5u;

    start = pal_now_ns();
    for (i = 0u; i < iterations; i++) {
        if (mprotect(page, page_size, PROT_READ) != 0) {
            break;
        }
        (void)vpage[0];
        if (mprotect(page, page_size, PROT_READ | PROT_WRITE) != 0) {
            break;
        }
        vpage[0] = (uint8_t)(vpage[0] + 1u);
        completed++;
    }
    end = pal_now_ns();

    if (completed == 0u) {
        goto cleanup;
    }
    out->single_ns = (end >= start) ? (end - start) : 0u;
    out->single_ops = (uint64_t)completed * 2u;

    if (cpu_count < 2u) {
        out->multi_skipped = true;
        status = LB_STATUS_OK;
        goto cleanup;
    }

    background = cpu_count - 1u;
    if (background > PAL_OS_SD_MAX_THREADS) {
        background = PAL_OS_SD_MAX_THREADS;
    }
    out->background_threads = background;

    for (i = 0u; i < background; i++) {
        contexts[i].page = vpage;
        contexts[i].stop = &stop;
        contexts[i].ready = ready;
        contexts[i].index = i;
        contexts[i].sink = 0u;
        threads[i] = pal_thread_spawn(pal_os_sd_worker, &contexts[i], -1, false);
        if (threads[i] == NULL) {
            goto cleanup;
        }
    }

    deadline = pal_now_ns() + PAL_OS_SD_READY_TIMEOUT_NS;
    for (i = 0u; i < background; i++) {
        while (pal_atomic_load_u32(&ready[i]) == 0u && pal_now_ns() < deadline) {
            /* Wait for the worker to enter its page-access loop. */
        }
        if (pal_atomic_load_u32(&ready[i]) == 0u) {
            goto cleanup;
        }
    }

    completed = 0u;
    start = pal_now_ns();
    for (i = 0u; i < iterations; i++) {
        if (mprotect(page, page_size, PROT_READ) != 0) {
            break;
        }
        (void)vpage[0];
        if (mprotect(page, page_size, PROT_READ | PROT_WRITE) != 0) {
            break;
        }
        vpage[0] = (uint8_t)(vpage[0] + 1u);
        completed++;
    }
    end = pal_now_ns();

    if (completed == 0u) {
        goto cleanup;
    }
    out->multi_ns = (end >= start) ? (end - start) : 0u;
    out->multi_ops = (uint64_t)completed * 2u;
    status = LB_STATUS_OK;

cleanup:
    pal_atomic_store_u32(&stop, 1u);
    for (i = 0u; i < PAL_OS_SD_MAX_THREADS; i++) {
        if (threads[i] != NULL) {
            (void)pal_thread_join(threads[i]);
            threads[i] = NULL;
        }
    }
    if (mapping != MAP_FAILED) {
        (void)munmap(mapping, mapping_size);
    }
    return status;
}

lb_status_t pal_os_fork_once(uint32_t forks, pal_os_fork_result_t *out)
{
    uint64_t start = 0u;
    uint64_t end = 0u;
    uint32_t i = 0u;
    uint32_t completed = 0u;
    lb_status_t status = LB_STATUS_ERROR;

    if (out == NULL || forks == 0u) {
        return LB_STATUS_INVALID_ARG;
    }
    out->elapsed_ns = 0u;
    out->forks = 0u;

    start = pal_now_ns();
    for (i = 0u; i < forks; i++) {
        const pid_t pid = fork();
        int wait_status = 0;

        if (pid < 0) {
            goto cleanup;
        }
        if (pid == 0) {
            _exit(0);
        }
        while (waitpid(pid, &wait_status, 0) < 0) {
            if (errno != EINTR) {
                goto cleanup;
            }
        }
        if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
            goto cleanup;
        }
        completed++;
    }
    end = pal_now_ns();

    out->elapsed_ns = (end >= start) ? (end - start) : 0u;
    out->forks = (uint64_t)completed;
    status = LB_STATUS_OK;

cleanup:
    return status;
}

lb_status_t pal_os_sig_roundtrip(uint32_t signals, pal_os_sig_result_t *out)
{
    struct sigaction action = {0};
    struct sigaction previous = {0};
    sigset_t sigusr1 = {0};
    sigset_t previous_mask = {0};
    uint64_t start = 0u;
    uint64_t end = 0u;
    uint32_t i = 0u;
    uint32_t completed = 0u;
    bool installed = false;
    bool mask_saved = false;
    lb_status_t status = LB_STATUS_ERROR;

    if (out == NULL || signals == 0u) {
        return LB_STATUS_INVALID_ARG;
    }
    out->elapsed_ns = 0u;
    out->signals = 0u;

    action.sa_handler = pal_os_sig_handler;
    if (sigemptyset(&action.sa_mask) != 0) {
        goto cleanup;
    }
    action.sa_flags = 0;
    if (sigaction(SIGUSR1, &action, &previous) != 0) {
        goto cleanup;
    }
    installed = true;
    g_pal_os_sig_count = 0;

    /*
     * A caller may have blocked SIGUSR1 for its own purposes. Unblock it for
     * the duration of the probe so pthread_kill() actually delivers, then put
     * the original mask back on every exit path.
     */
    if (sigemptyset(&sigusr1) != 0 || sigaddset(&sigusr1, SIGUSR1) != 0) {
        goto cleanup;
    }
    if (pthread_sigmask(SIG_UNBLOCK, &sigusr1, &previous_mask) != 0) {
        goto cleanup;
    }
    mask_saved = true;

    start = pal_now_ns();
    for (i = 0u; i < signals; i++) {
        if (pthread_kill(pthread_self(), SIGUSR1) != 0) {
            break;
        }
        completed++;
    }
    end = pal_now_ns();

    if (pthread_sigmask(SIG_SETMASK, &previous_mask, NULL) != 0) {
        goto cleanup;
    }
    mask_saved = false;

    if (sigaction(SIGUSR1, &previous, NULL) != 0) {
        goto cleanup;
    }
    installed = false;

    if (completed == 0u || g_pal_os_sig_count == 0) {
        goto cleanup;
    }
    out->elapsed_ns = (end >= start) ? (end - start) : 0u;
    out->signals = (uint64_t)completed;
    status = LB_STATUS_OK;

cleanup:
    if (mask_saved) {
        (void)pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);
    }
    if (installed) {
        (void)sigaction(SIGUSR1, &previous, NULL);
    }
    return status;
}

lb_status_t pal_os_sleep_us(uint32_t target_us, uint32_t samples, uint64_t *samples_ns,
                            size_t capacity, size_t *out_count)
{
    uint32_t i = 0u;

    if (target_us == 0u || samples == 0u || samples_ns == NULL || out_count == NULL ||
        capacity < (size_t)samples) {
        return LB_STATUS_INVALID_ARG;
    }
    *out_count = 0u;

    for (i = 0u; i < samples; i++) {
        struct timespec request = {0, 0};
        struct timespec remaining = {0, 0};
        const uint64_t start = pal_now_ns();
        uint64_t end = 0u;

        request.tv_sec = (time_t)(target_us / 1000000u);
        request.tv_nsec = (long)((target_us % 1000000u) * 1000u);
        while (nanosleep(&request, &remaining) != 0) {
            if (errno != EINTR) {
                return LB_STATUS_ERROR;
            }
            request = remaining;
        }
        end = pal_now_ns();
        samples_ns[i] = (end >= start) ? (end - start) : 0u;
        (*out_count)++;
    }
    return LB_STATUS_OK;
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

bool pal_pin_supported(void)
{
    return false;
}

bool pal_ilp_scaling_expected(void)
{
    return true;
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
