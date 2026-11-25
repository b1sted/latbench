/* SPDX-License-Identifier: GPL-3.0 */

#include "bench.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "defs.h"
#include "pal/compiler.h"
#include "pal/pal.h"
#include "passport.h"
#include "stats.h"
#include "tty.h"
#include "types.h"
#include "util.h"

/* Duration of each DVFS frequency-drift canary measurement. */
#define CORE_CANARY_MS 20u

/* Arena policy when the platform cannot report the amount of physical RAM. */
#define CORE_FALLBACK_BUDGET_BYTES (UINT64_C(4) * UINT64_C(1024) * UINT64_C(1024))

/* Smallest arena worth measuring in any subtest. */
#define CORE_MIN_ARENA_BYTES 4096u

/* core ilp: dependent multiply-add chains, one per available execution port. */
#define CORE_ILP_ITERATIONS UINT64_C(8000000)
#define CORE_ILP_WARMUP UINT64_C(200000)
#define CORE_ILP_REPEATS 3u
#define CORE_ILP_INT_MUL UINT64_C(3)
#define CORE_ILP_INT_ADD UINT64_C(1)
#define CORE_ILP_FP_MUL 1.00000011920928955078125
#define CORE_ILP_FP_ADD 0.5

/* core mlp: parallel pointer-chase chains over an arena larger than L3. */
#define CORE_MLP_MAX_BYTES (UINT64_C(64) * UINT64_C(1024) * UINT64_C(1024))
#define CORE_MLP_MAX_CHAINS 24u
#define CORE_MLP_NODE_BYTES 64u
#define CORE_MLP_NODE_PAD (CORE_MLP_NODE_BYTES - sizeof(void *))
#define CORE_MLP_ITERATIONS UINT64_C(262144)
#define CORE_MLP_REPEATS 3u
#define CORE_MLP_GAIN_MIN_PCT 5.0

/* core align: dependent unaligned reads through memcpy. */
#define CORE_ALIGN_ITERATIONS UINT64_C(10000000)
#define CORE_ALIGN_WARMUP UINT64_C(100000)
#define CORE_ALIGN_REPEATS 3u
#define CORE_ALIGN_CASES 9u

/* core stlf: store then dependent load through the store buffer. */
#define CORE_STLF_ITERATIONS UINT64_C(5000000)
#define CORE_STLF_WARMUP UINT64_C(100000)
#define CORE_STLF_REPEATS 3u
#define CORE_STLF_CASES 6u
#define CORE_STLF_STRADDLE_OFFSET 4u
#define CORE_STLF_MIN_BUFFER_BYTES 8192u

/* Compact auto mode: fewer iterations and repeats; the arena stays above L3. */
#define CORE_FAST_REPEATS 2u
#define CORE_ILP_FAST_ITERATIONS UINT64_C(1000000)
#define CORE_MLP_FAST_ITERATIONS UINT64_C(65536)
#define CORE_ALIGN_FAST_ITERATIONS UINT64_C(1000000)
#define CORE_STLF_FAST_ITERATIONS UINT64_C(1000000)

typedef uint64_t (*core_ilp_fn_t)(uint64_t iterations);

typedef uint64_t (*core_stlf_fn_t)(uint8_t *buf, size_t page_size, uint64_t iterations,
                                   size_t offset);

typedef struct core_node_t {
    struct core_node_t *next;
    /* Structural padding: fixes the node stride at one cache line. */
    uint8_t pad[CORE_MLP_NODE_PAD]; /* cppcheck-suppress unusedStructMember */
} core_node_t;

static const size_t CORE_ILP_CHAINS[] = {1u, 2u, 4u, 6u, 8u};
static const size_t CORE_MLP_CHAINS[] = {1u, 2u, 4u, 6u, 8u, 12u, 16u, 24u};

static const char *const CORE_ALIGN_NAMES[CORE_ALIGN_CASES] = {
    "line 0", "line+1", "line+4", "line+8", "line+56", "line+60", "line+63", "line+64", "page-4",
};

static const size_t CORE_ALIGN_OFFSETS[CORE_ALIGN_CASES] = {0u, 1u, 4u, 8u, 56u, 60u, 63u, 64u};

static const char *const CORE_STLF_NAMES[CORE_STLF_CASES] = {
    "8B @ +0", "4B @ +0", "4B @ +1", "1B @ +1", "8B @ +4", "8B @ page",
};

/*
 * Volatile result sink. Every timed loop publishes its checksum here so the
 * optimizer cannot discard the dependent loads or the dirty stores.
 */
static volatile uint64_t g_core_sink = 0u;

/*
 * Runtime-zero address jitter. The compiler cannot fold `value & mask` to zero
 * because the value is loaded from a volatile object, so each probe keeps a
 * genuine data dependency from the previous load to the next address.
 */
static volatile uint64_t g_core_zero_mask = 0u;

static size_t core_arena_cap(const lb_cfg_t *cfg, uint64_t limit_bytes, bool reserve_indices)
{
    uint64_t budget = pal_safe_max_mem_bytes((uint64_t)cfg->max_mem_mb);

    if (budget == 0u) {
        budget = CORE_FALLBACK_BUDGET_BYTES;
    }
    if (reserve_indices) {
        /* Leave one ninth of the budget for Sattolo index arrays. */
        budget = (budget / 9u) * 8u;
    }
    if (budget > limit_bytes) {
        budget = limit_bytes;
    }
#if SIZE_MAX < UINT64_MAX
    if (budget > (uint64_t)SIZE_MAX) {
        budget = (uint64_t)SIZE_MAX;
    }
#endif
    return (size_t)budget;
}

