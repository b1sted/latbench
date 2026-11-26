/* SPDX-License-Identifier: GPL-3.0 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "defs.h"
#include "stats.h"
#include "tty.h"
#include "types.h"

/* Capture files live in the working directory: standalone test binaries are
 * also run on targets where no build/ tree was copied. */
#define TEST_CAPTURE_OUT "test_tty_capture.out"
#define TEST_CAPTURE_CSV "test_tty_capture.csv"
#define TEST_CAPTURE_SIZE 8192u
#define TEST_PLOT_WIDTH 80u
#define TEST_CSV_COLUMNS 8

static int g_checks = 0;
static int g_failures = 0;
static bool g_stdout_captured = false;

static void check_true(bool condition, const char *name)
{
    g_checks++;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        g_failures++;
    }
}

static bool capture_start(const char *path)
{
    g_stdout_captured = (freopen(path, "w", stdout) != NULL);
    return g_stdout_captured;
}

static size_t capture_read(const char *path, char *out, size_t capacity)
{
    FILE *stream = NULL;
    size_t bytes = 0u;

    if (out == NULL || capacity == 0u) {
        return 0u;
    }
    out[0] = '\0';
    if (g_stdout_captured) {
        (void)fflush(stdout);
    }

    stream = fopen(path, "r");
    if (stream == NULL) {
        return 0u;
    }
    bytes = fread(out, 1u, capacity - 1u, stream);
    out[bytes] = '\0';
    (void)fclose(stream);
    return bytes;
}

static bool lines_fit(const char *text, size_t width)
{
    const char *line = text;

    if (text == NULL) {
        return false;
    }

    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        const size_t length = (end != NULL) ? (size_t)(end - line) : strlen(line);

        if (length > width) {
            return false;
        }
        if (end == NULL) {
            break;
        }
        line = end + 1;
    }
    return true;
}

static size_t count_lines(const char *text)
{
    size_t lines = 0u;
    const char *cursor = text;

    if (text == NULL) {
        return 0u;
    }
    while (*cursor != '\0') {
        if (*cursor == '\n') {
            lines++;
        }
        cursor++;
    }
    return lines;
}

static size_t count_substring(const char *text, const char *needle)
{
    size_t count = 0u;
    const char *cursor = text;

    if (text == NULL || needle == NULL || needle[0] == '\0') {
        return 0u;
    }
    while ((cursor = strstr(cursor, needle)) != NULL) {
        count++;
        cursor += strlen(needle);
    }
    return count;
}

static bool csv_rows_valid_cols(const char *text, size_t columns)
{
    const char *line = text;

    if (text == NULL || columns == 0u) {
        return false;
    }

    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        const size_t length = (end != NULL) ? (size_t)(end - line) : strlen(line);
        size_t i = 0u;
        size_t commas = 0u;

        for (i = 0u; i < length; i++) {
            if (line[i] == ',') {
                commas++;
            }
        }
        if (length > 0u && commas != (columns - 1u)) {
            return false;
        }
        if (end == NULL) {
            break;
        }
        line = end + 1;
    }
    return true;
}

static bool csv_rows_valid(const char *text)
{
    return csv_rows_valid_cols(text, TEST_CSV_COLUMNS);
}

static stats_series_t *make_series(void)
{
    const double l1[3] = {2.0, 2.1, 2.05};
    const double l2[3] = {8.0, 8.1, 8.05};
    const double l3[3] = {30.0, 30.5, 29.5};
    const double ram[3] = {80.0, 81.0, 79.0};
    stats_series_t *series = stats_series_create("cache-sweep", 4u);

    if (series == NULL) {
        return NULL;
    }

    if (stats_series_push(series, 4096u, l1, 3u) != LB_STATUS_OK ||
        stats_series_push(series, 32768u, l2, 3u) != LB_STATUS_OK ||
        stats_series_push(series, 1048576u, l3, 3u) != LB_STATUS_OK ||
        stats_series_push(series, 33554432u, ram, 3u) != LB_STATUS_OK) {
        stats_series_free(series);
        return NULL;
    }
    return series;
}

