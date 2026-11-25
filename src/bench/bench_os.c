/* SPDX-License-Identifier: GPL-3.0 */

#include "bench.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "pal/compiler.h"
#include "pal/pal.h"
#include "pal/pal_os.h"
#include "passport.h"
#include "stats.h"
#include "tty.h"
#include "types.h"

/* Duration of each DVFS frequency-drift canary measurement. */
#define OS_CANARY_MS 20u

/* os syscall: userland clock reads versus a guaranteed kernel transition. */
#define OS_SYSCALL_CALLS 20000u

/* os ctx: pipe round trips for the thread and process ping-pong probes. */
#define OS_CTX_ROUNDS 2000u

/* os wakeup: mutex and condition-variable round trips. */
#define OS_WAKEUP_ROUNDS 5000u

/* os shootdown: mprotect permission flips per mode. */
#define OS_SHOOTDOWN_ITERATIONS 2000u

/* os fork: fork, _exit, and waitpid batches. */
#define OS_FORK_ITERATIONS 200u

/* os sig: self-delivered SIGUSR1 round trips. */
#define OS_SIG_ITERATIONS 20000u

/* os sleep: samples per target and the two requested targets. */
#define OS_SLEEP_SAMPLES 500u
#define OS_SLEEP_TARGET_SHORT_US 1u
#define OS_SLEEP_TARGET_LONG_US 100u

/* Compact auto mode: shorter batches keep the passport run inside its budget. */
#define OS_FAST_SYSCALL_CALLS 10000u
#define OS_FAST_CTX_ROUNDS 500u
#define OS_FAST_WAKEUP_ROUNDS 1000u
#define OS_FAST_SHOOTDOWN_ITERATIONS 500u
#define OS_FAST_FORK_ITERATIONS 50u
#define OS_FAST_SIG_ITERATIONS 5000u
#define OS_FAST_SLEEP_SAMPLES 100u

#define OS_NS_PER_US 1000.0

static void os_apply_runtime(const lb_cfg_t *cfg)
{
    if (cfg->runtime_applied) {
        return;
    }

    if (cfg->cpu_id >= 0) {
        if (pal_pin_cpu(cfg->cpu_id) != LB_STATUS_OK) {
            tty_warn("CPU pinning to %" PRId32 " was not applied", cfg->cpu_id);
        }
    }

    if (cfg->qos_set) {
        if (pal_set_qos(cfg->qos_perf) != LB_STATUS_OK) {
            tty_warn("QoS request '%s' was not applied", cfg->qos_perf ? "perf" : "bg");
        }
    }
}

static void os_canary_finish(const lb_cfg_t *cfg, uint64_t baseline)
{
    const uint64_t after = stats_canary_measure(OS_CANARY_MS);
    const double drift = stats_canary_drift_pct(baseline, after);

    /*
     * Sleep/wakeup/fork probes deliberately block and reschedule, so big.LITTLE
     * systems migrate the thread between small and large cores. That migration
     * moves the canary far more than a DVFS change, and warning about it on
     * every run is pure noise. Keep the measured drift visible as a key/value
     * line, but do not emit a [WARN] for these probes.
     */
    tty_kv(cfg, "DVFS drift", "%+.1f%%", drift);
}

/* --------------------------------------------------------------- os syscall */

