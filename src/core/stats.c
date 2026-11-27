/* SPDX-License-Identifier: GPL-3.0 */

#include "stats.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pal/compiler.h"
#include "pal/pal.h"

/* Points allocated when stats_series_create() receives capacity zero. */
#define STATS_SERIES_DEFAULT_CAPACITY 8u

/* Maximum number of values used by the small median helper. */
#define STATS_SMALL_WINDOW 3u

/* Neighbourhood size used on each side of a candidate step. */
#define STATS_STEP_WINDOW 3u

/* A jump must multiply latency by at least this factor to count as a step. */
#define STATS_STEP_RATIO_MIN 1.30

/* Absolute guard against reporting a step in a near-zero or flat curve. */
#define STATS_STEP_MIN_JUMP_NS 0.25

/* Maximum max/min spread allowed on the upper plateau. */
#define STATS_STEP_PLATEAU_SPREAD_MAX 1.50

/*
 * The third upper point must not collapse far below the first one. This rejects
 * a two-point noise spike at the end of a sweep without demanding a perfectly
 * flat plateau when the next cache transition is nearby.
 */
#define STATS_STEP_SUSTAIN_RATIO 0.70

/*
 * A later step within this many points of the previous one, and with a small
 * working-set ratio, belongs to the same smooth slope and must not be reported
 * as a new cache level. The window is wide enough to cover the dense
 * hardware-aligned grid around a small L2, where a gradual L2-to-RAM slope used
 * to split a two-level cache into three levels.
 */
#define STATS_STEP_MERGE_POINTS 6u
#define STATS_STEP_MERGE_RATIO 3.00

/* Dependent ALU chain iterations between clock reads in the canary. */
#define STATS_CANARY_CHUNK 4096u

/*
 * Adaptive canary warm-up. Each slice is short enough to follow the P-state
 * ramp, and the loop stops once two consecutive slices agree within three
 * percent so a laptop or amd_pstate core has reached its working Turbo state
 * before the baseline is taken.
 */
#define STATS_CANARY_WARMUP_MS 25u
#define STATS_CANARY_WARMUP_MAX_MS 250u
#define STATS_CANARY_WARMUP_STABLE_PCT 3.0

/*
 * Some amd_pstate/EPP laptops stay in a low idle P-state for well over a
 * second even under a continuous ALU load, then jump to their working Turbo
 * frequency. Pay that platform delay once per process so the first probe is
 * not measured at the idle clock; later probes settle with the adaptive loop.
 */
#define STATS_CANARY_PRIME_MS 1600u

/* Tests and tiny probes only need a single short slice before the baseline. */
#define STATS_CANARY_SHORT_MS 5u
#define STATS_CANARY_SHORT_WARMUP_MS 25u

struct stats_point_t {
    size_t x;
    double *samples;
    size_t count;
};

struct stats_series_t {
    char label[LB_NAME_MAX];
    struct stats_point_t *points;
    size_t count;
    size_t capacity;
};

static int stats_compare_double(const void *left, const void *right)
{
    const double a = *(const double *)left;
    const double b = *(const double *)right;

    if (a < b) {
        return -1;
    }
    if (a > b) {
        return 1;
    }
    return 0;
}

static void stats_sort_small(double *values, size_t count)
{
    size_t i = 0u;

    for (i = 1u; i < count; i++) {
        const double value = values[i];
        size_t j = i;

        while (j > 0u && values[j - 1u] > value) {
            values[j] = values[j - 1u];
            j--;
        }
        values[j] = value;
    }
}

static double stats_median_small(const double *values, size_t count)
{
    double copy[STATS_SMALL_WINDOW] = {0.0};
    size_t i = 0u;

    if (values == NULL || count == 0u || count > STATS_SMALL_WINDOW) {
        return 0.0;
    }

    for (i = 0u; i < count; i++) {
        copy[i] = values[i];
    }
    stats_sort_small(copy, count);

    if ((count % 2u) == 0u) {
        return 0.5 * (copy[count / 2u - 1u] + copy[count / 2u]);
    }
    return copy[count / 2u];
}