static void make_steps(stats_steps_t *steps)
{
    (void)memset(steps, 0, sizeof(*steps));
    steps->count = 2u;
    steps->items[0].x = 49152u;
    steps->items[0].x_lower = 32768u;
    steps->items[0].x_upper = 65536u;
    steps->items[0].lower_ns = 2.05;
    steps->items[0].upper_ns = 8.05;
    steps->items[0].ratio = 8.05 / 2.05;

    steps->items[1].x = 2097152u;
    steps->items[1].x_lower = 1048576u;
    steps->items[1].x_upper = 4194304u;
    steps->items[1].lower_ns = 30.0;
    steps->items[1].upper_ns = 80.0;
    steps->items[1].ratio = 80.0 / 30.0;
}

static void test_plot_rejects_null_series(void)
{
    lb_cfg_t cfg = {0};

    check_true(tty_plot_series(&cfg, NULL, NULL, NULL, 0u, 0u) == LB_STATUS_INVALID_ARG,
               "plot rejects a NULL series");
}

static void test_plot_table(void)
{
    const char *names[2] = {"L1", "L2"};
    stats_series_t *series = make_series();
    stats_steps_t steps = {0};
    lb_cfg_t cfg = {0};
    char output[TEST_CAPTURE_SIZE] = {0};

    check_true(series != NULL, "table series is created");
    if (series == NULL) {
        return;
    }

    make_steps(&steps);
    cfg.no_color = true;
    cfg.csv = false;
    tty_set_no_color(true);

    check_true(capture_start(TEST_CAPTURE_OUT), "table capture starts");
    if (g_stdout_captured) {
        check_true(tty_plot_series(&cfg, series, &steps, names, 2u, 0u) == LB_STATUS_OK,
                   "table plot succeeds");
        (void)capture_read(TEST_CAPTURE_OUT, output, sizeof(output));

        check_true(strstr(output, "cache-sweep:") != NULL, "table prints the series heading");
        check_true(strstr(output, "Size") != NULL, "table prints a size column");
        check_true(strstr(output, "p10 ns") != NULL, "table prints a latency column");
        check_true(strstr(output, "Latency (log)") != NULL, "table labels the log bar");
        check_true(strstr(output, "KiB") != NULL, "table scales small sizes to KiB");
        check_true(strstr(output, "MiB") != NULL, "table scales large sizes to MiB");
        check_true(strstr(output, "Estimated thresholds:") != NULL,
                   "table prints the estimated thresholds");
        check_true(strstr(output, "~48 KiB (L1)") != NULL, "thresholds use size and L1 label");
        check_true(strstr(output, "~2 MiB (L2)") != NULL, "thresholds use size and L2 label");
        check_true(strstr(output, "#") != NULL, "table draws an ASCII bar");
        check_true(strstr(output, "\x1b") == NULL, "no-color plot emits no ANSI escapes");
        check_true(lines_fit(output, TEST_PLOT_WIDTH), "table stays within 80 columns");
    }

    stats_series_free(series);
}

static void test_plot_csv(void)
{
    const char *names[2] = {"L1", "L2"};
    stats_series_t *series = make_series();
    stats_steps_t steps = {0};
    lb_cfg_t cfg = {0};
    char output[TEST_CAPTURE_SIZE] = {0};

    check_true(series != NULL, "CSV series is created");
    if (series == NULL) {
        return;
    }

    make_steps(&steps);
    cfg.no_color = true;
    cfg.csv = true;
    tty_set_no_color(true);
    tty_reset_csv_headers();

    check_true(capture_start(TEST_CAPTURE_CSV), "CSV capture starts");
    if (g_stdout_captured) {
        check_true(tty_plot_series(&cfg, series, &steps, names, 2u, 0u) == LB_STATUS_OK,
                   "CSV plot succeeds");
        (void)capture_read(TEST_CAPTURE_CSV, output, sizeof(output));

        check_true(strstr(output, "test,x,p10_ns,min_ns,p50_ns,p90_ns,p99_ns,samples") == output,
                   "CSV output starts with a clean header");
        check_true(strstr(output, "cache-sweep,4096,") != NULL,
                   "CSV output keeps the series label and raw x");
        check_true(strstr(output, "Estimated thresholds") == NULL,
                   "CSV output has no threshold prose");
        check_true(strstr(output, "Latency") == NULL, "CSV output has no table heading");
        check_true(strstr(output, " #") == NULL, "CSV output has no bars");
        check_true(count_lines(output) == 5u, "CSV output has one header and four points");
        check_true(csv_rows_valid(output), "every CSV row has the expected column count");
    }

    stats_series_free(series);
}

