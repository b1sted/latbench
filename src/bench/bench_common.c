/* SPDX-License-Identifier: GPL-3.0 */

#include "bench_common.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pal/pal.h"
#include "stats.h"
#include "tty.h"
#include "types.h"

static const char *lb_bench_status_name(lb_status_t status)
{
    switch (status) {
    case LB_STATUS_OK:
        return "ok";
    case LB_STATUS_ERROR:
        return "error";
    case LB_STATUS_INVALID_ARG:
        return "invalid-arg";
    case LB_STATUS_NOMEM:
        return "nomem";
    case LB_STATUS_IO:
        return "io";
    case LB_STATUS_UNSUPPORTED:
        return "unsupported";
    default:
        return "unknown";
    }
}

void lb_bench_apply_runtime(const lb_cfg_t *cfg)
{
    if (cfg == NULL || cfg->runtime_applied) {
        return;
    }

    if (cfg->cpu_id >= 0) {
        const lb_status_t status = pal_pin_cpu(cfg->cpu_id);

        if (status != LB_STATUS_OK) {
            tty_warn("CPU pinning to %" PRId32 " was not applied (%s)", cfg->cpu_id,
                     lb_bench_status_name(status));
        }
    }

    if (cfg->qos_set) {
        const lb_status_t status = pal_set_qos(cfg->qos_perf);

        if (status != LB_STATUS_OK) {
            tty_warn("QoS request '%s' was not applied (%s)", cfg->qos_perf ? "perf" : "bg",
                     lb_bench_status_name(status));
        }
    }
}

size_t lb_bench_arena_cap(const lb_cfg_t *cfg, uint64_t limit_bytes, bool reserve_indices)
{
    uint64_t budget = 0u;

    if (cfg == NULL) {
        return 0u;
    }

    budget = pal_safe_max_mem_bytes((uint64_t)cfg->max_mem_mb);
    if (budget == 0u) {
        budget = LB_BENCH_FALLBACK_BUDGET_BYTES;
    }
    if (reserve_indices) {
        /* Leave one ninth of the budget for Sattolo/permutation arrays. */
        budget = (budget / 9u) * 8u;
    }
    if (budget > limit_bytes) {
        budget = limit_bytes;
    }
#if SIZE_MAX < UINT64_MAX
    if (budget > (uint64_t)SIZE_MAX) {
        budget = (uint64_t)SIZE_MAX;
    }
#endif
    return (size_t)budget;
}

uint32_t lb_bench_arena_flags(const lb_cfg_t *cfg)
{
    if (cfg == NULL) {
        return PAL_MEM_NO_THP;
    }
    return cfg->no_thp ? PAL_MEM_NO_THP : PAL_MEM_THP;
}

void lb_bench_cool(const lb_cfg_t *cfg)
{
    if (cfg != NULL && cfg->cool_ms > 0u) {
        pal_sleep_ms(cfg->cool_ms);
    }
}

void lb_bench_canary_finish(const lb_cfg_t *cfg, uint64_t baseline, bool warn_drift)
{
    const uint64_t after = stats_canary_measure(LB_BENCH_CANARY_MS);
    const double drift = stats_canary_drift_pct(baseline, after);

    if (warn_drift) {
        tty_warn_drift(cfg, drift);
    }
    tty_kv(cfg, "DVFS drift", "%+.1f%%", drift);
}