static uint32_t core_arena_flags(const lb_cfg_t *cfg)
{
    return cfg->no_thp ? PAL_MEM_NO_THP : PAL_MEM_THP;
}

static void core_apply_runtime(const lb_cfg_t *cfg)
{
    if (cfg->runtime_applied) {
        return;
    }

    if (cfg->cpu_id >= 0) {
        if (pal_pin_cpu(cfg->cpu_id) != LB_STATUS_OK) {
            tty_warn("CPU pinning to %" PRId32 " was not applied", cfg->cpu_id);
        }
    }

    if (cfg->qos_set) {
        if (pal_set_qos(cfg->qos_perf) != LB_STATUS_OK) {
            tty_warn("QoS request '%s' was not applied", cfg->qos_perf ? "perf" : "bg");
        }
    }
}

static void core_cool(const lb_cfg_t *cfg)
{
    if (cfg->cool_ms > 0u) {
        pal_sleep_ms(cfg->cool_ms);
    }
}

static void core_canary_finish(const lb_cfg_t *cfg, uint64_t baseline)
{
    const uint64_t after = stats_canary_measure(CORE_CANARY_MS);
    const double drift = stats_canary_drift_pct(baseline, after);

    tty_warn_drift(cfg, drift);
    tty_kv(cfg, "DVFS drift", "%+.1f%%", drift);
}

/* ---------------------------------------------------------------- core ilp */

#define CORE_ILP_INT_STEP(acc)                                                                     \
    do {                                                                                           \
        (acc) = ((acc) * CORE_ILP_INT_MUL) + CORE_ILP_INT_ADD;                                     \
        PAL_KEEP_INT(acc);                                                                         \
    } while (0)

#define CORE_ILP_FP_STEP(acc)                                                                      \
    do {                                                                                           \
        (acc) = ((acc) * CORE_ILP_FP_MUL) + CORE_ILP_FP_ADD;                                       \
        PAL_KEEP_FP(acc);                                                                          \
    } while (0)

/*
 * Each function runs a fixed number of independent multiply-add chains. The
 * no-vectorize attribute plus PAL_KEEP_* keeps the chains scalar and prevents
 * the optimizer from merging them through SLP, so K chains stay independent.
 */
static PAL_NOINLINE PAL_NO_VECTORIZE uint64_t core_ilp_int_k1(uint64_t iterations)
{
    uint64_t a0 = UINT64_C(1);
    uint64_t i = 0u;

    PAL_NO_VECTORIZE_LOOP
    for (i = 0u; i < iterations; i++) {
        CORE_ILP_INT_STEP(a0);
    }
    PAL_BARRIER();
    g_core_sink = a0;
    return a0;
}

static PAL_NOINLINE PAL_NO_VECTORIZE uint64_t core_ilp_int_k2(uint64_t iterations)
{
    uint64_t a0 = UINT64_C(1);
    uint64_t a1 = UINT64_C(3);
    uint64_t i = 0u;

    PAL_NO_VECTORIZE_LOOP
    for (i = 0u; i < iterations; i++) {
        CORE_ILP_INT_STEP(a0);
        CORE_ILP_INT_STEP(a1);
    }
    PAL_BARRIER();
    g_core_sink = a0 + a1;
    return g_core_sink;
}

static PAL_NOINLINE PAL_NO_VECTORIZE uint64_t core_ilp_int_k4(uint64_t iterations)
{
    uint64_t a0 = UINT64_C(1);
    uint64_t a1 = UINT64_C(3);
    uint64_t a2 = UINT64_C(5);
    uint64_t a3 = UINT64_C(7);
    uint64_t i = 0u;

    PAL_NO_VECTORIZE_LOOP
    for (i = 0u; i < iterations; i++) {
        CORE_ILP_INT_STEP(a0);
        CORE_ILP_INT_STEP(a1);
        CORE_ILP_INT_STEP(a2);
        CORE_ILP_INT_STEP(a3);
    }
    PAL_BARRIER();
    g_core_sink = a0 + a1 + a2 + a3;
    return g_core_sink;
}

static PAL_NOINLINE PAL_NO_VECTORIZE uint64_t core_ilp_int_k6(uint64_t iterations)
{
    uint64_t a0 = UINT64_C(1);
    uint64_t a1 = UINT64_C(3);
    uint64_t a2 = UINT64_C(5);
    uint64_t a3 = UINT64_C(7);
    uint64_t a4 = UINT64_C(9);
    uint64_t a5 = UINT64_C(11);
    uint64_t i = 0u;

    PAL_NO_VECTORIZE_LOOP
    for (i = 0u; i < iterations; i++) {
        CORE_ILP_INT_STEP(a0);
        CORE_ILP_INT_STEP(a1);
        CORE_ILP_INT_STEP(a2);
        CORE_ILP_INT_STEP(a3);
        CORE_ILP_INT_STEP(a4);
        CORE_ILP_INT_STEP(a5);
    }
    PAL_BARRIER();
    g_core_sink = a0 + a1 + a2 + a3 + a4 + a5;
    return g_core_sink;
}

