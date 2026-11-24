/* SPDX-License-Identifier: GPL-3.0 */

#include "bench.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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
#define BR_CANARY_MS 20u

/* branch hist: periodic 0/1 pattern, always indexed with the same mask. */
#define BR_HIST_ARRAY_BYTES 4096u
#define BR_HIST_RANDOM_BYTES ((size_t)262144u)
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
#define BR_RSB_JUMP_PCT 10.0
#define BR_RSB_DEPTH_COUNT 9u

/* branch btb: indirect calls through a table of unique trampolines. */
#define BR_BTB_MAX 1024u
#define BR_BTB_CALLS UINT64_C(2097152)
#define BR_BTB_REPEATS 3u
#define BR_BTB_JUMP_PCT 10.0
#define BR_BTB_COUNT_COUNT 7u

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
    4u, 8u, 12u, 16u, 20u, 24u, 32u, 48u, 64u,
};

static const size_t BR_BTB_COUNTS[BR_BTB_COUNT_COUNT] = {
    16u, 32u, 64u, 128u, 256u, 512u, 1024u,
};

/*
 * Volatile result sink. Every timed loop publishes its checksum here so the
 * optimizer cannot discard the dependent work.
 */
static volatile uint64_t g_br_sink = 0u;

/* The branch-history pattern is rewritten between measurements. */
static uint8_t g_br_hist_pattern[BR_HIST_ARRAY_BYTES] = {0};

/*
 * Aperiodic reference pattern. It is far longer than any realistic predictor
 * history, so its delta against the constant baseline exposes the full
 * mispredict penalty even on cores that learn every short periodic pattern.
 */
static uint8_t g_br_hist_random[BR_HIST_RANDOM_BYTES] = {0};

/* Indirect entry point for the recursive chain. */
static br_rsb_fn_t volatile g_br_rsb_entry = NULL;

static void br_apply_runtime(const lb_cfg_t *cfg)
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

static void br_cool(const lb_cfg_t *cfg)
{
    if (cfg->cool_ms > 0u) {
        pal_sleep_ms(cfg->cool_ms);
    }
}

static void br_canary_finish(const lb_cfg_t *cfg, uint64_t baseline)
{
    const uint64_t after = stats_canary_measure(BR_CANARY_MS);
    const double drift = stats_canary_drift_pct(baseline, after);

    tty_warn_drift(drift);
    tty_kv(cfg, "DVFS drift", "%+.1f%%", drift);
}

/* ------------------------------------------------------------- branch hist */

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

    for (i = 0u; i < BR_HIST_ARRAY_BYTES; i++) {
        g_br_hist_pattern[i] = base[i & mask];
    }
}

static void br_hist_random_fill(uint64_t seed)
{
    uint64_t state = seed;
    size_t i = 0u;

    for (i = 0u; i < BR_HIST_RANDOM_BYTES; i++) {
        g_br_hist_random[i] = (uint8_t)(util_rng_next(&state) & UINT64_C(1));
    }
}

static PAL_NOINLINE uint64_t br_hist_walk(const uint8_t *pattern, uint64_t iterations,
                                          uint64_t seed, size_t mask)
{
    uint64_t a = seed;
    uint64_t b = seed ^ UINT64_C(0x9e3779b97f4a7c15);
    uint64_t i = 0u;

    for (i = 0u; i < iterations; i++) {
        const uint8_t bit = pattern[(size_t)i & mask];

        if (bit != 0u) {
            PAL_BLOCK_IFCONV();
            a = (a + b) ^ (i + UINT64_C(0x2545f4914f6cdd1d));
            b = (b ^ a) + UINT64_C(0x6a09e667f3bcc909);
        } else {
            PAL_BLOCK_IFCONV();
            b = (b + a) ^ (i + UINT64_C(0x2545f4914f6cdd1d));
            a = (a ^ b) + UINT64_C(0x6a09e667f3bcc909);
        }
    }

    PAL_BARRIER();
    g_br_sink = a ^ b;
    return g_br_sink;
}

