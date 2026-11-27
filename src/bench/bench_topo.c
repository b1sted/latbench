/* SPDX-License-Identifier: GPL-3.0 */

#include "bench.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench_common.h"
#include "pal/compiler.h"
#include "pal/pal.h"
#include "passport.h"
#include "stats.h"
#include "tty.h"
#include "types.h"

/* topo atomic: uncontended fetch-add and sequential fence throughput. */
#define TOPO_ATOMIC_ITERATIONS UINT64_C(5000000)
#define TOPO_ATOMIC_WARMUP UINT64_C(500000)
#define TOPO_ATOMIC_REPEATS 3u

/* topo falseshare: page-aligned arena and counter byte offsets to probe. */
#define TOPO_FS_BUFFER_BYTES 4096u
#define TOPO_FS_CHUNK UINT64_C(1024)
#define TOPO_FS_DEFAULT_BATCH_MS 20u
#define TOPO_FS_MIN_BATCH_MS 5u
#define TOPO_FS_OFFSET_COUNT 9u
#define TOPO_FS_ATTEMPTS 3u
#define TOPO_FS_REPEATS 3u
#define TOPO_FS_READY_TIMEOUT_MS 5000u

/* topo pingpong: one shared cache line and short best-of-five batches per pair. */
#define TOPO_PP_SHARED_BYTES 4096u
#define TOPO_PP_BATCH_MS 2u
#define TOPO_PP_BATCH_COUNT 5u
#define TOPO_PP_MAX_CPUS 64u
#define TOPO_PP_TIME_CHECK_MASK UINT64_C(0x3f)
#define TOPO_PP_STOP_SEQ UINT32_MAX

/* Compact auto mode: fewer atomics and a small representative CPU subset. */
#define TOPO_FAST_REPEATS 2u
#define TOPO_ATOMIC_FAST_ITERATIONS UINT64_C(1000000)
#define TOPO_PP_FAST_MAX_CPUS 8u

static const size_t TOPO_FS_OFFSETS[TOPO_FS_OFFSET_COUNT] = {
    0u, 8u, 16u, 32u, 64u, 96u, 128u, 192u, 256u,
};

/*
 * Volatile result sink. Every timed loop publishes its checksum here so the
 * optimizer cannot discard the dependent work.
 */
static volatile uint64_t g_topo_sink = 0u;

typedef struct topo_fs_ctx_t {
    uint64_t *counter;
    uint32_t *ready;
    uint32_t *go;
    uint32_t *stop;
    uint64_t iterations;
    uint64_t elapsed_ns;
} topo_fs_ctx_t;

/*
 * Ping-pong state. All fields live inside the first 64-byte cache line of the
 * page-aligned arena, so the token handoff never leaves one line.
 */
typedef struct topo_pp_shared_t {
    uint32_t flag_a;
    uint32_t flag_b;
    uint32_t token;
    uint32_t ready;
    uint32_t stop;
} topo_pp_shared_t;

typedef struct topo_pp_ctx_t {
    topo_pp_shared_t *shared;
    bool is_a;
    bool cas_mode;
    bool apply_qos;
    bool qos_high;
    uint32_t batch_ms;
    uint64_t rtt_sum_ns;
    uint64_t rounds;
} topo_pp_ctx_t;

static bool topo_join_thread_status(pal_thread_t **thread, lb_status_t *pin_status)
{
    lb_status_t status = LB_STATUS_OK;

    if (thread == NULL || *thread == NULL) {
        return true;
    }

    status = pal_thread_join_ex(*thread, pin_status);
    if (status == LB_STATUS_OK) {
        *thread = NULL;
        return true;
    }
    return false;
}

static bool topo_join_thread(pal_thread_t **thread)
{
    return topo_join_thread_status(thread, NULL);
}

/* --------------------------------------------------------------- topo atomic */

static PAL_NOINLINE uint64_t topo_atomic_fetch_add_loop(uint64_t *cell, uint64_t iterations)
{
    uint64_t i = 0u;

    for (i = 0u; i < iterations; i++) {
        (void)pal_atomic_fetch_add_u64(cell, UINT64_C(1));
    }

    PAL_BARRIER();
    g_topo_sink = *cell;
    return g_topo_sink;
}

static PAL_NOINLINE uint64_t topo_atomic_fence_loop(uint64_t *cell, uint64_t iterations)
{
    uint64_t acc = UINT64_C(1);
    uint64_t i = 0u;

    (void)cell;
    for (i = 0u; i < iterations; i++) {
        pal_atomic_fence_seq();
        acc += i;
        PAL_KEEP_INT(acc);
    }

    PAL_BARRIER();
    g_topo_sink = acc;
    return acc;
}

