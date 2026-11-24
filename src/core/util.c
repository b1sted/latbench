/* SPDX-License-Identifier: GPL-3.0 */

#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static bool util_is_digit(char c)
{
    return c >= '0' && c <= '9';
}

uint64_t util_rng_next(uint64_t *state)
{
    uint64_t x = 0u;

    if (state == NULL) {
        return 0u;
    }

    x = *state;
    if (x == 0u) {
        x = UINT64_C(0x9e3779b97f4a7c15);
    }

    x ^= x >> 12u;
    x ^= x << 25u;
    x ^= x >> 27u;

    *state = x;
    return x * UINT64_C(0x2545f4914f6cdd1d);
}

lb_status_t util_sattolo_indices(size_t *indices, size_t count, uint64_t seed)
{
    uint64_t state = 0u;
    size_t i = 0u;

    if (indices == NULL) {
        return (count == 0u) ? LB_STATUS_OK : LB_STATUS_INVALID_ARG;
    }

    for (i = 0u; i < count; i++) {
        indices[i] = i;
    }
    if (count < 2u) {
        return LB_STATUS_OK;
    }

    state = seed;
    for (i = count - 1u; i > 0u; i--) {
        const size_t j = (size_t)(util_rng_next(&state) % (uint64_t)i);
        const size_t tmp = indices[i];

        indices[i] = indices[j];
        indices[j] = tmp;
    }
    return LB_STATUS_OK;
}

lb_status_t util_parse_u64(const char *text, uint64_t *out)
{
    char *end = NULL;
    unsigned long long value = 0ull;
    size_t start = 0u;

    if (text == NULL || out == NULL || text[0] == '\0') {
        return LB_STATUS_INVALID_ARG;
    }

    if (text[0] == '+') {
        start = 1u;
    }
    if (!util_is_digit(text[start])) {
        return LB_STATUS_INVALID_ARG;
    }

    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0') {
        return LB_STATUS_INVALID_ARG;
    }
#if ULLONG_MAX > UINT64_MAX
    if (value > (unsigned long long)UINT64_MAX) {
        return LB_STATUS_INVALID_ARG;
    }
#endif

    *out = (uint64_t)value;
    return LB_STATUS_OK;
}

lb_status_t util_parse_i32(const char *text, int32_t *out)
{
    char *end = NULL;
    long long value = 0ll;
    size_t start = 0u;

    if (text == NULL || out == NULL || text[0] == '\0') {
        return LB_STATUS_INVALID_ARG;
    }

    if (text[0] == '+' || text[0] == '-') {
        start = 1u;
    }
    if (!util_is_digit(text[start])) {
        return LB_STATUS_INVALID_ARG;
    }

    errno = 0;
    value = strtoll(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0') {
        return LB_STATUS_INVALID_ARG;
    }
    if (value < (long long)INT32_MIN || value > (long long)INT32_MAX) {
        return LB_STATUS_INVALID_ARG;
    }

    *out = (int32_t)value;
    return LB_STATUS_OK;
}
