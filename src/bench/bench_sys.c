/* SPDX-License-Identifier: GPL-3.0 */

#include "bench.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "defs.h"
#include "pal/compiler.h"
#include "pal/pal.h"
#include "tty.h"
#include "types.h"

#define SYS_TIMER_RESOLUTION_ATTEMPTS 1000
#define SYS_TIMER_RESOLUTION_SPIN_LIMIT 1000000u
#define SYS_TIMER_WARMUP_CALLS 1000u
#define SYS_TIMER_OVERHEAD_CALLS 100000u

/* sys throttle: report the sustained-load decay in fixed 500 ms intervals. */
#define SYS_THROTTLE_INTERVAL_MS 500u
#define SYS_THROTTLE_SHORT_INTERVALS 10u
#define SYS_THROTTLE_LONG_INTERVALS 20u

static uint64_t sys_bytes_to_mib(uint64_t bytes)
{
    return bytes / (UINT64_C(1024) * UINT64_C(1024));
}

static const char *sys_status_name(lb_status_t status)
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

static void sys_format_bytes_mib(uint64_t bytes, char *out, size_t capacity)
{
    char human[LB_NAME_MAX] = {0};

    if (out == NULL || capacity == 0u) {
        return;
    }

    (void)tty_format_bytes(bytes, human, sizeof(human));
    (void)snprintf(out, capacity, "%s (%" PRIu64 " MiB)", human, sys_bytes_to_mib(bytes));
}

static void sys_apply_runtime(const lb_cfg_t *cfg)
{
    if (cfg->runtime_applied) {
        return;
    }

    if (cfg->cpu_id >= 0) {
        const lb_status_t status = pal_pin_cpu(cfg->cpu_id);

        if (status != LB_STATUS_OK) {
            tty_warn("CPU pinning to %" PRId32 " was not applied (%s)", cfg->cpu_id,
                     sys_status_name(status));
        }
    }

    if (cfg->qos_set) {
        const lb_status_t status = pal_set_qos(cfg->qos_perf);

        if (status != LB_STATUS_OK) {
            tty_warn("QoS request '%s' was not applied (%s)", cfg->qos_perf ? "perf" : "bg",
                     sys_status_name(status));
        }
    }
}

static void sys_format_affinity_qos(const lb_cfg_t *cfg, char *out, size_t capacity)
{
    if (out == NULL || capacity == 0u) {
        return;
    }

    if (cfg->cpu_id >= 0 && cfg->qos_set) {
        (void)snprintf(out, capacity, "CPU %" PRId32 " / QoS %s", cfg->cpu_id,
                       cfg->qos_perf ? "perf" : "bg");
    } else if (cfg->cpu_id >= 0) {
        (void)snprintf(out, capacity, "CPU %" PRId32, cfg->cpu_id);
    } else if (cfg->qos_set) {
        (void)snprintf(out, capacity, "QoS %s", cfg->qos_perf ? "perf" : "bg");
    } else {
        (void)snprintf(out, capacity, "OS default");
    }
}