static double topo_atomic_measure(uint64_t (*fn)(uint64_t *, uint64_t), uint64_t *cell,
                                  uint64_t iterations, size_t repeats)
{
    uint64_t best = UINT64_MAX;
    size_t repeat = 0u;

    (void)fn(cell, iterations / 10u);
    for (repeat = 0u; repeat < repeats; repeat++) {
        const uint64_t begin = pal_now_ns();
        uint64_t end = 0u;
        uint64_t elapsed = 0u;

        (void)fn(cell, iterations);
        end = pal_now_ns();
        elapsed = (end > begin) ? (end - begin) : 0u;
        if (elapsed > 0u && elapsed < best) {
            best = elapsed;
        }
    }

    if (best == UINT64_MAX) {
        return 0.0;
    }
    return (double)best / (double)iterations;
}

lb_status_t topo_atomic_run(const lb_cfg_t *cfg)
{
    uint64_t cell = 0u;
    double fetch_ns = 0.0;
    double fence_ns = 0.0;
    uint64_t iterations = 0u;
    size_t repeats = 0u;
    uint64_t baseline = 0u;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    iterations = cfg->fast ? TOPO_ATOMIC_FAST_ITERATIONS : TOPO_ATOMIC_ITERATIONS;
    repeats = cfg->fast ? TOPO_FAST_REPEATS : TOPO_ATOMIC_REPEATS;

    lb_bench_apply_runtime(cfg);

    tty_section(cfg, "Topology atomics");
    tty_kv(cfg, "Iterations", "%" PRIu64, iterations);
    tty_kv(cfg, "Repeats", "%zu", repeats);
    tty_blank(cfg);
    tty_header(cfg, "  %-28s %12s", "Operation", "ns/op");
    tty_csv_row(cfg, "topo,atomic,operation,ns_per_op");

    baseline = stats_canary_baseline(LB_BENCH_CANARY_MS);

    fetch_ns = topo_atomic_measure(topo_atomic_fetch_add_loop, &cell, iterations, repeats);
    fence_ns = topo_atomic_measure(topo_atomic_fence_loop, &cell, iterations, repeats);

    tty_header(cfg, "  %-28s %12.2f", "fetch_add_u64 (L1)", fetch_ns);
    tty_header(cfg, "  %-28s %12.2f", "fence_seq + dependent add", fence_ns);
    tty_csv_row(cfg, "topo,atomic,fetch_add_u64,%.2f", fetch_ns);
    tty_csv_row(cfg, "topo,atomic,fence_seq,%.2f", fence_ns);

    if (!(fetch_ns > 0.0) || !(fence_ns > 0.0)) {
        tty_warn("topo atomic could not time one of the probes");
    }

    tty_blank(cfg);
    lb_bench_canary_finish(cfg, baseline, true);
    return LB_STATUS_OK;
}

/* ----------------------------------------------------------- topo falseshare */

static void topo_fs_worker(void *arg)
{
    topo_fs_ctx_t *ctx = arg;
    uint64_t ops = 0u;
    uint64_t start = 0u;
    uint64_t end = 0u;

    if (ctx == NULL || ctx->counter == NULL || ctx->ready == NULL || ctx->go == NULL ||
        ctx->stop == NULL) {
        return;
    }

    /*
     * Publish startup only after the trampoline has pinned this thread and
     * applied its QoS request. The main thread waits for both workers before
     * it starts the timed batch, so a delayed spawn cannot eat the window.
     */
    (void)pal_atomic_fetch_add_u32(ctx->ready, 1u);
    while (pal_atomic_load_u32_relaxed(ctx->go) == 0u) {
        /* spin until the main thread opens the batch */
    }

    start = pal_now_ns();
    while (pal_atomic_load_u32_relaxed(ctx->stop) == 0u) {
        uint64_t i = 0u;

        for (i = 0u; i < TOPO_FS_CHUNK; i++) {
            (void)pal_atomic_fetch_add_u64(ctx->counter, UINT64_C(1));
        }
        ops += TOPO_FS_CHUNK;
    }
    end = pal_now_ns();

    ctx->iterations = ops;
    ctx->elapsed_ns = (end > start) ? (end - start) : 1u;
}