static void test_csv_headers_repeat(void)
{
    const char *names[2] = {"L1", "L2"};
    stats_series_t *series = make_series();
    stats_steps_t steps = {0};
    lb_cfg_t cfg = {0};
    char output[TEST_CAPTURE_SIZE] = {0};
    const char *header = "test,x,p10_ns,min_ns,p50_ns,p90_ns,p99_ns,samples\n";

    check_true(series != NULL, "repeat series is created");
    if (series == NULL) {
        return;
    }

    make_steps(&steps);
    cfg.no_color = true;
    cfg.csv = true;
    tty_set_no_color(true);

    /* Without a reset, two renders share one header. */
    tty_reset_csv_headers();
    check_true(capture_start(TEST_CAPTURE_CSV), "repeat CSV capture starts");
    if (g_stdout_captured) {
        check_true(tty_plot_series(&cfg, series, &steps, names, 2u, 0u) == LB_STATUS_OK,
                   "first repeat plot succeeds");
        check_true(tty_plot_series(&cfg, series, &steps, names, 2u, 0u) == LB_STATUS_OK,
                   "second repeat plot succeeds");
        (void)capture_read(TEST_CAPTURE_CSV, output, sizeof(output));
        check_true(count_substring(output, header) == 1u,
                   "repeated plots emit exactly one CSV header");
    }

    /* A reset starts a fresh stream so the next render prints its header. */
    tty_reset_csv_headers();
    check_true(capture_start(TEST_CAPTURE_CSV), "reset CSV capture starts");
    if (g_stdout_captured) {
        check_true(tty_plot_series(&cfg, series, &steps, names, 2u, 0u) == LB_STATUS_OK,
                   "post-reset plot succeeds");
        (void)capture_read(TEST_CAPTURE_CSV, output, sizeof(output));
        check_true(strstr(output, header) == output,
                   "reset makes the next CSV render start with its header");
    }

    stats_series_free(series);
}

static void test_plot_pages(void)
{
    const char *names[2] = {"L1 dTLB", "L2 TLB"};
    stats_series_t *series = make_series();
    stats_steps_t steps = {0};
    lb_cfg_t cfg = {0};
    char output[TEST_CAPTURE_SIZE] = {0};

    check_true(series != NULL, "page series is created");
    if (series == NULL) {
        return;
    }

    make_steps(&steps);
    cfg.no_color = true;
    cfg.csv = false;
    tty_set_no_color(true);

    check_true(capture_start(TEST_CAPTURE_OUT), "page capture starts");
    if (g_stdout_captured) {
        check_true(tty_plot_series(&cfg, series, &steps, names, 2u, 4096u) == LB_STATUS_OK,
                   "page plot succeeds");
        (void)capture_read(TEST_CAPTURE_OUT, output, sizeof(output));

        check_true(strstr(output, "Pages") != NULL, "page plot adds a Pages column");
        check_true(strstr(output, "~48 KiB (~12 pages, L1 dTLB)") != NULL,
                   "thresholds report the page count next to the level label");
        check_true(lines_fit(output, TEST_PLOT_WIDTH), "page table stays within 80 columns");
    }

    stats_series_free(series);
}

static const uint64_t TEST_MATRIX[16] = {
    0u, 12u, 42u, 95u, 11u, 0u, 38u, 88u, 40u, 39u, 0u, 72u, 91u, 86u, 70u, 0u,
};

static const uint64_t TEST_MATRIX_LARGE[9] = {
    0u, UINT64_C(15686619), UINT64_C(2000000000), UINT64_C(15686619),
    0u, UINT64_C(150000),   UINT64_C(2000000000), UINT64_C(150000),
    0u,
};

