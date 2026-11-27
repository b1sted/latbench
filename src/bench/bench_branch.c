/* SPDX-License-Identifier: GPL-3.0 */

#include "bench.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bench_common.h"
#include "defs.h"
#include "pal/compiler.h"
#include "pal/pal.h"
#include "passport.h"
#include "stats.h"
#include "tty.h"
#include "types.h"
#include "util.h"

/* branch hist: bit-packed 0/1 patterns, always indexed with the same mask. */
#define BR_HIST_ARRAY_BYTES 4096u
#define BR_HIST_PATTERN_BITS ((size_t)BR_HIST_ARRAY_BYTES * 8u)
#define BR_HIST_RANDOM_BITS ((size_t)262144u)
#define BR_HIST_WORD_BITS 64u
#define BR_HIST_WORD_COUNT ((BR_HIST_RANDOM_BITS + BR_HIST_WORD_BITS - 1u) / BR_HIST_WORD_BITS)
#define BR_HIST_ITERATIONS UINT64_C(2000000)
#define BR_HIST_WARMUP UINT64_C(100000)
#define BR_HIST_REPEATS 3u
#define BR_HIST_DELTA_MIN_NS 0.10
#define BR_HIST_DELTA_MIN_PCT 5.0
#define BR_HIST_PERIOD_COUNT 12u

/* branch rsb: recursive call chains through a volatile function pointer. */
#define BR_RSB_ITERATIONS UINT64_C(200000)
#define BR_RSB_WARMUP UINT64_C(20000)
#define BR_RSB_REPEATS 3u
#define BR_RSB_JUMP_PCT 15.0
#define BR_RSB_SUSTAIN_PCT 12.0
#define BR_RSB_SMALL_DEPTH 16u
#define BR_RSB_DEPTH_COUNT 12u

/* branch btb: indirect calls through a table of unique trampolines. */
#define BR_BTB_MAX 4096u
#define BR_BTB_CALLS UINT64_C(2097152)
#define BR_BTB_REPEATS 3u
#define BR_BTB_JUMP_PCT 25.0
#define BR_BTB_SUSTAIN_PCT 20.0
#define BR_BTB_COUNT_COUNT 9u

/* branch btb callsites: distinct indirect PCs over a small target pool. */
#define BR_BTB_SITE_BLOCK_COUNT 64u
#define BR_BTB_SITE_BLOCK_SITES 64u
#define BR_BTB_SITE_COUNT_COUNT 7u
#define BR_BTB_SITE_POOL 16u

/* Compact auto mode: fewer iterations and repeats. */
#define BR_FAST_REPEATS 2u
#define BR_HIST_FAST_ITERATIONS UINT64_C(1000000)
#define BR_RSB_FAST_ITERATIONS UINT64_C(50000)
#define BR_BTB_FAST_CALLS UINT64_C(1048576)

typedef uint64_t (*br_rsb_fn_t)(uint64_t depth, uint64_t acc);
typedef uint64_t (*br_btb_fn_t)(uint64_t acc);

static const size_t BR_HIST_PERIODS[BR_HIST_PERIOD_COUNT] = {
    2u, 4u, 8u, 16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u,
};

static const uint64_t BR_RSB_DEPTHS[BR_RSB_DEPTH_COUNT] = {
    4u, 8u, 12u, 16u, 20u, 24u, 32u, 48u, 56u, 64u, 80u, 96u,
};

static const size_t BR_BTB_COUNTS[BR_BTB_COUNT_COUNT] = {
    16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u,
};

static const size_t BR_BTB_SITE_COUNTS[BR_BTB_SITE_COUNT_COUNT] = {
    64u, 128u, 256u, 512u, 1024u, 2048u, 4096u,
};

/*
 * Volatile result sink. Every timed loop publishes its checksum here so the
 * optimizer cannot discard the dependent work.
 */
static volatile uint64_t g_br_sink = 0u;

/*
 * The branch-history pattern is rewritten between measurements. Both the
 * periodic and the aperiodic reference use the same bit-packed indexing, so
 * their only difference is branch predictability, not an L1d/L2 footprint
 * change. 4096 words hold 262144 independent bits in 32 KiB.
 */
static uint64_t g_br_hist_bits[BR_HIST_WORD_COUNT] = {0u};

/* Indirect entry point for the recursive chain. */
static br_rsb_fn_t volatile g_br_rsb_entry = NULL;

/* ------------------------------------------------------------- branch hist */

static void br_hist_clear(void)
{
    (void)memset(g_br_hist_bits, 0, sizeof(g_br_hist_bits));
}

static void br_hist_set_bit(size_t index, bool value)
{
    const size_t word = index / BR_HIST_WORD_BITS;
    const uint64_t mask = UINT64_C(1) << (index % BR_HIST_WORD_BITS);

    if (word >= BR_HIST_WORD_COUNT) {
        return;
    }
    if (value) {
        g_br_hist_bits[word] |= mask;
    } else {
        g_br_hist_bits[word] &= ~mask;
    }
}

static void br_hist_fill(size_t period, uint64_t seed)
{
    uint8_t base[BR_HIST_ARRAY_BYTES] = {0};
    uint64_t state = seed;
    size_t mask = 0u;
    size_t i = 0u;

    if (period < 2u || period > BR_HIST_ARRAY_BYTES) {
        return;
    }
    mask = period - 1u;

    for (i = 0u; i < period; i++) {
        base[i] = (uint8_t)(util_rng_next(&state) & UINT64_C(1));
    }
    /* Guarantee both outcomes even when the random draw is degenerate. */
    base[0] = 0u;
    base[1] = 1u;

    br_hist_clear();
    for (i = 0u; i < BR_HIST_PATTERN_BITS; i++) {
        br_hist_set_bit(i, base[i & mask] != 0u);
    }
}