static bool stats_plateau_stable(const double *values, size_t count)
{
    double min_value = 0.0;
    double max_value = 0.0;
    size_t i = 0u;

    if (values == NULL || count == 0u) {
        return false;
    }

    min_value = values[0];
    max_value = values[0];
    for (i = 1u; i < count; i++) {
        if (values[i] < min_value) {
            min_value = values[i];
        }
        if (values[i] > max_value) {
            max_value = values[i];
        }
    }

    if (!(min_value > 0.0)) {
        return false;
    }
    return (max_value / min_value) <= STATS_STEP_PLATEAU_SPREAD_MAX;
}

static void stats_store_step(stats_step_t *step, const size_t *x, size_t fit_index,
                             size_t edge_index, double lower, double upper)
{
    const size_t x_lower = x[fit_index - 1u];
    const size_t x_upper = x[edge_index];

    step->x_lower = x_lower;
    step->x_upper = x_upper;
    /*
     * Report the last point of the lower plateau (X_fit) instead of the middle
     * of the boundary interval. The plateau end is the usable capacity of the
     * level; the first point beyond it has already paid the jump.
     */
    step->x = x_lower;
    step->lower_ns = lower;
    step->upper_ns = upper;
    step->ratio = upper / lower;
}

static bool stats_same_direction(double prev_lower, double prev_upper, double lower, double upper)
{
    return ((prev_upper >= prev_lower) == (upper >= lower));
}

/*
 * A stable intermediate plateau means two sharp transitions belong to separate
 * cache or TLB levels even when they are close on the grid. A run of one or two
 * points is part of a single transition, not a plateau.
 */
static bool stats_plateau_between(const double *values, size_t begin, size_t end)
{
    if (values == NULL || end <= begin || (end - begin) < STATS_STEP_WINDOW) {
        return false;
    }
    return stats_plateau_stable(values + begin, end - begin);
}

/*
 * Append a detected step, or extend the previous one when both describe the
 * same smooth slope. Steps arrive in increasing x order, so the last entry is
 * always the nearest candidate on the low side. `fit` is the last point of the
 * lower plateau (X_fit) and `edge` is the first point above the jump; they
 * differ when a transition spans two adjacent grid points.
 */
static void stats_add_step(stats_steps_t *steps, size_t *boundaries, size_t *edges, const size_t *x,
                           const double *smoothed, size_t fit, size_t edge, double lower,
                           double upper, bool is_gradual)
{
    stats_step_t *prev = NULL;

    if (steps->count > 0u) {
        prev = &steps->items[steps->count - 1u];
        if (stats_same_direction(prev->lower_ns, prev->upper_ns, lower, upper)) {
            const size_t prev_boundary = boundaries[steps->count - 1u];
            const size_t prev_edge = edges[steps->count - 1u];
            const size_t gap = (fit > prev_boundary) ? (fit - prev_boundary) : 0u;
            const double x_ratio = (double)x[fit] / (double)x[prev_boundary];
            const bool plateau = stats_plateau_between(smoothed, prev_edge, fit);

            /*
             * A smooth gradual ramp belongs to the same level as the step
             * before it. A sharp step only merges when there is no stable
             * plateau in between: two real levels separated by even a few flat
             * points must stay separate.
             */
            if ((gap <= STATS_STEP_MERGE_POINTS) && (x_ratio < STATS_STEP_MERGE_RATIO) &&
                (is_gradual || !plateau)) {
                prev->x_upper = x[edge];
                prev->x = prev->x_lower;
                prev->upper_ns = upper;
                prev->ratio = prev->upper_ns / prev->lower_ns;
                edges[steps->count - 1u] = edge;
                return;
            }
        }
    }

    if (steps->count >= STATS_MAX_STEPS) {
        return;
    }
    stats_store_step(&steps->items[steps->count], x, fit, edge, lower, upper);
    boundaries[steps->count] = fit;
    edges[steps->count] = edge;
    steps->count++;
}

