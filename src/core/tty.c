/* SPDX-License-Identifier: GPL-3.0 */

#include "tty.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pal/pal.h"
#include "types.h"

#define TTY_KEY_WIDTH 20
#define TTY_ANSI_RESET "\x1b[0m"
#define TTY_ANSI_BOLD "\x1b[1m"
#define TTY_ANSI_WARN "\x1b[1;33m"
#define TTY_ANSI_ERROR "\x1b[1;31m"

#define TTY_KIB UINT64_C(1024)
#define TTY_MIB (TTY_KIB * UINT64_C(1024))
#define TTY_GIB (TTY_MIB * UINT64_C(1024))
#define TTY_NS_PER_US UINT64_C(1000)
#define TTY_NS_PER_MS (TTY_NS_PER_US * UINT64_C(1000))
#define TTY_NS_PER_S (TTY_NS_PER_MS * UINT64_C(1000))

/* Keep plots inside the width of a classic 80-column terminal. */
#define TTY_PLOT_WIDTH 76u
#define TTY_PLOT_BAR_WIDTH 20u
#define TTY_PLOT_FIELD_WIDTH 9
#define TTY_PLOT_FLAT_RATIO 1.5
#define TTY_DRIFT_WARN_DROP_PCT 10.0

/* Latency heatmap: seven-character cells keep common topologies readable. */
#define TTY_MATRIX_MAX_SIZE 256u
#define TTY_MATRIX_CELL_WIDTH 7
#define TTY_MATRIX_ROW_WIDTH 3
#define TTY_MATRIX_SEPARATOR "-------"
#define TTY_MATRIX_FAST_NS UINT64_C(30)
#define TTY_MATRIX_SLOW_NS UINT64_C(80)
#define TTY_ANSI_MATRIX_FAST "\x1b[32m"
#define TTY_ANSI_MATRIX_MID "\x1b[33m"
#define TTY_ANSI_MATRIX_SLOW "\x1b[31m"

static bool g_tty_section_started = false;
static bool g_tty_no_color = false;
static bool g_tty_csv_series_header = false;
static bool g_tty_csv_matrix_header = false;

static FILE *tty_info_stream(const lb_cfg_t *cfg)
{
    if (cfg != NULL && (cfg->csv || cfg->auto_mode || cfg->quiet)) {
        return stderr;
    }
    return stdout;
}

/*
 * Auto mode hides the per-probe detail so the final Passport card stays the
 * only thing on stdout. In CSV mode the passport records are written through
 * tty_csv_row(), so suppressing the human-readable tables here keeps stderr
 * limited to progress and diagnostics. Quiet mode suppresses informational
 * output entirely.
 */
static bool tty_info_suppressed(const lb_cfg_t *cfg)
{
    if (cfg == NULL) {
        return false;
    }
    if (cfg->quiet) {
        return true;
    }
    return cfg->auto_mode;
}

/*
 * Auto mode owns the final machine-readable result, so probe plots and matrices
 * are always suppressed there, including in CSV mode. Otherwise an auto --csv
 * run would mix the per-probe CSV schemas with the passport records.
 */
static bool tty_plot_suppressed(const lb_cfg_t *cfg)
{
    if (cfg == NULL) {
        return false;
    }
    if (cfg->quiet) {
        return true;
    }
    return cfg->auto_mode;
}

static bool tty_color_enabled(const lb_cfg_t *cfg, const FILE *stream)
{
    if (stream == NULL) {
        return false;
    }
    if (cfg != NULL && (cfg->csv || cfg->no_color)) {
        return false;
    }
    if (g_tty_no_color) {
        return false;
    }
    if (getenv("NO_COLOR") != NULL) {
        return false;
    }
    return pal_is_tty(stream == stderr);
}

static void tty_vline(FILE *stream, const char *prefix, const char *fmt, va_list args)
    PAL_PRINTF_FORMAT(3, 0);