static void br_hist_random_fill(uint64_t seed)
{
    uint64_t state = seed;
    size_t i = 0u;

    br_hist_clear();
    for (i = 0u; i < BR_HIST_RANDOM_BITS; i++) {
        br_hist_set_bit(i, (util_rng_next(&state) & UINT64_C(1)) != 0u);
    }
}

static PAL_NOINLINE uint64_t br_hist_walk(const uint64_t *pattern, uint64_t iterations,
                                          uint64_t seed, size_t mask)
{
    uint64_t a = seed;
    uint64_t b = seed ^ UINT64_C(0x9e3779b97f4a7c15);
    uint64_t i = 0u;

    for (i = 0u; i < iterations; i++) {
        const size_t bit_index = (size_t)i & mask;
        const uint64_t word = pattern[bit_index >> 6u];
        const uint8_t bit = (uint8_t)((word >> (bit_index & 63u)) & UINT64_C(1));

        if (bit != 0u) {
            PAL_BLOCK_IFCONV_T();
            a = (a + b) ^ (i + UINT64_C(0x2545f4914f6cdd1d));
            b = (b ^ a) + UINT64_C(0x6a09e667f3bcc909);
        } else {
            PAL_BLOCK_IFCONV_F();
            b = (b + a) ^ (i + UINT64_C(0x2545f4914f6cdd1d));
            a = (a ^ b) + UINT64_C(0x6a09e667f3bcc909);
        }
    }

    PAL_BARRIER();
    g_br_sink = a ^ b;
    return g_br_sink;
}

static double br_hist_sample(const uint64_t *pattern, uint64_t iterations, uint64_t seed,
                             size_t mask)
{
    const uint64_t begin = pal_now_ns();
    const uint64_t sink = br_hist_walk(pattern, iterations, seed, mask);
    uint64_t end = pal_now_ns();

    g_br_sink = sink;
    if (end <= begin) {
        end = begin + 1u;
    }
    return (double)(end - begin) / (double)iterations;
}

static double br_hist_measure(const lb_cfg_t *cfg, const uint64_t *pattern, uint64_t seed,
                              size_t mask)
{
    double samples[BR_HIST_REPEATS] = {0.0};
    const uint64_t iterations = cfg->fast ? BR_HIST_FAST_ITERATIONS : BR_HIST_ITERATIONS;
    const size_t repeats = cfg->fast ? BR_FAST_REPEATS : BR_HIST_REPEATS;
    size_t repeat = 0u;

    (void)br_hist_walk(pattern, BR_HIST_WARMUP, seed, mask);
    for (repeat = 0u; repeat < repeats; repeat++) {
        samples[repeat] = br_hist_sample(pattern, iterations, seed + (uint64_t)repeat, mask);
    }
    lb_bench_cool(cfg);

    return stats_percentile(samples, repeats, STATS_NOISE_PERCENTILE);
}

lb_status_t br_hist_run(const lb_cfg_t *cfg)
{
    double base_ns = 0.0;
    double peak_delta = 0.0;
    double random_delta = 0.0;
    size_t history_index = 0u;
    bool history_found = false;
    uint64_t baseline = 0u;
    size_t index = 0u;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    lb_bench_apply_runtime(cfg);

    tty_csv_row(cfg, "branch,hist,pattern,ns_per_branch,delta_ns,delta_pct");
    tty_section(cfg, "Branch history");
    tty_kv(cfg, "Pattern", "periodic 0/1, period 2 .. 4096 branches");
    tty_kv(cfg, "Iterations / repeats", "%" PRIu64 " / %u",
           cfg->fast ? BR_HIST_FAST_ITERATIONS : BR_HIST_ITERATIONS,
           cfg->fast ? BR_FAST_REPEATS : BR_HIST_REPEATS);
    tty_blank(cfg);
    tty_header(cfg, "  %-10s %12s %12s %10s", "Pattern", "ns/branch", "delta ns", "delta");

    baseline = stats_canary_baseline(LB_BENCH_CANARY_MS);

    br_hist_clear();
    base_ns = br_hist_measure(cfg, g_br_hist_bits, UINT64_C(0x13198a2e03707344),
                              (size_t)(BR_HIST_PATTERN_BITS - 1u));
    tty_header(cfg, "  %-10s %12.4f %12.4f %9.1f%%", "const 0", base_ns, 0.0, 0.0);
    tty_csv_row(cfg, "branch,hist,const,%.4f,0.0000,0.00", base_ns);

    for (index = 0u; index < BR_HIST_PERIOD_COUNT; index++) {
        const size_t period = BR_HIST_PERIODS[index];
        const uint64_t seed =
            UINT64_C(0x13198a2e03707344) ^ ((uint64_t)period * UINT64_C(0x2545f4914f6cdd1d));
        double ns_per_branch = 0.0;
        double delta_ns = 0.0;
        double delta_pct = 0.0;

        br_hist_fill(period, seed);
        ns_per_branch =
            br_hist_measure(cfg, g_br_hist_bits, seed, (size_t)(BR_HIST_PATTERN_BITS - 1u));
        delta_ns = ns_per_branch - base_ns;
        delta_pct = (base_ns > 0.0) ? ((delta_ns / base_ns) * 100.0) : 0.0;

        if (delta_ns > peak_delta) {
            peak_delta = delta_ns;
        }
        if (!history_found && delta_ns >= BR_HIST_DELTA_MIN_NS &&
            delta_pct >= BR_HIST_DELTA_MIN_PCT) {
            history_found = true;
            history_index = index;
        }

        tty_header(cfg, "  %-10zu %12.4f %12.4f %9.1f%%", period, ns_per_branch, delta_ns,
                   delta_pct);
        tty_csv_row(cfg, "branch,hist,%zu,%.4f,%.4f,%.2f", period, ns_per_branch, delta_ns,
                    delta_pct);
    }

    {
        const uint64_t seed = UINT64_C(0xbb67ae8584caa73b);
        double ns_per_branch = 0.0;
        double delta_pct = 0.0;

        br_hist_random_fill(seed);
        ns_per_branch =
            br_hist_measure(cfg, g_br_hist_bits, seed, (size_t)(BR_HIST_RANDOM_BITS - 1u));
        delta_pct = (base_ns > 0.0) ? (((ns_per_branch - base_ns) / base_ns) * 100.0) : 0.0;

        random_delta = ns_per_branch - base_ns;
        tty_header(cfg, "  %-10s %12.4f %12.4f %9.1f%%", "random", ns_per_branch, random_delta,
                   delta_pct);
        tty_csv_row(cfg, "branch,hist,random,%.4f,%.4f,%.2f", ns_per_branch, random_delta,
                    delta_pct);
    }

    tty_blank(cfg);
    if (history_found) {
        tty_header(cfg, "  History estimate: ~%zu branches (first period over %.0f%% and %.2f ns)",
                   BR_HIST_PERIODS[history_index], BR_HIST_DELTA_MIN_PCT, BR_HIST_DELTA_MIN_NS);
        tty_csv_row(cfg, "branch,hist,history_estimate,%zu,branches",
                    BR_HIST_PERIODS[history_index]);
    } else {
        tty_header(cfg, "  History estimate: beyond %u branches", BR_HIST_ARRAY_BYTES);
        tty_csv_row(cfg, "branch,hist,history_estimate,%u,branches", BR_HIST_ARRAY_BYTES);
    }

    if (random_delta > peak_delta) {
        peak_delta = random_delta;
    }
    if (peak_delta > 0.0) {
        lb_passport_mutable()->mispredict_ns = peak_delta * 2.0;
        tty_header(cfg, "  Mispredict penalty: ~%.2f ns (2 x peak delta %.2f ns)", peak_delta * 2.0,
                   peak_delta);
        tty_csv_row(cfg, "branch,hist,mispredict_penalty,%.4f,ns", peak_delta * 2.0);
    } else {
        lb_passport_mutable()->mispredict_ns = 0.0;
        tty_header(cfg, "  Mispredict penalty: below measurement noise");
        tty_csv_row(cfg, "branch,hist,mispredict_penalty,0.0000,ns");
    }

    tty_blank(cfg);
    lb_bench_canary_finish(cfg, baseline, true);
    return LB_STATUS_OK;
}