static void topo_pick_pair(const lb_cfg_t *cfg, uint32_t *cpu_a, uint32_t *cpu_b)
{
    int32_t cpus[TOPO_PP_MAX_CPUS] = {0};
    size_t avail = 0u;
    size_t index = 0u;

    if (cpu_a == NULL || cpu_b == NULL) {
        return;
    }

    *cpu_a = 0u;
    *cpu_b = 1u;

    if (pal_cpu_list(cpus, (size_t)TOPO_PP_MAX_CPUS, &avail) != LB_STATUS_OK || avail < 2u) {
        return;
    }

    if (cfg != NULL && cfg->cpu_id >= 0) {
        bool found = false;
        size_t scan = 0u;

        for (scan = 0u; scan < avail; scan++) {
            if (cpus[scan] == cfg->cpu_id) {
                index = scan;
                found = true;
                break;
            }
        }
        if (!found) {
            tty_warn("requested CPU %" PRId32 " is not available, using CPUs %" PRId32
                     " and %" PRId32,
                     cfg->cpu_id, cpus[0], cpus[1]);
            index = 0u;
        }
    }

    if (cpus[index] >= 0) {
        *cpu_a = (uint32_t)cpus[index];
    }
    index = (index + 1u) % avail;
    if (cpus[index] >= 0) {
        *cpu_b = (uint32_t)cpus[index];
    }
}

static uint32_t topo_fs_estimate_line(const double *samples, size_t count)
{
    double best = 0.0;
    size_t i = 0u;

    if (samples == NULL || count == 0u || !(samples[0] > 0.0)) {
        return 0u;
    }

    best = samples[0];
    for (i = 1u; i < count; i++) {
        if (samples[i] > 0.0 && samples[i] < best) {
            best = samples[i];
        }
    }

    /* Require a real drop before claiming a coherence-line boundary. */
    if (!(best < samples[0] * 0.8)) {
        return 0u;
    }

    /*
     * A 32-byte line is legitimate on some cores (and under emulation). Skip
     * the sub-line offsets and report the first plateau at or above 32 bytes.
     * The plateau must hold for every later offset, so a single dip at 64 B
     * cannot make a 128-byte SoC report a false 64-byte line.
     */
    for (i = 0u; i < count; i++) {
        size_t j = 0u;
        bool plateau = true;

        if (TOPO_FS_OFFSETS[i] < 32u) {
            continue;
        }
        if (!(samples[i] <= best * 1.15)) {
            continue;
        }
        for (j = i + 1u; j < count; j++) {
            if (!(samples[j] <= best * 1.25)) {
                plateau = false;
                break;
            }
        }
        if (!plateau) {
            continue;
        }
        if (TOPO_FS_OFFSETS[i] <= 32u) {
            return 32u;
        }
        if (TOPO_FS_OFFSETS[i] <= 64u) {
            return 64u;
        }
        return 128u;
    }
    return 0u;
}

/*
 * Run one false-sharing batch. Both workers must report ready before the
 * batch timer starts; otherwise a thread that is still waiting for its CPU
 * slot can burn the whole window and leave the counters at zero.
 */
static bool topo_fs_step(topo_fs_ctx_t *ctx_a, topo_fs_ctx_t *ctx_b, uint32_t *ready, uint32_t *go,
                         uint32_t *stop, uint32_t batch_ms, bool pin, uint32_t cpu_a,
                         uint32_t cpu_b, bool *out_pin_ok)
{
    const uint64_t timeout_ns = (uint64_t)TOPO_FS_READY_TIMEOUT_MS * UINT64_C(1000000);
    const uint64_t deadline = pal_now_ns() + timeout_ns;
    pal_thread_t *thread_a = NULL;
    pal_thread_t *thread_b = NULL;
    lb_status_t pin_a = LB_STATUS_OK;
    lb_status_t pin_b = LB_STATUS_OK;
    bool joined_a = false;
    bool joined_b = false;
    bool ready_ok = false;
    bool ok = false;

    if (out_pin_ok != NULL) {
        *out_pin_ok = true;
    }

    pal_atomic_store_u32_relaxed(ready, 0u);
    pal_atomic_store_u32_relaxed(go, 0u);
    pal_atomic_store_u32_relaxed(stop, 0u);

    thread_a = pal_thread_spawn(topo_fs_worker, ctx_a, pin ? (int32_t)cpu_a : -1, !pin);
    thread_b = pal_thread_spawn(topo_fs_worker, ctx_b, pin ? (int32_t)cpu_b : -1, !pin);
    if (thread_a == NULL || thread_b == NULL) {
        pal_atomic_store_u32_relaxed(go, 1u);
        pal_atomic_store_u32_relaxed(stop, 1u);
        (void)topo_join_thread(&thread_a);
        (void)topo_join_thread(&thread_b);
        return false;
    }

    while (pal_atomic_load_u32_relaxed(ready) < 2u) {
        if (pal_now_ns() >= deadline) {
            break;
        }
    }
    ready_ok = pal_atomic_load_u32_relaxed(ready) >= 2u;

    pal_atomic_store_u32_relaxed(go, 1u);
    if (ready_ok) {
        pal_sleep_ms(batch_ms);
    }
    pal_atomic_store_u32_relaxed(stop, 1u);

    joined_a = topo_join_thread_status(&thread_a, pin ? &pin_a : NULL);
    joined_b = topo_join_thread_status(&thread_b, pin ? &pin_b : NULL);
    if (!joined_a || !joined_b) {
        return false;
    }

    if (out_pin_ok != NULL && pin && (pin_a != LB_STATUS_OK || pin_b != LB_STATUS_OK)) {
        *out_pin_ok = false;
    }

    ok = ready_ok && ctx_a->iterations > 0u && ctx_b->iterations > 0u;
    return ok;
}