static void tty_vline(FILE *stream, const char *prefix, const char *fmt, va_list args)
{
    if (fmt == NULL) {
        return;
    }

    if (prefix != NULL) {
        (void)fputs(prefix, stream);
    }
    (void)vfprintf(stream, fmt, args);
    (void)fputc('\n', stream);
}

void tty_set_no_color(bool no_color)
{
    g_tty_no_color = no_color;
}

void tty_reset_csv_headers(void)
{
    g_tty_csv_series_header = false;
    g_tty_csv_matrix_header = false;
}

void tty_header(const lb_cfg_t *cfg, const char *fmt, ...)
{
    va_list args;
    FILE *stream = NULL;

    if (fmt == NULL || tty_info_suppressed(cfg)) {
        return;
    }

    stream = tty_info_stream(cfg);
    va_start(args, fmt);
    tty_vline(stream, NULL, fmt, args);
    va_end(args);
}

void tty_blank(const lb_cfg_t *cfg)
{
    FILE *stream = NULL;

    if (cfg == NULL || cfg->auto_mode || cfg->csv || cfg->quiet) {
        return;
    }

    stream = tty_info_stream(cfg);
    (void)fputc('\n', stream);
}

void tty_section(const lb_cfg_t *cfg, const char *title)
{
    FILE *stream = NULL;
    bool color = false;

    if (title == NULL || tty_info_suppressed(cfg)) {
        return;
    }

    stream = tty_info_stream(cfg);
    color = tty_color_enabled(cfg, stream);

    if (g_tty_section_started) {
        (void)fputc('\n', stream);
    }
    g_tty_section_started = true;

    if (color) {
        (void)fprintf(stream, "%s%s:%s\n", TTY_ANSI_BOLD, title, TTY_ANSI_RESET);
    } else {
        (void)fprintf(stream, "%s:\n", title);
    }
}

void tty_kv(const lb_cfg_t *cfg, const char *key, const char *fmt, ...)
{
    va_list args;
    FILE *stream = NULL;

    if (key == NULL || fmt == NULL || tty_info_suppressed(cfg)) {
        return;
    }

    stream = tty_info_stream(cfg);
    (void)fprintf(stream, "  %-*s ", TTY_KEY_WIDTH, key);

    va_start(args, fmt);
    tty_vline(stream, NULL, fmt, args);
    va_end(args);
}

void tty_csv_row(const lb_cfg_t *cfg, const char *fmt, ...)
{
    va_list args;

    /*
     * Auto mode publishes its own single-schema passport table through a render
     * copy with auto_mode cleared. Suppressing every probe row here keeps the
     * auto --csv stream free of the per-probe headers and column layouts.
     */
    if (cfg == NULL || !cfg->csv || cfg->auto_mode || fmt == NULL) {
        return;
    }

    va_start(args, fmt);
    tty_vline(stdout, NULL, fmt, args);
    va_end(args);
}

void tty_progress(const lb_cfg_t *cfg, const char *fmt, ...)
{
    va_list args;
    FILE *stream = NULL;

    if (fmt == NULL || (cfg != NULL && cfg->quiet)) {
        return;
    }

    stream = tty_info_stream(cfg);
    va_start(args, fmt);
    tty_vline(stream, NULL, fmt, args);
    va_end(args);
}

static void tty_status(const char *color, const char *prefix, const char *fmt, va_list args)
    PAL_PRINTF_FORMAT(3, 0);

static void tty_status(const char *color, const char *prefix, const char *fmt, va_list args)
{
    bool use_color = false;

    if (fmt == NULL) {
        return;
    }

    use_color = !g_tty_no_color && getenv("NO_COLOR") == NULL && pal_is_tty(true);
    if (use_color) {
        (void)fputs(color, stderr);
    }
    (void)fputs(prefix, stderr);
    if (use_color) {
        (void)fputs(TTY_ANSI_RESET, stderr);
    }
    tty_vline(stderr, NULL, fmt, args);
}

void tty_warn(const char *fmt, ...)
{
    va_list args;

    if (fmt == NULL) {
        return;
    }

    va_start(args, fmt);
    tty_status(TTY_ANSI_WARN, "[WARN] ", fmt, args);
    va_end(args);
}