/* -------------------------------------------------------------- branch rsb */

static PAL_NOINLINE uint64_t br_rsb_recurse(uint64_t depth, uint64_t acc)
{
    uint64_t value = acc + depth;

    if (depth > 0u) {
        value = br_rsb_recurse(depth - 1u, value);
    }
    /* Dependent use after the call defeats tail-call elimination. */
    return value ^ depth;
}

static PAL_NOINLINE uint64_t br_rsb_walk(br_rsb_fn_t volatile *entry, uint64_t depth,
                                         uint64_t iterations)
{
    uint64_t acc = UINT64_C(0x517cc1b727220a95);
    uint64_t i = 0u;

    for (i = 0u; i < iterations; i++) {
        const br_rsb_fn_t fn = *entry;

        acc = fn(depth, acc + i);
        acc ^= depth;
    }

    PAL_BARRIER();
    g_br_sink = acc;
    return acc;
}

static double br_rsb_sample(uint64_t depth, uint64_t iterations)
{
    const uint64_t calls = iterations * (depth + 1u);
    const uint64_t begin = pal_now_ns();
    const uint64_t sink = br_rsb_walk(&g_br_rsb_entry, depth, iterations);
    uint64_t end = pal_now_ns();

    g_br_sink = sink;
    if (end <= begin || calls == 0u) {
        return 0.0;
    }
    return (double)(end - begin) / (double)calls;
}