lb_status_t os_syscall_run(const lb_cfg_t *cfg)
{
    pal_os_syscall_result_t result = {0};
    uint32_t calls = 0u;
    uint64_t baseline = 0u;
    double timer_ns = 0.0;
    double syscall_ns = 0.0;
    double delta_ns = 0.0;
    lb_status_t status = LB_STATUS_OK;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    calls = cfg->fast ? OS_FAST_SYSCALL_CALLS : OS_SYSCALL_CALLS;

    os_apply_runtime(cfg);
    tty_csv_row(cfg, "os,syscall,path,ns_per_call");
    tty_section(cfg, "OS syscall");
    tty_kv(cfg, "Calls", "%" PRIu32, calls);
    baseline = stats_canary_baseline(OS_CANARY_MS);

    status = pal_os_syscall_probe(calls, &result);
    if (status != LB_STATUS_OK || result.calls == 0u) {
        tty_err("os syscall: probe failed (status %d)", (int)status);
        return (status != LB_STATUS_OK) ? status : LB_STATUS_ERROR;
    }

    timer_ns = (double)result.timer_ns / (double)result.calls;
    syscall_ns = (double)result.syscall_ns / (double)result.calls;
    delta_ns = syscall_ns - timer_ns;
    lb_passport_mutable()->os_syscall_ns = syscall_ns;

    tty_header(cfg, "  %-24s %14s", "Path", "ns/call");
    tty_header(cfg, "  %-24s %14.2f", "vDSO/timer", timer_ns);
    tty_header(cfg, "  %-24s %14.2f", "kernel syscall (write)", syscall_ns);
    tty_kv(cfg, "Delta", "%.2f ns/call", delta_ns);
    tty_csv_row(cfg, "os,syscall,vdso_timer,%.2f", timer_ns);
    tty_csv_row(cfg, "os,syscall,kernel_write,%.2f", syscall_ns);
    tty_csv_row(cfg, "os,syscall,delta,%.2f", delta_ns);

    os_canary_finish(cfg, baseline);
    return LB_STATUS_OK;
}

/* ------------------------------------------------------------------- os ctx */

static lb_status_t os_ctx_run_one(const lb_cfg_t *cfg, const char *name, bool proc, uint32_t rounds)
{
    pal_os_ctx_result_t result = {0};
    double us_per_switch = 0.0;
    lb_status_t status = LB_STATUS_OK;

    status = proc ? pal_os_ctx_proc(rounds, &result) : pal_os_ctx_thread(rounds, &result);
    if (status != LB_STATUS_OK || result.switches == 0u) {
        tty_err("os ctx %s: probe failed (status %d)", name, (int)status);
        return (status != LB_STATUS_OK) ? status : LB_STATUS_ERROR;
    }

    us_per_switch = ((double)result.elapsed_ns / (double)result.switches) / OS_NS_PER_US;
    if (proc) {
        lb_passport_mutable()->os_ctx_proc_us = us_per_switch;
    } else {
        lb_passport_mutable()->os_ctx_thread_us = us_per_switch;
    }
    tty_header(cfg, "  %-10s %12" PRIu64 " %14.3f", name, result.switches, us_per_switch);
    tty_csv_row(cfg, "os,ctx,%s,%.3f", name, us_per_switch);
    return LB_STATUS_OK;
}

lb_status_t os_ctx_run(const lb_cfg_t *cfg)
{
    bool run_thread = false;
    bool run_proc = false;
    uint32_t rounds = 0u;
    uint64_t baseline = 0u;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    rounds = cfg->fast ? OS_FAST_CTX_ROUNDS : OS_CTX_ROUNDS;

    os_apply_runtime(cfg);
    run_thread = (cfg->os_ctx_mode == LB_OS_CTX_BOTH) || (cfg->os_ctx_mode == LB_OS_CTX_THREAD);
    run_proc = (cfg->os_ctx_mode == LB_OS_CTX_BOTH) || (cfg->os_ctx_mode == LB_OS_CTX_PROC);

    tty_csv_row(cfg, "os,ctx,mode,us_per_switch");
    tty_section(cfg, "OS context switch");
    tty_kv(cfg, "Rounds", "%" PRIu32 " per mode", rounds);
    tty_kv(cfg, "Mechanism", "1-byte pipe ping-pong");
    tty_header(cfg, "  %-10s %12s %14s", "Mode", "Switches", "us/switch");
    baseline = stats_canary_baseline(OS_CANARY_MS);

    if (run_thread) {
        const lb_status_t status = os_ctx_run_one(cfg, "thread", false, rounds);

        if (status != LB_STATUS_OK) {
            return status;
        }
    }
    if (run_proc) {
        const lb_status_t status = os_ctx_run_one(cfg, "proc", true, rounds);

        if (status != LB_STATUS_OK) {
            return status;
        }
    }

    os_canary_finish(cfg, baseline);
    return LB_STATUS_OK;
}