void tty_err(const char *fmt, ...)
{
    va_list args;

    if (fmt == NULL) {
        return;
    }

    va_start(args, fmt);
    tty_status(TTY_ANSI_ERROR, "[ERROR] ", fmt, args);
    va_end(args);
}

void tty_warn_drift(const lb_cfg_t *cfg, double drift_pct)
{
    /*
     * A positive drift means the core sped up (Turbo ramp), which is not a
     * measurement defect; the actual percentage is still shown in the DVFS
     * drift line. Only a real frequency drop (throttling) is worth a warning.
     */
    if (cfg != NULL && cfg->suppress_drift_warn) {
        return;
    }
    if (drift_pct < -TTY_DRIFT_WARN_DROP_PCT) {
        tty_warn("frequency drift %+.1f%% (drop limit -%.0f%%)", drift_pct,
                 TTY_DRIFT_WARN_DROP_PCT);
    }
}

static void tty_format_scaled(uint64_t bytes, uint64_t unit, const char *suffix, char *out,
                              size_t capacity)
{
    if (bytes % unit == 0u) {
        (void)snprintf(out, capacity, "%" PRIu64 " %s", bytes / unit, suffix);
    } else {
        (void)snprintf(out, capacity, "%.1f %s", (double)bytes / (double)unit, suffix);
    }
}

const char *tty_format_bytes(uint64_t bytes, char *out, size_t capacity)
{
    if (out == NULL || capacity == 0u) {
        return "";
    }

    if (bytes >= TTY_GIB) {
        tty_format_scaled(bytes, TTY_GIB, "GiB", out, capacity);
    } else if (bytes >= TTY_MIB) {
        tty_format_scaled(bytes, TTY_MIB, "MiB", out, capacity);
    } else if (bytes >= TTY_KIB) {
        tty_format_scaled(bytes, TTY_KIB, "KiB", out, capacity);
    } else {
        (void)snprintf(out, capacity, "%" PRIu64 " B", bytes);
    }
    return out;
}

const char *tty_format_ns(uint64_t ns, char *out, size_t capacity)
{
    if (out == NULL || capacity == 0u) {
        return "";
    }

    if (ns >= TTY_NS_PER_S) {
        (void)snprintf(out, capacity, "%.2f s", (double)ns / (double)TTY_NS_PER_S);
    } else if (ns >= TTY_NS_PER_MS) {
        (void)snprintf(out, capacity, "%.2f ms", (double)ns / (double)TTY_NS_PER_MS);
    } else if (ns >= TTY_NS_PER_US) {
        (void)snprintf(out, capacity, "%.2f us", (double)ns / (double)TTY_NS_PER_US);
    } else {
        (void)snprintf(out, capacity, "%" PRIu64 " ns", ns);
    }
    return out;
}

/*
 * Small log2 approximation for bar placement. A linear interpolation of the
 * mantissa is accurate enough for a twenty-cell display and keeps the renderer
 * free of a libm dependency.
 */
static double tty_log2_approx(double value)
{
    int exponent = 0;
    double mantissa = value;

    if (!(mantissa > 0.0)) {
        return 0.0;
    }
    while (mantissa >= 2.0) {
        mantissa *= 0.5;
        exponent++;
    }
    while (mantissa < 1.0) {
        mantissa *= 2.0;
        exponent--;
    }
    return (double)exponent + (mantissa - 1.0);
}

static size_t tty_bar_length(double value, double min_value, double max_value, size_t width)
{
    double low = 0.0;
    double high = 0.0;
    double position = 0.0;
    size_t filled = 0u;

    if (width == 0u) {
        return 0u;
    }
    if (!(max_value > min_value)) {
        return width / 2u;
    }

    low = tty_log2_approx(min_value);
    high = tty_log2_approx(max_value);
    if (!(high > low)) {
        return width / 2u;
    }

    position = (tty_log2_approx(value) - low) / (high - low);
    if (!(position > 0.0)) {
        position = 0.0;
    }
    if (position > 1.0) {
        position = 1.0;
    }

    if (width == 1u) {
        return 1u;
    }

    /* Keep the smallest value visible as a one-cell baseline. */
    filled = 1u + (size_t)((position * (double)(width - 1u)) + 0.5);
    if (filled > width) {
        filled = width;
    }
    return filled;
}

