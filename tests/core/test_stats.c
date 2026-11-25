/* SPDX-License-Identifier: GPL-3.0 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stats.h"

#define TEST_STEP_TOLERANCE 1e-9

static int g_checks = 0;
static int g_failures = 0;

static void check_true(bool condition, const char *name)
{
    g_checks++;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        g_failures++;
    }
}

static bool approx(double actual, double expected)
{
    double delta = actual - expected;

    if (delta < 0.0) {
        delta = -delta;
    }
    return delta <= TEST_STEP_TOLERANCE;
}

static void build_log_x(size_t *x, size_t count, size_t first_shift)
{
    size_t i = 0u;

    for (i = 0u; i < count; i++) {
        x[i] = ((size_t)1u) << (first_shift + i);
    }
}

static void test_percentile(void)
{
    const double values[5] = {5.0, 1.0, 3.0, 2.0, 4.0};
    const double even[4] = {1.0, 2.0, 3.0, 4.0};
    const double duplicates[5] = {2.0, 2.0, 2.0, 2.0, 2.0};

    check_true(approx(stats_percentile(values, 5u, 0.0), 1.0), "p0 returns the minimum");
    check_true(approx(stats_percentile(values, 5u, 10.0), 1.0), "p10 returns the low tail");
    check_true(approx(stats_percentile(values, 5u, 50.0), 3.0), "p50 returns the median");
    check_true(approx(stats_percentile(values, 5u, 90.0), 5.0), "p90 returns the high tail");
    check_true(approx(stats_percentile(values, 5u, 100.0), 5.0), "p100 returns the maximum");
    check_true(approx(stats_percentile(values, 5u, 150.0), 5.0), "percentile above 100 is clamped");
    check_true(approx(stats_percentile(values, 5u, -10.0), 1.0), "percentile below 0 is clamped");
    check_true(approx(stats_percentile(even, 4u, 50.0), 2.0), "p50 of an even sample count");
    check_true(approx(stats_percentile(duplicates, 5u, 50.0), 2.0),
               "percentile handles duplicate values");

    check_true(values[0] == 5.0 && values[1] == 1.0, "stats_percentile does not modify the input");
    check_true(stats_percentile(NULL, 5u, 50.0) == 0.0, "stats_percentile(NULL) returns zero");
    check_true(stats_percentile(values, 0u, 50.0) == 0.0, "stats_percentile(count=0) returns zero");
}

static void test_smooth_median3(void)
{
    const double spike[5] = {10.0, 10.0, 1000.0, 10.0, 10.0};
    const double ramp[5] = {1.0, 2.0, 3.0, 4.0, 5.0};
    const double edges[3] = {1.0, 100.0, 1.0};
    const double single[1] = {42.0};
    double out[5] = {0.0};
    size_t i = 0u;
    lb_status_t status = LB_STATUS_ERROR;

    status = stats_smooth_median3(spike, 5u, out);
    check_true(status == LB_STATUS_OK, "stats_smooth_median3(spike) succeeds");
    for (i = 0u; i < 5u; i++) {
        check_true(out[i] == 10.0, "median3 removes a single outlier");
    }

    status = stats_smooth_median3(ramp, 5u, out);
    check_true(status == LB_STATUS_OK, "stats_smooth_median3(ramp) succeeds");
    for (i = 0u; i < 5u; i++) {
        check_true(out[i] == ramp[i], "median3 preserves a monotonic ramp");
    }

    status = stats_smooth_median3(edges, 3u, out);
    check_true(status == LB_STATUS_OK, "stats_smooth_median3(edges) succeeds");
    for (i = 0u; i < 3u; i++) {
        check_true(out[i] == 1.0, "median3 replicates edge values");
    }

    status = stats_smooth_median3(single, 1u, out);
    check_true(status == LB_STATUS_OK && out[0] == 42.0, "median3 copies a single sample");

    check_true(stats_smooth_median3(NULL, 1u, out) == LB_STATUS_INVALID_ARG,
               "median3 rejects a NULL input");
    check_true(stats_smooth_median3(ramp, 5u, NULL) == LB_STATUS_INVALID_ARG,
               "median3 rejects a NULL output");
    check_true(stats_smooth_median3(NULL, 0u, NULL) == LB_STATUS_OK,
               "median3 accepts an empty input");
}

static void test_detect_three_levels(void)
{
    enum { POINT_COUNT = 16 };
    size_t x[POINT_COUNT] = {0u};
    double latency[POINT_COUNT] = {0.0};
    stats_steps_t steps = {0};
    size_t i = 0u;
    lb_status_t status = LB_STATUS_ERROR;

    build_log_x(x, POINT_COUNT, 12u);
    for (i = 0u; i < POINT_COUNT; i++) {
        if (x[i] <= 32768u) {
            latency[i] = 2.0;
        } else if (x[i] <= 8388608u) {
            latency[i] = 8.0;
        } else {
            latency[i] = 30.0;
        }
    }

    status = stats_detect_steps(x, latency, POINT_COUNT, &steps);
    check_true(status == LB_STATUS_OK, "three-level detection succeeds");
    check_true(steps.count == 2u, "three-level curve reports two steps");

    if (steps.count == 2u) {
        check_true(steps.items[0].x_lower == 32768u && steps.items[0].x_upper == 65536u,
                   "L1/L2 step lands on the 32-64 KiB boundary");
        check_true(steps.items[0].x == 32768u,
                   "L1/L2 capacity reports the last lower plateau point");
        check_true(steps.items[0].x >= 32768u && steps.items[0].x <= 65536u,
                   "L1/L2 threshold estimate is inside the boundary interval");
        check_true(approx(steps.items[0].lower_ns, 2.0) && approx(steps.items[0].upper_ns, 8.0),
                   "L1/L2 plateau latencies are recovered");
        check_true(approx(steps.items[0].ratio, 4.0), "L1/L2 ratio is four");

        check_true(steps.items[1].x_lower == 8388608u && steps.items[1].x_upper == 16777216u,
                   "L2/RAM step lands on the 8-16 MiB boundary");
        check_true(steps.items[1].x == 8388608u,
                   "L2/RAM capacity reports the last lower plateau point");
        check_true(steps.items[1].x >= 8388608u && steps.items[1].x <= 16777216u,
                   "L2/RAM threshold estimate is inside the boundary interval");
        check_true(approx(steps.items[1].lower_ns, 8.0) && approx(steps.items[1].upper_ns, 30.0),
                   "L2/RAM plateau latencies are recovered");
        check_true(approx(steps.items[1].ratio, 3.75), "L2/RAM ratio is 3.75");
    }
}

static void test_detect_tlb_edge(void)
{
    enum { POINT_COUNT = 7 };
    const size_t x[POINT_COUNT] = {131072u,  262144u,  524288u, 1048576u,
                                   2097152u, 4194304u, 8388608u};
    const double latency[POINT_COUNT] = {0.82, 0.82, 2.23, 2.37, 4.50, 4.48, 6.88};
    stats_steps_t steps = {0};

    check_true(stats_detect_steps(x, latency, POINT_COUNT, &steps) == LB_STATUS_OK,
               "tlb edge detection succeeds");
    check_true(steps.count == 3u, "tlb edge curve reports all three jumps");
    if (steps.count >= 1u) {
        check_true(steps.items[0].x_lower == 262144u && steps.items[0].x_upper == 524288u,
                   "tlb edge keeps the first 256-512 KiB boundary");
        check_true(steps.items[0].x == 262144u,
                   "tlb edge reports the last lower plateau page count");
        check_true(steps.items[0].ratio > 2.5, "tlb edge keeps the first sharp 2.7x jump");
    }
    if (steps.count >= 2u) {
        check_true(steps.items[1].x_lower == 1048576u && steps.items[1].x_upper == 2097152u,
                   "tlb edge places the second step at the 1-2 MiB boundary");
    }
    if (steps.count >= 3u) {
        check_true(steps.items[2].x_lower == 4194304u && steps.items[2].x_upper == 8388608u,
                   "tlb edge keeps the final 4-8 MiB boundary");
    }
}

static void test_detect_flat_with_ram(void)
{
    enum { POINT_COUNT = 16 };
    size_t x[POINT_COUNT] = {0u};
    double latency[POINT_COUNT] = {0.0};
    stats_steps_t steps = {0};
    size_t i = 0u;

    build_log_x(x, POINT_COUNT, 12u);
    for (i = 0u; i < POINT_COUNT; i++) {
        latency[i] = (x[i] <= 4194304u) ? 2.0 : 25.0;
    }

    check_true(stats_detect_steps(x, latency, POINT_COUNT, &steps) == LB_STATUS_OK,
               "flat L1/RAM detection succeeds");
    check_true(steps.count == 1u, "flat L1/RAM curve reports only the RAM step");
    if (steps.count == 1u) {
        check_true(steps.items[0].x_lower == 4194304u && steps.items[0].x_upper == 8388608u,
                   "flat L1/RAM threshold lands on the 4-8 MiB boundary");
        check_true(steps.items[0].x >= 4194304u && steps.items[0].x <= 8388608u,
                   "flat L1/RAM threshold estimate is inside the boundary interval");
    }
}

static void test_detect_two_level_dense_grid(void)
{
    enum { POINT_COUNT = 30 };
    const size_t kib[POINT_COUNT] = {
        1u,  2u,  4u,  8u,   12u,  16u,  20u,  24u,  28u,  32u,  36u,  40u,  48u,  56u,  64u,
        72u, 80u, 96u, 112u, 128u, 160u, 192u, 224u, 256u, 320u, 384u, 448u, 512u, 640u, 768u,
    };
    size_t x[POINT_COUNT] = {0u};
    double latency[POINT_COUNT] = {0.0};
    stats_steps_t steps = {0};
    size_t i = 0u;

    for (i = 0u; i < POINT_COUNT; i++) {
        x[i] = kib[i] * 1024u;
        if (kib[i] <= 32u) {
            latency[i] = 3.0;
        } else if (kib[i] <= 128u) {
            latency[i] = 11.0;
        } else {
            /* Dense gradual L2-to-RAM tail that used to split into a third level. */
            latency[i] = 11.0 + (180.0 * (double)(i - 19u) / (double)(POINT_COUNT - 19u));
        }
    }

    check_true(stats_detect_steps(x, latency, POINT_COUNT, &steps) == LB_STATUS_OK,
               "dense two-level detection succeeds");
    check_true(steps.count == 2u, "dense two-level curve does not split into three levels");
    if (steps.count >= 1u) {
        check_true(steps.items[0].x == 32768u, "dense two-level L1 capacity is exactly 32 KiB");
    }
    if (steps.count >= 2u) {
        check_true(steps.items[1].x == 131072u, "dense two-level L2 capacity is exactly 128 KiB");
    }
}

