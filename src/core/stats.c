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

/* Candidate steps at most this many points apart belong to the same jump. */
#define STATS_STEP_MERGE_GAP STATS_STEP_WINDOW

/* Dependent ALU chain iterations between clock reads in the canary. */
#define STATS_CANARY_CHUNK 4096u

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

static void stats_store_step(stats_step_t *step, const size_t *x, size_t index, double lower,
                             double upper)
{
    const size_t x_lower = x[index - 1u];
    const size_t x_upper = x[index];

    step->x_lower = x_lower;
    step->x_upper = x_upper;
    step->x = x_lower + (x_upper - x_lower) / 2u;
    step->lower_ns = lower;
    step->upper_ns = upper;
    step->ratio = upper / lower;
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

double stats_percentile(const double *samples, size_t count, double pct)
{
    double *sorted = NULL;
    double clamped = pct;
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

    /* The negated comparison also maps NaN to the low end. */
    if (!(clamped >= 0.0)) {
        clamped = 0.0;
    }
    if (clamped > 100.0) {
        clamped = 100.0;
    }

    if (clamped <= 0.0) {
        result = sorted[0];
    } else if (clamped >= 100.0) {
        result = sorted[count - 1u];
    } else {
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
        result = sorted[index];
    }

    free(sorted);
    return result;
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

lb_status_t stats_detect_steps(const size_t *x, const double *latency_ns, size_t count,
                               stats_steps_t *steps)
{
    double *smoothed = NULL;
    double min_jump = 0.0;
    size_t i = 0u;
    size_t last_index = 0u;
    bool have_last = false;
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
    if (count > SIZE_MAX / sizeof(*smoothed)) {
        return LB_STATUS_NOMEM;
    }

    smoothed = malloc(count * sizeof(*smoothed));
    if (smoothed == NULL) {
        return LB_STATUS_NOMEM;
    }
    status = stats_smooth_median3(latency_ns, count, smoothed);
    if (status != LB_STATUS_OK) {
        goto cleanup;
    }

    min_jump = stats_percentile(smoothed, count, STATS_NOISE_PERCENTILE) * 0.02;
    if (min_jump < STATS_STEP_MIN_JUMP_NS) {
        min_jump = STATS_STEP_MIN_JUMP_NS;
    }

    for (i = 1u; i < count; i++) {
        const size_t left = (i < STATS_STEP_WINDOW) ? i : STATS_STEP_WINDOW;
        const size_t right = ((count - i) < STATS_STEP_WINDOW) ? (count - i) : STATS_STEP_WINDOW;
        const double lower = stats_median_small(smoothed + i - left, left);
        const double upper = stats_median_small(smoothed + i, right);
        double ratio = 0.0;

        if (!(lower > 0.0)) {
            continue;
        }
        ratio = upper / lower;
        if (ratio < STATS_STEP_RATIO_MIN) {
            continue;
        }
        if ((upper - lower) < min_jump) {
            continue;
        }
        if (!stats_plateau_stable(smoothed + i, right)) {
            continue;
        }

        if (have_last && (i - last_index) <= STATS_STEP_MERGE_GAP) {
            stats_step_t *last = &steps->items[steps->count - 1u];

            if (ratio > last->ratio) {
                stats_store_step(last, x, i, lower, upper);
            }
            last_index = i;
            continue;
        }

        if (steps->count >= STATS_MAX_STEPS) {
            break;
        }
        stats_store_step(&steps->items[steps->count], x, i, lower, upper);
        steps->count++;
        last_index = i;
        have_last = true;
    }

    status = LB_STATUS_OK;

cleanup:
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

double stats_canary_drift_pct(uint64_t baseline_ops, uint64_t after_ops)
{
    if (baseline_ops == 0u) {
        return 0.0;
    }
    return (((double)after_ops / (double)baseline_ops) - 1.0) * 100.0;
}