static size_t tty_bar_length_linear(double value, double max_value, size_t width)
{
    double position = 0.0;
    size_t filled = 0u;

    if (width == 0u) {
        return 0u;
    }
    if (!(max_value > 0.0)) {
        return 0u;
    }

    position = value / max_value;
    if (!(position > 0.0)) {
        position = 0.0;
    }
    if (position > 1.0) {
        position = 1.0;
    }

    filled = (size_t)((position * (double)width) + 0.5);
    if (filled > width) {
        filled = width;
    }
    return filled;
}

static const char *tty_format_bar(double value, double min_value, double max_value, bool linear,
                                  char *out, size_t capacity)
{
    size_t filled = 0u;
    size_t i = 0u;

    if (out == NULL || capacity == 0u) {
        return "";
    }

    if (linear) {
        filled = tty_bar_length_linear(value, max_value, TTY_PLOT_BAR_WIDTH);
    } else {
        filled = tty_bar_length(value, min_value, max_value, TTY_PLOT_BAR_WIDTH);
    }
    if (filled >= capacity) {
        filled = capacity - 1u;
    }
    for (i = 0u; i < filled; i++) {
        out[i] = '#';
    }
    out[filled] = '\0';
    return out;
}

static const char *tty_threshold_name(const char *const *level_names, size_t level_name_count,
                                      size_t index, char *fallback, size_t capacity)
{
    if (level_names != NULL && index < level_name_count && level_names[index] != NULL) {
        return level_names[index];
    }

    (void)snprintf(fallback, capacity, "step %zu", index + 1u);
    return fallback;
}

static size_t tty_pages_for_bytes(size_t bytes, size_t page_size)
{
    if (page_size == 0u) {
        return 0u;
    }
    return (bytes + (page_size / 2u)) / page_size;
}

static void tty_print_thresholds(const lb_cfg_t *cfg, const stats_steps_t *steps,
                                 const char *const *level_names, size_t level_name_count,
                                 size_t page_size)
{
    char list[LB_CSV_MAX] = {0};
    char size[LB_NAME_MAX] = {0};
    char fallback[LB_NAME_MAX] = {0};
    size_t used = 0u;
    size_t i = 0u;
    bool fits = true;

    if (steps == NULL || steps->count == 0u) {
        tty_header(cfg, "  Estimated thresholds: none detected");
        return;
    }

    for (i = 0u; i < steps->count && i < STATS_MAX_STEPS; i++) {
        const char *name =
            tty_threshold_name(level_names, level_name_count, i, fallback, sizeof(fallback));
        int written = 0;

        (void)tty_format_bytes((uint64_t)steps->items[i].x, size, sizeof(size));
        if (page_size > 0u) {
            written = snprintf(list + used, sizeof(list) - used, "%s~%s (~%zu pages, %s)",
                               (i == 0u) ? "" : ", ", size,
                               tty_pages_for_bytes(steps->items[i].x, page_size), name);
        } else {
            written = snprintf(list + used, sizeof(list) - used, "%s~%s (%s)",
                               (i == 0u) ? "" : ", ", size, name);
        }
        if (written < 0 || (size_t)written >= (sizeof(list) - used)) {
            fits = false;
            break;
        }
        used += (size_t)written;
    }

    if (fits && ((strlen("Estimated thresholds: ") + used) <= TTY_PLOT_WIDTH)) {
        tty_header(cfg, "  Estimated thresholds: %s", list);
        return;
    }

    tty_header(cfg, "  Estimated thresholds:");
    for (i = 0u; i < steps->count && i < STATS_MAX_STEPS; i++) {
        const char *name =
            tty_threshold_name(level_names, level_name_count, i, fallback, sizeof(fallback));

        (void)tty_format_bytes((uint64_t)steps->items[i].x, size, sizeof(size));
        if (page_size > 0u) {
            tty_header(cfg, "  ~%s (~%zu pages, %s)", size,
                       tty_pages_for_bytes(steps->items[i].x, page_size), name);
        } else {
            tty_header(cfg, "  ~%s (%s)", size, name);
        }
    }
}