/* ---------------------------------------------------------------- os wakeup */

lb_status_t os_wakeup_run(const lb_cfg_t *cfg)
{
    pal_os_wakeup_result_t result = {0};
    uint32_t rounds = 0u;
    uint64_t baseline = 0u;
    double us_per_wakeup = 0.0;
    lb_status_t status = LB_STATUS_OK;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    rounds = cfg->fast ? OS_FAST_WAKEUP_ROUNDS : OS_WAKEUP_ROUNDS;

    os_apply_runtime(cfg);
    tty_csv_row(cfg, "os,wakeup,metric,value,unit");
    tty_section(cfg, "OS wakeup");
    tty_kv(cfg, "Rounds", "%" PRIu32, rounds);
    tty_kv(cfg, "Mechanism", "pthread_mutex + pthread_cond_signal");
    baseline = stats_canary_baseline(OS_CANARY_MS);

    status = pal_os_wakeup(rounds, &result);
    if (status != LB_STATUS_OK || result.wakeups == 0u) {
        tty_err("os wakeup: probe failed (status %d)", (int)status);
        return (status != LB_STATUS_OK) ? status : LB_STATUS_ERROR;
    }

    us_per_wakeup = ((double)result.elapsed_ns / (double)result.wakeups) / OS_NS_PER_US;
    lb_passport_mutable()->os_wakeup_us = us_per_wakeup;
    tty_kv(cfg, "Wakeups", "%" PRIu64, result.wakeups);
    tty_kv(cfg, "Latency", "%.3f us/wakeup", us_per_wakeup);
    tty_csv_row(cfg, "os,wakeup,us_per_wakeup,%.3f,us", us_per_wakeup);

    os_canary_finish(cfg, baseline);
    return LB_STATUS_OK;
}

/* ------------------------------------------------------------- os shootdown */

lb_status_t os_shootdown_run(const lb_cfg_t *cfg)
{
    pal_os_shootdown_result_t result = {0};
    uint32_t iterations = 0u;
    uint64_t baseline = 0u;
    double single_us = 0.0;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    iterations = cfg->fast ? OS_FAST_SHOOTDOWN_ITERATIONS : OS_SHOOTDOWN_ITERATIONS;

    os_apply_runtime(cfg);
    tty_csv_row(cfg, "os,shootdown,mode,us_per_mprotect");
    tty_section(cfg, "OS shootdown");
    tty_kv(cfg, "Iterations", "%" PRIu32, iterations);
    tty_kv(cfg, "Page", "%zu bytes", pal_page_size());
    baseline = stats_canary_baseline(OS_CANARY_MS);

    {
        const lb_status_t status = pal_os_shootdown(iterations, &result);

        if (status != LB_STATUS_OK || result.single_ops == 0u) {
            tty_err("os shootdown: probe failed (status %d)", (int)status);
            return (status != LB_STATUS_OK) ? status : LB_STATUS_ERROR;
        }
    }

    single_us = ((double)result.single_ns / (double)result.single_ops) / OS_NS_PER_US;
    lb_passport_mutable()->os_shootdown_single_us = single_us;
    tty_header(cfg, "  %-16s %14.3f us/mprotect", "1 thread", single_us);
    tty_csv_row(cfg, "os,shootdown,single,%.3f", single_us);

    if (result.multi_skipped) {
        tty_kv(cfg, "Multi-thread", "skipped (need at least 2 CPUs)");
        tty_csv_row(cfg, "os,shootdown,multi,skipped");
    } else if (result.multi_ops > 0u) {
        const double multi_us = ((double)result.multi_ns / (double)result.multi_ops) / OS_NS_PER_US;
        const double growth = (single_us > 0.0) ? (multi_us / single_us) : 0.0;

        lb_passport_mutable()->os_shootdown_multi_us = multi_us;
        tty_header(cfg, "  %-16s %14.3f us/mprotect", "multi-thread", multi_us);
        tty_kv(cfg, "Background", "%" PRIu32 " threads", result.background_threads);
        tty_kv(cfg, "Growth", "%.2fx", growth);
        tty_csv_row(cfg, "os,shootdown,multi,%.3f", multi_us);
        tty_csv_row(cfg, "os,shootdown,growth,%.2f", growth);
    } else {
        tty_warn("os shootdown: multi-thread probe produced no samples");
    }

    os_canary_finish(cfg, baseline);
    return LB_STATUS_OK;
}

