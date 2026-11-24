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

/* Print an aligned "key  value" line for the passport or info sections. */
void tty_kv(const lb_cfg_t *cfg, const char *key, const char *fmt, ...) PAL_PRINTF_FORMAT(3, 4);

/* Emit one CSV record to stdout. No output is produced outside CSV mode. */
void tty_csv_row(const lb_cfg_t *cfg, const char *fmt, ...) PAL_PRINTF_FORMAT(2, 3);

/* Progress line. Goes to stderr in CSV mode and to stdout otherwise. */
void tty_progress(const lb_cfg_t *cfg, const char *fmt, ...) PAL_PRINTF_FORMAT(2, 3);

/* Warnings and errors always go to stderr with a stable prefix. */
void tty_warn(const char *fmt, ...) PAL_PRINTF_FORMAT(1, 2);
void tty_err(const char *fmt, ...) PAL_PRINTF_FORMAT(1, 2);

/*
 * Warn when a DVFS canary drift exceeds ten percent in either direction.
 * Drift within the tolerance, or a non-finite value, produces no output.
 */
void tty_warn_drift(double drift_pct);

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

/* Render a byte count as a binary unit, for example "27.08 GiB". */
const char *tty_format_bytes(uint64_t bytes, char *out, size_t capacity);

/* Render a duration in the most readable unit, for example "2.48 ms". */
const char *tty_format_ns(uint64_t ns, char *out, size_t capacity);

#endif /* LB_TTY_H */