lb_status_t topo_falseshare_run(const lb_cfg_t *cfg)
{
    const uint32_t cpu_count = pal_cpu_count();
    pal_mem_t *arena = NULL;
    uint8_t *base = NULL;
    uint32_t stop = 0u;
    uint32_t ready = 0u;
    uint32_t go = 0u;
    uint32_t cpu_a = 0u;
    uint32_t cpu_b = 1u;
    uint32_t batch_ms = 0u;
    bool pin = false;
    bool pin_warned = false;
    bool header_printed = false;
    double samples[TOPO_FS_OFFSET_COUNT] = {0.0};
    uint32_t line_bytes = 0u;
    uint64_t baseline = 0u;
    size_t offset_index = 0u;
    lb_status_t status = LB_STATUS_OK;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }
    if (cpu_count < 2u) {
        tty_header(cfg, "topo falseshare: skipped (need at least 2 CPUs, found %" PRIu32 ")",
                   cpu_count);
        return LB_STATUS_OK;
    }

    pin = pal_pin_supported();
    if (pin) {
        topo_pick_pair(cfg, &cpu_a, &cpu_b);
    }
    batch_ms = (cfg->batch_ms > 0u) ? cfg->batch_ms : TOPO_FS_DEFAULT_BATCH_MS;
    if (batch_ms < TOPO_FS_MIN_BATCH_MS) {
        batch_ms = TOPO_FS_MIN_BATCH_MS;
    }
    if (cfg->fast && batch_ms > TOPO_FS_MIN_BATCH_MS) {
        batch_ms = TOPO_FS_MIN_BATCH_MS;
    }

    arena = pal_mem_alloc(TOPO_FS_BUFFER_BYTES, PAL_MEM_NO_THP);
    if (arena == NULL) {
        tty_err("cannot allocate the %u-byte false-sharing buffer", TOPO_FS_BUFFER_BYTES);
        return LB_STATUS_NOMEM;
    }
    base = pal_mem_data(arena);
    if (base == NULL) {
        status = LB_STATUS_NOMEM;
        goto cleanup;
    }
    (void)memset(base, 0, TOPO_FS_BUFFER_BYTES);

    tty_section(cfg, "Topology false sharing");
    tty_kv(cfg, "Buffer", "%u bytes (page-aligned)", TOPO_FS_BUFFER_BYTES);
    tty_kv(cfg, "Batch", "%" PRIu32 " ms per offset", batch_ms);

    baseline = stats_canary_baseline(LB_BENCH_CANARY_MS);

    for (offset_index = 0u; offset_index < (size_t)TOPO_FS_OFFSET_COUNT; offset_index++) {
        const size_t offset = TOPO_FS_OFFSETS[offset_index];
        uint64_t *counter_a = (uint64_t *)(void *)(base);
        uint64_t *counter_b = (uint64_t *)(void *)(base + offset);
        double repeat_samples[TOPO_FS_REPEATS] = {0.0};
        size_t repeat = 0u;

        for (repeat = 0u; repeat < (size_t)TOPO_FS_REPEATS; repeat++) {
            topo_fs_ctx_t ctx_a = {0};
            topo_fs_ctx_t ctx_b = {0};
            bool step_ok = false;
            bool step_pin_ok = true;
            uint32_t attempt = 0u;

            ctx_a.counter = counter_a;
            ctx_a.ready = &ready;
            ctx_a.go = &go;
            ctx_a.stop = &stop;
            ctx_b.counter = counter_b;
            ctx_b.ready = &ready;
            ctx_b.go = &go;
            ctx_b.stop = &stop;

            /*
             * A throttled scheduler can still starve a freshly spawned worker.
             * Retry the whole handshake a couple of times before giving up.
             */
            for (attempt = 0u; attempt < TOPO_FS_ATTEMPTS && !step_ok; attempt++) {
                *counter_a = 0u;
                *counter_b = 0u;
                ctx_a.iterations = 0u;
                ctx_b.iterations = 0u;
                step_pin_ok = true;
                step_ok = topo_fs_step(&ctx_a, &ctx_b, &ready, &go, &stop, batch_ms, pin, cpu_a,
                                       cpu_b, &step_pin_ok);
            }
            if (!step_ok) {
                tty_err("false-sharing workers produced no samples");
                status = LB_STATUS_ERROR;
                goto cleanup;
            }
            if (pin && !step_pin_ok && !pin_warned) {
                tty_warn("CPU pinning was not applied to every false-sharing worker; "
                         "treating the run as unpinned");
                pin_warned = true;
            }
            if (!header_printed) {
                if (pin && !pin_warned) {
                    tty_kv(cfg, "Cores", "%" PRIu32 " and %" PRIu32 " (pinned)", cpu_a, cpu_b);
                } else if (pin) {
                    tty_kv(cfg, "Cores", "%" PRIu32 " and %" PRIu32 " (pin denied, unpinned)",
                           cpu_a, cpu_b);
                } else {
                    tty_kv(cfg, "Cores", "2 threads, QoS perf (unpinned)");
                }
                tty_blank(cfg);
                tty_header(cfg, "  %8s %14s", "Offset", "ns/op");
                tty_csv_row(cfg, "topo,falseshare,offset_bytes,ns_per_op");
                header_printed = true;
            }

            repeat_samples[repeat] = (((double)ctx_a.elapsed_ns / (double)ctx_a.iterations) +
                                      ((double)ctx_b.elapsed_ns / (double)ctx_b.iterations)) *
                                     0.5;
        }

        samples[offset_index] =
            stats_percentile(repeat_samples, (size_t)TOPO_FS_REPEATS, STATS_NOISE_PERCENTILE);

        tty_header(cfg, "  %6zu B %14.2f", offset, samples[offset_index]);
        tty_csv_row(cfg, "topo,falseshare,%zu,%.2f", offset, samples[offset_index]);
    }

    line_bytes = topo_fs_estimate_line(samples, (size_t)TOPO_FS_OFFSET_COUNT);
    lb_passport_mutable()->coherence_line_bytes = line_bytes;
    tty_blank(cfg);
    if (line_bytes == 0u) {
        tty_kv(cfg, "Coherence line", "not detected (no clear latency drop)");
    } else {
        tty_kv(cfg, "Coherence line", "~%" PRIu32 " B (latency drop threshold)", line_bytes);
    }

    tty_blank(cfg);
    {
        const bool warn_drift = pin && !pin_warned && (cfg->cpu_id >= 0);

        lb_bench_canary_finish(cfg, baseline, warn_drift);
    }

