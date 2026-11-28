/* SPDX-License-Identifier: GPL-3.0 */

#include "cli.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bench.h"
#include "defs.h"
#include "tty.h"
#include "types.h"
#include "util.h"

#define LB_DEFAULT_BATCH_MS 20u
#define LB_DEFAULT_COOL_MS 0u

static const lb_subtest_t SYS_SUBTESTS[] = {
    {"info", "platform limits and runtime settings", sys_info_run},
    {"timer", "clock resolution and pal_now_ns() overhead", sys_timer_run},
    {"throttle", "5-10 s sustained-load turbo and throttle decay", sys_throttle_run},
};

static const lb_subtest_t MEM_SUBTESTS[] = {
    {"size", "cache size from random pointer-chase latency", mem_size_run},
    {"line", "cache line size from stride latency", mem_line_run},
    {"assoc", "cache associativity from a set-conflict scan", mem_assoc_run},
    {"prefetch", "sequential, strided, and random prefetcher probes", mem_prefetch_run},
};

static const lb_subtest_t TLB_SUBTESTS[] = {
    {"size", "TLB capacity from a randomized page pointer chase", tlb_size_run},
    {"fault", "first-touch page-fault latency versus a warm second pass", tlb_fault_run},
};

static const lb_subtest_t CORE_SUBTESTS[] = {
    {"ilp", "dependent-chain throughput for ALU and FPU execution ports", core_ilp_run},
    {"mlp", "parallel pointer-chase throughput and memory-level parallelism", core_mlp_run},
    {"align", "unaligned read latency inside and across a cache line", core_align_run},
    {"stlf", "store-to-load forwarding latency and 4K aliasing", core_stlf_run},
};

static const lb_subtest_t BRANCH_SUBTESTS[] = {
    {"hist", "predictor history and mispredict penalty from branch patterns", br_hist_run},
    {"rsb", "return-stack buffer capacity from recursive call chains", br_rsb_run},
    {"btb", "BTB and L1i capacity from indirect-call trampolines", br_btb_run},
};

static const lb_subtest_t DRAM_SUBTESTS[] = {
    {"bw", "sequential read, write, and copy bandwidth", dram_bw_run},
    {"row", "DRAM row-buffer locality versus random page switches", dram_row_run},
    {"lat", "pure DRAM latency (THP) versus 4 KiB page-walk penalty", dram_lat_run},
};

static const lb_subtest_t TOPO_SUBTESTS[] = {
    {"atomic", "uncontended atomic fetch-add and sequential fence cost", topo_atomic_run},
    {"falseshare", "two-core false sharing versus counter byte offset", topo_falseshare_run},
    {"pingpong", "core-to-core token ping-pong one-way latency (RTT/2)", topo_pingpong_run},
};

static const lb_subtest_t OS_SUBTESTS[] = {
    {"syscall", "userland timer versus a guaranteed kernel write() transition", os_syscall_run},
    {"ctx", "thread or process context switch over a pipe ping-pong", os_ctx_run},
    {"wakeup", "scheduler wakeup cost with mutex and condition variable", os_wakeup_run},
    {"shootdown", "mprotect TLB shootdown cost, single and multi-thread", os_shootdown_run},
    {"fork", "fork, child _exit(0), and parent waitpid cost", os_fork_run},
    {"sig", "self-delivered SIGUSR1 signal round trip", os_sig_run},
    {"sleep", "nanosleep overshoot at 1 us and 100 us targets", os_sleep_run},
    {"swap", "pageout via MADV_PAGEOUT and random swap-in fault latency", os_swap_run},
};

static const lb_module_t BENCH_MODULES[] = {
    {"sys", "system calibration and clock diagnostics", SYS_SUBTESTS, LB_ARRAY_SIZE(SYS_SUBTESTS)},
    {"mem", "cache size, line, associativity, and prefetcher probes", MEM_SUBTESTS,
     LB_ARRAY_SIZE(MEM_SUBTESTS)},
    {"tlb", "TLB capacity and page-fault latency", TLB_SUBTESTS, LB_ARRAY_SIZE(TLB_SUBTESTS)},
    {"core", "pipeline ILP, MLP, alignment, and store-forwarding probes", CORE_SUBTESTS,
     LB_ARRAY_SIZE(CORE_SUBTESTS)},
    {"branch", "branch predictor, RSB, and BTB capacity probes", BRANCH_SUBTESTS,
     LB_ARRAY_SIZE(BRANCH_SUBTESTS)},
    {"dram", "memory bandwidth and DRAM row-buffer locality", DRAM_SUBTESTS,
     LB_ARRAY_SIZE(DRAM_SUBTESTS)},
    {"topo", "core-to-core atomics, false sharing, and ping-pong", TOPO_SUBTESTS,
     LB_ARRAY_SIZE(TOPO_SUBTESTS)},
    {"os", "syscall, context switch, shootdown, fork, signal, and sleep probes", OS_SUBTESTS,
     LB_ARRAY_SIZE(OS_SUBTESTS)},
};

