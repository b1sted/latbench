/* SPDX-License-Identifier: GPL-3.0 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define TEST_MAX_COUNT 1024u

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

static void test_safe_free(void)
{
    int *value = malloc(sizeof(*value));

    check_true(value != NULL, "malloc for LB_SAFE_FREE succeeds");
    if (value != NULL) {
        *value = 42;
        LB_SAFE_FREE(value);
        check_true(value == NULL, "LB_SAFE_FREE clears the pointer");
    }

    LB_SAFE_FREE(value);
    check_true(value == NULL, "LB_SAFE_FREE(NULL) is safe");
}

static void test_rng(void)
{
    uint64_t first_state = UINT64_C(0x123456789abcdef0);
    uint64_t second_state = UINT64_C(0x123456789abcdef0);
    uint64_t zero_state = 0u;
    const uint64_t first = util_rng_next(&first_state);
    const uint64_t second = util_rng_next(&second_state);
    const uint64_t zero_result = util_rng_next(&zero_state);

    check_true(first == second, "util_rng_next is deterministic for equal seeds");
    check_true(first_state == second_state, "util_rng_next advances equal seeds identically");
    check_true(first != 0u, "util_rng_next produces a nonzero value");
    check_true(util_rng_next(NULL) == 0u, "util_rng_next(NULL) returns zero");
    check_true(zero_result != 0u && zero_state != 0u,
               "util_rng_next escapes the zero-state fixed point");
}

static void test_sattolo_count(size_t count, uint64_t seed)
{
    size_t indices[TEST_MAX_COUNT];
    bool seen[TEST_MAX_COUNT];
    char name[96];
    size_t i = 0u;
    size_t cursor = 0u;
    bool unique = true;
    bool single_cycle = true;
    lb_status_t status = LB_STATUS_ERROR;

    memset(indices, 0, sizeof(indices));
    memset(seen, 0, sizeof(seen));

    status = util_sattolo_indices(indices, count, seed);
    (void)snprintf(name, sizeof(name), "util_sattolo_indices(count=%zu) succeeds", count);
    check_true(status == LB_STATUS_OK, name);

    if (status != LB_STATUS_OK) {
        return;
    }

    for (i = 0u; i < count; i++) {
        if (indices[i] >= count || seen[indices[i]]) {
            unique = false;
            break;
        }
        seen[indices[i]] = true;
    }
    (void)snprintf(name, sizeof(name), "util_sattolo_indices(count=%zu) is a permutation", count);
    check_true(unique, name);

    memset(seen, 0, sizeof(seen));
    cursor = 0u;
    for (i = 0u; i < count; i++) {
        if (cursor >= count || seen[cursor]) {
            single_cycle = false;
            break;
        }
        seen[cursor] = true;
        cursor = indices[cursor];
    }
    (void)snprintf(name, sizeof(name),
                   "util_sattolo_indices(count=%zu) visits every index once and closes at 0",
                   count);
    check_true(single_cycle && cursor == 0u, name);
}

static void test_sattolo(void)
{
    size_t indices[4] = {0};

    test_sattolo_count(1u, UINT64_C(1));
    test_sattolo_count(2u, UINT64_C(2));
    test_sattolo_count(16u, UINT64_C(3));
    test_sattolo_count(1024u, UINT64_C(4));

    check_true(util_sattolo_indices(NULL, 4u, UINT64_C(5)) == LB_STATUS_INVALID_ARG,
               "util_sattolo_indices(NULL, count>0) is rejected");
    check_true(util_sattolo_indices(NULL, 0u, UINT64_C(5)) == LB_STATUS_OK,
               "util_sattolo_indices(NULL, 0) is a no-op");
    check_true(util_sattolo_indices(indices, 0u, UINT64_C(5)) == LB_STATUS_OK,
               "util_sattolo_indices(count=0) is a no-op");
}

static void check_parse_u64_ok(const char *text, uint64_t expected)
{
    uint64_t value = 0u;
    const lb_status_t status = util_parse_u64(text, &value);
    char name[96];

    (void)snprintf(name, sizeof(name), "util_parse_u64(\"%s\") succeeds", text);
    check_true(status == LB_STATUS_OK, name);
    (void)snprintf(name, sizeof(name), "util_parse_u64(\"%s\") returns the expected value", text);
    check_true(status == LB_STATUS_OK && value == expected, name);
}

static void check_parse_u64_invalid(const char *text)
{
    uint64_t value = 0u;
    const lb_status_t status = util_parse_u64(text, &value);
    char name[96];

    (void)snprintf(name, sizeof(name), "util_parse_u64(\"%s\") is rejected", text);
    check_true(status == LB_STATUS_INVALID_ARG, name);
}

static void check_parse_i32_ok(const char *text, int32_t expected)
{
    int32_t value = 0;
    const lb_status_t status = util_parse_i32(text, &value);
    char name[96];

    (void)snprintf(name, sizeof(name), "util_parse_i32(\"%s\") succeeds", text);
    check_true(status == LB_STATUS_OK, name);
    (void)snprintf(name, sizeof(name), "util_parse_i32(\"%s\") returns the expected value", text);
    check_true(status == LB_STATUS_OK && value == expected, name);
}

static void check_parse_i32_invalid(const char *text)
{
    int32_t value = 0;
    const lb_status_t status = util_parse_i32(text, &value);
    char name[96];

    (void)snprintf(name, sizeof(name), "util_parse_i32(\"%s\") is rejected", text);
    check_true(status == LB_STATUS_INVALID_ARG, name);
}

static void test_parse_u64(void)
{
    uint64_t dummy = 0u;

    check_parse_u64_ok("0", UINT64_C(0));
    check_parse_u64_ok("42", UINT64_C(42));
    check_parse_u64_ok("+42", UINT64_C(42));
    check_parse_u64_ok("18446744073709551615", UINT64_MAX);
    check_parse_u64_ok("000000001", UINT64_C(1));

    check_parse_u64_invalid("");
    check_parse_u64_invalid("-1");
    check_parse_u64_invalid("+");
    check_parse_u64_invalid(" 42");
    check_parse_u64_invalid("42 ");
    check_parse_u64_invalid("12abc");
    check_parse_u64_invalid("0x10");
    check_parse_u64_invalid("18446744073709551616");

    check_true(util_parse_u64(NULL, &dummy) == LB_STATUS_INVALID_ARG,
               "util_parse_u64(NULL) is rejected");
    check_true(util_parse_u64("1", NULL) == LB_STATUS_INVALID_ARG,
               "util_parse_u64(out=NULL) is rejected");
}

static void test_parse_i32(void)
{
    int32_t dummy = 0;

    check_parse_i32_ok("0", 0);
    check_parse_i32_ok("42", 42);
    check_parse_i32_ok("+42", 42);
    check_parse_i32_ok("-42", -42);
    check_parse_i32_ok("2147483647", INT32_MAX);
    check_parse_i32_ok("-2147483648", INT32_MIN);

    check_parse_i32_invalid("");
    check_parse_i32_invalid("+");
    check_parse_i32_invalid("-");
    check_parse_i32_invalid(" -1");
    check_parse_i32_invalid("1 ");
    check_parse_i32_invalid("7x");
    check_parse_i32_invalid("2147483648");
    check_parse_i32_invalid("-2147483649");

    check_true(util_parse_i32(NULL, &dummy) == LB_STATUS_INVALID_ARG,
               "util_parse_i32(NULL) is rejected");
    check_true(util_parse_i32("1", NULL) == LB_STATUS_INVALID_ARG,
               "util_parse_i32(out=NULL) is rejected");
}

int main(void)
{
    test_safe_free();
    test_rng();
    test_sattolo();
    test_parse_u64();
    test_parse_i32();

    printf("test_util: %d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