static double br_hist_sample(const uint8_t *pattern, uint64_t iterations, uint64_t seed,
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

static double br_hist_measure(const lb_cfg_t *cfg, const uint8_t *pattern, uint64_t seed,
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
    br_cool(cfg);

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

    br_apply_runtime(cfg);

    tty_csv_row(cfg, "branch,hist,pattern,ns_per_branch,delta_ns,delta_pct");
    tty_section(cfg, "Branch history");
    tty_kv(cfg, "Pattern", "periodic 0/1, period 2 .. 4096 branches");
    tty_kv(cfg, "Iterations / repeats", "%" PRIu64 " / %u",
           cfg->fast ? BR_HIST_FAST_ITERATIONS : BR_HIST_ITERATIONS,
           cfg->fast ? BR_FAST_REPEATS : BR_HIST_REPEATS);
    tty_header(cfg, "  %-10s %12s %12s %10s", "Pattern", "ns/branch", "delta ns", "delta");

    baseline = stats_canary_baseline(BR_CANARY_MS);

    memset(g_br_hist_pattern, 0, sizeof(g_br_hist_pattern));
    base_ns = br_hist_measure(cfg, g_br_hist_pattern, UINT64_C(0x13198a2e03707344),
                              (size_t)(BR_HIST_ARRAY_BYTES - 1u));
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
            br_hist_measure(cfg, g_br_hist_pattern, seed, (size_t)(BR_HIST_ARRAY_BYTES - 1u));
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
            br_hist_measure(cfg, g_br_hist_random, seed, (size_t)(BR_HIST_RANDOM_BYTES - 1u));
        delta_pct = (base_ns > 0.0) ? (((ns_per_branch - base_ns) / base_ns) * 100.0) : 0.0;

        random_delta = ns_per_branch - base_ns;
        tty_header(cfg, "  %-10s %12.4f %12.4f %9.1f%%", "random", ns_per_branch, random_delta,
                   delta_pct);
        tty_csv_row(cfg, "branch,hist,random,%.4f,%.4f,%.2f", ns_per_branch, random_delta,
                    delta_pct);
    }

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

    br_canary_finish(cfg, baseline);
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
    double base_ns = 0.0;
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

    br_apply_runtime(cfg);

    g_br_rsb_entry = br_rsb_recurse;
    PAL_BARRIER();

    tty_csv_row(cfg, "branch,rsb,depth,ns_per_call,delta_pct");
    tty_section(cfg, "Return-stack buffer");
    tty_kv(cfg, "Recursion", "noinline chain through a volatile function pointer");
    tty_kv(cfg, "Iterations / repeats", "%" PRIu64 " / %zu", iterations, repeats);
    tty_header(cfg, "  %7s %14s %12s %10s", "Depth", "Calls/top", "ns/call", "delta");

    baseline = stats_canary_baseline(BR_CANARY_MS);

    for (index = 0u; index < BR_RSB_DEPTH_COUNT; index++) {
        const uint64_t depth = BR_RSB_DEPTHS[index];
        double samples[BR_RSB_REPEATS] = {0.0};
        double ns_per_call = 0.0;
        double delta_pct = 0.0;
        size_t repeat = 0u;

        (void)br_rsb_walk(&g_br_rsb_entry, depth, iterations / 10u);
        for (repeat = 0u; repeat < repeats; repeat++) {
            samples[repeat] = br_rsb_sample(depth, iterations);
        }
        br_cool(cfg);

        ns_per_call = stats_percentile(samples, repeats, STATS_NOISE_PERCENTILE);
        if (index == 0u) {
            base_ns = ns_per_call;
        } else if (base_ns > 0.0) {
            delta_pct = ((ns_per_call - base_ns) / base_ns) * 100.0;
        }
        if (index > 0u && !threshold_found && delta_pct >= BR_RSB_JUMP_PCT) {
            threshold_found = true;
            threshold_index = index;
        }

        tty_header(cfg, "  %7" PRIu64 " %14" PRIu64 " %12.4f %9.1f%%", depth, depth + 1u,
                   ns_per_call, delta_pct);
        tty_csv_row(cfg, "branch,rsb,%" PRIu64 ",%.4f,%.2f", depth, ns_per_call, delta_pct);
    }

    if (threshold_found) {
        lb_passport_mutable()->rsb_frames = BR_RSB_DEPTHS[threshold_index - 1u];
        tty_header(cfg, "  RSB threshold: ~%" PRIu64 " frames (next depth +%.0f%%)",
                   BR_RSB_DEPTHS[threshold_index - 1u], BR_RSB_JUMP_PCT);
        tty_csv_row(cfg, "branch,rsb,threshold,%" PRIu64 ",frames",
                    BR_RSB_DEPTHS[threshold_index - 1u]);
    } else {
        lb_passport_mutable()->rsb_frames = BR_RSB_DEPTHS[BR_RSB_DEPTH_COUNT - 1u];
        tty_header(cfg, "  RSB threshold: beyond %" PRIu64 " frames",
                   BR_RSB_DEPTHS[BR_RSB_DEPTH_COUNT - 1u]);
        tty_csv_row(cfg, "branch,rsb,threshold,%" PRIu64 ",frames",
                    BR_RSB_DEPTHS[BR_RSB_DEPTH_COUNT - 1u]);
    }

    br_canary_finish(cfg, baseline);
    return LB_STATUS_OK;
}

