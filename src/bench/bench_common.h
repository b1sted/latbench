/* SPDX-License-Identifier: GPL-3.0 */
#ifndef LB_BENCH_COMMON_H
#define LB_BENCH_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"

/* Duration of each DVFS frequency-drift canary measurement. */
#define LB_BENCH_CANARY_MS 20u

/* Arena policy when the platform cannot report the amount of physical RAM. */
#define LB_BENCH_FALLBACK_BUDGET_BYTES (UINT64_C(4) * UINT64_C(1024) * UINT64_C(1024))

/*
 * Apply --cpu and --qos once per run. Probes call this before measuring; when
 * the configuration already has runtime_applied set (for example because
 * "auto" applied it for the whole run), the helper is a no-op.
 */
void lb_bench_apply_runtime(const lb_cfg_t *cfg);

/*
 * Clip the requested arena to limit_bytes and to the platform's safe memory
 * budget. reserve_indices keeps one ninth of the budget free for Sattolo or
 * permutation index arrays.
 */
size_t lb_bench_arena_cap(const lb_cfg_t *cfg, uint64_t limit_bytes, bool reserve_indices);

/* Translate the --thp / --no-thp policy into pal_mem_alloc() flags. */
uint32_t lb_bench_arena_flags(const lb_cfg_t *cfg);

/* Sleep for --cool-ms when a cool-down was requested. */
void lb_bench_cool(const lb_cfg_t *cfg);

/*
 * Finish a DVFS canary run and report the drift. warn_drift is false for probes
 * that deliberately block and reschedule, where big.LITTLE migration moves the
 * canary far more than a DVFS change; those still print the measured value but
 * skip the [WARN] line.
 */
void lb_bench_canary_finish(const lb_cfg_t *cfg, uint64_t baseline, bool warn_drift);

#endif /* LB_BENCH_COMMON_H */