stats_series_t *stats_series_create(const char *label, size_t capacity)
{
    stats_series_t *series = NULL;
    size_t effective_capacity = capacity;

    if (effective_capacity == 0u) {
        effective_capacity = STATS_SERIES_DEFAULT_CAPACITY;
    }
    if (effective_capacity > SIZE_MAX / sizeof(struct stats_point_t)) {
        return NULL;
    }

    series = malloc(sizeof(*series));
    if (series == NULL) {
        return NULL;
    }

    series->points = malloc(effective_capacity * sizeof(*series->points));
    if (series->points == NULL) {
        free(series);
        return NULL;
    }

    series->label[0] = '\0';
    if (label != NULL) {
        (void)snprintf(series->label, sizeof(series->label), "%s", label);
    }
    series->count = 0u;
    series->capacity = effective_capacity;
    return series;
}

lb_status_t stats_series_push(stats_series_t *series, size_t x, const double *samples, size_t count)
{
    double *copy = NULL;
    struct stats_point_t *grown = NULL;
    lb_status_t status = LB_STATUS_NOMEM;
    size_t new_capacity = 0u;

    if (series == NULL || samples == NULL || count == 0u) {
        return LB_STATUS_INVALID_ARG;
    }
    if (count > SIZE_MAX / sizeof(*copy)) {
        return LB_STATUS_NOMEM;
    }

    copy = malloc(count * sizeof(*copy));
    if (copy == NULL) {
        goto cleanup;
    }
    (void)memcpy(copy, samples, count * sizeof(*copy));

    if (series->count == series->capacity) {
        if (series->capacity > SIZE_MAX / 2u) {
            goto cleanup;
        }
        new_capacity = series->capacity * 2u;
        if (new_capacity > SIZE_MAX / sizeof(*grown)) {
            goto cleanup;
        }
        grown = realloc(series->points, new_capacity * sizeof(*grown));
        if (grown == NULL) {
            goto cleanup;
        }
        series->points = grown;
        series->capacity = new_capacity;
    }

    series->points[series->count].x = x;
    series->points[series->count].samples = copy;
    series->points[series->count].count = count;
    series->count++;
    copy = NULL;
    status = LB_STATUS_OK;

cleanup:
    free(copy);
    return status;
}

const char *stats_series_label(const stats_series_t *series)
{
    if (series == NULL) {
        return "";
    }
    return series->label;
}

size_t stats_series_count(const stats_series_t *series)
{
    if (series == NULL) {
        return 0u;
    }
    return series->count;
}