lb_status_t br_rsb_run(const lb_cfg_t *cfg)
{
    double ns_per_call[BR_RSB_DEPTH_COUNT] = {0.0};
    double base_ns = 0.0;
    size_t base_index = 0u;
    size_t threshold_index = 0u;
    bool threshold_found = false;
    uint64_t iterations = 0u;
    size_t repeats = 0u;
    uint64_t baseline = 0u;
    size_t index = 0u;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    iterations = cfg->fast ? BR_RSB_FAST_ITERATIONS : BR_RSB_ITERATIONS;
    repeats = cfg->fast ? BR_FAST_REPEATS : BR_RSB_REPEATS;

    lb_bench_apply_runtime(cfg);

    g_br_rsb_entry = br_rsb_recurse;
    PAL_BARRIER();

    tty_csv_row(cfg, "branch,rsb,depth,ns_per_call,delta_pct");
    tty_section(cfg, "Return-stack buffer");
    tty_kv(cfg, "Recursion", "noinline chain through a volatile function pointer");
    tty_kv(cfg, "Iterations / repeats", "%" PRIu64 " / %zu", iterations, repeats);
    tty_blank(cfg);
    tty_header(cfg, "  %7s %14s %12s %10s", "Depth", "Calls/top", "ns/call", "delta");

    baseline = stats_canary_baseline(LB_BENCH_CANARY_MS);

    for (index = 0u; index < BR_RSB_DEPTH_COUNT; index++) {
        const uint64_t depth = BR_RSB_DEPTHS[index];
        double samples[BR_RSB_REPEATS] = {0.0};
        size_t repeat = 0u;

        (void)br_rsb_walk(&g_br_rsb_entry, depth, iterations / 10u);
        for (repeat = 0u; repeat < repeats; repeat++) {
            samples[repeat] = br_rsb_sample(depth, iterations);
        }
        lb_bench_cool(cfg);
        ns_per_call[index] = stats_percentile(samples, repeats, STATS_NOISE_PERCENTILE);
    }

    /*
     * The shallow depths carry call-entry overhead that can dwarf the RSB miss
     * on some cores, so the baseline is the cheapest shallow depth rather than
     * a fixed depth == 4 reading.
     */
    base_ns = ns_per_call[0];
    for (index = 0u; index < BR_RSB_DEPTH_COUNT; index++) {
        if (BR_RSB_DEPTHS[index] <= BR_RSB_SMALL_DEPTH && ns_per_call[index] < base_ns) {
            base_ns = ns_per_call[index];
            base_index = index;
        }
    }

    for (index = 0u; index < BR_RSB_DEPTH_COUNT; index++) {
        const uint64_t depth = BR_RSB_DEPTHS[index];
        double delta_pct = 0.0;

        if (base_ns > 0.0) {
            delta_pct = ((ns_per_call[index] - base_ns) / base_ns) * 100.0;
        }
        /*
         * A real RSB overflow is persistent: the next depth must stay above the
         * sustain threshold. A single spike (depth 8 on Cortex-A715) no longer
         * produces a false ~4-frame capacity. Only depths above the chosen
         * baseline can overflow the RSB; comparing a shallower depth against a
         * baseline measured at a deeper one would report a false threshold.
         */
        if (!threshold_found && index > base_index && (index + 1u) < BR_RSB_DEPTH_COUNT &&
            ns_per_call[index] >= (base_ns * (1.0 + (BR_RSB_JUMP_PCT / 100.0))) &&
            ns_per_call[index + 1u] >= (base_ns * (1.0 + (BR_RSB_SUSTAIN_PCT / 100.0)))) {
            threshold_found = true;
            threshold_index = index;
        }

        tty_header(cfg, "  %7" PRIu64 " %14" PRIu64 " %12.4f %9.1f%%", depth, depth + 1u,
                   ns_per_call[index], delta_pct);
        tty_csv_row(cfg, "branch,rsb,%" PRIu64 ",%.4f,%.2f", depth, ns_per_call[index], delta_pct);
    }

    if (threshold_found) {
        lb_passport_mutable()->rsb_frames = BR_RSB_DEPTHS[threshold_index - 1u];
        tty_blank(cfg);
        tty_header(cfg, "  RSB threshold: ~%" PRIu64 " frames (next depth +%.0f%%)",
                   BR_RSB_DEPTHS[threshold_index - 1u], BR_RSB_JUMP_PCT);
        tty_csv_row(cfg, "branch,rsb,threshold,%" PRIu64 ",frames",
                    BR_RSB_DEPTHS[threshold_index - 1u]);
    } else {
        lb_passport_mutable()->rsb_frames = BR_RSB_DEPTHS[BR_RSB_DEPTH_COUNT - 1u];
        tty_blank(cfg);
        tty_header(cfg, "  RSB threshold: beyond %" PRIu64 " frames",
                   BR_RSB_DEPTHS[BR_RSB_DEPTH_COUNT - 1u]);
        tty_csv_row(cfg, "branch,rsb,threshold,%" PRIu64 ",frames",
                    BR_RSB_DEPTHS[BR_RSB_DEPTH_COUNT - 1u]);
    }

    tty_blank(cfg);
    lb_bench_canary_finish(cfg, baseline, true);
    return LB_STATUS_OK;
}

/* -------------------------------------------------------------- branch btb */


/*
 * Each trampoline mixes the unique (row, col) identity into the accumulator.
 * Five dependent operations keep the body around 48-64 bytes so 4096 of them
 * overflow a 128-192 KiB L1i, while the distinct constants defeat -fipa-icf.
 */
#define BR_BTB_TRAMPOLINE(row, col)                                                                \
    static PAL_NOINLINE uint64_t br_btb_fn_##row##_##col(uint64_t acc)                             \
    {                                                                                              \
        const uint64_t id = ((uint64_t)(row) * UINT64_C(16)) + (uint64_t)(col);                    \
        acc ^= id * UINT64_C(0x9e3779b97f4a7c15);                                                  \
        acc = (acc << 7u) | (acc >> 57u);                                                          \
        acc += id * UINT64_C(0x2545f4914f6cdd1d);                                                  \
        acc *= UINT64_C(0xbf58476d1ce4e5b9);                                                       \
        return acc ^ id;                                                                           \
    }

/* One sequence of 16 adjacent targets in a single 256-entry row group. */
#define BR_BTB_SEQ_16(m, row)                                                                      \
    m(row, 0) m(row, 1) m(row, 2) m(row, 3) m(row, 4) m(row, 5) m(row, 6) m(row, 7) m(row, 8)      \
        m(row, 9) m(row, 10) m(row, 11) m(row, 12) m(row, 13) m(row, 14) m(row, 15)

/* 16 row groups make one 256-target block without preprocessor arithmetic. */
#define BR_BTB_SEQ_256(m, r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r13, r14, r15)    \
    BR_BTB_SEQ_16(m, r0)                                                                           \
    BR_BTB_SEQ_16(m, r1)                                                                           \
    BR_BTB_SEQ_16(m, r2)                                                                           \
    BR_BTB_SEQ_16(m, r3)                                                                           \
    BR_BTB_SEQ_16(m, r4)                                                                           \
    BR_BTB_SEQ_16(m, r5)                                                                           \
    BR_BTB_SEQ_16(m, r6)                                                                           \
    BR_BTB_SEQ_16(m, r7)                                                                           \
    BR_BTB_SEQ_16(m, r8)                                                                           \
    BR_BTB_SEQ_16(m, r9)                                                                           \
    BR_BTB_SEQ_16(m, r10)                                                                          \
    BR_BTB_SEQ_16(m, r11)                                                                          \
    BR_BTB_SEQ_16(m, r12)                                                                          \
    BR_BTB_SEQ_16(m, r13)                                                                          \
    BR_BTB_SEQ_16(m, r14)                                                                          \
    BR_BTB_SEQ_16(m, r15)

