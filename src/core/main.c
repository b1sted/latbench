/* SPDX-License-Identifier: GPL-3.0 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bench.h"
#include "defs.h"
#include "pal/compiler.h"
#include "pal/pal.h"
#include "tty.h"
#include "types.h"
#include "util.h"

#define LB_VERSION "0.1.0"
#define LB_DEFAULT_BATCH_MS 20u
#define LB_DEFAULT_COOL_MS 0u

#define LB_SELFTEST_MAX_NODES 4096u
#define LB_SELFTEST_CHAIN_NODES 4096u
#define LB_SELFTEST_CHASE_BASE UINT64_C(1048576)
#define LB_SELFTEST_CHASE_ROUNDS 3u
#define LB_SELFTEST_RATIO_MIN 1.6

#define HELP_FLAG_FMT "  %-22s %s"

typedef struct lb_selftest_node_t {
    struct lb_selftest_node_t *next;
} lb_selftest_node_t;

/* Published by the selftest chase so the optimizer keeps the dependent loads. */
static volatile uint64_t g_lb_selftest_sink = 0u;

static const lb_subtest_t SYS_SUBTESTS[] = {
    {"info", "platform limits and runtime settings", sys_info_run},
    {"timer", "clock resolution and pal_now_ns() overhead", sys_timer_run},
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

static const lb_module_t BENCH_MODULES[] = {
    {"sys", "system calibration and clock diagnostics", SYS_SUBTESTS, LB_ARRAY_SIZE(SYS_SUBTESTS)},
    {"mem", "cache size, line, associativity, and prefetcher probes", MEM_SUBTESTS,
     LB_ARRAY_SIZE(MEM_SUBTESTS)},
    {"tlb", "TLB capacity and page-fault latency", TLB_SUBTESTS, LB_ARRAY_SIZE(TLB_SUBTESTS)},
    {"core", "pipeline ILP, MLP, alignment, and store-forwarding probes", CORE_SUBTESTS,
     LB_ARRAY_SIZE(CORE_SUBTESTS)},
};

const lb_module_t *lb_bench_modules(size_t *count)
{
    if (count != NULL) {
        *count = LB_ARRAY_SIZE(BENCH_MODULES);
    }
    return BENCH_MODULES;
}

static const lb_module_t *find_module(const char *name)
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

static const lb_subtest_t *find_subtest(const lb_module_t *module, const char *name)
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

static bool lb_selftest_sattolo_check(const lb_cfg_t *cfg)
{
    static const size_t LB_SELFTEST_SIZES[] = {1u, 2u, 3u, 16u, 257u, 4096u};
    size_t indices[LB_SELFTEST_MAX_NODES] = {0};
    uint8_t seen[LB_SELFTEST_MAX_NODES] = {0};
    bool ok = true;
    size_t size_index = 0u;

    for (size_index = 0u; size_index < LB_ARRAY_SIZE(LB_SELFTEST_SIZES); size_index++) {
        const size_t count = LB_SELFTEST_SIZES[size_index];
        const uint64_t seed =
            UINT64_C(0x9e3779b97f4a7c15) ^ ((uint64_t)count * UINT64_C(0x2545f4914f6cdd1d));
        bool size_ok = true;
        size_t i = 0u;

        if (util_sattolo_indices(indices, count, seed) != LB_STATUS_OK) {
            tty_err("selftest: sattolo count=%zu failed", count);
            ok = false;
            continue;
        }

        memset(seen, 0, count * sizeof(seen[0]));
        for (i = 0u; i < count; i++) {
            if (indices[i] >= count || seen[indices[i]] != 0u) {
                size_ok = false;
                break;
            }
            seen[indices[i]] = 1u;
        }

        if (size_ok) {
            size_t cursor = 0u;

            memset(seen, 0, count * sizeof(seen[0]));
            for (i = 0u; i < count; i++) {
                if (cursor >= count || seen[cursor] != 0u) {
                    size_ok = false;
                    break;
                }
                seen[cursor] = 1u;
                cursor = indices[cursor];
            }
            if (size_ok && cursor != 0u) {
                size_ok = false;
            }
        }

        if (!size_ok) {
            tty_err("selftest: sattolo count=%zu is not a single cycle", count);
            ok = false;
        }
    }

    if (ok) {
        tty_header(cfg, "  Sattolo single-cycle: %zu sizes ok", LB_ARRAY_SIZE(LB_SELFTEST_SIZES));
    }
    return ok;
}

static bool lb_selftest_build_chain(lb_selftest_node_t *nodes, size_t count)
{
    size_t order[LB_SELFTEST_MAX_NODES] = {0};
    size_t i = 0u;

    if (nodes == NULL || count == 0u || count > LB_SELFTEST_MAX_NODES) {
        return false;
    }
    if (util_sattolo_indices(order, count, UINT64_C(0x13198a2e03707344) ^ (uint64_t)count) !=
        LB_STATUS_OK) {
        return false;
    }

    for (i = 0u; i < count; i++) {
        nodes[i].next = &nodes[order[i]];
    }
    return true;
}

static PAL_NOINLINE uint64_t lb_selftest_chase(const lb_selftest_node_t *start, uint64_t hops)
{
    const lb_selftest_node_t *p = start;
    uint64_t done = 0u;

    while ((done + UINT64_C(16)) <= hops) {
        LB_UNROLL_8(p = p->next);
        LB_UNROLL_8(p = p->next);
        done += UINT64_C(16);
    }
    while (done < hops) {
        p = p->next;
        done++;
    }

    PAL_BARRIER();
    g_lb_selftest_sink = (uint64_t)(uintptr_t)p;
    return g_lb_selftest_sink;
}

static bool lb_selftest_chase_check(const lb_cfg_t *cfg)
{
    static lb_selftest_node_t nodes[LB_SELFTEST_CHAIN_NODES];
    const uint64_t base_hops = LB_SELFTEST_CHASE_BASE;
    uint64_t best_one = UINT64_MAX;
    uint64_t best_two = UINT64_MAX;
    double ratio = 0.0;
    size_t round = 0u;

    if (!lb_selftest_build_chain(nodes, LB_SELFTEST_CHAIN_NODES)) {
        tty_err("selftest: cannot build the pointer-chase chain");
        return false;
    }

    /* Warm the chain, its cache lines, and the clock path before timing. */
    (void)lb_selftest_chase(&nodes[0], (uint64_t)LB_SELFTEST_CHAIN_NODES);

    for (round = 0u; round < LB_SELFTEST_CHASE_ROUNDS; round++) {
        uint64_t begin = pal_now_ns();
        uint64_t end = 0u;
        uint64_t elapsed = 0u;

        (void)lb_selftest_chase(&nodes[0], base_hops);
        end = pal_now_ns();
        elapsed = (end > begin) ? (end - begin) : 0u;
        if (elapsed < best_one) {
            best_one = elapsed;
        }

        begin = pal_now_ns();
        (void)lb_selftest_chase(&nodes[0], base_hops * UINT64_C(2));
        end = pal_now_ns();
        elapsed = (end > begin) ? (end - begin) : 0u;
        if (elapsed < best_two) {
            best_two = elapsed;
        }
    }

    if (best_one == 0u || best_two == 0u) {
        tty_err("selftest: pointer chase was optimized away");
        return false;
    }

    ratio = (double)best_two / (double)best_one;
    tty_header(cfg, "  Pointer chase: %.3f ms @ %" PRIu64 ", %.3f ms @ %" PRIu64 ", ratio %.2fx",
               (double)best_one / 1.0e6, base_hops, (double)best_two / 1.0e6,
               base_hops * UINT64_C(2), ratio);

    if (ratio < LB_SELFTEST_RATIO_MIN) {
        tty_err("selftest: pointer-chase ratio %.2fx is below %.1fx (hot loop folded?)", ratio,
                LB_SELFTEST_RATIO_MIN);
        return false;
    }
    return true;
}

static lb_status_t lb_selftest_run(const lb_cfg_t *cfg)
{
    bool ok = true;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    tty_section(cfg, "Selftest");
    if (!lb_selftest_sattolo_check(cfg)) {
        ok = false;
    }
    if (!lb_selftest_chase_check(cfg)) {
        ok = false;
    }

    if (!ok) {
        tty_err("selftest FAILED");
        return LB_STATUS_ERROR;
    }

    tty_header(cfg, "selftest: PASS");
    return LB_STATUS_OK;
}

static void print_help(const lb_cfg_t *cfg)
{
    const lb_module_t *modules = NULL;
    size_t module_count = 0u;
    size_t i = 0u;

    modules = lb_bench_modules(&module_count);

    tty_header(cfg, "Usage: latbench <module> <subtest> [OPTIONS]");
    tty_header(cfg, "%s", "");

    tty_section(cfg, "Runtime");
    tty_header(cfg, HELP_FLAG_FMT, "    --cpu <id>", "pin worker to logical CPU <id>");
    tty_header(cfg, HELP_FLAG_FMT, "    --qos <perf|bg>", "request perf or bg QoS");
    tty_header(cfg, HELP_FLAG_FMT, "    --max-mem <MiB>", "cap arena size (default: 25% of RAM)");
    tty_header(cfg, HELP_FLAG_FMT, "    --batch-ms <ms>",
               "measurement batch duration (default: 20)");
    tty_header(cfg, HELP_FLAG_FMT, "    --cool-ms <ms>", "cool-down between batches (default: 0)");
    tty_header(cfg, HELP_FLAG_FMT, "    --no-thp", "avoid transparent huge pages (default)");
    tty_header(cfg, HELP_FLAG_FMT, "    --thp", "allow transparent huge pages");

    tty_section(cfg, "Output");
    tty_header(cfg, HELP_FLAG_FMT, "    --csv", "emit CSV to stdout; diagnostics to stderr");
    tty_header(cfg, HELP_FLAG_FMT, "    --no-color", "disable color output");

    tty_section(cfg, "Memory");
    tty_header(cfg, HELP_FLAG_FMT, "    --write",
               "modify words on the mem size walk (dirty write-back)");

    tty_section(cfg, "System");
    tty_header(cfg, HELP_FLAG_FMT, "-h, --help", "display this help and exit");
    tty_header(cfg, HELP_FLAG_FMT, "-V, --version", "output version information and exit");

    tty_section(cfg, "Diagnostics");
    tty_header(cfg, HELP_FLAG_FMT, "    selftest",
               "verify Sattolo integrity and -O2 pointer-chase viability");

    tty_section(cfg, "Modules");
    for (i = 0u; i < module_count; i++) {
        size_t j = 0u;

        for (j = 0u; j < modules[i].subtest_count; j++) {
            char label[LB_NAME_MAX] = {0};

            (void)snprintf(label, sizeof(label), "%s %s", modules[i].name,
                           modules[i].subtests[j].name);
            tty_header(cfg, HELP_FLAG_FMT, label, modules[i].subtests[j].help);
        }
    }
}

static void print_help_to_stderr(const lb_cfg_t *cfg)
{
    lb_cfg_t err_cfg = *cfg;

    err_cfg.csv = true;
    print_help(&err_cfg);
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

static lb_status_t parse_args(int argc, char **argv, lb_cfg_t *cfg, bool *show_help,
                              bool *show_version, const char **module_name,
                              const char **subtest_name)
{
    const char *positionals[2] = {NULL, NULL};
    int positional_count = 0;
    int i = 0;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            *show_help = true;
            return LB_STATUS_OK;
        }
        if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
            *show_version = true;
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
            continue;
        }
        if (strcmp(arg, "--thp") == 0) {
            cfg->no_thp = false;
            continue;
        }
        if (strcmp(arg, "--write") == 0) {
            cfg->write = true;
            continue;
        }

        if (strcmp(arg, "--cpu") == 0 || strcmp(arg, "--qos") == 0 ||
            strcmp(arg, "--max-mem") == 0 || strcmp(arg, "--batch-ms") == 0 ||
            strcmp(arg, "--cool-ms") == 0) {
            const char *value = NULL;

            if (i + 1 >= argc) {
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

    *module_name = positionals[0];
    *subtest_name = positionals[1];
    return LB_STATUS_OK;
}

int main(int argc, char **argv)
{
    lb_cfg_t cfg = {0};
    bool show_help = false;
    bool show_version = false;
    const char *module_name = NULL;
    const char *subtest_name = NULL;
    const lb_module_t *module = NULL;
    const lb_subtest_t *subtest = NULL;
    lb_status_t status = LB_STATUS_OK;

    cfg.cpu_id = -1;
    cfg.qos_perf = false;
    cfg.qos_set = false;
    cfg.max_mem_mb = 0u;
    cfg.batch_ms = LB_DEFAULT_BATCH_MS;
    cfg.cool_ms = LB_DEFAULT_COOL_MS;
    cfg.no_thp = true;
    cfg.csv = false;
    cfg.no_color = false;
    cfg.write = false;

    status = parse_args(argc, argv, &cfg, &show_help, &show_version, &module_name, &subtest_name);
    if (status != LB_STATUS_OK) {
        print_help_to_stderr(&cfg);
        return 2;
    }

    tty_set_no_color(cfg.no_color);

    if (show_version) {
        tty_header(NULL, "latbench v%s (%s)", LB_VERSION, pal_arch_name());
        return 0;
    }

    if (show_help) {
        print_help(&cfg);
        return 0;
    }

    if (module_name == NULL) {
        if (argc == 1) {
            print_help(&cfg);
            return 0;
        }
        tty_err("missing module name");
        print_help_to_stderr(&cfg);
        return 2;
    }

    if (strcmp(module_name, "selftest") == 0) {
        if (subtest_name != NULL) {
            tty_err("selftest does not take a subtest");
            print_help_to_stderr(&cfg);
            return 2;
        }
        return (lb_selftest_run(&cfg) == LB_STATUS_OK) ? 0 : 1;
    }

    if (subtest_name == NULL) {
        tty_err("missing subtest for module '%s'", module_name);
        print_help_to_stderr(&cfg);
        return 2;
    }

    module = find_module(module_name);
    if (module == NULL) {
        tty_err("unknown module '%s'", module_name);
        print_help_to_stderr(&cfg);
        return 2;
    }

    subtest = find_subtest(module, subtest_name);
    if (subtest == NULL) {
        tty_err("unknown subtest '%s' for module '%s'", subtest_name, module_name);
        print_help_to_stderr(&cfg);
        return 2;
    }

    if (subtest->run == NULL) {
        tty_err("subtest '%s %s' is not implemented", module_name, subtest_name);
        return 1;
    }

    status = subtest->run(&cfg);
    if (status != LB_STATUS_OK) {
        tty_err("subtest '%s %s' failed (status %d)", module_name, subtest_name, (int)status);
        return 1;
    }

    return 0;
}