static PAL_NOINLINE PAL_NO_VECTORIZE uint64_t core_ilp_int_k8(uint64_t iterations)
{
    uint64_t a0 = UINT64_C(1);
    uint64_t a1 = UINT64_C(3);
    uint64_t a2 = UINT64_C(5);
    uint64_t a3 = UINT64_C(7);
    uint64_t a4 = UINT64_C(9);
    uint64_t a5 = UINT64_C(11);
    uint64_t a6 = UINT64_C(13);
    uint64_t a7 = UINT64_C(15);
    uint64_t i = 0u;

    PAL_NO_VECTORIZE_LOOP
    for (i = 0u; i < iterations; i++) {
        CORE_ILP_INT_STEP(a0);
        CORE_ILP_INT_STEP(a1);
        CORE_ILP_INT_STEP(a2);
        CORE_ILP_INT_STEP(a3);
        CORE_ILP_INT_STEP(a4);
        CORE_ILP_INT_STEP(a5);
        CORE_ILP_INT_STEP(a6);
        CORE_ILP_INT_STEP(a7);
    }
    PAL_BARRIER();
    g_core_sink = a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
    return g_core_sink;
}

static PAL_NOINLINE PAL_NO_VECTORIZE uint64_t core_ilp_fp_k1(uint64_t iterations)
{
    double a0 = 1.0;
    uint64_t i = 0u;

    PAL_NO_VECTORIZE_LOOP
    for (i = 0u; i < iterations; i++) {
        CORE_ILP_FP_STEP(a0);
    }
    PAL_BARRIER();
    g_core_sink = (uint64_t)a0;
    return g_core_sink;
}

static PAL_NOINLINE PAL_NO_VECTORIZE uint64_t core_ilp_fp_k2(uint64_t iterations)
{
    double a0 = 1.0;
    double a1 = 3.0;
    uint64_t i = 0u;

    PAL_NO_VECTORIZE_LOOP
    for (i = 0u; i < iterations; i++) {
        CORE_ILP_FP_STEP(a0);
        CORE_ILP_FP_STEP(a1);
    }
    PAL_BARRIER();
    g_core_sink = (uint64_t)(a0 + a1);
    return g_core_sink;
}

static PAL_NOINLINE PAL_NO_VECTORIZE uint64_t core_ilp_fp_k4(uint64_t iterations)
{
    double a0 = 1.0;
    double a1 = 3.0;
    double a2 = 5.0;
    double a3 = 7.0;
    uint64_t i = 0u;

    PAL_NO_VECTORIZE_LOOP
    for (i = 0u; i < iterations; i++) {
        CORE_ILP_FP_STEP(a0);
        CORE_ILP_FP_STEP(a1);
        CORE_ILP_FP_STEP(a2);
        CORE_ILP_FP_STEP(a3);
    }
    PAL_BARRIER();
    g_core_sink = (uint64_t)(a0 + a1 + a2 + a3);
    return g_core_sink;
}

static PAL_NOINLINE PAL_NO_VECTORIZE uint64_t core_ilp_fp_k6(uint64_t iterations)
{
    double a0 = 1.0;
    double a1 = 3.0;
    double a2 = 5.0;
    double a3 = 7.0;
    double a4 = 9.0;
    double a5 = 11.0;
    uint64_t i = 0u;

    PAL_NO_VECTORIZE_LOOP
    for (i = 0u; i < iterations; i++) {
        CORE_ILP_FP_STEP(a0);
        CORE_ILP_FP_STEP(a1);
        CORE_ILP_FP_STEP(a2);
        CORE_ILP_FP_STEP(a3);
        CORE_ILP_FP_STEP(a4);
        CORE_ILP_FP_STEP(a5);
    }
    PAL_BARRIER();
    g_core_sink = (uint64_t)(a0 + a1 + a2 + a3 + a4 + a5);
    return g_core_sink;
}

static PAL_NOINLINE PAL_NO_VECTORIZE uint64_t core_ilp_fp_k8(uint64_t iterations)
{
    double a0 = 1.0;
    double a1 = 3.0;
    double a2 = 5.0;
    double a3 = 7.0;
    double a4 = 9.0;
    double a5 = 11.0;
    double a6 = 13.0;
    double a7 = 15.0;
    uint64_t i = 0u;

    PAL_NO_VECTORIZE_LOOP
    for (i = 0u; i < iterations; i++) {
        CORE_ILP_FP_STEP(a0);
        CORE_ILP_FP_STEP(a1);
        CORE_ILP_FP_STEP(a2);
        CORE_ILP_FP_STEP(a3);
        CORE_ILP_FP_STEP(a4);
        CORE_ILP_FP_STEP(a5);
        CORE_ILP_FP_STEP(a6);
        CORE_ILP_FP_STEP(a7);
    }
    PAL_BARRIER();
    g_core_sink = (uint64_t)(a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7);
    return g_core_sink;
}

static const core_ilp_fn_t CORE_ILP_INT_FNS[] = {
    core_ilp_int_k1, core_ilp_int_k2, core_ilp_int_k4, core_ilp_int_k6, core_ilp_int_k8,
};

static const core_ilp_fn_t CORE_ILP_FP_FNS[] = {
    core_ilp_fp_k1, core_ilp_fp_k2, core_ilp_fp_k4, core_ilp_fp_k6, core_ilp_fp_k8,
};

static double core_ilp_sample(core_ilp_fn_t fn, uint64_t iterations, size_t chains)
{
    const uint64_t begin = pal_now_ns();
    const uint64_t sink = fn(iterations);
    uint64_t end = pal_now_ns();
    const double ops = (double)iterations * (double)chains;

    g_core_sink = sink;
    if (end <= begin) {
        end = begin + 1u;
    }
    if (!(ops > 0.0)) {
        return 0.0;
    }
    return (double)(end - begin) / ops;
}