/* All 4096 targets: 16 blocks of 256 functions each. */
#define BR_BTB_ALL(m)                                                                              \
    BR_BTB_SEQ_256(m, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15)                        \
    BR_BTB_SEQ_256(m, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31)              \
    BR_BTB_SEQ_256(m, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47)              \
    BR_BTB_SEQ_256(m, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63)              \
    BR_BTB_SEQ_256(m, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79)              \
    BR_BTB_SEQ_256(m, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95)              \
    BR_BTB_SEQ_256(m, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111)  \
    BR_BTB_SEQ_256(m, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126,   \
                   127)                                                                            \
    BR_BTB_SEQ_256(m, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142,   \
                   143)                                                                            \
    BR_BTB_SEQ_256(m, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158,   \
                   159)                                                                            \
    BR_BTB_SEQ_256(m, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174,   \
                   175)                                                                            \
    BR_BTB_SEQ_256(m, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190,   \
                   191)                                                                            \
    BR_BTB_SEQ_256(m, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206,   \
                   207)                                                                            \
    BR_BTB_SEQ_256(m, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222,   \
                   223)                                                                            \
    BR_BTB_SEQ_256(m, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238,   \
                   239)                                                                            \
    BR_BTB_SEQ_256(m, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254,   \
                   255)

BR_BTB_ALL(BR_BTB_TRAMPOLINE)

#define BR_BTB_ENTRY(row, col) br_btb_fn_##row##_##col,

static br_btb_fn_t volatile g_br_btb_fns[BR_BTB_MAX] = {BR_BTB_ALL(BR_BTB_ENTRY)};

/*
 * Multi-callsite probe: 64 noinline blocks with 64 unrolled indirect call
 * instructions each. Targets rotate through a 16-entry pool, so the target
 * footprint stays inside L1i while the probe exercises 4096 distinct
 * indirect-branch PCs.
 */
#define BR_BTB_SITE_CALL(id, k)                                                                    \
    acc = g_br_btb_fns[(((size_t)(id) + (size_t)(k)) & (BR_BTB_SITE_POOL - 1u))](acc);

#define BR_BTB_SITE_BLOCK(id)                                                                      \
    static PAL_NOINLINE uint64_t br_btb_site_blk_##id(uint64_t acc)                                \
    {                                                                                              \
        BR_BTB_SITE_CALL(id, 0)                                                                    \
        BR_BTB_SITE_CALL(id, 1)                                                                    \
        BR_BTB_SITE_CALL(id, 2)                                                                    \
        BR_BTB_SITE_CALL(id, 3)                                                                    \
        BR_BTB_SITE_CALL(id, 4)                                                                    \
        BR_BTB_SITE_CALL(id, 5)                                                                    \
        BR_BTB_SITE_CALL(id, 6)                                                                    \
        BR_BTB_SITE_CALL(id, 7)                                                                    \
        BR_BTB_SITE_CALL(id, 8)                                                                    \
        BR_BTB_SITE_CALL(id, 9)                                                                    \
        BR_BTB_SITE_CALL(id, 10)                                                                   \
        BR_BTB_SITE_CALL(id, 11)                                                                   \
        BR_BTB_SITE_CALL(id, 12)                                                                   \
        BR_BTB_SITE_CALL(id, 13)                                                                   \
        BR_BTB_SITE_CALL(id, 14)                                                                   \
        BR_BTB_SITE_CALL(id, 15)                                                                   \
        BR_BTB_SITE_CALL(id, 16)                                                                   \
        BR_BTB_SITE_CALL(id, 17)                                                                   \
        BR_BTB_SITE_CALL(id, 18)                                                                   \
        BR_BTB_SITE_CALL(id, 19)                                                                   \
        BR_BTB_SITE_CALL(id, 20)                                                                   \
        BR_BTB_SITE_CALL(id, 21)                                                                   \
        BR_BTB_SITE_CALL(id, 22)                                                                   \
        BR_BTB_SITE_CALL(id, 23)                                                                   \
        BR_BTB_SITE_CALL(id, 24)                                                                   \
        BR_BTB_SITE_CALL(id, 25)                                                                   \
        BR_BTB_SITE_CALL(id, 26)                                                                   \
        BR_BTB_SITE_CALL(id, 27)                                                                   \
        BR_BTB_SITE_CALL(id, 28)                                                                   \
        BR_BTB_SITE_CALL(id, 29)                                                                   \
        BR_BTB_SITE_CALL(id, 30)                                                                   \
        BR_BTB_SITE_CALL(id, 31)                                                                   \
        BR_BTB_SITE_CALL(id, 32)                                                                   \
        BR_BTB_SITE_CALL(id, 33)                                                                   \
        BR_BTB_SITE_CALL(id, 34)                                                                   \
        BR_BTB_SITE_CALL(id, 35)                                                                   \
        BR_BTB_SITE_CALL(id, 36)                                                                   \
        BR_BTB_SITE_CALL(id, 37)                                                                   \
        BR_BTB_SITE_CALL(id, 38)                                                                   \
        BR_BTB_SITE_CALL(id, 39)                                                                   \
        BR_BTB_SITE_CALL(id, 40)                                                                   \
        BR_BTB_SITE_CALL(id, 41)                                                                   \
        BR_BTB_SITE_CALL(id, 42)                                                                   \
        BR_BTB_SITE_CALL(id, 43)                                                                   \
        BR_BTB_SITE_CALL(id, 44)                                                                   \
        BR_BTB_SITE_CALL(id, 45)                                                                   \
        BR_BTB_SITE_CALL(id, 46)                                                                   \
        BR_BTB_SITE_CALL(id, 47)                                                                   \
        BR_BTB_SITE_CALL(id, 48)                                                                   \
        BR_BTB_SITE_CALL(id, 49)                                                                   \
        BR_BTB_SITE_CALL(id, 50)                                                                   \
        BR_BTB_SITE_CALL(id, 51)                                                                   \
        BR_BTB_SITE_CALL(id, 52)                                                                   \
        BR_BTB_SITE_CALL(id, 53)                                                                   \
        BR_BTB_SITE_CALL(id, 54)                                                                   \
        BR_BTB_SITE_CALL(id, 55)                                                                   \
        BR_BTB_SITE_CALL(id, 56)                                                                   \
        BR_BTB_SITE_CALL(id, 57)                                                                   \
        BR_BTB_SITE_CALL(id, 58)                                                                   \
        BR_BTB_SITE_CALL(id, 59)                                                                   \
        BR_BTB_SITE_CALL(id, 60)                                                                   \
        BR_BTB_SITE_CALL(id, 61)                                                                   \
        BR_BTB_SITE_CALL(id, 62)                                                                   \
        BR_BTB_SITE_CALL(id, 63)                                                                   \
        return acc ^ ((uint64_t)(id) * UINT64_C(0x9e3779b97f4a7c15));                              \
    }