/* -------------------------------------------------------------- branch btb */

#define BR_BTB_TRAMPOLINE(id)                                                                      \
    static PAL_NOINLINE uint64_t br_btb_fn_##id(uint64_t acc)                                      \
    {                                                                                              \
        return (acc * UINT64_C(33)) ^ ((uint64_t)(id) * UINT64_C(0x9e3779b97f4a7c15));             \
    }

#define BR_BTB_SEQ_16(m, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15)     \
    m(a0) m(a1) m(a2) m(a3) m(a4) m(a5) m(a6) m(a7) m(a8) m(a9) m(a10) m(a11) m(a12) m(a13) m(a14) \
        m(a15)

#define BR_BTB_ALL(m)                                                                              \
    BR_BTB_SEQ_16(m, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15)                         \
    BR_BTB_SEQ_16(m, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31)               \
    BR_BTB_SEQ_16(m, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47)               \
    BR_BTB_SEQ_16(m, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63)               \
    BR_BTB_SEQ_16(m, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79)               \
    BR_BTB_SEQ_16(m, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95)               \
    BR_BTB_SEQ_16(m, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111)   \
    BR_BTB_SEQ_16(m, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126,    \
                  127)                                                                             \
    BR_BTB_SEQ_16(m, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142,    \
                  143)                                                                             \
    BR_BTB_SEQ_16(m, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158,    \
                  159)                                                                             \
    BR_BTB_SEQ_16(m, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174,    \
                  175)                                                                             \
    BR_BTB_SEQ_16(m, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190,    \
                  191)                                                                             \
    BR_BTB_SEQ_16(m, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206,    \
                  207)                                                                             \
    BR_BTB_SEQ_16(m, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222,    \
                  223)                                                                             \
    BR_BTB_SEQ_16(m, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238,    \
                  239)                                                                             \
    BR_BTB_SEQ_16(m, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254,    \
                  255)                                                                             \
    BR_BTB_SEQ_16(m, 256, 257, 258, 259, 260, 261, 262, 263, 264, 265, 266, 267, 268, 269, 270,    \
                  271)                                                                             \
    BR_BTB_SEQ_16(m, 272, 273, 274, 275, 276, 277, 278, 279, 280, 281, 282, 283, 284, 285, 286,    \
                  287)                                                                             \
    BR_BTB_SEQ_16(m, 288, 289, 290, 291, 292, 293, 294, 295, 296, 297, 298, 299, 300, 301, 302,    \
                  303)                                                                             \
    BR_BTB_SEQ_16(m, 304, 305, 306, 307, 308, 309, 310, 311, 312, 313, 314, 315, 316, 317, 318,    \
                  319)                                                                             \
    BR_BTB_SEQ_16(m, 320, 321, 322, 323, 324, 325, 326, 327, 328, 329, 330, 331, 332, 333, 334,    \
                  335)                                                                             \
    BR_BTB_SEQ_16(m, 336, 337, 338, 339, 340, 341, 342, 343, 344, 345, 346, 347, 348, 349, 350,    \
                  351)                                                                             \
    BR_BTB_SEQ_16(m, 352, 353, 354, 355, 356, 357, 358, 359, 360, 361, 362, 363, 364, 365, 366,    \
                  367)                                                                             \
    BR_BTB_SEQ_16(m, 368, 369, 370, 371, 372, 373, 374, 375, 376, 377, 378, 379, 380, 381, 382,    \
                  383)                                                                             \
    BR_BTB_SEQ_16(m, 384, 385, 386, 387, 388, 389, 390, 391, 392, 393, 394, 395, 396, 397, 398,    \
                  399)                                                                             \
    BR_BTB_SEQ_16(m, 400, 401, 402, 403, 404, 405, 406, 407, 408, 409, 410, 411, 412, 413, 414,    \
                  415)                                                                             \
    BR_BTB_SEQ_16(m, 416, 417, 418, 419, 420, 421, 422, 423, 424, 425, 426, 427, 428, 429, 430,    \
                  431)                                                                             \
    BR_BTB_SEQ_16(m, 432, 433, 434, 435, 436, 437, 438, 439, 440, 441, 442, 443, 444, 445, 446,    \
                  447)                                                                             \
    BR_BTB_SEQ_16(m, 448, 449, 450, 451, 452, 453, 454, 455, 456, 457, 458, 459, 460, 461, 462,    \
                  463)                                                                             \
    BR_BTB_SEQ_16(m, 464, 465, 466, 467, 468, 469, 470, 471, 472, 473, 474, 475, 476, 477, 478,    \
                  479)                                                                             \
    BR_BTB_SEQ_16(m, 480, 481, 482, 483, 484, 485, 486, 487, 488, 489, 490, 491, 492, 493, 494,    \
                  495)                                                                             \
    BR_BTB_SEQ_16(m, 496, 497, 498, 499, 500, 501, 502, 503, 504, 505, 506, 507, 508, 509, 510,    \
                  511)                                                                             \
    BR_BTB_SEQ_16(m, 512, 513, 514, 515, 516, 517, 518, 519, 520, 521, 522, 523, 524, 525, 526,    \
                  527)                                                                             \
    BR_BTB_SEQ_16(m, 528, 529, 530, 531, 532, 533, 534, 535, 536, 537, 538, 539, 540, 541, 542,    \
                  543)                                                                             \
    BR_BTB_SEQ_16(m, 544, 545, 546, 547, 548, 549, 550, 551, 552, 553, 554, 555, 556, 557, 558,    \
                  559)                                                                             \
    BR_BTB_SEQ_16(m, 560, 561, 562, 563, 564, 565, 566, 567, 568, 569, 570, 571, 572, 573, 574,    \
                  575)                                                                             \
    BR_BTB_SEQ_16(m, 576, 577, 578, 579, 580, 581, 582, 583, 584, 585, 586, 587, 588, 589, 590,    \
                  591)                                                                             \
    BR_BTB_SEQ_16(m, 592, 593, 594, 595, 596, 597, 598, 599, 600, 601, 602, 603, 604, 605, 606,    \
                  607)                                                                             \
    BR_BTB_SEQ_16(m, 608, 609, 610, 611, 612, 613, 614, 615, 616, 617, 618, 619, 620, 621, 622,    \
                  623)                                                                             \
    BR_BTB_SEQ_16(m, 624, 625, 626, 627, 628, 629, 630, 631, 632, 633, 634, 635, 636, 637, 638,    \
                  639)                                                                             \
    BR_BTB_SEQ_16(m, 640, 641, 642, 643, 644, 645, 646, 647, 648, 649, 650, 651, 652, 653, 654,    \
                  655)                                                                             \
    BR_BTB_SEQ_16(m, 656, 657, 658, 659, 660, 661, 662, 663, 664, 665, 666, 667, 668, 669, 670,    \
                  671)                                                                             \
    BR_BTB_SEQ_16(m, 672, 673, 674, 675, 676, 677, 678, 679, 680, 681, 682, 683, 684, 685, 686,    \
                  687)                                                                             \
    BR_BTB_SEQ_16(m, 688, 689, 690, 691, 692, 693, 694, 695, 696, 697, 698, 699, 700, 701, 702,    \
                  703)                                                                             \
    BR_BTB_SEQ_16(m, 704, 705, 706, 707, 708, 709, 710, 711, 712, 713, 714, 715, 716, 717, 718,    \
                  719)                                                                             \
    BR_BTB_SEQ_16(m, 720, 721, 722, 723, 724, 725, 726, 727, 728, 729, 730, 731, 732, 733, 734,    \
                  735)                                                                             \
    BR_BTB_SEQ_16(m, 736, 737, 738, 739, 740, 741, 742, 743, 744, 745, 746, 747, 748, 749, 750,    \
                  751)                                                                             \
    BR_BTB_SEQ_16(m, 752, 753, 754, 755, 756, 757, 758, 759, 760, 761, 762, 763, 764, 765, 766,    \
                  767)                                                                             \
    BR_BTB_SEQ_16(m, 768, 769, 770, 771, 772, 773, 774, 775, 776, 777, 778, 779, 780, 781, 782,    \
                  783)                                                                             \
    BR_BTB_SEQ_16(m, 784, 785, 786, 787, 788, 789, 790, 791, 792, 793, 794, 795, 796, 797, 798,    \
                  799)                                                                             \
    BR_BTB_SEQ_16(m, 800, 801, 802, 803, 804, 805, 806, 807, 808, 809, 810, 811, 812, 813, 814,    \
                  815)                                                                             \
    BR_BTB_SEQ_16(m, 816, 817, 818, 819, 820, 821, 822, 823, 824, 825, 826, 827, 828, 829, 830,    \
                  831)                                                                             \
    BR_BTB_SEQ_16(m, 832, 833, 834, 835, 836, 837, 838, 839, 840, 841, 842, 843, 844, 845, 846,    \
                  847)                                                                             \
    BR_BTB_SEQ_16(m, 848, 849, 850, 851, 852, 853, 854, 855, 856, 857, 858, 859, 860, 861, 862,    \
                  863)                                                                             \
    BR_BTB_SEQ_16(m, 864, 865, 866, 867, 868, 869, 870, 871, 872, 873, 874, 875, 876, 877, 878,    \
                  879)                                                                             \
    BR_BTB_SEQ_16(m, 880, 881, 882, 883, 884, 885, 886, 887, 888, 889, 890, 891, 892, 893, 894,    \
                  895)                                                                             \
    BR_BTB_SEQ_16(m, 896, 897, 898, 899, 900, 901, 902, 903, 904, 905, 906, 907, 908, 909, 910,    \
                  911)                                                                             \
    BR_BTB_SEQ_16(m, 912, 913, 914, 915, 916, 917, 918, 919, 920, 921, 922, 923, 924, 925, 926,    \
                  927)                                                                             \
    BR_BTB_SEQ_16(m, 928, 929, 930, 931, 932, 933, 934, 935, 936, 937, 938, 939, 940, 941, 942,    \
                  943)                                                                             \
    BR_BTB_SEQ_16(m, 944, 945, 946, 947, 948, 949, 950, 951, 952, 953, 954, 955, 956, 957, 958,    \
                  959)                                                                             \
    BR_BTB_SEQ_16(m, 960, 961, 962, 963, 964, 965, 966, 967, 968, 969, 970, 971, 972, 973, 974,    \
                  975)                                                                             \
    BR_BTB_SEQ_16(m, 976, 977, 978, 979, 980, 981, 982, 983, 984, 985, 986, 987, 988, 989, 990,    \
                  991)                                                                             \
    BR_BTB_SEQ_16(m, 992, 993, 994, 995, 996, 997, 998, 999, 1000, 1001, 1002, 1003, 1004, 1005,   \
                  1006, 1007)                                                                      \
    BR_BTB_SEQ_16(m, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 1015, 1016, 1017, 1018, 1019, 1020, \
                  1021, 1022, 1023)

