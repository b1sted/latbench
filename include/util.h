/* SPDX-License-Identifier: GPL-3.0 */
#ifndef LB_UTIL_H
#define LB_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "types.h"

/*
 * Free a heap pointer and clear it. The argument must be a modifiable pointer
 * lvalue; freeing NULL is safe, as with free() itself.
 */
#define LB_SAFE_FREE(ptr)                                                                          \
    do {                                                                                           \
        free(ptr);                                                                                 \
        (ptr) = NULL;                                                                              \
    } while (0)

/*
 * Deterministic xorshift64* generator. Returns zero when state is NULL;
 * otherwise advances *state and returns the next 64-bit value. A zero state is
 * replaced with a fixed nonzero seed before advancing so the sequence never
 * collapses into the xorshift fixed point.
 */
uint64_t util_rng_next(uint64_t *state);

/*
 * Fill indices[0 .. count-1] with a permutation containing exactly one cycle of
 * length count (Sattolo's algorithm). Every index in [0, count) is visited once.
 * count == 0 is a no-op and permits indices == NULL. The result is deterministic
 * for a given seed.
 */
lb_status_t util_sattolo_indices(size_t *indices, size_t count, uint64_t seed);

/*
 * Strict decimal parsers for CLI values. They reject empty input, leading or
 * trailing whitespace, non-numeric tails, and out-of-range values. A single
 * leading sign is accepted where the target type permits it ('+' for u64,
 * '+' or '-' for i32).
 */
lb_status_t util_parse_u64(const char *text, uint64_t *out);
lb_status_t util_parse_i32(const char *text, int32_t *out);

#endif /* LB_UTIL_H */