static void test_detect_smooth_slope(void)
{
    enum { POINT_COUNT = 16 };
    const double ramp[POINT_COUNT] = {10.0, 10.0, 10.0, 10.0, 10.0,  10.0,  10.0,  10.0,
                                      20.0, 30.0, 45.0, 68.0, 100.0, 100.0, 100.0, 100.0};
    size_t x[POINT_COUNT] = {0u};
    double latency[POINT_COUNT] = {0.0};
    stats_steps_t steps = {0};
    size_t i = 0u;

    for (i = 0u; i < POINT_COUNT; i++) {
        x[i] = (i + 1u) * 4096u;
        latency[i] = ramp[i];
    }

    check_true(stats_detect_steps(x, latency, POINT_COUNT, &steps) == LB_STATUS_OK,
               "smooth slope detection succeeds");
    check_true(steps.count == 1u, "smooth slope collapses into one step");
    if (steps.count == 1u) {
        check_true(steps.items[0].x_lower == x[7] && steps.items[0].x_upper == x[11],
                   "smooth slope spans the whole ramp");
        check_true(steps.items[0].x == x[7], "smooth slope reports the last pre-ramp point");
        check_true(approx(steps.items[0].lower_ns, 10.0) && approx(steps.items[0].upper_ns, 100.0),
                   "smooth slope keeps the outer plateaus");
        check_true(approx(steps.items[0].ratio, 10.0), "smooth slope ratio covers the ramp");
    }
}