lb_status_t stats_series_detect_steps(const stats_series_t *series, stats_steps_t *steps)
{
    size_t *x = NULL;
    double *latency = NULL;
    size_t i = 0u;
    lb_status_t status = LB_STATUS_NOMEM;

    if (series == NULL || steps == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    (void)memset(steps, 0, sizeof(*steps));
    if (series->count < 4u) {
        return LB_STATUS_OK;
    }
    if (series->count > SIZE_MAX / sizeof(*x) || series->count > SIZE_MAX / sizeof(*latency)) {
        return LB_STATUS_NOMEM;
    }

    x = malloc(series->count * sizeof(*x));
    latency = malloc(series->count * sizeof(*latency));
    if (x == NULL || latency == NULL) {
        goto cleanup;
    }

    for (i = 0u; i < series->count; i++) {
        x[i] = series->points[i].x;
        latency[i] = stats_percentile(series->points[i].samples, series->points[i].count,
                                      STATS_NOISE_PERCENTILE);
    }

    status = stats_detect_steps(x, latency, series->count, steps);

cleanup:
    free(latency);
    free(x);
    return status;
}

void stats_series_free(stats_series_t *series)
{
    size_t i = 0u;

    if (series == NULL) {
        return;
    }

    for (i = 0u; i < series->count; i++) {
        free(series->points[i].samples);
    }
    free(series->points);
    free(series);
}

static double stats_percentile_sorted(const double *sorted, size_t count, double pct)
{
    double clamped = pct;

    if (sorted == NULL || count == 0u) {
        return 0.0;
    }

    /* The negated comparison also maps NaN to the low end. */
    if (!(clamped >= 0.0)) {
        clamped = 0.0;
    }
    if (clamped > 100.0) {
        clamped = 100.0;
    }

    if (clamped <= 0.0) {
        return sorted[0];
    }
    if (clamped >= 100.0) {
        return sorted[count - 1u];
    }

    {
        const double rank_value = (clamped / 100.0) * (double)count;
        size_t rank = (size_t)rank_value;
        size_t index = 0u;

        if ((double)rank < rank_value) {
            rank++;
        }
        index = (rank == 0u) ? 0u : rank - 1u;
        if (index >= count) {
            index = count - 1u;
        }
        return sorted[index];
    }
}

double stats_percentile(const double *samples, size_t count, double pct)
{
    double *sorted = NULL;
    double result = 0.0;

    if (samples == NULL || count == 0u) {
        return 0.0;
    }
    if (count > SIZE_MAX / sizeof(*sorted)) {
        return 0.0;
    }

    sorted = malloc(count * sizeof(*sorted));
    if (sorted == NULL) {
        return 0.0;
    }
    (void)memcpy(sorted, samples, count * sizeof(*sorted));
    qsort(sorted, count, sizeof(*sorted), stats_compare_double);

    result = stats_percentile_sorted(sorted, count, pct);
    free(sorted);
    return result;
}

lb_status_t stats_series_summary(const stats_series_t *series, size_t index,
                                 stats_point_summary_t *out)
{
    const struct stats_point_t *point = NULL;
    double *sorted = NULL;

    if (series == NULL || out == NULL || index >= series->count) {
        return LB_STATUS_INVALID_ARG;
    }

    point = &series->points[index];
    if (point->count > SIZE_MAX / sizeof(*sorted)) {
        return LB_STATUS_NOMEM;
    }

    sorted = malloc(point->count * sizeof(*sorted));
    if (sorted == NULL) {
        return LB_STATUS_NOMEM;
    }
    (void)memcpy(sorted, point->samples, point->count * sizeof(*sorted));
    qsort(sorted, point->count, sizeof(*sorted), stats_compare_double);

    out->x = point->x;
    out->sample_count = point->count;
    out->min_ns = sorted[0];
    out->p10_ns = stats_percentile_sorted(sorted, point->count, STATS_NOISE_PERCENTILE);
    out->p50_ns = stats_percentile_sorted(sorted, point->count, 50.0);
    out->p90_ns = stats_percentile_sorted(sorted, point->count, 90.0);
    out->p99_ns = stats_percentile_sorted(sorted, point->count, 99.0);

    free(sorted);
    return LB_STATUS_OK;
}

lb_status_t stats_smooth_median3(const double *samples, size_t count, double *out)
{
    size_t i = 0u;

    if (count == 0u) {
        return LB_STATUS_OK;
    }
    if (samples == NULL || out == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    for (i = 0u; i < count; i++) {
        double window[STATS_SMALL_WINDOW] = {0.0};

        if (count == 1u) {
            out[i] = samples[i];
        } else if (i == 0u) {
            window[0] = samples[0];
            window[1] = samples[0];
            window[2] = samples[1];
            out[i] = stats_median_small(window, STATS_SMALL_WINDOW);
        } else if (i == count - 1u) {
            window[0] = samples[count - 2u];
            window[1] = samples[count - 1u];
            window[2] = samples[count - 1u];
            out[i] = stats_median_small(window, STATS_SMALL_WINDOW);
        } else {
            window[0] = samples[i - 1u];
            window[1] = samples[i];
            window[2] = samples[i + 1u];
            out[i] = stats_median_small(window, STATS_SMALL_WINDOW);
        }
    }
    return LB_STATUS_OK;
}

static bool stats_jump_significant(double lower, double upper, double min_jump)
{
    if (!(lower > 0.0)) {
        return false;
    }
    if ((upper / lower) < STATS_STEP_RATIO_MIN) {
        return false;
    }
    return (upper - lower) >= min_jump;
}

/*
 * Median of up to STATS_STEP_WINDOW smoothed points in [begin, end). Callers
 * always pass a non-empty range.
 */
static double stats_window_median(const double *values, size_t begin, size_t end)
{
    size_t count = end - begin;

    if (count > STATS_STEP_WINDOW) {
        count = STATS_STEP_WINDOW;
    }
    return stats_median_small(values + begin, count);
}

/*
 * Measure the plateau immediately below and above a boundary. lower_begin and
 * upper_end delimit the region owned by this step, so a neighbouring step can
 * never leak into either plateau. Each side uses at most STATS_STEP_WINDOW
 * points.
 */
static void stats_measure_plateaus(const double *values, size_t lower_boundary,
                                   size_t upper_boundary, size_t lower_begin, size_t upper_end,
                                   double *lower_out, double *upper_out)
{
    size_t lower_start = lower_begin;
    size_t upper_stop = upper_end;

    if (lower_boundary > lower_begin + STATS_STEP_WINDOW) {
        lower_start = lower_boundary - STATS_STEP_WINDOW;
    }
    if ((upper_boundary + STATS_STEP_WINDOW) < upper_end) {
        upper_stop = upper_boundary + STATS_STEP_WINDOW;
    }

    *lower_out = stats_window_median(values, lower_start, lower_boundary);
    *upper_out = stats_window_median(values, upper_boundary, upper_stop);
}

/*
 * Gradual fallback: look for the strongest smoothed step inside a region that
 * contains no sharp adjacent jump. This keeps a slow monotonic ramp detectable
 * without letting the same ramp produce a cascade of spurious steps.
 */
static bool stats_find_gradual(const double *values, size_t begin, size_t end, double min_jump,
                               size_t *boundary_out)
{
    double best_ratio = 0.0;
    size_t best = 0u;
    size_t i = 0u;
    bool found = false;

    if ((end - begin) < ((size_t)STATS_STEP_WINDOW * 2u)) {
        return false;
    }

    for (i = begin + STATS_STEP_WINDOW; (i + STATS_STEP_WINDOW) <= end; i++) {
        const double lower = stats_window_median(values, i - STATS_STEP_WINDOW, i);
        const double upper = stats_window_median(values, i, i + STATS_STEP_WINDOW);
        double ratio = 0.0;

        if (!stats_jump_significant(lower, upper, min_jump)) {
            continue;
        }
        if (!stats_plateau_stable(values + i, STATS_STEP_WINDOW)) {
            continue;
        }
        /*
         * At the very start of a dense curve the first two points can be
         * noisier than the rest. Require a stable lower window before the
         * earliest gradual boundary so a pair of low points followed by a
         * normal plateau does not look like a real level change.
         */
        if (begin == 0u && i == (size_t)STATS_STEP_WINDOW &&
            !stats_plateau_stable(values + begin, STATS_STEP_WINDOW)) {
            continue;
        }
        ratio = upper / lower;
        if (!found || ratio > best_ratio) {
            best_ratio = ratio;
            best = i;
            found = true;
        }
    }

    if (found) {
        *boundary_out = best;
    }
    return found;
}

lb_status_t stats_detect_steps(const size_t *x, const double *latency_ns, size_t count,
                               stats_steps_t *steps)
{
    double *smoothed = NULL;
    size_t *sharp = NULL;
    size_t *sharp_fit = NULL;
    size_t boundaries[STATS_MAX_STEPS] = {0u};
    size_t edges[STATS_MAX_STEPS] = {0u};
    size_t sharp_count = 0u;
    double min_jump = 0.0;
    size_t i = 0u;
    size_t segment_begin = 0u;
    size_t segment_index = 0u;
    lb_status_t status = LB_STATUS_NOMEM;

    if (steps == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    (void)memset(steps, 0, sizeof(*steps));
    if (count == 0u) {
        return LB_STATUS_OK;
    }
    if (x == NULL || latency_ns == NULL) {
        return LB_STATUS_INVALID_ARG;
    }
    for (i = 1u; i < count; i++) {
        if (x[i] <= x[i - 1u]) {
            return LB_STATUS_INVALID_ARG;
        }
    }
    if (count < 4u) {
        return LB_STATUS_OK;
    }
    if (count > SIZE_MAX / sizeof(*smoothed) || count > SIZE_MAX / sizeof(*sharp) ||
        count > SIZE_MAX / sizeof(*sharp_fit)) {
        return LB_STATUS_NOMEM;
    }

    smoothed = malloc(count * sizeof(*smoothed));
    sharp = malloc(count * sizeof(*sharp));
    sharp_fit = malloc(count * sizeof(*sharp_fit));
    if (smoothed == NULL || sharp == NULL || sharp_fit == NULL) {
        goto cleanup;
    }
    status = stats_smooth_median3(latency_ns, count, smoothed);
    if (status != LB_STATUS_OK) {
        goto cleanup;
    }

    min_jump = stats_percentile(smoothed, count, STATS_NOISE_PERCENTILE) * 0.02;
    if (min_jump < STATS_STEP_MIN_JUMP_NS) {
        min_jump = STATS_STEP_MIN_JUMP_NS;
    }

    /*
     * Sharp steps are found directly between adjacent smoothed points. This
     * preserves the exact plateau edge (the old window-only scan smeared the
     * first jump into the next one). Two adjacent candidates are the same
     * physical transition: keep the first boundary as X_fit so it stays on the
     * lower plateau, and only move the measured upper edge when the second
     * point is stronger.
     *
     * Dense grids need three points on the starting plateau so an outlier at
     * the first sample cannot create a false boundary. Short curves keep the
     * earlier minimum because they have fewer points to spare.
     */
    {
        const size_t min_start = (count >= 12u) ? 3u : 2u;

        for (i = min_start; i < count; i++) {
            if (!stats_jump_significant(smoothed[i - 1u], smoothed[i], min_jump)) {
                continue;
            }
            /*
             * The first transition is also compared against the median of the
             * lower window. A single spike or dip at x[0] or x[1] is ignored by
             * that median, while a real low plateau still clears the ratio.
             */
            if (sharp_count == 0u) {
                const size_t lower_begin =
                    (i > (size_t)STATS_STEP_WINDOW) ? (i - (size_t)STATS_STEP_WINDOW) : 0u;
                const double lower_med = stats_window_median(smoothed, lower_begin, i);

                if (!stats_jump_significant(lower_med, smoothed[i], min_jump)) {
                    continue;
                }
            }
            /*
             * A step needs at least three points on the upper side. A step that
             * only appears in the last one or two grid points is a noisy arena
             * ceiling, not a cache level.
             */
            if ((count - i) < 3u) {
                continue;
            }
            if (smoothed[i + 2u] < (smoothed[i] * STATS_STEP_SUSTAIN_RATIO)) {
                continue;
            }
            if (sharp_count > 0u && i == (sharp[sharp_count - 1u] + 1u)) {
                if ((smoothed[i] / smoothed[i - 1u]) >
                    (smoothed[sharp[sharp_count - 1u]] / smoothed[sharp[sharp_count - 1u] - 1u])) {
                    sharp[sharp_count - 1u] = i;
                }
                continue;
            }
            sharp[sharp_count] = i;
            sharp_fit[sharp_count] = i;
            sharp_count++;
        }
    }

    for (segment_index = 0u; segment_index <= sharp_count; segment_index++) {
        size_t segment_end = (segment_index < sharp_count) ? sharp[segment_index] : count;
        size_t gradual = 0u;

        if (segment_end > segment_begin &&
            stats_find_gradual(smoothed, segment_begin, segment_end, min_jump, &gradual)) {
            double lower = 0.0;
            double upper = 0.0;

            if (steps->count >= STATS_MAX_STEPS) {
                break;
            }
            stats_measure_plateaus(smoothed, gradual, gradual, segment_begin, segment_end, &lower,
                                   &upper);
            stats_add_step(steps, boundaries, edges, x, smoothed, gradual, gradual, lower, upper,
                           true);
        }

        if (segment_index == sharp_count) {
            break;
        }
        if (steps->count >= STATS_MAX_STEPS) {
            break;
        }

        {
            const size_t edge = sharp[segment_index];
            const size_t fit = sharp_fit[segment_index];
            const size_t lower_begin = (segment_index == 0u) ? 0u : sharp[segment_index - 1u];
            const size_t upper_end =
                ((segment_index + 1u) < sharp_count) ? sharp[segment_index + 1u] : count;
            double lower = 0.0;
            double upper = 0.0;

            stats_measure_plateaus(smoothed, fit, edge, lower_begin, upper_end, &lower, &upper);
            stats_add_step(steps, boundaries, edges, x, smoothed, fit, edge, lower, upper, false);
        }
        segment_begin = sharp[segment_index];
    }

    status = LB_STATUS_OK;

cleanup:
    free(sharp_fit);
    free(sharp);
    free(smoothed);
    return status;
}

uint64_t stats_canary_measure(uint32_t duration_ms)
{
    uint64_t state = UINT64_C(0x243f6a8885a308d3);
    uint64_t ops = 0u;
    uint64_t start = 0u;
    uint64_t deadline = 0u;

    if (duration_ms == 0u) {
        return 0u;
    }

    start = pal_now_ns();
    deadline = start + ((uint64_t)duration_ms * UINT64_C(1000000));

    for (;;) {
        uint32_t chunk = 0u;

        for (chunk = 0u; chunk < STATS_CANARY_CHUNK; chunk++) {
            state ^= state >> 12u;
            state ^= state << 25u;
            state ^= state >> 27u;
            state *= UINT64_C(0x2545f4914f6cdd1d);
        }
        ops += (uint64_t)STATS_CANARY_CHUNK;
        PAL_KEEP_INT(state);
        if (pal_now_ns() >= deadline) {
            break;
        }
    }

    PAL_KEEP_INT(state);
    return ops;
}

static bool g_canary_primed = false;

static bool stats_canary_settled(uint64_t previous, uint64_t current)
{
    double earlier = 0.0;
    double later = 0.0;
    double difference = 0.0;

    if (previous == 0u || current == 0u) {
        return false;
    }

    earlier = (double)previous;
    later = (double)current;
    difference = (earlier > later) ? (earlier - later) : (later - earlier);
    return (difference / earlier) <= (STATS_CANARY_WARMUP_STABLE_PCT / 100.0);
}

uint64_t stats_canary_baseline(uint32_t duration_ms)
{
    uint64_t previous = 0u;
    uint32_t elapsed_ms = 0u;

    if (duration_ms == 0u) {
        return 0u;
    }
    if (duration_ms <= STATS_CANARY_SHORT_MS) {
        (void)stats_canary_measure(STATS_CANARY_SHORT_WARMUP_MS);
        return stats_canary_measure(duration_ms);
    }
    if (!g_canary_primed) {
        (void)stats_canary_measure(STATS_CANARY_PRIME_MS);
        g_canary_primed = true;
    }

    while (elapsed_ms < STATS_CANARY_WARMUP_MAX_MS) {
        const uint64_t current = stats_canary_measure(STATS_CANARY_WARMUP_MS);

        elapsed_ms += STATS_CANARY_WARMUP_MS;
        if (previous != 0u && stats_canary_settled(previous, current)) {
            break;
        }
        previous = current;
    }

    return stats_canary_measure(duration_ms);
}

double stats_canary_drift_pct(uint64_t baseline_ops, uint64_t after_ops)
{
    if (baseline_ops == 0u) {
        return 0.0;
    }
    return (((double)after_ops / (double)baseline_ops) - 1.0) * 100.0;
}