static void test_matrix_table(void)
{
    lb_cfg_t cfg = {0};
    char output[TEST_CAPTURE_SIZE] = {0};

    cfg.no_color = true;
    cfg.csv = false;
    tty_set_no_color(true);

    check_true(capture_start(TEST_CAPTURE_OUT), "matrix capture starts");
    if (g_stdout_captured) {
        check_true(tty_print_matrix(&cfg, "Topology ping-pong", TEST_MATRIX, 4u) == LB_STATUS_OK,
                   "matrix print succeeds");
        (void)capture_read(TEST_CAPTURE_OUT, output, sizeof(output));

        check_true(strstr(output, "Topology ping-pong:") != NULL, "matrix prints its heading");
        check_true(strstr(output, "    |") != NULL, "matrix prints a corner and column rule");
        check_true(strstr(output, "  0 |") != NULL, "matrix prints row cpu 0");
        check_true(strstr(output, "  3 |") != NULL, "matrix prints row cpu 3");
        check_true(strstr(output, "-") != NULL, "matrix prints diagonal dashes");
        check_true(strstr(output, "95") != NULL, "matrix prints a slow latency");
        check_true(strstr(output, "\x1b") == NULL, "no-color matrix emits no ANSI escapes");
        check_true(lines_fit(output, TEST_PLOT_WIDTH), "matrix stays within 80 columns");
    }
}

static void test_matrix_large_cells(void)
{
    lb_cfg_t cfg = {0};
    char output[TEST_CAPTURE_SIZE] = {0};

    cfg.no_color = true;
    cfg.csv = false;
    tty_set_no_color(true);

    check_true(capture_start(TEST_CAPTURE_OUT), "large matrix capture starts");
    if (g_stdout_captured) {
        check_true(tty_print_matrix(&cfg, "Large outliers", TEST_MATRIX_LARGE, 3u) == LB_STATUS_OK,
                   "large matrix print succeeds");
        (void)capture_read(TEST_CAPTURE_OUT, output, sizeof(output));

        check_true(strstr(output, "15.7ms") != NULL, "matrix abbreviates multi-millisecond cells");
        check_true(strstr(output, "150us") != NULL, "matrix abbreviates sub-millisecond cells");
        check_true(strstr(output, ">999ms") != NULL, "matrix caps second-scale cells");
        check_true(strstr(output, "15686619") == NULL, "matrix never prints a raw outlier");
        check_true(lines_fit(output, TEST_PLOT_WIDTH), "large matrix stays within 80 columns");
    }
}

static void test_matrix_csv(void)
{
    lb_cfg_t cfg = {0};
    char output[TEST_CAPTURE_SIZE] = {0};

    cfg.no_color = true;
    cfg.csv = true;
    tty_set_no_color(true);
    tty_reset_csv_headers();

    check_true(capture_start(TEST_CAPTURE_CSV), "matrix CSV capture starts");
    if (g_stdout_captured) {
        check_true(tty_print_matrix(&cfg, "Topology ping-pong", TEST_MATRIX, 4u) == LB_STATUS_OK,
                   "matrix CSV print succeeds");
        (void)capture_read(TEST_CAPTURE_CSV, output, sizeof(output));

        check_true(strstr(output, "cpu_a,cpu_b,latency_ns") == output,
                   "matrix CSV starts with a clean header");
        check_true(strstr(output, "0,1,12") != NULL, "matrix CSV prints a row-major pair");
        check_true(strstr(output, "3,2,70") != NULL, "matrix CSV prints the mirrored pair");
        check_true(strstr(output, "0,0,") == NULL, "matrix CSV skips the diagonal");
        check_true(count_lines(output) == 13u, "matrix CSV has a header and twelve pairs");
        check_true(csv_rows_valid_cols(output, 3u), "matrix CSV rows have three columns");
        check_true(strstr(output, "Topology") == NULL, "matrix CSV stdout has no prose");
    }
}

int main(void)
{
    test_plot_rejects_null_series();
    test_plot_table();
    test_plot_pages();
    test_plot_csv();
    test_csv_headers_repeat();
    test_matrix_table();
    test_matrix_large_cells();
    test_matrix_csv();

    if (g_stdout_captured) {
        (void)fflush(stdout);
    }
    (void)remove(TEST_CAPTURE_OUT);
    (void)remove(TEST_CAPTURE_CSV);

    fprintf(stderr, "test_tty: %d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