static void tty_csv_field(const char *text, char *out, size_t capacity)
{
    size_t i = 0u;

    if (out == NULL || capacity == 0u) {
        return;
    }
    if (text == NULL) {
        out[0] = '\0';
        return;
    }

    while (text[i] != '\0' && (i + 1u) < capacity) {
        const char c = text[i];

        out[i] = (c == ',' || c == '\n' || c == '\r') ? '_' : c;
        i++;
    }
    out[i] = '\0';
}

/*
 * Pick one fixed decimal precision for the whole latency table. Using a single
 * format per render keeps the decimal points vertically aligned, unlike the old
 * %.4g output that changed width from row to row.
 */
static int tty_latency_precision(double max_value)
{
    if (max_value < 10.0) {
        return 3;
    }
    if (max_value < 100.0) {
        return 2;
    }
    if (max_value < 1000.0) {
        return 1;
    }
    return 0;
}

lb_status_t tty_plot_series_ex(const lb_cfg_t *cfg, const stats_series_t *series,
                               const stats_steps_t *steps, const char *const *level_names,
                               size_t level_name_count, size_t page_size, bool print_section)
{
    const char *label = NULL;
    stats_point_summary_t *summaries = NULL;
    char label_field[LB_NAME_MAX] = {0};
    size_t count = 0u;
    lb_status_t status = LB_STATUS_OK;

    if (series == NULL) {
        return LB_STATUS_INVALID_ARG;
    }
    if (tty_plot_suppressed(cfg)) {
        return LB_STATUS_OK;
    }

    label = stats_series_label(series);
    if (label == NULL || label[0] == '\0') {
        label = "series";
    }
    tty_csv_field(label, label_field, sizeof(label_field));

    count = stats_series_count(series);
    if (count > 0u) {
        if (count > SIZE_MAX / sizeof(*summaries)) {
            return LB_STATUS_NOMEM;
        }
        summaries = malloc(count * sizeof(*summaries));
        if (summaries == NULL) {
            return LB_STATUS_NOMEM;
        }

        for (size_t i = 0u; i < count; i++) {
            status = stats_series_summary(series, i, &summaries[i]);
            if (status != LB_STATUS_OK) {
                goto cleanup;
            }
        }
    }

    if (print_section) {
        tty_section(cfg, label);
    }

    if (cfg != NULL && cfg->csv) {
        if (!g_tty_csv_series_header) {
            tty_csv_row(cfg, "test,x,p10_ns,min_ns,p50_ns,p90_ns,p99_ns,samples");
            g_tty_csv_series_header = true;
        }
        for (size_t i = 0u; i < count; i++) {
            tty_csv_row(cfg, "%s,%zu,%.2f,%.2f,%.2f,%.2f,%.2f,%zu", label_field, summaries[i].x,
                        summaries[i].p10_ns, summaries[i].min_ns, summaries[i].p50_ns,
                        summaries[i].p90_ns, summaries[i].p99_ns, summaries[i].sample_count);
        }
    } else if (count == 0u) {
        tty_header(cfg, "  (no points)");
    } else {
        char header[LB_LINE_MAX] = {0};
        double min_value = 0.0;
        double max_value = 0.0;
        bool have_value = false;
        bool flat_scale = false;

        for (size_t i = 0u; i < count; i++) {
            const double value = summaries[i].p10_ns;

            if (!(value > 0.0)) {
                continue;
            }
            if (!have_value || value < min_value) {
                min_value = value;
            }
            if (!have_value || value > max_value) {
                max_value = value;
            }
            have_value = true;
        }
        if (have_value && (min_value > 0.0)) {
            flat_scale = (max_value / min_value) < TTY_PLOT_FLAT_RATIO;
        }
        const int precision = tty_latency_precision(have_value ? max_value : 0.0);

        if (page_size > 0u) {
            (void)snprintf(header, sizeof(header), "  %*s %*s %*s %*s %*s  %s",
                           TTY_PLOT_FIELD_WIDTH, "Size", TTY_PLOT_FIELD_WIDTH, "p10 ns",
                           TTY_PLOT_FIELD_WIDTH, "p50 ns", TTY_PLOT_FIELD_WIDTH, "p90 ns",
                           TTY_PLOT_FIELD_WIDTH, "Pages", "Latency (log)");
        } else {
            (void)snprintf(header, sizeof(header), "  %*s %*s %*s %*s  %s", TTY_PLOT_FIELD_WIDTH,
                           "Size", TTY_PLOT_FIELD_WIDTH, "p10 ns", TTY_PLOT_FIELD_WIDTH, "p50 ns",
                           TTY_PLOT_FIELD_WIDTH, "p90 ns", "Latency (log)");
        }
        tty_header(cfg, "%s", header);

        for (size_t i = 0u; i < count; i++) {
            char size[32] = {0};
            char p10[32] = {0};
            char p50[32] = {0};
            char p90[32] = {0};
            char pages[32] = {0};
            char bar[TTY_PLOT_BAR_WIDTH + 1u] = {0};
            char line[LB_LINE_MAX] = {0};

            (void)tty_format_bytes((uint64_t)summaries[i].x, size, sizeof(size));
            (void)snprintf(p10, sizeof(p10), "%.*f", precision, summaries[i].p10_ns);
            (void)snprintf(p50, sizeof(p50), "%.*f", precision, summaries[i].p50_ns);
            (void)snprintf(p90, sizeof(p90), "%.*f", precision, summaries[i].p90_ns);
            (void)snprintf(pages, sizeof(pages), "%zu",
                           tty_pages_for_bytes(summaries[i].x, page_size));
            if (have_value) {
                (void)tty_format_bar(summaries[i].p10_ns, min_value, max_value, flat_scale, bar,
                                     sizeof(bar));
            }

            if (page_size > 0u) {
                (void)snprintf(line, sizeof(line), "  %*s %*s %*s %*s %*s  %s",
                               TTY_PLOT_FIELD_WIDTH, size, TTY_PLOT_FIELD_WIDTH, p10,
                               TTY_PLOT_FIELD_WIDTH, p50, TTY_PLOT_FIELD_WIDTH, p90,
                               TTY_PLOT_FIELD_WIDTH, pages, bar);
            } else {
                (void)snprintf(line, sizeof(line), "  %*s %*s %*s %*s  %s", TTY_PLOT_FIELD_WIDTH,
                               size, TTY_PLOT_FIELD_WIDTH, p10, TTY_PLOT_FIELD_WIDTH, p50,
                               TTY_PLOT_FIELD_WIDTH, p90, bar);
            }
            tty_header(cfg, "%s", line);
        }
    }

    tty_blank(cfg);
    tty_print_thresholds(cfg, steps, level_names, level_name_count, page_size);
    status = LB_STATUS_OK;

cleanup:
    free(summaries);
    return status;
}