const lb_module_t *lb_bench_modules(size_t *count)
{
    if (count != NULL) {
        *count = LB_ARRAY_SIZE(BENCH_MODULES);
    }
    return BENCH_MODULES;
}

void lb_cfg_init(lb_cfg_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    (void)memset(cfg, 0, sizeof(*cfg));
    cfg->cpu_id = -1;
    cfg->batch_ms = LB_DEFAULT_BATCH_MS;
    cfg->cool_ms = LB_DEFAULT_COOL_MS;
    cfg->no_thp = true;
    cfg->os_ctx_mode = LB_OS_CTX_BOTH;
}

const lb_module_t *lb_cli_find_module(const char *name)
{
    const lb_module_t *modules = NULL;
    size_t count = 0u;
    size_t i = 0u;

    modules = lb_bench_modules(&count);
    if (name == NULL) {
        return NULL;
    }

    for (i = 0u; i < count; i++) {
        if (strcmp(modules[i].name, name) == 0) {
            return &modules[i];
        }
    }
    return NULL;
}

const lb_subtest_t *lb_cli_find_subtest(const lb_module_t *module, const char *name)
{
    size_t i = 0u;

    if (module == NULL || name == NULL) {
        return NULL;
    }

    for (i = 0u; i < module->subtest_count; i++) {
        if (strcmp(module->subtests[i].name, name) == 0) {
            return &module->subtests[i];
        }
    }
    return NULL;
}

bool lb_cli_mode_valid(const char *module, const char *subtest, bool mode_is_topo)
{
    if (module == NULL || subtest == NULL) {
        return false;
    }
    if (strcmp(module, "topo") == 0 && strcmp(subtest, "pingpong") == 0) {
        return mode_is_topo;
    }
    if (strcmp(module, "os") == 0 && strcmp(subtest, "ctx") == 0) {
        return !mode_is_topo;
    }
    return false;
}

static lb_status_t parse_u32_option(const char *option, const char *text, uint32_t *out)
{
    uint64_t value = 0u;

    if (util_parse_u64(text, &value) != LB_STATUS_OK || value > (uint64_t)UINT32_MAX) {
        tty_err("invalid value for %s: '%s'", option, text);
        return LB_STATUS_INVALID_ARG;
    }

    *out = (uint32_t)value;
    return LB_STATUS_OK;
}

static lb_status_t parse_cpu_option(const char *text, int32_t *out)
{
    int32_t value = 0;
    const lb_status_t status = util_parse_i32(text, &value);

    if (status != LB_STATUS_OK || value < 0) {
        tty_err("invalid value for --cpu: '%s'", text);
        return LB_STATUS_INVALID_ARG;
    }

    *out = value;
    return LB_STATUS_OK;
}

static lb_status_t parse_qos_option(const char *text, lb_cfg_t *cfg)
{
    if (strcmp(text, "perf") == 0) {
        cfg->qos_perf = true;
    } else if (strcmp(text, "bg") == 0) {
        cfg->qos_perf = false;
    } else {
        tty_err("invalid value for --qos: '%s' (expected perf or bg)", text);
        return LB_STATUS_INVALID_ARG;
    }

    cfg->qos_set = true;
    return LB_STATUS_OK;
}