static void test_detect_noisy_gradual(void)
{
    enum { POINT_COUNT = 24 };
    size_t x[POINT_COUNT] = {0u};
    double latency[POINT_COUNT] = {0.0};
    stats_steps_t steps = {0};
    size_t i = 0u;

    for (i = 0u; i < POINT_COUNT; i++) {
        const size_t ramp_start = 8u;
        const size_t ramp_end = 16u;
        double value = 10.0;
        double noise = 0.0;

        x[i] = (i + 1u) * 4096u;
        if (i >= ramp_start && i <= ramp_end) {
            value += 20.0 * (double)(i - ramp_start) / (double)(ramp_end - ramp_start);
        } else if (i > ramp_end) {
            value = 30.0;
        }
        noise = ((double)((i * 7u) % 5u) - 2.0) * 0.4;
        latency[i] = value + noise;
    }

    check_true(stats_detect_steps(x, latency, POINT_COUNT, &steps) == LB_STATUS_OK,
               "noisy gradual detection succeeds");
    check_true(steps.count == 1u, "noisy gradual curve reports one transition");
    if (steps.count == 1u) {
        check_true(steps.items[0].x >= x[8] && steps.items[0].x <= x[16],
                   "noisy gradual threshold lands inside the ramp");
        check_true(steps.items[0].upper_ns > steps.items[0].lower_ns * 1.30,
                   "noisy gradual transition is significant");
    }
}