cleanup:
    pal_atomic_store_u32_relaxed(&go, 1u);
    pal_atomic_store_u32_relaxed(&stop, 1u);
    pal_mem_free(arena);
    return status;
}

/* ------------------------------------------------------------- topo pingpong */

static void topo_pp_responder(topo_pp_shared_t *shared, bool cas_mode)
{
    uint32_t seq = 1u;

    pal_atomic_store_u32(&shared->ready, 1u);

    if (cas_mode) {
        for (;;) {
            (void)pal_atomic_cas_u32_relaxed(&shared->token, 1u, 0u);
            /*
             * Check the stop flag even when the CAS misses: the requester may
             * have released the token and then finished, leaving it at zero.
             */
            if (pal_atomic_load_u32_relaxed(&shared->stop) != 0u) {
                return;
            }
        }
    }

    for (;;) {
        uint32_t value = pal_atomic_load_u32_relaxed(&shared->flag_a);

        /* Bare spin: no PAUSE, YIELD, or sched_yield in the handoff path. */
        while (value != seq) {
            if (pal_atomic_load_u32_relaxed(&shared->stop) != 0u) {
                return;
            }
            value = pal_atomic_load_u32_relaxed(&shared->flag_a);
        }
        pal_atomic_store_u32_relaxed(&shared->flag_b, seq);
        seq++;
    }
}

static void topo_pp_measure_flag(topo_pp_shared_t *shared, uint64_t deadline, topo_pp_ctx_t *ctx)
{
    uint32_t seq = 1u;
    uint64_t rounds = 0u;
    uint64_t start = 0u;
    uint64_t end = 0u;

    /*
     * Read the clock once per batch. The old per-round-trip timestamps folded
     * one pal_now_ns() call into every RTT and broke the purity of the spin.
     */
    start = pal_now_ns();
    for (;;) {
        pal_atomic_store_u32_relaxed(&shared->flag_a, seq);
        while (pal_atomic_load_u32_relaxed(&shared->flag_b) != seq) {
            /* Bare spin: the token handoff must not be slowed by a pause. */
        }
        rounds++;
        seq++;
        if (seq == TOPO_PP_STOP_SEQ) {
            break;
        }
        if ((rounds & TOPO_PP_TIME_CHECK_MASK) == 0u && pal_now_ns() >= deadline) {
            break;
        }
    }
    end = pal_now_ns();

    ctx->rtt_sum_ns = (end > start) ? (end - start) : 0u;
    ctx->rounds = rounds;
}