lb_status_t sys_info_run(const lb_cfg_t *cfg)
{
    char value[LB_LINE_MAX] = {0};
    char selectors[LB_LINE_MAX] = {0};
    uint64_t ram_bytes = 0u;
    uint64_t max_mem_bytes = 0u;
    size_t page_size = 0u;
    uint32_t cpu_count = 0u;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    sys_apply_runtime(cfg);

    page_size = pal_page_size();
    ram_bytes = pal_total_ram_bytes();
    max_mem_bytes = pal_safe_max_mem_bytes((uint64_t)cfg->max_mem_mb);
    cpu_count = pal_cpu_count();

    tty_csv_row(cfg, "module,subtest,metric,value,unit");

    tty_section(cfg, "Platform");
    tty_kv(cfg, "Architecture", "%s", pal_arch_name());
    if (page_size > 0u) {
        (void)tty_format_bytes((uint64_t)page_size, value, sizeof(value));
        tty_kv(cfg, "Page size", "%s (%zu bytes)", value, page_size);
    } else {
        tty_kv(cfg, "Page size", "unknown");
    }
    tty_csv_row(cfg, "sys,info,page_size,%zu,bytes", page_size);

    if (ram_bytes > 0u) {
        sys_format_bytes_mib(ram_bytes, value, sizeof(value));
        tty_kv(cfg, "Physical RAM", "%s", value);
    } else {
        tty_kv(cfg, "Physical RAM", "unknown");
    }
    tty_csv_row(cfg, "sys,info,total_ram,%" PRIu64 ",bytes", ram_bytes);

    tty_kv(cfg, "Logical CPUs", "%" PRIu32, cpu_count);
    tty_csv_row(cfg, "sys,info,cpu_count,%" PRIu32 ",count", cpu_count);

    tty_section(cfg, "Runtime");
    if (cfg->max_mem_mb > 0u) {
        tty_csv_row(cfg, "sys,info,requested_max_mem,%" PRIu32 ",mib", cfg->max_mem_mb);
    }

    if (max_mem_bytes > 0u) {
        sys_format_bytes_mib(max_mem_bytes, value, sizeof(value));
        tty_kv(cfg, "Arena limit", "%s", value);
    } else {
        tty_kv(cfg, "Arena limit", "unknown");
    }
    tty_csv_row(cfg, "sys,info,max_mem,%" PRIu64 ",bytes", max_mem_bytes);

    tty_kv(cfg, "THP policy", "%s", cfg->no_thp ? "no-thp" : "thp");
    tty_csv_row(cfg, "sys,info,thp_policy,%s,policy", cfg->no_thp ? "no-thp" : "thp");

    tty_kv(cfg, "Batch / cool", "%" PRIu32 " ms / %" PRIu32 " ms", cfg->batch_ms, cfg->cool_ms);
    tty_csv_row(cfg, "sys,info,batch_ms,%" PRIu32 ",ms", cfg->batch_ms);
    tty_csv_row(cfg, "sys,info,cool_ms,%" PRIu32 ",ms", cfg->cool_ms);

    sys_format_affinity_qos(cfg, selectors, sizeof(selectors));
    tty_kv(cfg, "Affinity / QoS", "%s", selectors);
    tty_csv_row(cfg, "sys,info,cpu_pin,%s,status",
                cfg->cpu_id >= 0 ? "requested" : "not-requested");
    tty_csv_row(cfg, "sys,info,cpu_id,%" PRId32 ",id", cfg->cpu_id);
    tty_csv_row(cfg, "sys,info,qos_mode,%s,mode",
                cfg->qos_set ? (cfg->qos_perf ? "perf" : "bg") : "none");
    tty_csv_row(cfg, "sys,info,qos_status,%s,status", cfg->qos_set ? "requested" : "not-requested");

    return LB_STATUS_OK;
}

static uint64_t sys_timer_min_step(void)
{
    uint64_t best = 0u;
    int attempt = 0;

    for (attempt = 0; attempt < SYS_TIMER_RESOLUTION_ATTEMPTS; attempt++) {
        const uint64_t start = pal_now_ns();
        uint64_t now = start;
        uint32_t spins = 0u;

        while (now == start && spins < SYS_TIMER_RESOLUTION_SPIN_LIMIT) {
            now = pal_now_ns();
            spins++;
        }
        if (now != start) {
            const uint64_t delta = now - start;

            if (best == 0u || delta < best) {
                best = delta;
            }
        }
    }
    return best;
}

static uint64_t sys_timer_measure_overhead(void)
{
    uint64_t last = 0u;
    uint64_t start = 0u;
    uint64_t end = 0u;
    uint32_t i = 0u;

    for (i = 0u; i < SYS_TIMER_WARMUP_CALLS; i++) {
        last = pal_now_ns();
        PAL_BARRIER();
    }
    PAL_KEEP_INT(last);

    start = pal_now_ns();
    for (i = 0u; i < SYS_TIMER_OVERHEAD_CALLS; i++) {
        last = pal_now_ns();
        PAL_BARRIER();
    }
    end = pal_now_ns();
    PAL_KEEP_INT(last);

    return (end >= start) ? (end - start) : 0u;
}

static void sys_format_ns_per_call(uint64_t total_ns, uint32_t calls, char *out, size_t capacity)
{
    uint64_t whole = 0u;
    uint64_t centi = 0u;

    if (out == NULL || capacity == 0u || calls == 0u) {
        return;
    }

    whole = total_ns / (uint64_t)calls;
    centi = ((total_ns % (uint64_t)calls) * 100u) / (uint64_t)calls;
    (void)snprintf(out, capacity, "%" PRIu64 ".%02" PRIu64, whole, centi);
}