static void test_detect_flat_plateau(void)
{
    enum { POINT_COUNT = 24 };
    size_t x[POINT_COUNT] = {0u};
    double latency[POINT_COUNT] = {0.0};
    double noisy[POINT_COUNT] = {0.0};
    stats_steps_t steps = {0};
    size_t i = 0u;

    for (i = 0u; i < POINT_COUNT; i++) {
        x[i] = (i + 1u) * 4096u;
        latency[i] = 7.0;
        noisy[i] = 7.0 + (((double)((i * 3u) % 7u) - 3.0) * 0.01);
    }

    check_true(stats_detect_steps(x, latency, POINT_COUNT, &steps) == LB_STATUS_OK,
               "flat plateau detection succeeds");
    check_true(steps.count == 0u, "exactly flat plateau reports no steps");

    check_true(stats_detect_steps(x, noisy, POINT_COUNT, &steps) == LB_STATUS_OK,
               "slightly noisy plateau detection succeeds");
    check_true(steps.count == 0u, "slightly noisy plateau reports no steps");
}

static void test_detect_validation(void)
{
    const size_t x[4] = {1u, 2u, 2u, 4u};
    const size_t sorted_x[4] = {1u, 2u, 3u, 4u};
    const double latency[4] = {1.0, 2.0, 3.0, 4.0};
    stats_steps_t steps = {0};

    check_true(stats_detect_steps(NULL, latency, 4u, &steps) == LB_STATUS_INVALID_ARG,
               "detector rejects NULL x");
    check_true(stats_detect_steps(sorted_x, NULL, 4u, &steps) == LB_STATUS_INVALID_ARG,
               "detector rejects NULL latency");
    check_true(stats_detect_steps(sorted_x, latency, 4u, NULL) == LB_STATUS_INVALID_ARG,
               "detector rejects NULL output");
    check_true(stats_detect_steps(x, latency, 4u, &steps) == LB_STATUS_INVALID_ARG,
               "detector rejects non-increasing x");
    check_true(stats_detect_steps(NULL, NULL, 0u, &steps) == LB_STATUS_OK,
               "detector accepts an empty curve");
}