static void topo_pp_measure_cas(topo_pp_shared_t *shared, uint64_t deadline, topo_pp_ctx_t *ctx)
{
    uint64_t rounds = 0u;
    uint64_t start = 0u;
    uint64_t end = 0u;

    /*
     * The first 0 -> 1 acquisition succeeds immediately and is not a round
     * trip, so it is excluded from the round count below. Timing still wraps
     * the whole batch: the clock is read only before and after the loop.
     */
    start = pal_now_ns();
    for (;;) {
        while (!pal_atomic_cas_u32_relaxed(&shared->token, 0u, 1u)) {
            /* Bare spin: wait for the responder to release the token. */
        }
        rounds++;
        if ((rounds & TOPO_PP_TIME_CHECK_MASK) == 0u && pal_now_ns() >= deadline) {
            break;
        }
    }
    end = pal_now_ns();

    ctx->rtt_sum_ns = (end > start) ? (end - start) : 0u;
    ctx->rounds = (rounds > 1u) ? (rounds - 1u) : 0u;
}

static void topo_pp_worker(void *arg)
{
    topo_pp_ctx_t *ctx = arg;
    topo_pp_shared_t *shared = NULL;
    uint64_t deadline = 0u;

    if (ctx == NULL || ctx->shared == NULL) {
        return;
    }
    shared = ctx->shared;

    if (ctx->apply_qos) {
        (void)pal_set_qos(ctx->qos_high);
    }

    if (!ctx->is_a) {
        topo_pp_responder(shared, ctx->cas_mode);
        return;
    }

    while (pal_atomic_load_u32(&shared->ready) == 0u) {
        if (pal_atomic_load_u32_relaxed(&shared->stop) != 0u) {
            return;
        }
    }

    deadline = pal_now_ns() + ((uint64_t)ctx->batch_ms * UINT64_C(1000000));
    if (ctx->cas_mode) {
        topo_pp_measure_cas(shared, deadline, ctx);
    } else {
        topo_pp_measure_flag(shared, deadline, ctx);
    }
    pal_atomic_store_u32_relaxed(&shared->stop, 1u);
}