lb_status_t tty_plot_series(const lb_cfg_t *cfg, const stats_series_t *series,
                            const stats_steps_t *steps, const char *const *level_names,
                            size_t level_name_count, size_t page_size)
{
    return tty_plot_series_ex(cfg, series, steps, level_names, level_name_count, page_size, true);
}

static const char *tty_matrix_cell_color(uint64_t ns)
{
    if (ns <= TTY_MATRIX_FAST_NS) {
        return TTY_ANSI_MATRIX_FAST;
    }
    if (ns <= TTY_MATRIX_SLOW_NS) {
        return TTY_ANSI_MATRIX_MID;
    }
    return TTY_ANSI_MATRIX_SLOW;
}

/*
 * Matrix cells are narrow, and a single scheduler outlier can produce an
 * eight-digit nanosecond value that used to collide with the next column.
 * Keep every rendered value short enough for the cell width and switch to us
 * or ms once the number would no longer fit.
 */
static void tty_matrix_format_latency(uint64_t ns, char *out, size_t capacity)
{
    if (out == NULL || capacity == 0u) {
        return;
    }

    if (ns < UINT64_C(100000)) {
        (void)snprintf(out, capacity, "%" PRIu64, ns);
    } else if (ns < TTY_NS_PER_MS) {
        (void)snprintf(out, capacity, "%" PRIu64 "us", (ns + UINT64_C(500)) / TTY_NS_PER_US);
    } else if (ns < (TTY_NS_PER_MS * UINT64_C(100))) {
        (void)snprintf(out, capacity, "%.1fms", (double)ns / (double)TTY_NS_PER_MS);
    } else if (ns < TTY_NS_PER_S) {
        (void)snprintf(out, capacity, "%" PRIu64 "ms", ns / TTY_NS_PER_MS);
    } else {
        (void)snprintf(out, capacity, ">999ms");
    }
}