BR_BTB_ALL(BR_BTB_TRAMPOLINE)

#define BR_BTB_ENTRY(id) br_btb_fn_##id,

static br_btb_fn_t volatile g_br_btb_fns[BR_BTB_MAX] = {BR_BTB_ALL(BR_BTB_ENTRY)};

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
    double base_ns = 0.0;
    size_t threshold_index = 0u;
    bool threshold_found = false;
    uint64_t calls = 0u;
    size_t repeats = 0u;
    uint64_t baseline = 0u;
    size_t index = 0u;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    calls = cfg->fast ? BR_BTB_FAST_CALLS : BR_BTB_CALLS;
    repeats = cfg->fast ? BR_FAST_REPEATS : BR_BTB_REPEATS;

    br_apply_runtime(cfg);

    tty_csv_row(cfg, "branch,btb,functions,ns_per_call,delta_pct");
    tty_section(cfg, "Branch target buffer");
    tty_kv(cfg, "Trampolines", "16 .. 1024 unique noinline targets");
    tty_kv(cfg, "Calls / repeats", "%" PRIu64 " / %zu", calls, repeats);
    tty_header(cfg, "  %9s %12s %10s", "Functions", "ns/call", "delta");

    baseline = stats_canary_baseline(BR_CANARY_MS);

    for (index = 0u; index < BR_BTB_COUNT_COUNT; index++) {
        const size_t count = BR_BTB_COUNTS[index];
        const uint64_t iterations = calls / (uint64_t)count;
        double samples[BR_BTB_REPEATS] = {0.0};
        double ns_per_call = 0.0;
        double delta_pct = 0.0;
        size_t repeat = 0u;

        (void)br_btb_walk(count, iterations / 8u);
        for (repeat = 0u; repeat < repeats; repeat++) {
            samples[repeat] = br_btb_sample(count, iterations);
        }
        br_cool(cfg);

        ns_per_call = stats_percentile(samples, repeats, STATS_NOISE_PERCENTILE);
        if (index == 0u) {
            base_ns = ns_per_call;
        } else if (base_ns > 0.0) {
            delta_pct = ((ns_per_call - base_ns) / base_ns) * 100.0;
        }
        if (index > 0u && !threshold_found && delta_pct >= BR_BTB_JUMP_PCT) {
            threshold_found = true;
            threshold_index = index;
        }

        tty_header(cfg, "  %9zu %12.4f %9.1f%%", count, ns_per_call, delta_pct);
        tty_csv_row(cfg, "branch,btb,%zu,%.4f,%.2f", count, ns_per_call, delta_pct);
    }

    if (threshold_found) {
        lb_passport_mutable()->btb_targets = BR_BTB_COUNTS[threshold_index - 1u];
        tty_header(cfg, "  BTB / L1i capacity: ~%zu targets (next count +%.0f%%)",
                   BR_BTB_COUNTS[threshold_index - 1u], BR_BTB_JUMP_PCT);
        tty_csv_row(cfg, "branch,btb,capacity,%zu,targets", BR_BTB_COUNTS[threshold_index - 1u]);
    } else {
        lb_passport_mutable()->btb_targets = BR_BTB_COUNTS[BR_BTB_COUNT_COUNT - 1u];
        tty_header(cfg, "  BTB / L1i capacity: beyond %zu targets",
                   BR_BTB_COUNTS[BR_BTB_COUNT_COUNT - 1u]);
        tty_csv_row(cfg, "branch,btb,capacity,%zu,targets", BR_BTB_COUNTS[BR_BTB_COUNT_COUNT - 1u]);
    }

    br_canary_finish(cfg, baseline);
    return LB_STATUS_OK;
}
