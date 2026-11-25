/* SPDX-License-Identifier: GPL-3.0 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "auto.h"
#include "passport.h"
#include "types.h"

/* Capture files live in the working directory so the test binary is portable. */
#define TEST_CAPTURE_CSV "test_auto_capture.csv"
#define TEST_CAPTURE_SIZE 16384u
#define TEST_CSV_COLUMNS 5u
#define TEST_CSV_HEADER "module,subtest,metric,value,unit\n"
#define TEST_PASSPORT_PREFIX "auto,passport,"
#define TEST_PASSPORT_PREFIX_LEN 14u

static bool capture_start(const char *path)
{
    return freopen(path, "w", stdout) != NULL;
}

static size_t capture_read(const char *path, char *out, size_t capacity)
{
    FILE *stream = NULL;
    size_t bytes = 0u;

    if (out == NULL || capacity == 0u) {
        return 0u;
    }
    out[0] = '\0';
    (void)fflush(stdout);

    stream = fopen(path, "r");
    if (stream == NULL) {
        return 0u;
    }
    bytes = fread(out, 1u, capacity - 1u, stream);
    out[bytes] = '\0';
    (void)fclose(stream);
    return bytes;
}

static bool csv_has_fixed_columns(const char *text, size_t columns)
{
    const char *line = text;

    if (text == NULL || columns == 0u) {
        return false;
    }

    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        const size_t length = (end != NULL) ? (size_t)(end - line) : strlen(line);
        size_t commas = 0u;
        size_t i = 0u;

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

static bool csv_rows_are_passport(const char *text)
{
    const char *line = NULL;

    if (text == NULL) {
        return false;
    }
    line = strchr(text, '\n');
    if (line == NULL) {
        return false;
    }
    line++;

    while (*line != '\0') {
        if (strncmp(line, TEST_PASSPORT_PREFIX, TEST_PASSPORT_PREFIX_LEN) != 0) {
            return false;
        }
        line = strchr(line, '\n');
        if (line == NULL) {
            break;
        }
        line++;
    }
    return true;
}

static bool csv_has_single_header(const char *text)
{
    const char *first = NULL;
    const char *second = NULL;

    if (text == NULL) {
        return false;
    }
    first = strstr(text, TEST_CSV_HEADER);
    if (first == NULL) {
        return false;
    }
    second = strstr(first + strlen(TEST_CSV_HEADER), TEST_CSV_HEADER);
    return second == NULL;
}

int main(void)
{
    lb_cfg_t cfg = {0};
    lb_status_t status = LB_STATUS_ERROR;
    const lb_passport_t *passport = NULL;
    char output[TEST_CAPTURE_SIZE] = {0};
    int failures = 0;
    bool captured = false;

    cfg.cpu_id = -1;
    cfg.batch_ms = 1u;
    cfg.max_mem_mb = 4u;
    cfg.no_thp = true;
    cfg.csv = true;
    cfg.quiet = true;
    cfg.suppress_drift_warn = true;
    cfg.os_ctx_mode = LB_OS_CTX_BOTH;

    captured = capture_start(TEST_CAPTURE_CSV);
    status = lb_auto_run(&cfg);
    if (!captured) {
        fprintf(stderr, "FAIL: cannot capture auto CSV stdout\n");
        failures++;
    } else {
        (void)capture_read(TEST_CAPTURE_CSV, output, sizeof(output));
    }

    if (status != LB_STATUS_OK) {
        fprintf(stderr, "FAIL: lb_auto_run returned status %d\n", (int)status);
        failures++;
    }

    passport = lb_passport_get();
    if (passport->arch == NULL) {
        fprintf(stderr, "FAIL: auto did not publish the architecture\n");
        failures++;
    }
    if (passport->page_size == 0u) {
        fprintf(stderr, "FAIL: auto did not publish the page size\n");
        failures++;
    }
    if (passport->cpu_count == 0u) {
        fprintf(stderr, "FAIL: auto did not publish the CPU count\n");
        failures++;
    }

    if (captured) {
        if (strstr(output, TEST_CSV_HEADER) != output) {
            fprintf(stderr, "FAIL: auto CSV does not start with the passport header\n");
            failures++;
        }
        if (!csv_has_single_header(output)) {
            fprintf(stderr, "FAIL: auto CSV emits more than one header\n");
            failures++;
        }
        if (!csv_has_fixed_columns(output, TEST_CSV_COLUMNS)) {
            fprintf(stderr, "FAIL: auto CSV has a row without five columns\n");
            failures++;
        }
        if (!csv_rows_are_passport(output)) {
            fprintf(stderr, "FAIL: auto CSV contains a non-passport data row\n");
            failures++;
        }
        if (strstr(output, "mem,assoc") != NULL) {
            fprintf(stderr, "FAIL: auto CSV leaked the mem assoc schema\n");
            failures++;
        }
        if (strstr(output, "test,x,") != NULL) {
            fprintf(stderr, "FAIL: auto CSV leaked the series plot schema\n");
            failures++;
        }
        if (strstr(output, "cpu_a,cpu_b") != NULL) {
            fprintf(stderr, "FAIL: auto CSV leaked the topology matrix schema\n");
            failures++;
        }
        if (strstr(output, TEST_PASSPORT_PREFIX) == NULL) {
            fprintf(stderr, "FAIL: auto CSV contains no passport records\n");
            failures++;
        }
    }

    (void)remove(TEST_CAPTURE_CSV);
    fprintf(stderr, "test_auto: %s\n", (failures == 0) ? "ok" : "FAILED");
    return (failures == 0) ? 0 : 1;
}