static void core_ilp_table(const lb_cfg_t *cfg, const char *type, const core_ilp_fn_t fns[],
                           uint32_t *best_chains, double *best_speedup)
{
    const uint64_t iterations = cfg->fast ? CORE_ILP_FAST_ITERATIONS : CORE_ILP_ITERATIONS;
    const size_t repeats = cfg->fast ? CORE_FAST_REPEATS : CORE_ILP_REPEATS;
    double base_ns = 0.0;
    size_t index = 0u;

    for (index = 0u; index < LB_ARRAY_SIZE(CORE_ILP_CHAINS); index++) {
        const size_t chains = CORE_ILP_CHAINS[index];
        double samples[CORE_ILP_REPEATS] = {0.0};
        double ns_per_op = 0.0;
        double speedup = 1.0;
        size_t repeat = 0u;

        (void)fns[index](CORE_ILP_WARMUP);
        for (repeat = 0u; repeat < repeats; repeat++) {
            samples[repeat] = core_ilp_sample(fns[index], iterations, chains);
        }
        core_cool(cfg);

        ns_per_op = stats_percentile(samples, repeats, STATS_NOISE_PERCENTILE);
        if (index == 0u) {
            base_ns = ns_per_op;
        }
        if (ns_per_op > 0.0) {
            speedup = base_ns / ns_per_op;
        }
        if (best_chains != NULL && best_speedup != NULL && speedup > *best_speedup) {
            *best_speedup = speedup;
            *best_chains = (uint32_t)chains;
        }

        tty_header(cfg, "  %-4s %7zu %10.4f %8.2fx", type, chains, ns_per_op, speedup);
        tty_csv_row(cfg, "core,ilp,%s,%zu,%.4f,%.2f", type, chains, ns_per_op, speedup);
    }
}

lb_status_t core_ilp_run(const lb_cfg_t *cfg)
{
    lb_passport_t *passport = lb_passport_mutable();
    uint64_t baseline = 0u;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    core_apply_runtime(cfg);

    tty_section(cfg, "Instruction-level parallelism");
    tty_kv(cfg, "Iterations / chains", "%" PRIu64 " / 1,2,4,6,8",
           cfg->fast ? CORE_ILP_FAST_ITERATIONS : CORE_ILP_ITERATIONS);
    tty_header(cfg, "  %-4s %7s %10s %9s", "Type", "Chains", "ns/op", "Speedup");
    tty_csv_row(cfg, "core,ilp,type,chains,ns_per_op,speedup");

    baseline = stats_canary_baseline(CORE_CANARY_MS);

    core_ilp_table(cfg, "ALU", CORE_ILP_INT_FNS, &passport->alu_ilp_chains,
                   &passport->alu_ilp_speedup);
    core_ilp_table(cfg, "FPU", CORE_ILP_FP_FNS, &passport->fpu_ilp_chains,
                   &passport->fpu_ilp_speedup);

    core_canary_finish(cfg, baseline);
    return LB_STATUS_OK;
}

/* ---------------------------------------------------------------- core mlp */

static lb_status_t core_mlp_build(core_node_t *nodes, size_t node_count, size_t chains,
                                  size_t *indices, const core_node_t *starts[])
{
    size_t group = 0u;
    size_t chain = 0u;

    if (nodes == NULL || indices == NULL || starts == NULL || chains == 0u) {
        return LB_STATUS_INVALID_ARG;
    }
    group = node_count / chains;
    if (group < 2u) {
        return LB_STATUS_NOMEM;
    }

    for (chain = 0u; chain < chains; chain++) {
        const uint64_t seed = UINT64_C(0x6a09e667f3bcc909) ^ ((uint64_t)chain << 32u) ^
                              ((uint64_t)group * UINT64_C(0x2545f4914f6cdd1d));
        size_t i = 0u;
        lb_status_t status = util_sattolo_indices(indices, group, seed);

        if (status != LB_STATUS_OK) {
            return status;
        }

        for (i = 0u; i < group; i++) {
            core_node_t *const node = &nodes[chain + (i * chains)];
            core_node_t *const next = &nodes[chain + (indices[i] * chains)];

            node->next = next;
        }
        starts[chain] = &nodes[chain];
    }
    return LB_STATUS_OK;
}

static PAL_NOINLINE PAL_NO_VECTORIZE uint64_t core_mlp_chase(const core_node_t *const *starts,
                                                             size_t chains, uint64_t iterations)
{
    const core_node_t *p[CORE_MLP_MAX_CHAINS] = {NULL};
    uint64_t acc = 0u;
    uint64_t i = 0u;
    size_t chain = 0u;

    for (chain = 0u; chain < chains; chain++) {
        p[chain] = starts[chain];
    }

    for (i = 0u; i < iterations; i++) {
        PAL_NO_VECTORIZE_LOOP
        for (chain = 0u; chain < chains; chain++) {
            p[chain] = p[chain]->next;
        }
    }

    for (chain = 0u; chain < chains; chain++) {
        acc += (uint64_t)(uintptr_t)p[chain];
    }
    PAL_BARRIER();
    g_core_sink = acc;
    return acc;
}