/* ------------------------------------------------------------------ os fork */

lb_status_t os_fork_run(const lb_cfg_t *cfg)
{
    pal_os_fork_result_t result = {0};
    uint32_t iterations = 0u;
    uint64_t baseline = 0u;
    double us_per_fork = 0.0;
    lb_status_t status = LB_STATUS_OK;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    iterations = cfg->fast ? OS_FAST_FORK_ITERATIONS : OS_FORK_ITERATIONS;

    os_apply_runtime(cfg);
    tty_csv_row(cfg, "os,fork,metric,value,unit");
    tty_section(cfg, "OS fork");
    tty_kv(cfg, "Batch", "%" PRIu32 " forks", iterations);
    tty_kv(cfg, "Path", "fork + _exit(0) + waitpid");
    baseline = stats_canary_baseline(OS_CANARY_MS);

    status = pal_os_fork_once(iterations, &result);
    if (status != LB_STATUS_OK || result.forks == 0u) {
        tty_err("os fork: probe failed (status %d)", (int)status);
        return (status != LB_STATUS_OK) ? status : LB_STATUS_ERROR;
    }

    us_per_fork = ((double)result.elapsed_ns / (double)result.forks) / OS_NS_PER_US;
    lb_passport_mutable()->os_fork_us = us_per_fork;
    tty_kv(cfg, "Forks", "%" PRIu64, result.forks);
    tty_kv(cfg, "Latency", "%.3f us/fork", us_per_fork);
    tty_csv_row(cfg, "os,fork,us_per_fork,%.3f,us", us_per_fork);

    os_canary_finish(cfg, baseline);
    return LB_STATUS_OK;
}

/* ------------------------------------------------------------------- os sig */

lb_status_t os_sig_run(const lb_cfg_t *cfg)
{
    pal_os_sig_result_t result = {0};
    uint32_t iterations = 0u;
    uint64_t baseline = 0u;
    double ns_per_signal = 0.0;
    double us_per_signal = 0.0;
    lb_status_t status = LB_STATUS_OK;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    iterations = cfg->fast ? OS_FAST_SIG_ITERATIONS : OS_SIG_ITERATIONS;

    os_apply_runtime(cfg);
    tty_csv_row(cfg, "os,sig,metric,value,unit");
    tty_section(cfg, "OS signal");
    tty_kv(cfg, "Signals", "%" PRIu32, iterations);
    tty_kv(cfg, "Path", "pthread_kill(pthread_self(), SIGUSR1)");
    baseline = stats_canary_baseline(OS_CANARY_MS);

    status = pal_os_sig_roundtrip(iterations, &result);
    if (status != LB_STATUS_OK || result.signals == 0u) {
        tty_err("os sig: probe failed (status %d)", (int)status);
        return (status != LB_STATUS_OK) ? status : LB_STATUS_ERROR;
    }

    ns_per_signal = (double)result.elapsed_ns / (double)result.signals;
    us_per_signal = ns_per_signal / OS_NS_PER_US;
    tty_kv(cfg, "Delivered", "%" PRIu64, result.signals);
    tty_kv(cfg, "Latency", "%.2f ns/signal (%.3f us)", ns_per_signal, us_per_signal);
    tty_csv_row(cfg, "os,sig,ns_per_signal,%.2f,ns", ns_per_signal);

    os_canary_finish(cfg, baseline);
    return LB_STATUS_OK;
}