static void test_series(void)
{
    const double quiet[3] = {4.0, 4.1, 3.9};
    const double busy[3] = {40.0, 41.0, 39.0};
    size_t x[8] = {0u};
    stats_series_t *series = NULL;
    stats_steps_t steps = {0};
    size_t i = 0u;

    series = stats_series_create("cache-sweep", 2u);
    check_true(series != NULL, "stats_series_create succeeds");
    check_true(stats_series_count(series) == 0u, "new series is empty");
    check_true(strcmp(stats_series_label(series), "cache-sweep") == 0, "series keeps its label");
    check_true(strcmp(stats_series_label(NULL), "") == 0, "NULL series has an empty label");

    for (i = 0u; i < 8u; i++) {
        x[i] = (i + 1u) * 4096u;
        check_true(stats_series_push(series, x[i], (i < 4u) ? quiet : busy, 3u) == LB_STATUS_OK,
                   "stats_series_push succeeds");
    }
    check_true(stats_series_count(series) == 8u, "series grows beyond its initial capacity");

    check_true(stats_series_detect_steps(series, &steps) == LB_STATUS_OK,
               "series step detection succeeds");
    check_true(steps.count == 1u, "series step detection finds the transition");
    if (steps.count == 1u) {
        check_true(steps.items[0].x_lower == 16384u && steps.items[0].x_upper == 20480u,
                   "series threshold lands between the last quiet and first busy point");
        check_true(steps.items[0].x == 16384u, "series reports the last quiet point as capacity");
    }

    check_true(stats_series_push(NULL, 1u, quiet, 3u) == LB_STATUS_INVALID_ARG,
               "push rejects a NULL series");
    check_true(stats_series_push(series, 1u, NULL, 3u) == LB_STATUS_INVALID_ARG,
               "push rejects NULL samples");
    check_true(stats_series_push(series, 1u, quiet, 0u) == LB_STATUS_INVALID_ARG,
               "push rejects an empty sample batch");
    check_true(stats_series_detect_steps(NULL, &steps) == LB_STATUS_INVALID_ARG,
               "series detection rejects a NULL series");
    check_true(stats_series_detect_steps(series, NULL) == LB_STATUS_INVALID_ARG,
               "series detection rejects a NULL output");

    stats_series_free(series);
    stats_series_free(NULL);
    check_true(stats_series_count(NULL) == 0u, "NULL series has zero points");
}

static void test_series_summary(void)
{
    const double samples[5] = {5.0, 1.0, 3.0, 2.0, 4.0};
    stats_series_t *series = NULL;
    stats_point_summary_t summary = {0};

    series = stats_series_create("summary", 1u);
    check_true(series != NULL, "summary series is created");
    if (series == NULL) {
        return;
    }

    check_true(stats_series_push(series, 4096u, samples, 5u) == LB_STATUS_OK,
               "summary series accepts a point");
    check_true(stats_series_summary(series, 0u, &summary) == LB_STATUS_OK,
               "stats_series_summary succeeds");
    check_true(summary.x == 4096u, "summary keeps the working-set size");
    check_true(summary.sample_count == 5u, "summary keeps the sample count");
    check_true(approx(summary.min_ns, 1.0), "summary reports the minimum");
    check_true(approx(summary.p10_ns, 1.0), "summary reports p10");
    check_true(approx(summary.p50_ns, 3.0), "summary reports the median");
    check_true(approx(summary.p90_ns, 5.0), "summary reports p90");
    check_true(approx(summary.p99_ns, 5.0), "summary reports p99");

    check_true(stats_series_summary(NULL, 0u, &summary) == LB_STATUS_INVALID_ARG,
               "summary rejects a NULL series");
    check_true(stats_series_summary(series, 0u, NULL) == LB_STATUS_INVALID_ARG,
               "summary rejects a NULL output");
    check_true(stats_series_summary(series, 1u, &summary) == LB_STATUS_INVALID_ARG,
               "summary rejects an out-of-range index");

    stats_series_free(series);
}

static void test_canary(void)
{
    check_true(approx(stats_canary_drift_pct(1000u, 900u), -10.0),
               "canary drift reports a ten percent slowdown");
    check_true(approx(stats_canary_drift_pct(1000u, 1100u), 10.0),
               "canary drift reports a ten percent speedup");
    check_true(approx(stats_canary_drift_pct(1000u, 1000u), 0.0),
               "canary drift is zero for equal throughput");
    check_true(approx(stats_canary_drift_pct(0u, 1000u), 0.0),
               "canary drift is zero without a baseline");
    check_true(stats_canary_measure(0u) == 0u, "zero-duration canary returns zero");
    check_true(stats_canary_measure(1u) > 0u, "one-millisecond canary completes work");
    check_true(stats_canary_baseline(0u) == 0u, "zero-duration warmed baseline returns zero");
    check_true(stats_canary_baseline(1u) > 0u, "warmed baseline completes work");
}

int main(void)
{
    test_percentile();
    test_smooth_median3();
    test_detect_three_levels();
    test_detect_tlb_edge();
    test_detect_flat_with_ram();
    test_detect_two_level_dense_grid();
    test_detect_smooth_slope();
    test_detect_noisy_gradual();
    test_detect_flat_plateau();
    test_detect_validation();
    test_series();
    test_series_summary();
    test_canary();

    printf("test_stats: %d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
