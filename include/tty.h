/* SPDX-License-Identifier: GPL-3.0 */
#ifndef LB_TTY_H
#define LB_TTY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pal/compiler.h"
#include "stats.h"
#include "types.h"

/*
 * Rendering helpers. In CSV mode every informational, warning, progress, and
 * error message is written to stderr, so stdout carries only clean CSV records.
 * Without CSV mode, headers, key-value lines, and progress go to stdout while
 * warnings and errors stay on stderr.
 */
void tty_header(const lb_cfg_t *cfg, const char *fmt, ...) PAL_PRINTF_FORMAT(2, 3);

/* Disable all ANSI styling, including [WARN] and [ERROR] status prefixes. */
void tty_set_no_color(bool no_color);

/* Print a "Title:" section heading. Blank line between sections. */
void tty_section(const lb_cfg_t *cfg, const char *title);

/*
 * Print one blank line to separate a parameter block from its table or a table
 * from its footer. Suppressed in auto, CSV, and quiet modes so machine-readable
 * output stays untouched.
 */
void tty_blank(const lb_cfg_t *cfg);

/* Print an aligned "key  value" line for the passport or info sections. */
void tty_kv(const lb_cfg_t *cfg, const char *key, const char *fmt, ...) PAL_PRINTF_FORMAT(3, 4);

/*
 * Emit one CSV record to stdout. No output is produced outside CSV mode.
 * Auto mode suppresses probe records because the final passport table is the
 * only CSV schema that belongs on stdout for an "auto --csv" run.
 */
void tty_csv_row(const lb_cfg_t *cfg, const char *fmt, ...) PAL_PRINTF_FORMAT(2, 3);

/*
 * Forget the one-shot CSV headers emitted by tty_plot_series() and
 * tty_print_matrix(). Tests and callers that start an independent CSV stream
 * use this so the next render begins with a fresh header.
 */
void tty_reset_csv_headers(void);

/* Progress line. Goes to stderr in CSV mode and to stdout otherwise. */
void tty_progress(const lb_cfg_t *cfg, const char *fmt, ...) PAL_PRINTF_FORMAT(2, 3);

/* Warnings and errors always go to stderr with a stable prefix. */
void tty_warn(const char *fmt, ...) PAL_PRINTF_FORMAT(1, 2);
void tty_err(const char *fmt, ...) PAL_PRINTF_FORMAT(1, 2);

/*
 * Warn only when a DVFS canary drifts below -10%, which indicates real
 * throttling. Positive drift is a Turbo ramp and is reported silently in the
 * "DVFS drift" line instead. Non-finite values never warn.
 */
void tty_warn_drift(const lb_cfg_t *cfg, double drift_pct);

/*
 * Render one working-set latency series as a compact table plus an ASCII bar.
 * Sizes are byte counts and are scaled to KiB/MiB/GiB. The optional
 * level_names array labels detected thresholds (for example "L1", "L2"); short
 * or missing labels fall back to "step N". When page_size is non-zero, the
 * threshold summary also reports the equivalent page count and the table gains
 * a Pages column.
 *
 * Heading, table, and threshold summary go to stdout, except in CSV mode where
 * stdout receives only the data header and one clean record per point. All
 * informational text then moves to stderr.
 */
lb_status_t tty_plot_series(const lb_cfg_t *cfg, const stats_series_t *series,
                            const stats_steps_t *steps, const char *const *level_names,
                            size_t level_name_count, size_t page_size);

/*
 * Same renderer as tty_plot_series(), but callers that already printed their
 * own section heading can pass print_section = false. The series label is still
 * used for the CSV "test" column.
 */
lb_status_t tty_plot_series_ex(const lb_cfg_t *cfg, const stats_series_t *series,
                               const stats_steps_t *steps, const char *const *level_names,
                               size_t level_name_count, size_t page_size, bool print_section);

/*
 * Render an N x N CPU-to-CPU latency matrix for "topo pingpong". latency_ns is
 * row-major: cell (a, b) is at latency_ns[a * count + b]. Row and column
 * headings are logical CPU ids 0..count-1 and the diagonal is shown as "-".
 *
 * In TTY mode the table is printed with a restrained three-colour heatmap:
 * fast same-cluster/SMT cells are green, medium cells yellow, and slow
 * cross-CCX/NUMA cells red. Headings stay unstyled. In CSV mode stdout carries
 * only "cpu_a,cpu_b,latency_ns" records for the off-diagonal cells.
 */
lb_status_t tty_print_matrix(const lb_cfg_t *cfg, const char *title, const uint64_t *latency_ns,
                             size_t count);

/*
 * Render an N x N CPU-to-CPU latency matrix using the caller's logical CPU ids.
 * cpu_ids supplies the heading for each row and column and must contain count
 * non-negative entries; when it is NULL the renderer falls back to ids
 * 0..count-1. latency_ns uses the same row-major layout as tty_print_matrix().
 * The heatmap thresholds adapt to the fastest off-diagonal latency so slow
 * platforms do not turn the whole table red, while x86 and Apple Silicon keep
 * the fixed 30 ns / 80 ns boundaries.
 *
 * Returns LB_STATUS_INVALID_ARG for a NULL matrix, an empty or oversized table,
 * or a negative entry in cpu_ids.
 */
lb_status_t tty_print_matrix_cpus(const lb_cfg_t *cfg, const char *title, const int32_t *cpu_ids,
                                  const uint64_t *latency_ns, size_t count);

/* Render a byte count as a binary unit, for example "27.08 GiB". */
const char *tty_format_bytes(uint64_t bytes, char *out, size_t capacity);

/* Render a duration in the most readable unit, for example "2.48 ms". */
const char *tty_format_ns(uint64_t ns, char *out, size_t capacity);

#endif /* LB_TTY_H */