/* ----------------------------------------------------------------- os sleep */

static lb_status_t os_sleep_run_target(const lb_cfg_t *cfg, uint32_t target_us, uint32_t samples)
{
    uint64_t raw[OS_SLEEP_SAMPLES] = {0};
    double values[OS_SLEEP_SAMPLES] = {0.0};
    size_t count = 0u;
    size_t i = 0u;
    double p10_ns = 0.0;
    double p50_ns = 0.0;
    double p90_ns = 0.0;
    double overshoot_us = 0.0;
    lb_status_t status = LB_STATUS_OK;

    status = pal_os_sleep_us(target_us, samples, raw, (size_t)OS_SLEEP_SAMPLES, &count);
    if (status != LB_STATUS_OK || count == 0u) {
        tty_err("os sleep %" PRIu32 " us: probe failed (status %d)", target_us, (int)status);
        return (status != LB_STATUS_OK) ? status : LB_STATUS_ERROR;
    }

    for (i = 0u; i < count; i++) {
        values[i] = (double)raw[i];
    }
    p10_ns = stats_percentile(values, count, 10.0);
    p50_ns = stats_percentile(values, count, 50.0);
    p90_ns = stats_percentile(values, count, 90.0);
    overshoot_us = (p50_ns - ((double)target_us * OS_NS_PER_US)) / OS_NS_PER_US;
    if (target_us == OS_SLEEP_TARGET_SHORT_US) {
        lb_passport_mutable()->os_sleep_1us_p50_us = p50_ns / OS_NS_PER_US;
    }

    tty_header(cfg, "  %8" PRIu32 " us %12.3f %12.3f %12.3f %12.3f", target_us,
               p10_ns / OS_NS_PER_US, p50_ns / OS_NS_PER_US, p90_ns / OS_NS_PER_US, overshoot_us);
    tty_csv_row(cfg, "os,sleep,%" PRIu32 ",%.3f,%.3f,%.3f,%.3f", target_us, p10_ns / OS_NS_PER_US,
                p50_ns / OS_NS_PER_US, p90_ns / OS_NS_PER_US, overshoot_us);
    return LB_STATUS_OK;
}

lb_status_t os_sleep_run(const lb_cfg_t *cfg)
{
    uint32_t samples = 0u;
    uint64_t baseline = 0u;
    lb_status_t status = LB_STATUS_OK;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    samples = cfg->fast ? OS_FAST_SLEEP_SAMPLES : OS_SLEEP_SAMPLES;

    os_apply_runtime(cfg);
    tty_csv_row(cfg, "os,sleep,target_us,p10_us,p50_us,p90_us,overshoot_us");
    tty_section(cfg, "OS sleep");
    tty_kv(cfg, "Samples", "%" PRIu32 " per target", samples);
    tty_header(cfg, "  %10s %12s %12s %12s %12s", "Target", "p10", "p50", "p90", "overshoot");
    baseline = stats_canary_baseline(OS_CANARY_MS);

    status = os_sleep_run_target(cfg, OS_SLEEP_TARGET_SHORT_US, samples);
    if (status != LB_STATUS_OK) {
        return status;
    }
    status = os_sleep_run_target(cfg, OS_SLEEP_TARGET_LONG_US, samples);
    if (status != LB_STATUS_OK) {
        return status;
    }

    os_canary_finish(cfg, baseline);
    return LB_STATUS_OK;
}