BR_BTB_SITE_BLOCK(0)
BR_BTB_SITE_BLOCK(1)
BR_BTB_SITE_BLOCK(2)
BR_BTB_SITE_BLOCK(3)
BR_BTB_SITE_BLOCK(4)
BR_BTB_SITE_BLOCK(5)
BR_BTB_SITE_BLOCK(6)
BR_BTB_SITE_BLOCK(7)
BR_BTB_SITE_BLOCK(8)
BR_BTB_SITE_BLOCK(9)
BR_BTB_SITE_BLOCK(10)
BR_BTB_SITE_BLOCK(11)
BR_BTB_SITE_BLOCK(12)
BR_BTB_SITE_BLOCK(13)
BR_BTB_SITE_BLOCK(14)
BR_BTB_SITE_BLOCK(15)
BR_BTB_SITE_BLOCK(16)
BR_BTB_SITE_BLOCK(17)
BR_BTB_SITE_BLOCK(18)
BR_BTB_SITE_BLOCK(19)
BR_BTB_SITE_BLOCK(20)
BR_BTB_SITE_BLOCK(21)
BR_BTB_SITE_BLOCK(22)
BR_BTB_SITE_BLOCK(23)
BR_BTB_SITE_BLOCK(24)
BR_BTB_SITE_BLOCK(25)
BR_BTB_SITE_BLOCK(26)
BR_BTB_SITE_BLOCK(27)
BR_BTB_SITE_BLOCK(28)
BR_BTB_SITE_BLOCK(29)
BR_BTB_SITE_BLOCK(30)
BR_BTB_SITE_BLOCK(31)
BR_BTB_SITE_BLOCK(32)
BR_BTB_SITE_BLOCK(33)
BR_BTB_SITE_BLOCK(34)
BR_BTB_SITE_BLOCK(35)
BR_BTB_SITE_BLOCK(36)
BR_BTB_SITE_BLOCK(37)
BR_BTB_SITE_BLOCK(38)
BR_BTB_SITE_BLOCK(39)
BR_BTB_SITE_BLOCK(40)
BR_BTB_SITE_BLOCK(41)
BR_BTB_SITE_BLOCK(42)
BR_BTB_SITE_BLOCK(43)
BR_BTB_SITE_BLOCK(44)
BR_BTB_SITE_BLOCK(45)
BR_BTB_SITE_BLOCK(46)
BR_BTB_SITE_BLOCK(47)
BR_BTB_SITE_BLOCK(48)
BR_BTB_SITE_BLOCK(49)
BR_BTB_SITE_BLOCK(50)
BR_BTB_SITE_BLOCK(51)
BR_BTB_SITE_BLOCK(52)
BR_BTB_SITE_BLOCK(53)
BR_BTB_SITE_BLOCK(54)
BR_BTB_SITE_BLOCK(55)
BR_BTB_SITE_BLOCK(56)
BR_BTB_SITE_BLOCK(57)
BR_BTB_SITE_BLOCK(58)
BR_BTB_SITE_BLOCK(59)
BR_BTB_SITE_BLOCK(60)
BR_BTB_SITE_BLOCK(61)
BR_BTB_SITE_BLOCK(62)
BR_BTB_SITE_BLOCK(63)

#define BR_BTB_SITE_ENTRY(id) br_btb_site_blk_##id,