static double core_mlp_sample(const core_node_t *const starts[], size_t chains, uint64_t iterations)
{
    const uint64_t begin = pal_now_ns();
    const uint64_t sink = core_mlp_chase(starts, chains, iterations);
    uint64_t end = pal_now_ns();
    const double accesses = (double)iterations * (double)chains;

    g_core_sink = sink;
    if (end <= begin) {
        end = begin + 1u;
    }
    if (!(accesses > 0.0)) {
        return 0.0;
    }
    return (double)(end - begin) / accesses;
}

lb_status_t core_mlp_run(const lb_cfg_t *cfg)
{
    pal_mem_t *arena = NULL;
    core_node_t *nodes = NULL;
    size_t *indices = NULL;
    const core_node_t *starts[CORE_MLP_MAX_CHAINS] = {NULL};
    double throughput[CORE_MLP_MAX_CHAINS] = {0.0};
    uint64_t iterations = 0u;
    size_t repeats = 0u;
    size_t chain_count = 0u;
    size_t arena_bytes = 0u;
    size_t node_count = 0u;
    size_t chain_index = 0u;
    size_t saturated = 0u;
    bool saturated_found = false;
    double peak = 0.0;
    uint64_t baseline = 0u;
    lb_status_t status = LB_STATUS_NOMEM;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    iterations = cfg->fast ? CORE_MLP_FAST_ITERATIONS : CORE_MLP_ITERATIONS;
    repeats = cfg->fast ? CORE_FAST_REPEATS : CORE_MLP_REPEATS;

    core_apply_runtime(cfg);

    arena_bytes = core_arena_cap(cfg, CORE_MLP_MAX_BYTES, true);
    if (arena_bytes < CORE_MIN_ARENA_BYTES) {
        tty_err("memory budget is too small for core mlp");
        return LB_STATUS_NOMEM;
    }
    arena_bytes = (arena_bytes / CORE_MLP_NODE_BYTES) * CORE_MLP_NODE_BYTES;
    node_count = arena_bytes / CORE_MLP_NODE_BYTES;
    if (node_count < ((size_t)2u * (size_t)CORE_MLP_MAX_CHAINS)) {
        tty_err("memory budget is too small for core mlp");
        return LB_STATUS_NOMEM;
    }

    arena = pal_mem_alloc(arena_bytes, core_arena_flags(cfg));
    if (arena == NULL) {
        tty_err("cannot allocate a %zu-byte core mlp arena", arena_bytes);
        goto cleanup;
    }
    nodes = pal_mem_data(arena);
    if (nodes == NULL) {
        goto cleanup;
    }

    indices = malloc(node_count * sizeof(*indices));
    if (indices == NULL) {
        goto cleanup;
    }

    {
        char human[LB_NAME_MAX] = {0};

        (void)tty_format_bytes((uint64_t)arena_bytes, human, sizeof(human));
        tty_section(cfg, "Memory-level parallelism");
        tty_kv(cfg, "Arena", "%s", human);
        tty_kv(cfg, "Nodes", "%zu", node_count);
        tty_kv(cfg, "Chains", "1,2,4,6,8,12,16,24");
        tty_kv(cfg, "Iterations / repeats", "%" PRIu64 " / %zu", iterations, repeats);
    }
    tty_header(cfg, "  %7s %12s %14s", "Chains", "ns/access", "M accesses/s");
    tty_csv_row(cfg, "core,mlp,chains,ns_per_access,m_accesses_per_s");

    baseline = stats_canary_baseline(CORE_CANARY_MS);

    for (chain_index = 0u; chain_index < LB_ARRAY_SIZE(CORE_MLP_CHAINS); chain_index++) {
        const size_t chains = CORE_MLP_CHAINS[chain_index];
        double samples[CORE_MLP_REPEATS] = {0.0};
        double ns_per_access = 0.0;
        double mega_accesses = 0.0;
        size_t repeat = 0u;

        if (node_count / chains < 2u) {
            break;
        }

        status = core_mlp_build(nodes, node_count, chains, indices, starts);
        if (status != LB_STATUS_OK) {
            goto cleanup;
        }

        (void)core_mlp_chase(starts, chains, iterations / 8u);
        for (repeat = 0u; repeat < repeats; repeat++) {
            samples[repeat] = core_mlp_sample(starts, chains, iterations);
        }
        core_cool(cfg);

        ns_per_access = stats_percentile(samples, repeats, STATS_NOISE_PERCENTILE);
        if (ns_per_access > 0.0) {
            mega_accesses = 1000.0 / ns_per_access;
        }
        throughput[chain_count] = mega_accesses;
        if (mega_accesses > peak) {
            peak = mega_accesses;
        }
        chain_count++;

        tty_header(cfg, "  %7zu %12.2f %14.1f", chains, ns_per_access, mega_accesses);
        tty_csv_row(cfg, "core,mlp,%zu,%.3f,%.2f", chains, ns_per_access, mega_accesses);
    }

    if (chain_count > 0u && peak > 0.0) {
        for (chain_index = 1u; chain_index < chain_count; chain_index++) {
            if (throughput[chain_index] <
                (throughput[chain_index - 1u] * (1.0 + (CORE_MLP_GAIN_MIN_PCT / 100.0)))) {
                saturated = chain_index - 1u;
                saturated_found = true;
                break;
            }
        }

        if (saturated_found) {
            tty_header(cfg,
                       "  Saturation: ~%zu chains (next step gains under %.0f%%, peak "
                       "%.1f M accesses/s)",
                       CORE_MLP_CHAINS[saturated], CORE_MLP_GAIN_MIN_PCT, peak);
        } else {
            tty_header(cfg, "  Saturation: not reached within %zu chains (peak %.1f M accesses/s)",
                       CORE_MLP_CHAINS[chain_count - 1u], peak);
        }
    }

    core_canary_finish(cfg, baseline);
    status = LB_STATUS_OK;

cleanup:
    free(indices);
    pal_mem_free(arena);
    return status;
}