lb_status_t tty_print_matrix(const lb_cfg_t *cfg, const char *title, const uint64_t *latency_ns,
                             size_t count)
{
    FILE *stream = NULL;
    bool color = false;
    size_t i = 0u;
    size_t j = 0u;

    if (latency_ns == NULL || count == 0u || count > (size_t)TTY_MATRIX_MAX_SIZE) {
        return LB_STATUS_INVALID_ARG;
    }
    if (tty_plot_suppressed(cfg)) {
        return LB_STATUS_OK;
    }

    tty_section(cfg, (title != NULL) ? title : "CPU latency matrix");

    if (cfg != NULL && cfg->csv) {
        if (!g_tty_csv_matrix_header) {
            tty_csv_row(cfg, "cpu_a,cpu_b,latency_ns");
            g_tty_csv_matrix_header = true;
        }
        for (i = 0u; i < count; i++) {
            for (j = 0u; j < count; j++) {
                if (i != j) {
                    tty_csv_row(cfg, "%zu,%zu,%" PRIu64, i, j, latency_ns[i * count + j]);
                }
            }
        }
        return LB_STATUS_OK;
    }

    stream = tty_info_stream(cfg);
    color = tty_color_enabled(cfg, stream);

    (void)fprintf(stream, "    |");
    for (j = 0u; j < count; j++) {
        (void)fprintf(stream, "%*zu ", TTY_MATRIX_CELL_WIDTH, j);
    }
    (void)fputc('\n', stream);

    (void)fputs("----+", stream);
    for (j = 0u; j < count; j++) {
        (void)fputs(TTY_MATRIX_SEPARATOR " ", stream);
    }
    (void)fputc('\n', stream);

    for (i = 0u; i < count; i++) {
        (void)fprintf(stream, "%*zu |", TTY_MATRIX_ROW_WIDTH, i);
        for (j = 0u; j < count; j++) {
            char cell[32] = {0};
            const char *cell_color = NULL;

            if (i == j) {
                (void)snprintf(cell, sizeof(cell), "-");
            } else {
                tty_matrix_format_latency(latency_ns[i * count + j], cell, sizeof(cell));
                if (color) {
                    cell_color = tty_matrix_cell_color(latency_ns[i * count + j]);
                }
            }

            if (cell_color != NULL) {
                (void)fputs(cell_color, stream);
            }
            (void)fprintf(stream, "%*s ", TTY_MATRIX_CELL_WIDTH, cell);
            if (cell_color != NULL) {
                (void)fputs(TTY_ANSI_RESET, stream);
            }
        }
        (void)fputc('\n', stream);
    }

    if (color) {
        (void)fprintf(stream,
                      "  legend: <= %" PRIu64 " ns fast, <= %" PRIu64 " ns mid, > %" PRIu64
                      " ns slow\n",
                      TTY_MATRIX_FAST_NS, TTY_MATRIX_SLOW_NS, TTY_MATRIX_SLOW_NS);
    }
    return LB_STATUS_OK;
}