static br_btb_fn_t volatile g_br_btb_site_blks[BR_BTB_SITE_BLOCK_COUNT] = {
    BR_BTB_SITE_ENTRY(0) BR_BTB_SITE_ENTRY(1) BR_BTB_SITE_ENTRY(2) BR_BTB_SITE_ENTRY(
        3) BR_BTB_SITE_ENTRY(4) BR_BTB_SITE_ENTRY(5) BR_BTB_SITE_ENTRY(6) BR_BTB_SITE_ENTRY(7)
        BR_BTB_SITE_ENTRY(8) BR_BTB_SITE_ENTRY(9) BR_BTB_SITE_ENTRY(10) BR_BTB_SITE_ENTRY(
            11) BR_BTB_SITE_ENTRY(12) BR_BTB_SITE_ENTRY(13) BR_BTB_SITE_ENTRY(14)
            BR_BTB_SITE_ENTRY(15) BR_BTB_SITE_ENTRY(16) BR_BTB_SITE_ENTRY(17) BR_BTB_SITE_ENTRY(
                18) BR_BTB_SITE_ENTRY(19) BR_BTB_SITE_ENTRY(20) BR_BTB_SITE_ENTRY(21)
                BR_BTB_SITE_ENTRY(22) BR_BTB_SITE_ENTRY(23) BR_BTB_SITE_ENTRY(24) BR_BTB_SITE_ENTRY(
                    25) BR_BTB_SITE_ENTRY(26) BR_BTB_SITE_ENTRY(27) BR_BTB_SITE_ENTRY(28)
                    BR_BTB_SITE_ENTRY(29) BR_BTB_SITE_ENTRY(30) BR_BTB_SITE_ENTRY(
                        31) BR_BTB_SITE_ENTRY(32) BR_BTB_SITE_ENTRY(33) BR_BTB_SITE_ENTRY(34)
                        BR_BTB_SITE_ENTRY(35) BR_BTB_SITE_ENTRY(36) BR_BTB_SITE_ENTRY(
                            37) BR_BTB_SITE_ENTRY(38) BR_BTB_SITE_ENTRY(39) BR_BTB_SITE_ENTRY(40)
                            BR_BTB_SITE_ENTRY(41) BR_BTB_SITE_ENTRY(42) BR_BTB_SITE_ENTRY(43)
                                BR_BTB_SITE_ENTRY(44) BR_BTB_SITE_ENTRY(45) BR_BTB_SITE_ENTRY(46)
                                    BR_BTB_SITE_ENTRY(47) BR_BTB_SITE_ENTRY(48) BR_BTB_SITE_ENTRY(
                                        49) BR_BTB_SITE_ENTRY(50) BR_BTB_SITE_ENTRY(51)
                                        BR_BTB_SITE_ENTRY(52) BR_BTB_SITE_ENTRY(53)
                                            BR_BTB_SITE_ENTRY(54) BR_BTB_SITE_ENTRY(55)
                                                BR_BTB_SITE_ENTRY(56) BR_BTB_SITE_ENTRY(57)
                                                    BR_BTB_SITE_ENTRY(58) BR_BTB_SITE_ENTRY(59)
                                                        BR_BTB_SITE_ENTRY(60) BR_BTB_SITE_ENTRY(61)
                                                            BR_BTB_SITE_ENTRY(62)
                                                                BR_BTB_SITE_ENTRY(63)};

static PAL_NOINLINE uint64_t br_btb_walk(size_t count, uint64_t iterations)
{
    uint64_t acc = UINT64_C(0x517cc1b727220a95);
    uint64_t it = 0u;

    for (it = 0u; it < iterations; it++) {
        for (size_t i = 0u; i < count; i++) {
            const br_btb_fn_t fn = g_br_btb_fns[i];

            acc = fn(acc);
        }
    }

    PAL_BARRIER();
    g_br_sink = acc;
    return acc;
}

static PAL_NOINLINE uint64_t br_btb_sites_walk(size_t blocks, uint64_t iterations)
{
    uint64_t acc = UINT64_C(0x517cc1b727220a95);
    uint64_t it = 0u;

    for (it = 0u; it < iterations; it++) {
        size_t block = 0u;

        for (block = 0u; block < blocks; block++) {
            const br_btb_fn_t fn = g_br_btb_site_blks[block];

            acc = fn(acc);
        }
    }

    PAL_BARRIER();
    g_br_sink = acc;
    return acc;
}

static double br_btb_sites_sample(size_t blocks, uint64_t iterations)
{
    const uint64_t calls = iterations * (uint64_t)blocks * (uint64_t)BR_BTB_SITE_BLOCK_SITES;
    const uint64_t begin = pal_now_ns();
    const uint64_t sink = br_btb_sites_walk(blocks, iterations);
    uint64_t end = pal_now_ns();

    g_br_sink = sink;
    if (end <= begin || calls == 0u) {
        return 0.0;
    }
    return (double)(end - begin) / (double)calls;
}

static double br_btb_sample(size_t count, uint64_t iterations)
{
    const uint64_t calls = iterations * (uint64_t)count;
    const uint64_t begin = pal_now_ns();
    const uint64_t sink = br_btb_walk(count, iterations);
    uint64_t end = pal_now_ns();

    g_br_sink = sink;
    if (end <= begin || calls == 0u) {
        return 0.0;
    }
    return (double)(end - begin) / (double)calls;
}

