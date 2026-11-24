/* SPDX-License-Identifier: GPL-3.0 */
#ifndef LB_STATS_H
#define LB_STATS_H

/*
 * Statistical helpers for latency series: robust percentiles, median
 * smoothing, cache/TLB step detection, and the DVFS frequency-drift canary.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "defs.h"
#include "types.h"

/*
 * Low percentile used to reject OS scheduling noise before a series point is
 * analysed. The 10th percentile discards the slow tail without hiding a
 * genuine plateau rise.
 */
#define STATS_NOISE_PERCENTILE 10.0

/* Maximum number of thresholds reported by one step-detection pass. */
#define STATS_MAX_STEPS 8u

/* Opaque working-set latency series. */
typedef struct stats_series_t stats_series_t;

/*
 * Read-only summary of one series point. Percentiles are computed over the raw
 * samples of that point, so renderers can show a stable distribution without
 * exposing the internal sample storage.
 */
typedef struct stats_point_summary_t {
    size_t x;            /* working-set size at this point */
    size_t sample_count; /* number of raw samples behind the summary */
    double min_ns;       /* smallest sample */
    double p10_ns;       /* 10th percentile, used by the step detector */
    double p50_ns;       /* median */
    double p90_ns;       /* 90th percentile */
    double p99_ns;       /* 99th percentile */
} stats_point_summary_t;

/*
 * One detected latency step. The threshold is estimated between the last point
 * of the lower plateau and the first point of the upper plateau.
 */
typedef struct stats_step_t {
    size_t x;        /* estimated threshold */
    size_t x_lower;  /* working-set size of the last lower-plateau point */
    size_t x_upper;  /* working-set size of the first upper-plateau point */
    double lower_ns; /* median latency on the lower plateau */
    double upper_ns; /* median latency on the upper plateau */
    double ratio;    /* upper_ns / lower_ns */
} stats_step_t;

/* Fixed-capacity list of detected steps. */
typedef struct stats_steps_t {
    stats_step_t items[STATS_MAX_STEPS];
    size_t count;
} stats_steps_t;

/*
 * Create an empty series with an optional label and an initial point capacity.
 * A capacity of zero selects a small default. Returns NULL on allocation
 * failure; free the result with stats_series_free().
 */
stats_series_t *stats_series_create(const char *label, size_t capacity);

/*
 * Append one point at working-set size x together with count raw samples in
 * nanoseconds. The samples are copied. Pushes should use strictly increasing x
 * values so that step detection can report ordered thresholds. Returns
 * LB_STATUS_INVALID_ARG for NULL inputs or count == 0, and LB_STATUS_NOMEM when
 * growing the series fails.
 */
lb_status_t stats_series_push(stats_series_t *series, size_t x, const double *samples,
                              size_t count);

/* Series label, or an empty string when series is NULL. */
const char *stats_series_label(const stats_series_t *series);

/* Number of points in the series, or zero when series is NULL. */
size_t stats_series_count(const stats_series_t *series);

/*
 * Summarise one point by index. The percentile values use the same rank rule
 * as stats_percentile(). Returns LB_STATUS_INVALID_ARG for NULL inputs or an
 * out-of-range index, and LB_STATUS_NOMEM when the temporary sort buffer cannot
 * be allocated.
 */
lb_status_t stats_series_summary(const stats_series_t *series, size_t index,
                                 stats_point_summary_t *out);

/*
 * Detect latency steps in a series. Each point is reduced to its 10th
 * percentile before smoothing, then passed to stats_detect_steps().
 */
lb_status_t stats_series_detect_steps(const stats_series_t *series, stats_steps_t *steps);

/* Release a series and all raw sample storage. Safe to call with NULL. */
void stats_series_free(stats_series_t *series);

/*
 * Percentile in [0, 100] using a sorted copy of the samples. A percentile
 * outside the range is clamped. Returns zero when samples is NULL, count is
 * zero, or the temporary copy cannot be allocated. The input is not modified.
 */
double stats_percentile(const double *samples, size_t count, double pct);

/*
 * Median smoothing over a three-point window, with edge values replicated.
 * A single outlier is replaced by the median of its neighbours. samples and out
 * must not overlap. count == 0 is a no-op. Returns LB_STATUS_INVALID_ARG when
 * a non-empty input has a NULL pointer.
 */
lb_status_t stats_smooth_median3(const double *samples, size_t count, double *out);

/*
 * Detect significant latency jumps that settle on a new plateau. x must be
 * strictly increasing and latency_ns must contain count finite, positive
 * values. The output list is overwritten with at most STATS_MAX_STEPS entries.
 * A curve with fewer than four points reports no steps.
 */
lb_status_t stats_detect_steps(const size_t *x, const double *latency_ns, size_t count,
                               stats_steps_t *steps);

/*
 * DVFS Guard canary: run a dependent register ALU chain for duration_ms and
 * return the number of chain iterations completed. Returns zero when
 * duration_ms is zero.
 */
uint64_t stats_canary_measure(uint32_t duration_ms);

/*
 * Relative throughput drift between two canary measurements, in percent.
 * A positive result means the second run completed more work. Returns zero
 * when baseline_ops is zero.
 */
double stats_canary_drift_pct(uint64_t baseline_ops, uint64_t after_ops);

#endif /* LB_STATS_H */
