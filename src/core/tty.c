/* SPDX-License-Identifier: GPL-3.0 */

#include "tty.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "pal/pal.h"
#include "types.h"

#define TTY_KEY_WIDTH 16
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

static bool g_tty_section_started = false;
static bool g_tty_no_color = false;

static FILE *tty_info_stream(const lb_cfg_t *cfg)
{
    if (cfg != NULL && cfg->csv) {
        return stderr;
    }
    return stdout;
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

void tty_header(const lb_cfg_t *cfg, const char *fmt, ...)
{
    va_list args;
    FILE *stream = NULL;

    if (fmt == NULL) {
        return;
    }

    stream = tty_info_stream(cfg);
    va_start(args, fmt);
    tty_vline(stream, NULL, fmt, args);
    va_end(args);
}

void tty_section(const lb_cfg_t *cfg, const char *title)
{
    FILE *stream = NULL;
    bool color = false;

    if (title == NULL) {
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

    if (key == NULL || fmt == NULL) {
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

    if (cfg == NULL || !cfg->csv || fmt == NULL) {
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

    if (fmt == NULL) {
        return;
    }

    stream = tty_info_stream(cfg);
    va_start(args, fmt);
    tty_vline(stream, NULL, fmt, args);
    va_end(args);
}

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