static lb_status_t topo_pp_run_pair(uint32_t cpu_a, uint32_t cpu_b, bool qos_a, bool qos_b,
                                    uint32_t batch_ms, bool cas_mode, bool pin, bool *pin_warned,
                                    uint64_t *out_ns)
{
    pal_mem_t *arena = NULL;
    topo_pp_shared_t *shared = NULL;
    topo_pp_ctx_t ctx_a = {0};
    topo_pp_ctx_t ctx_b = {0};
    pal_thread_t *thread_a = NULL;
    pal_thread_t *thread_b = NULL;
    lb_status_t pin_a = LB_STATUS_OK;
    lb_status_t pin_b = LB_STATUS_OK;
    bool joined_a = false;
    bool joined_b = false;
    lb_status_t status = LB_STATUS_ERROR;
    double one_way_ns = 0.0;

    if (out_ns == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    arena = pal_mem_alloc(TOPO_PP_SHARED_BYTES, PAL_MEM_NO_THP);
    if (arena == NULL) {
        tty_err("cannot allocate the %u-byte ping-pong buffer", TOPO_PP_SHARED_BYTES);
        return LB_STATUS_NOMEM;
    }
    shared = pal_mem_data(arena);
    if (shared == NULL) {
        status = LB_STATUS_NOMEM;
        goto cleanup;
    }
    (void)memset(shared, 0, sizeof(*shared));

    ctx_a.shared = shared;
    ctx_a.is_a = true;
    ctx_a.cas_mode = cas_mode;
    ctx_a.apply_qos = !pin;
    ctx_a.qos_high = qos_a;
    ctx_a.batch_ms = batch_ms;

    ctx_b.shared = shared;
    ctx_b.is_a = false;
    ctx_b.cas_mode = cas_mode;
    ctx_b.apply_qos = !pin;
    ctx_b.qos_high = qos_b;
    ctx_b.batch_ms = batch_ms;

    thread_a = pal_thread_spawn(topo_pp_worker, &ctx_a, pin ? (int32_t)cpu_a : -1, false);
    thread_b = pal_thread_spawn(topo_pp_worker, &ctx_b, pin ? (int32_t)cpu_b : -1, false);
    if (thread_a == NULL || thread_b == NULL) {
        pal_atomic_store_u32_relaxed(&shared->stop, 1u);
        (void)topo_join_thread(&thread_a);
        (void)topo_join_thread(&thread_b);
        tty_err("cannot spawn the ping-pong workers");
        status = LB_STATUS_ERROR;
        goto cleanup;
    }

    joined_a = topo_join_thread_status(&thread_a, pin ? &pin_a : NULL);
    joined_b = topo_join_thread_status(&thread_b, pin ? &pin_b : NULL);
    if (!joined_a || !joined_b) {
        status = LB_STATUS_ERROR;
        goto cleanup;
    }
    if (pin && (pin_a != LB_STATUS_OK || pin_b != LB_STATUS_OK) && pin_warned != NULL &&
        !*pin_warned) {
        tty_warn("CPU pinning was not applied to every ping-pong worker; "
                 "latency may include cross-core migration");
        *pin_warned = true;
    }
    if (ctx_a.rounds == 0u || ctx_a.rtt_sum_ns == 0u) {
        tty_err("ping-pong produced no samples");
        status = LB_STATUS_ERROR;
        goto cleanup;
    }

    one_way_ns = ((double)ctx_a.rtt_sum_ns / (double)ctx_a.rounds) * 0.5;
    if (!(one_way_ns > 0.0)) {
        tty_err("ping-pong latency was not measurable");
        status = LB_STATUS_ERROR;
        goto cleanup;
    }

    *out_ns = (uint64_t)(one_way_ns + 0.5);
    status = LB_STATUS_OK;

cleanup:
    if (shared != NULL) {
        pal_atomic_store_u32_relaxed(&shared->stop, 1u);
    }
    (void)topo_join_thread(&thread_a);
    (void)topo_join_thread(&thread_b);
    pal_mem_free(arena);
    return status;
}

/*
 * A single batch can be hit by a scheduler timeslice or a DVFS transition and
 * report a huge outlier. Three short independent batches keep the same total
 * budget while letting the minimum discard those one-off stalls.
 */
static lb_status_t topo_pp_run_pair_best(uint32_t cpu_a, uint32_t cpu_b, bool qos_a, bool qos_b,
                                         bool cas_mode, bool pin, bool *pin_warned,
                                         uint64_t *out_ns)
{
    uint64_t best = UINT64_MAX;
    size_t batch = 0u;

    if (out_ns == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    for (batch = 0u; batch < TOPO_PP_BATCH_COUNT; batch++) {
        uint64_t sample_ns = 0u;
        const lb_status_t status = topo_pp_run_pair(cpu_a, cpu_b, qos_a, qos_b, TOPO_PP_BATCH_MS,
                                                    cas_mode, pin, pin_warned, &sample_ns);

        if (status != LB_STATUS_OK) {
            return status;
        }
        if (sample_ns < best) {
            best = sample_ns;
        }
    }

    *out_ns = best;
    return LB_STATUS_OK;
}

static int topo_pp_compare_u64(const void *lhs, const void *rhs)
{
    const uint64_t a = *(const uint64_t *)lhs;
    const uint64_t b = *(const uint64_t *)rhs;

    if (a < b) {
        return -1;
    }
    if (a > b) {
        return 1;
    }
    return 0;
}

static void topo_pp_publish(const uint64_t *values, size_t count)
{
    uint64_t *sorted = NULL;
    lb_passport_t *passport = lb_passport_mutable();

    if (values == NULL || count == 0u || count > (SIZE_MAX / sizeof(*sorted))) {
        return;
    }
    sorted = malloc(count * sizeof(*sorted));
    if (sorted == NULL) {
        return;
    }
    (void)memcpy(sorted, values, count * sizeof(*sorted));
    qsort(sorted, count, sizeof(*sorted), topo_pp_compare_u64);
    passport->pp_min_ns = sorted[0];
    passport->pp_med_ns = sorted[count / 2u];
    passport->pp_max_ns = sorted[count - 1u];
    free(sorted);
}

static lb_status_t topo_pp_matrix(const lb_cfg_t *cfg, bool cas_mode)
{
    int32_t cpus[TOPO_PP_MAX_CPUS] = {0};
    uint64_t *matrix = NULL;
    uint64_t *pair_ns = NULL;
    size_t count = 0u;
    const size_t max_count = cfg->fast ? (size_t)TOPO_PP_FAST_MAX_CPUS : (size_t)TOPO_PP_MAX_CPUS;
    size_t pairs = 0u;
    size_t pair_index = 0u;
    size_t i = 0u;
    bool pin_warned = false;
    lb_status_t status = pal_cpu_list(cpus, (size_t)TOPO_PP_MAX_CPUS, &count);

    if (status != LB_STATUS_OK || count < 2u) {
        tty_header(cfg, "topo pingpong: skipped (need at least 2 available CPUs, found %zu)",
                   count);
        return LB_STATUS_OK;
    }
    if (count > max_count) {
        if (!cfg->fast) {
            tty_warn("matrix limited to the first %zu CPUs (found %zu)", max_count, count);
        }
        count = max_count;
    }
    if (count > SIZE_MAX / count) {
        return LB_STATUS_NOMEM;
    }

    matrix = calloc(count * count, sizeof(*matrix));
    if (matrix == NULL) {
        tty_err("cannot allocate a %zu x %zu latency matrix", count, count);
        return LB_STATUS_NOMEM;
    }
    pairs = count * (count - 1u) / 2u;
    if (pairs > 0u) {
        pair_ns = malloc(pairs * sizeof(*pair_ns));
        if (pair_ns == NULL) {
            tty_err("cannot allocate %zu ping-pong samples", pairs);
            free(matrix);
            return LB_STATUS_NOMEM;
        }
    }

    tty_section(cfg, "Topo ping-pong");
    tty_kv(cfg, "Mode", "%s token handoff", cas_mode ? "CAS" : "flag");
    tty_kv(cfg, "CPUs", "%zu", count);
    tty_kv(cfg, "Pairs", "%zu (best of %u x %u ms)", pairs, TOPO_PP_BATCH_COUNT, TOPO_PP_BATCH_MS);

    for (i = 0u; i < count; i++) {
        size_t j = 0u;

        for (j = i + 1u; j < count; j++) {
            uint64_t one_way_ns = 0u;

            if (cpus[i] < 0 || cpus[j] < 0) {
                continue;
            }
            status = topo_pp_run_pair_best((uint32_t)cpus[i], (uint32_t)cpus[j], true, true,
                                           cas_mode, true, &pin_warned, &one_way_ns);
            if (status != LB_STATUS_OK) {
                goto cleanup;
            }
            matrix[i * count + j] = one_way_ns;
            matrix[j * count + i] = one_way_ns;
            if (pair_ns != NULL) {
                pair_ns[pair_index] = one_way_ns;
                pair_index++;
            }
        }
    }

    topo_pp_publish(pair_ns, pair_index);
    tty_blank(cfg);
    status = tty_print_matrix_cpus(cfg, "One-way latency (RTT/2, ns)", cpus, matrix, count);

cleanup:
    free(pair_ns);
    free(matrix);
    return status;
}

static lb_status_t topo_pp_mac_pairs(const lb_cfg_t *cfg, bool cas_mode)
{
    static const char *const TOPO_PP_NAMES[3] = {"P-P", "P-E", "E-E"};
    static const bool TOPO_PP_QOS_A[3] = {true, true, false};
    static const bool TOPO_PP_QOS_B[3] = {true, false, false};
    uint64_t values[3] = {0u, 0u, 0u};
    size_t value_count = 0u;
    size_t i = 0u;

    tty_section(cfg, "Topo ping-pong");
    tty_kv(cfg, "Mode", "%s token handoff", cas_mode ? "CAS" : "flag");
    tty_kv(cfg, "QoS", "P = perf, E = efficiency (unpinned)");
    tty_kv(cfg, "Batch", "best of %u x %u ms per pair", TOPO_PP_BATCH_COUNT, TOPO_PP_BATCH_MS);
    tty_blank(cfg);
    tty_header(cfg, "  %-6s %16s", "Pair", "one-way ns");
    tty_csv_row(cfg, "topo,pingpong,pair,one_way_ns");

    for (i = 0u; i < 3u; i++) {
        uint64_t one_way_ns = 0u;
        const lb_status_t status = topo_pp_run_pair_best(0u, 0u, TOPO_PP_QOS_A[i], TOPO_PP_QOS_B[i],
                                                         cas_mode, false, NULL, &one_way_ns);

        if (status != LB_STATUS_OK) {
            return status;
        }
        tty_header(cfg, "  %-6s %16" PRIu64, TOPO_PP_NAMES[i], one_way_ns);
        tty_csv_row(cfg, "topo,pingpong,%s,%" PRIu64, TOPO_PP_NAMES[i], one_way_ns);
        values[value_count] = one_way_ns;
        value_count++;
    }

    topo_pp_publish(values, value_count);
    return LB_STATUS_OK;
}

lb_status_t topo_pingpong_run(const lb_cfg_t *cfg)
{
    bool cas_mode = false;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    cas_mode = cfg->topo_pingpong_cas;
    if (pal_pin_supported()) {
        return topo_pp_matrix(cfg, cas_mode);
    }
    if (pal_cpu_count() < 2u) {
        tty_header(cfg, "topo pingpong: skipped (need at least 2 CPUs, found %" PRIu32 ")",
                   pal_cpu_count());
        return LB_STATUS_OK;
    }
    return topo_pp_mac_pairs(cfg, cas_mode);
}