/* -------------------------------------------------------------- core align */

static uint8_t *core_align_page(uint8_t *ptr, size_t page_size)
{
    const size_t remainder = (size_t)((uintptr_t)ptr % (uintptr_t)page_size);

    if (remainder == 0u) {
        return ptr;
    }
    return ptr + (page_size - remainder);
}

static PAL_NOINLINE PAL_NO_VECTORIZE uint64_t core_align_probe(const uint8_t *buf, size_t offset,
                                                               uint64_t iterations, uint64_t mask)
{
    uint64_t acc = 0u;
    uint64_t val = 0u;
    size_t next = offset;
    uint64_t i = 0u;

    PAL_NO_VECTORIZE_LOOP
    for (i = 0u; i < iterations; i++) {
        memcpy(&val, buf + next, sizeof(val));
        acc += val;
        PAL_KEEP_INT(acc);
        next = offset ^ (size_t)(val & mask);
    }

    PAL_BARRIER();
    g_core_sink = acc;
    return acc;
}

static double core_align_sample(const uint8_t *buf, size_t offset, uint64_t mask,
                                uint64_t iterations)
{
    const uint64_t begin = pal_now_ns();
    const uint64_t sink = core_align_probe(buf, offset, iterations, mask);
    uint64_t end = pal_now_ns();

    g_core_sink = sink;
    if (end <= begin) {
        end = begin + 1u;
    }
    return (double)(end - begin) / (double)iterations;
}

lb_status_t core_align_run(const lb_cfg_t *cfg)
{
    pal_mem_t *arena = NULL;
    uint8_t *base = NULL;
    size_t page_size = 0u;
    size_t arena_bytes = 0u;
    size_t offsets[CORE_ALIGN_CASES] = {0u};
    uint64_t mask = 0u;
    uint64_t iterations = 0u;
    size_t repeats = 0u;
    uint64_t baseline = 0u;
    size_t case_index = 0u;
    lb_status_t status = LB_STATUS_NOMEM;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    iterations = cfg->fast ? CORE_ALIGN_FAST_ITERATIONS : CORE_ALIGN_ITERATIONS;
    repeats = cfg->fast ? CORE_FAST_REPEATS : CORE_ALIGN_REPEATS;

    page_size = pal_page_size();
    if (page_size == 0u) {
        tty_err("cannot determine the system page size");
        return LB_STATUS_UNSUPPORTED;
    }
    if (page_size > (SIZE_MAX / 3u)) {
        tty_err("page size %zu is too large for core align", page_size);
        return LB_STATUS_UNSUPPORTED;
    }

    core_apply_runtime(cfg);

    arena_bytes = 3u * page_size;
    arena = pal_mem_alloc(arena_bytes, core_arena_flags(cfg));
    if (arena == NULL) {
        tty_err("cannot allocate a %zu-byte core align buffer", arena_bytes);
        goto cleanup;
    }
    base = core_align_page((uint8_t *)pal_mem_data(arena), page_size);
    if (base == NULL) {
        goto cleanup;
    }
    memset(base, 0, 2u * page_size);

    for (case_index = 0u; case_index < (CORE_ALIGN_CASES - 1u); case_index++) {
        offsets[case_index] = CORE_ALIGN_OFFSETS[case_index];
    }
    offsets[CORE_ALIGN_CASES - 1u] = page_size - 4u;

    mask = g_core_zero_mask;

    {
        char human[LB_NAME_MAX] = {0};

        (void)tty_format_bytes((uint64_t)(2u * page_size), human, sizeof(human));
        tty_section(cfg, "Unaligned reads");
        tty_kv(cfg, "Page size", "%zu bytes", page_size);
        tty_kv(cfg, "Buffer", "%s", human);
        tty_kv(cfg, "Iterations / repeats", "%" PRIu64 " / %zu", iterations, repeats);
    }
    tty_header(cfg, "  %-12s %8s %12s", "Offset", "Bytes", "ns/read");
    tty_csv_row(cfg, "core,align,offset,ns_per_read");

    baseline = stats_canary_baseline(CORE_CANARY_MS);

    for (case_index = 0u; case_index < CORE_ALIGN_CASES; case_index++) {
        double samples[CORE_ALIGN_REPEATS] = {0.0};
        double ns_per_read = 0.0;
        size_t repeat = 0u;

        (void)core_align_probe(base, offsets[case_index], CORE_ALIGN_WARMUP, mask);
        for (repeat = 0u; repeat < repeats; repeat++) {
            samples[repeat] = core_align_sample(base, offsets[case_index], mask, iterations);
        }
        core_cool(cfg);

        ns_per_read = stats_percentile(samples, repeats, STATS_NOISE_PERCENTILE);
        tty_header(cfg, "  %-12s %8zu %12.4f", CORE_ALIGN_NAMES[case_index], offsets[case_index],
                   ns_per_read);
        tty_csv_row(cfg, "core,align,%zu,%.4f", offsets[case_index], ns_per_read);
    }

    core_canary_finish(cfg, baseline);
    status = LB_STATUS_OK;

cleanup:
    pal_mem_free(arena);
    return status;
}