lb_status_t br_btb_run(const lb_cfg_t *cfg)
{
    double ns_per_call[BR_BTB_COUNT_COUNT] = {0.0};
    double delta_pct[BR_BTB_COUNT_COUNT] = {0.0};
    double site_ns[BR_BTB_SITE_COUNT_COUNT] = {0.0};
    double site_delta[BR_BTB_SITE_COUNT_COUNT] = {0.0};
    double base_ns = 0.0;
    size_t threshold_index = 0u;
    bool threshold_found = false;
    size_t site_threshold_index = 0u;
    bool site_threshold_found = false;
    uint64_t calls = 0u;
    size_t repeats = 0u;
    uint64_t baseline = 0u;
    size_t index = 0u;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    calls = cfg->fast ? BR_BTB_FAST_CALLS : BR_BTB_CALLS;
    repeats = cfg->fast ? BR_FAST_REPEATS : BR_BTB_REPEATS;

    lb_bench_apply_runtime(cfg);

    tty_csv_row(cfg, "branch,btb,functions,ns_per_call,delta_pct");
    tty_section(cfg, "Branch target buffer");
    tty_kv(cfg, "Trampolines", "16 .. 4096 unique noinline targets");
    tty_kv(cfg, "Callsites", "64 .. 4096 distinct indirect PCs");
    tty_kv(cfg, "Calls / repeats", "%" PRIu64 " / %zu", calls, repeats);
    tty_blank(cfg);
    tty_header(cfg, "  %9s %12s %10s", "Functions", "ns/call", "delta");

    baseline = stats_canary_baseline(LB_BENCH_CANARY_MS);

    for (index = 0u; index < BR_BTB_COUNT_COUNT; index++) {
        const size_t count = BR_BTB_COUNTS[index];
        uint64_t iterations = calls / (uint64_t)count;
        double samples[BR_BTB_REPEATS] = {0.0};
        size_t repeat = 0u;

        if (iterations < 64u) {
            iterations = 64u;
        }
        (void)br_btb_walk(count, iterations / 8u);
        for (repeat = 0u; repeat < repeats; repeat++) {
            samples[repeat] = br_btb_sample(count, iterations);
        }
        lb_bench_cool(cfg);
        ns_per_call[index] = stats_percentile(samples, repeats, STATS_NOISE_PERCENTILE);
    }

    base_ns = ns_per_call[0];
    for (index = 1u; index < BR_BTB_COUNT_COUNT; index++) {
        delta_pct[index] =
            (base_ns > 0.0) ? (((ns_per_call[index] - base_ns) / base_ns) * 100.0) : 0.0;
    }

    for (index = 0u; index < BR_BTB_COUNT_COUNT; index++) {
        const size_t count = BR_BTB_COUNTS[index];

        /*
         * Require a persistent jump: a +11% noise blip must not shrink the
         * reported BTB/L1i capacity, while a real overflow stays above +20%
         * on the following count.
         */
        if (!threshold_found && index > 0u && delta_pct[index] >= BR_BTB_JUMP_PCT) {
            const bool sustained = ((index + 1u) >= BR_BTB_COUNT_COUNT) ||
                                   (delta_pct[index + 1u] >= BR_BTB_SUSTAIN_PCT);

            if (sustained) {
                threshold_found = true;
                threshold_index = index;
            }
        }

        tty_header(cfg, "  %9zu %12.4f %9.1f%%", count, ns_per_call[index], delta_pct[index]);
        tty_csv_row(cfg, "branch,btb,%zu,%.4f,%.2f", count, ns_per_call[index], delta_pct[index]);
    }

    if (threshold_found) {
        lb_passport_mutable()->btb_targets = BR_BTB_COUNTS[threshold_index - 1u];
        tty_blank(cfg);
        tty_header(cfg, "  BTB / L1i capacity: ~%zu targets (next count +%.0f%%)",
                   BR_BTB_COUNTS[threshold_index - 1u], BR_BTB_JUMP_PCT);
        tty_csv_row(cfg, "branch,btb,capacity,%zu,targets", BR_BTB_COUNTS[threshold_index - 1u]);
    } else {
        lb_passport_mutable()->btb_targets = BR_BTB_COUNTS[BR_BTB_COUNT_COUNT - 1u];
        tty_blank(cfg);
        tty_header(cfg, "  BTB / L1i capacity: beyond %zu targets",
                   BR_BTB_COUNTS[BR_BTB_COUNT_COUNT - 1u]);
        tty_csv_row(cfg, "branch,btb,capacity,%zu,targets", BR_BTB_COUNTS[BR_BTB_COUNT_COUNT - 1u]);
    }

    for (index = 0u; index < BR_BTB_SITE_COUNT_COUNT; index++) {
        const size_t count = BR_BTB_SITE_COUNTS[index];
        const size_t blocks = count / (size_t)BR_BTB_SITE_BLOCK_SITES;
        uint64_t iterations = calls / (uint64_t)count;
        double samples[BR_BTB_REPEATS] = {0.0};
        size_t repeat = 0u;

        if (iterations < 64u) {
            iterations = 64u;
        }
        (void)br_btb_sites_walk(blocks, iterations / 8u);
        for (repeat = 0u; repeat < repeats; repeat++) {
            samples[repeat] = br_btb_sites_sample(blocks, iterations);
        }
        lb_bench_cool(cfg);
        site_ns[index] = stats_percentile(samples, repeats, STATS_NOISE_PERCENTILE);
    }

    base_ns = site_ns[0];
    for (index = 0u; index < BR_BTB_SITE_COUNT_COUNT; index++) {
        site_delta[index] =
            (base_ns > 0.0) ? (((site_ns[index] - base_ns) / base_ns) * 100.0) : 0.0;
    }

    tty_blank(cfg);
    tty_header(cfg, "  Callsites (distinct indirect PCs)");
    tty_header(cfg, "  %9s %12s %10s", "Sites", "ns/call", "delta");
    for (index = 0u; index < BR_BTB_SITE_COUNT_COUNT; index++) {
        const size_t count = BR_BTB_SITE_COUNTS[index];

        if (!site_threshold_found && index > 0u && site_delta[index] >= BR_BTB_JUMP_PCT) {
            const bool sustained = ((index + 1u) >= BR_BTB_SITE_COUNT_COUNT) ||
                                   (site_delta[index + 1u] >= BR_BTB_SUSTAIN_PCT);

            if (sustained) {
                site_threshold_found = true;
                site_threshold_index = index;
            }
        }

        tty_header(cfg, "  %9zu %12.4f %9.1f%%", count, site_ns[index], site_delta[index]);
        tty_csv_row(cfg, "branch,btb,sites_%zu,%.4f,%.2f", count, site_ns[index],
                    site_delta[index]);
    }

    if (site_threshold_found) {
        tty_header(cfg, "  Callsite capacity: ~%zu sites (next count +%.0f%%)",
                   BR_BTB_SITE_COUNTS[site_threshold_index - 1u], BR_BTB_JUMP_PCT);
        tty_csv_row(cfg, "branch,btb,callsite_capacity,%zu,sites",
                    BR_BTB_SITE_COUNTS[site_threshold_index - 1u]);
    } else {
        tty_header(cfg, "  Callsite capacity: beyond %zu sites",
                   BR_BTB_SITE_COUNTS[BR_BTB_SITE_COUNT_COUNT - 1u]);
        tty_csv_row(cfg, "branch,btb,callsite_capacity,%zu,sites",
                    BR_BTB_SITE_COUNTS[BR_BTB_SITE_COUNT_COUNT - 1u]);
    }

    tty_blank(cfg);
    lb_bench_canary_finish(cfg, baseline, true);
    return LB_STATUS_OK;
}