lb_status_t lb_cli_parse(int argc, char *const *argv, lb_cfg_t *cfg, lb_cli_result_t *out)
{
    const char *positionals[2] = {NULL, NULL};
    int positional_count = 0;
    int i = 0;
    bool threads_set = false;

    if (cfg == NULL || out == NULL || argc < 0 || (argc > 0 && argv == NULL)) {
        return LB_STATUS_INVALID_ARG;
    }
    (void)memset(out, 0, sizeof(*out));

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (arg == NULL) {
            return LB_STATUS_INVALID_ARG;
        }

        /*
         * Keep scanning after --help so a later --no-color still reaches the
         * renderer. Help output is printed after parsing completes.
         */
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            out->show_help = true;
            continue;
        }
        if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
            out->show_version = true;
            return LB_STATUS_OK;
        }
        if (strcmp(arg, "--no-color") == 0) {
            cfg->no_color = true;
            tty_set_no_color(true);
            continue;
        }
        if (strcmp(arg, "--csv") == 0) {
            cfg->csv = true;
            continue;
        }
        if (strcmp(arg, "--no-thp") == 0) {
            cfg->no_thp = true;
            cfg->thp_set = true;
            continue;
        }
        if (strcmp(arg, "--thp") == 0) {
            cfg->no_thp = false;
            cfg->thp_set = true;
            continue;
        }
        if (strcmp(arg, "--write") == 0) {
            cfg->write = true;
            continue;
        }
        if (strcmp(arg, "--long") == 0) {
            cfg->throttle_long = true;
            continue;
        }
        if (strcmp(arg, "--no-timing") == 0) {
            cfg->selftest_no_timing = true;
            continue;
        }
        if (strcmp(arg, "--mode") == 0) {
            if (i + 1 >= argc || argv[i + 1] == NULL) {
                tty_err("option %s requires a value", arg);
                return LB_STATUS_INVALID_ARG;
            }
            i++;
            if (strcmp(argv[i], "cas") == 0) {
                cfg->topo_pingpong_cas = true;
                out->mode_set = true;
                out->mode_is_topo = true;
            } else if (strcmp(argv[i], "flag") == 0) {
                cfg->topo_pingpong_cas = false;
                out->mode_set = true;
                out->mode_is_topo = true;
            } else if (strcmp(argv[i], "thread") == 0) {
                cfg->os_ctx_mode = LB_OS_CTX_THREAD;
                out->mode_set = true;
                out->mode_is_topo = false;
            } else if (strcmp(argv[i], "proc") == 0) {
                cfg->os_ctx_mode = LB_OS_CTX_PROC;
                out->mode_set = true;
                out->mode_is_topo = false;
            } else {
                tty_err("invalid value for --mode: '%s' (expected cas, flag, thread, or proc)",
                        argv[i]);
                return LB_STATUS_INVALID_ARG;
            }
            continue;
        }

        if (strcmp(arg, "--cpu") == 0 || strcmp(arg, "--qos") == 0 ||
            strcmp(arg, "--max-mem") == 0 || strcmp(arg, "--batch-ms") == 0 ||
            strcmp(arg, "--cool-ms") == 0 || strcmp(arg, "--threads") == 0) {
            const char *value = NULL;

            if (i + 1 >= argc || argv[i + 1] == NULL) {
                tty_err("option %s requires a value", arg);
                return LB_STATUS_INVALID_ARG;
            }
            i++;
            value = argv[i];

            if (strcmp(arg, "--cpu") == 0) {
                if (parse_cpu_option(value, &cfg->cpu_id) != LB_STATUS_OK) {
                    return LB_STATUS_INVALID_ARG;
                }
            } else if (strcmp(arg, "--qos") == 0) {
                if (parse_qos_option(value, cfg) != LB_STATUS_OK) {
                    return LB_STATUS_INVALID_ARG;
                }
            } else if (strcmp(arg, "--max-mem") == 0) {
                if (parse_u32_option(arg, value, &cfg->max_mem_mb) != LB_STATUS_OK) {
                    return LB_STATUS_INVALID_ARG;
                }
            } else if (strcmp(arg, "--batch-ms") == 0) {
                if (parse_u32_option(arg, value, &cfg->batch_ms) != LB_STATUS_OK) {
                    return LB_STATUS_INVALID_ARG;
                }
            } else if (strcmp(arg, "--threads") == 0) {
                if (parse_u32_option(arg, value, &cfg->threads) != LB_STATUS_OK) {
                    return LB_STATUS_INVALID_ARG;
                }
                if (cfg->threads == 0u) {
                    tty_err("invalid value for --threads: '%s' (must be >= 1)", value);
                    return LB_STATUS_INVALID_ARG;
                }
                threads_set = true;
            } else {
                if (parse_u32_option(arg, value, &cfg->cool_ms) != LB_STATUS_OK) {
                    return LB_STATUS_INVALID_ARG;
                }
            }
            continue;
        }

        if (arg[0] == '-') {
            tty_err("unknown option '%s'", arg);
            return LB_STATUS_INVALID_ARG;
        }
        if (positional_count >= 2) {
            tty_err("unexpected argument '%s'", arg);
            return LB_STATUS_INVALID_ARG;
        }

        positionals[positional_count] = arg;
        positional_count++;
    }

    out->module_name = positionals[0];
    out->subtest_name = positionals[1];

    if (threads_set && !out->show_help) {
        if (positionals[0] == NULL || positionals[1] == NULL ||
            strcmp(positionals[0], "dram") != 0 || strcmp(positionals[1], "bw") != 0) {
            tty_err("--threads is only valid for 'dram bw'");
            return LB_STATUS_INVALID_ARG;
        }
    }
    return LB_STATUS_OK;
}