/* --------------------------------------------------------------- core stlf */

/*
 * The store and the dependent load must reach the hardware as real memory
 * operations. Clang is aggressive about forwarding a store to a later load and
 * about hoisting a load whose address it can prove constant, which turns this
 * probe into a register-only loop (0.2 ns/op instead of a store-buffer stall).
 *
 * Both addresses are therefore derived before the loop from volatile offsets.
 * The compiler cannot fold them to constants, and because it cannot prove that
 * the write and the read do not alias, every iteration must issue a real store
 * and a real load. The only loop-carried dependency is the value loaded by one
 * iteration feeding the store of the next one; there is deliberately no
 * compiler barrier between the store and the load, since that would spill the
 * loop state to the stack and add stack store-forwarding traffic.
 */
static PAL_NOINLINE uint64_t core_stlf_full(uint8_t *buf, size_t page_size, uint64_t iterations,
                                            size_t offset)
{
    volatile size_t v_zero = 0u;
    volatile size_t v_off = offset;
    uint8_t *p_wr = buf + v_zero;
    const uint8_t *p_rd = buf + v_off;
    uint64_t val = UINT64_C(1);
    uint64_t loaded = 0u;
    uint64_t i = 0u;

    (void)page_size;
    PAL_NO_VECTORIZE_LOOP
    for (i = 0u; i < iterations; i++) {
        memcpy(p_wr, &val, sizeof(val));
        memcpy(&loaded, p_rd, sizeof(loaded));
        val += loaded;
    }

    PAL_BARRIER();
    g_core_sink = val;
    return val;
}

static PAL_NOINLINE uint64_t core_stlf_narrow4(uint8_t *buf, size_t page_size, uint64_t iterations,
                                               size_t offset)
{
    volatile size_t v_zero = 0u;
    volatile size_t v_off = offset;
    uint8_t *p_wr = buf + v_zero;
    const uint8_t *p_rd = buf + v_off;
    uint64_t val = UINT64_C(1);
    uint32_t loaded = 0u;
    uint64_t i = 0u;

    (void)page_size;
    PAL_NO_VECTORIZE_LOOP
    for (i = 0u; i < iterations; i++) {
        memcpy(p_wr, &val, sizeof(val));
        memcpy(&loaded, p_rd, sizeof(loaded));
        val += (uint64_t)loaded;
    }

    PAL_BARRIER();
    g_core_sink = val;
    return val;
}

static PAL_NOINLINE uint64_t core_stlf_partial4(uint8_t *buf, size_t page_size, uint64_t iterations,
                                                size_t offset)
{
    volatile size_t v_zero = 0u;
    volatile size_t v_off = offset + 1u;
    uint8_t *p_wr = buf + v_zero;
    const uint8_t *p_rd = buf + v_off;
    uint64_t val = UINT64_C(1);
    uint32_t loaded = 0u;
    uint64_t i = 0u;

    (void)page_size;
    PAL_NO_VECTORIZE_LOOP
    for (i = 0u; i < iterations; i++) {
        memcpy(p_wr, &val, sizeof(val));
        memcpy(&loaded, p_rd, sizeof(loaded));
        val += (uint64_t)loaded;
    }

    PAL_BARRIER();
    g_core_sink = val;
    return val;
}

/*
 * Straddling 8-byte load that starts four bytes into the previous 8-byte store.
 * Modern Intel can forward the 4B @ +1 case quickly, but a load that crosses
 * the store's upper half still stalls on Intel, AMD, and ARM64.
 */
static PAL_NOINLINE uint64_t core_stlf_straddle8(uint8_t *buf, size_t page_size,
                                                 uint64_t iterations, size_t offset)
{
    volatile size_t v_zero = 0u;
    volatile size_t v_off = offset + CORE_STLF_STRADDLE_OFFSET;
    uint8_t *p_wr = buf + v_zero;
    const uint8_t *p_rd = buf + v_off;
    uint64_t val = UINT64_C(1);
    uint64_t loaded = 0u;
    uint64_t i = 0u;

    (void)page_size;
    PAL_NO_VECTORIZE_LOOP
    for (i = 0u; i < iterations; i++) {
        memcpy(p_wr, &val, sizeof(val));
        memcpy(&loaded, p_rd, sizeof(loaded));
        val += loaded;
    }

    PAL_BARRIER();
    g_core_sink = val;
    return val;
}

static PAL_NOINLINE uint64_t core_stlf_partial1(uint8_t *buf, size_t page_size, uint64_t iterations,
                                                size_t offset)
{
    volatile size_t v_zero = 0u;
    volatile size_t v_off = offset + 1u;
    uint8_t *p_wr = buf + v_zero;
    const uint8_t *p_rd = buf + v_off;
    uint64_t val = UINT64_C(1);
    uint8_t loaded = 0u;
    uint64_t i = 0u;

    (void)page_size;
    PAL_NO_VECTORIZE_LOOP
    for (i = 0u; i < iterations; i++) {
        memcpy(p_wr, &val, sizeof(val));
        memcpy(&loaded, p_rd, sizeof(loaded));
        val += (uint64_t)loaded;
    }

    PAL_BARRIER();
    g_core_sink = val;
    return val;
}