lb_status_t sys_timer_run(const lb_cfg_t *cfg)
{
    char overhead[LB_LINE_MAX] = {0};
    char step[LB_LINE_MAX] = {0};
    char batch[LB_LINE_MAX] = {0};
    const uint64_t calls = (uint64_t)SYS_TIMER_OVERHEAD_CALLS;
    uint64_t resolution_ns = 0u;
    uint64_t total_ns = 0u;
    bool clock_limited = false;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    sys_apply_runtime(cfg);

    resolution_ns = sys_timer_min_step();
    total_ns = sys_timer_measure_overhead();
    sys_format_ns_per_call(total_ns, SYS_TIMER_OVERHEAD_CALLS, overhead, sizeof(overhead));
    clock_limited = (resolution_ns > 0u && resolution_ns <= total_ns / calls);

    tty_csv_row(cfg, "module,subtest,metric,value,unit");
    tty_section(cfg, "Timer");
    if (resolution_ns > 0u) {
        (void)tty_format_ns(resolution_ns, step, sizeof(step));
        if (clock_limited) {
            tty_kv(cfg, "Min step", "%s (bounded by call overhead)", step);
        } else {
            tty_kv(cfg, "Min step", "%s", step);
        }
    } else {
        tty_kv(cfg, "Min step", "not measurable");
    }
    tty_csv_row(cfg, "sys,timer,timer_resolution,%" PRIu64 ",ns", resolution_ns);

    tty_kv(cfg, "Call overhead", "%s ns/call", overhead);
    tty_csv_row(cfg, "sys,timer,now_ns_overhead,%s,ns/call", overhead);

    (void)tty_format_ns(total_ns, batch, sizeof(batch));
    tty_kv(cfg, "Batch total", "%s (%" PRIu64 " calls)", batch, calls);
    tty_csv_row(cfg, "sys,timer,now_ns_batch,%" PRIu32 ",calls",
                (uint32_t)SYS_TIMER_OVERHEAD_CALLS);
    tty_csv_row(cfg, "sys,timer,batch_total,%" PRIu64 ",ns", total_ns);
    tty_csv_row(cfg, "sys,timer,clock_step_le_overhead,%s,bool", clock_limited ? "true" : "false");

    return LB_STATUS_OK;
}

lb_status_t sys_throttle_run(const lb_cfg_t *cfg)
{
    uint32_t intervals = SYS_THROTTLE_SHORT_INTERVALS;
    double first_ops_per_us = 0.0;
    double min_pct = 100.0;
    double final_pct = 100.0;
    uint32_t index = 0u;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    sys_apply_runtime(cfg);

    if (cfg->throttle_long) {
        intervals = SYS_THROTTLE_LONG_INTERVALS;
    }

    tty_csv_row(cfg, "sys,throttle,interval,elapsed_s,ops_per_us,percent_of_first");
    tty_section(cfg, "Sustained-load throttle");
    tty_kv(cfg, "Load", "dependent ALU canary, no idle gaps");
    tty_kv(cfg, "Duration", "%" PRIu32 " ms (%" PRIu32 " intervals)",
           intervals * SYS_THROTTLE_INTERVAL_MS, intervals);
    tty_kv(cfg, "Interval", "%u ms", SYS_THROTTLE_INTERVAL_MS);
    tty_header(cfg, "  %8s %12s %12s", "Elapsed", "ops/us", "% first");

    for (index = 0u; index < intervals; index++) {
        const uint64_t ops = stats_canary_measure(SYS_THROTTLE_INTERVAL_MS);
        const double ops_per_us = (double)ops / ((double)SYS_THROTTLE_INTERVAL_MS * 1000.0);
        const double elapsed_s = (double)(index + 1u) * ((double)SYS_THROTTLE_INTERVAL_MS / 1000.0);
        double pct = 100.0;

        if (index == 0u) {
            first_ops_per_us = ops_per_us;
        }
        if (first_ops_per_us > 0.0) {
            pct = (ops_per_us / first_ops_per_us) * 100.0;
        }
        if (pct < min_pct) {
            min_pct = pct;
        }
        final_pct = pct;

        tty_header(cfg, "  %7.1f s %12.2f %11.1f%%", elapsed_s, ops_per_us, pct);
        tty_csv_row(cfg, "sys,throttle,%" PRIu32 ",%.1f,%.2f,%.1f", index + 1u, elapsed_s,
                    ops_per_us, pct);
    }

    tty_header(cfg, "  Minimum: %.1f%% (drop %.1f%%), final: %.1f%%", min_pct, 100.0 - min_pct,
               final_pct);
    tty_csv_row(cfg, "sys,throttle,minimum,%.1f,percent", min_pct);
    tty_csv_row(cfg, "sys,throttle,drop,%.1f,percent", 100.0 - min_pct);
    tty_csv_row(cfg, "sys,throttle,final,%.1f,percent", final_pct);

    return LB_STATUS_OK;
}