static PAL_NOINLINE uint64_t core_stlf_alias(uint8_t *buf, size_t page_size, uint64_t iterations,
                                             size_t offset)
{
    volatile size_t v_zero = 0u;
    volatile size_t v_page = page_size + offset;
    uint8_t *p_wr = buf + v_zero;
    const uint8_t *p_rd = buf + v_page;
    uint64_t val = UINT64_C(1);
    uint64_t loaded = 0u;
    uint64_t i = 0u;

    PAL_NO_VECTORIZE_LOOP
    for (i = 0u; i < iterations; i++) {
        memcpy(p_wr, &val, sizeof(val));
        memcpy(&loaded, p_rd, sizeof(loaded));
        val += loaded;
    }

    PAL_BARRIER();
    g_core_sink = val;
    return val;
}

static const core_stlf_fn_t CORE_STLF_FNS[CORE_STLF_CASES] = {
    core_stlf_full,     core_stlf_narrow4,   core_stlf_partial4,
    core_stlf_partial1, core_stlf_straddle8, core_stlf_alias,
};

static double core_stlf_sample(core_stlf_fn_t fn, uint8_t *buf, size_t page_size, size_t offset,
                               uint64_t iterations)
{
    const uint64_t begin = pal_now_ns();
    const uint64_t sink = fn(buf, page_size, iterations, offset);
    uint64_t end = pal_now_ns();

    g_core_sink = sink;
    if (end <= begin) {
        end = begin + 1u;
    }
    return (double)(end - begin) / (double)iterations;
}

lb_status_t core_stlf_run(const lb_cfg_t *cfg)
{
    pal_mem_t *arena = NULL;
    uint8_t *base = NULL;
    size_t page_size = 0u;
    size_t arena_bytes = 0u;
    size_t offset = 0u;
    uint64_t iterations = 0u;
    size_t repeats = 0u;
    uint64_t baseline = 0u;
    uint64_t seed = UINT64_C(0x9e3779b97f4a7c15);
    size_t case_index = 0u;
    double worst_stall_ns = 0.0;
    const char *worst_name = NULL;
    lb_status_t status = LB_STATUS_NOMEM;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    iterations = cfg->fast ? CORE_STLF_FAST_ITERATIONS : CORE_STLF_ITERATIONS;
    repeats = cfg->fast ? CORE_FAST_REPEATS : CORE_STLF_REPEATS;

    page_size = pal_page_size();
    if (page_size == 0u) {
        tty_err("cannot determine the system page size");
        return LB_STATUS_UNSUPPORTED;
    }
    if (page_size > (SIZE_MAX / 3u)) {
        tty_err("page size %zu is too large for core stlf", page_size);
        return LB_STATUS_UNSUPPORTED;
    }

    core_apply_runtime(cfg);

    arena_bytes = 3u * page_size;
    if (arena_bytes < (CORE_STLF_MIN_BUFFER_BYTES + page_size)) {
        arena_bytes = CORE_STLF_MIN_BUFFER_BYTES + page_size;
    }
    arena = pal_mem_alloc(arena_bytes, core_arena_flags(cfg));
    if (arena == NULL) {
        tty_err("cannot allocate a %zu-byte core stlf buffer", arena_bytes);
        goto cleanup;
    }
    base = core_align_page((uint8_t *)pal_mem_data(arena), page_size);
    if (base == NULL) {
        goto cleanup;
    }
    memset(base, 0, 2u * page_size);
    /* The aliasing probe reads this line while the store targets offset zero. */
    memcpy(base + page_size, &seed, sizeof(seed));

    offset = (size_t)g_core_zero_mask;

    {
        char human[LB_NAME_MAX] = {0};

        (void)tty_format_bytes((uint64_t)(2u * page_size), human, sizeof(human));
        tty_section(cfg, "Store-to-load forwarding");
        tty_kv(cfg, "Page size", "%zu bytes", page_size);
        tty_kv(cfg, "Buffer", "%s", human);
        tty_kv(cfg, "Iterations / repeats", "%" PRIu64 " / %zu", iterations, repeats);
    }
    tty_header(cfg, "  %-12s %12s", "Variant", "ns/op");
    tty_csv_row(cfg, "core,stlf,variant,ns_per_op");

    baseline = stats_canary_baseline(CORE_CANARY_MS);

    for (case_index = 0u; case_index < CORE_STLF_CASES; case_index++) {
        double samples[CORE_STLF_REPEATS] = {0.0};
        double ns_per_op = 0.0;
        size_t repeat = 0u;

        (void)CORE_STLF_FNS[case_index](base, page_size, iterations / 20u, offset);
        for (repeat = 0u; repeat < repeats; repeat++) {
            samples[repeat] =
                core_stlf_sample(CORE_STLF_FNS[case_index], base, page_size, offset, iterations);
        }
        core_cool(cfg);

        ns_per_op = stats_percentile(samples, repeats, STATS_NOISE_PERCENTILE);
        if (case_index == 2u || case_index == 4u) {
            if (ns_per_op > worst_stall_ns) {
                worst_stall_ns = ns_per_op;
                worst_name = CORE_STLF_NAMES[case_index];
            }
        }
        tty_header(cfg, "  %-12s %12.4f", CORE_STLF_NAMES[case_index], ns_per_op);
        tty_csv_row(cfg, "core,stlf,%s,%.4f", CORE_STLF_NAMES[case_index], ns_per_op);
    }

    lb_passport_mutable()->stlf_stall_ns = worst_stall_ns;
    lb_passport_mutable()->stlf_variant = worst_name;

    core_canary_finish(cfg, baseline);
    status = LB_STATUS_OK;

cleanup:
    pal_mem_free(arena);
    return status;
}
