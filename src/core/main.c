/* SPDX-License-Identifier: GPL-3.0 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "auto.h"
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

/* Branch sanity: an unpredictable 50/50 pattern must pay a mispredict penalty. */
#define LB_SELFTEST_BRANCH_PATTERN_BYTES 65536u
#define LB_SELFTEST_BRANCH_ITERATIONS UINT64_C(4000000)
#define LB_SELFTEST_BRANCH_WARMUP UINT64_C(200000)
#define LB_SELFTEST_BRANCH_ROUNDS 3u
/* In-order cores pay a smaller absolute mispredict penalty on this loop, so a
 * cmov conversion still sits far below this floor while real branches exceed
 * it. The 50/50 pattern is ~1.3x on ARMv7 and ~3.5x on x86-64. */
#define LB_SELFTEST_BRANCH_RATIO_MIN 1.2

/* Core ILP sanity: four independent chains must beat a single chain. */
#define LB_SELFTEST_ILP_ITERATIONS UINT64_C(8000000)
#define LB_SELFTEST_ILP_WARMUP UINT64_C(200000)
#define LB_SELFTEST_ILP_ROUNDS 3u
#define LB_SELFTEST_ILP_CHAINS 4u
#define LB_SELFTEST_ILP_GAIN_MIN 1.5
#define LB_SELFTEST_ILP_MUL UINT64_C(6364136223846793005)
#define LB_SELFTEST_ILP_ADD UINT64_C(1)

/* Two leading spaces plus a 22-column label: descriptions start at column 25. */
#define HELP_LINE_FMT "  %-22s%s"

typedef struct lb_selftest_node_t {
    struct lb_selftest_node_t *next;
} lb_selftest_node_t;

/* Published by the selftest probes so the optimizer keeps the dependent work. */
static volatile uint64_t g_lb_selftest_sink = 0u;

/* Branch-pattern storage shared by the constant and 50/50 sanity walks. */
static uint8_t g_lb_selftest_branch_pattern[LB_SELFTEST_BRANCH_PATTERN_BYTES] = {0};

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

static PAL_NOINLINE PAL_NO_VECTORIZE uint64_t lb_selftest_branch_walk(const uint8_t *pattern,
                                                                      uint64_t iterations)
{
    uint64_t a = UINT64_C(0x9e3779b97f4a7c15);
    uint64_t b = UINT64_C(0xbb67ae8584caa73b);
    uint64_t i = 0u;

    PAL_NO_VECTORIZE_LOOP
    for (i = 0u; i < iterations; i++) {
        const uint8_t bit = pattern[(size_t)i & (LB_SELFTEST_BRANCH_PATTERN_BYTES - 1u)];

        if (bit != 0u) {
            PAL_BLOCK_IFCONV_T();
            a = (a + b) ^ (i + UINT64_C(0x2545f4914f6cdd1d));
        } else {
            PAL_BLOCK_IFCONV_F();
            b = (b + a) ^ (i + UINT64_C(0x6a09e667f3bcc909));
        }
    }

    PAL_BARRIER();
    g_lb_selftest_sink = a ^ b;
    return g_lb_selftest_sink;
}

static double lb_selftest_branch_time(const uint8_t *pattern)
{
    uint64_t best = UINT64_MAX;
    size_t round = 0u;

    (void)lb_selftest_branch_walk(pattern, LB_SELFTEST_BRANCH_WARMUP);
    for (round = 0u; round < LB_SELFTEST_BRANCH_ROUNDS; round++) {
        const uint64_t begin = pal_now_ns();
        uint64_t end = 0u;
        uint64_t elapsed = 0u;

        (void)lb_selftest_branch_walk(pattern, LB_SELFTEST_BRANCH_ITERATIONS);
        end = pal_now_ns();
        elapsed = (end > begin) ? (end - begin) : 0u;
        if (elapsed > 0u && elapsed < best) {
            best = elapsed;
        }
    }

    return (best == UINT64_MAX) ? 0.0 : (double)best;
}

static bool lb_selftest_branch_check(const lb_cfg_t *cfg)
{
    double const_ns = 0.0;
    double random_ns = 0.0;
    double ratio = 0.0;
    uint64_t state = UINT64_C(0x243f6a8885a308d3);
    size_t i = 0u;

    memset(g_lb_selftest_branch_pattern, 0, sizeof(g_lb_selftest_branch_pattern));
    const_ns = lb_selftest_branch_time(g_lb_selftest_branch_pattern);

    for (i = 0u; i < LB_SELFTEST_BRANCH_PATTERN_BYTES; i++) {
        g_lb_selftest_branch_pattern[i] = (uint8_t)(util_rng_next(&state) & UINT64_C(1));
    }
    random_ns = lb_selftest_branch_time(g_lb_selftest_branch_pattern);

    if (const_ns <= 0.0 || random_ns <= 0.0) {
        tty_err("selftest: branch sanity could not be timed");
        return false;
    }

    ratio = random_ns / const_ns;
    tty_header(cfg, "  Branch pattern: const %.3f ms, 50/50 %.3f ms, ratio %.2fx", const_ns / 1.0e6,
               random_ns / 1.0e6, ratio);

    if (ratio < LB_SELFTEST_BRANCH_RATIO_MIN) {
        tty_err("selftest: branch ratio %.2fx is below %.1fx (cmov/csel conversion?)", ratio,
                LB_SELFTEST_BRANCH_RATIO_MIN);
        return false;
    }
    return true;
}

static PAL_NOINLINE PAL_NO_VECTORIZE uint64_t lb_selftest_ilp_k1(uint64_t iterations)
{
    uint64_t a0 = UINT64_C(1);
    uint64_t i = 0u;

    PAL_NO_VECTORIZE_LOOP
    for (i = 0u; i < iterations; i++) {
        a0 = (a0 * LB_SELFTEST_ILP_MUL) + LB_SELFTEST_ILP_ADD;
        PAL_KEEP_INT(a0);
    }

    PAL_BARRIER();
    g_lb_selftest_sink = a0;
    return a0;
}

static PAL_NOINLINE PAL_NO_VECTORIZE uint64_t lb_selftest_ilp_k4(uint64_t iterations)
{
    uint64_t a0 = UINT64_C(1);
    uint64_t a1 = UINT64_C(3);
    uint64_t a2 = UINT64_C(5);
    uint64_t a3 = UINT64_C(7);
    uint64_t i = 0u;

    PAL_NO_VECTORIZE_LOOP
    for (i = 0u; i < iterations; i++) {
        a0 = (a0 * LB_SELFTEST_ILP_MUL) + LB_SELFTEST_ILP_ADD;
        a1 = (a1 * LB_SELFTEST_ILP_MUL) + LB_SELFTEST_ILP_ADD;
        a2 = (a2 * LB_SELFTEST_ILP_MUL) + LB_SELFTEST_ILP_ADD;
        a3 = (a3 * LB_SELFTEST_ILP_MUL) + LB_SELFTEST_ILP_ADD;
        PAL_KEEP_INT(a0);
        PAL_KEEP_INT(a1);
        PAL_KEEP_INT(a2);
        PAL_KEEP_INT(a3);
    }

    PAL_BARRIER();
    g_lb_selftest_sink = a0 + a1 + a2 + a3;
    return g_lb_selftest_sink;
}

static double lb_selftest_ilp_rate(uint64_t (*fn)(uint64_t), size_t chains)
{
    uint64_t best = UINT64_MAX;
    size_t round = 0u;

    (void)fn(LB_SELFTEST_ILP_WARMUP);
    for (round = 0u; round < LB_SELFTEST_ILP_ROUNDS; round++) {
        const uint64_t begin = pal_now_ns();
        uint64_t end = 0u;
        uint64_t elapsed = 0u;

        (void)fn(LB_SELFTEST_ILP_ITERATIONS);
        end = pal_now_ns();
        elapsed = (end > begin) ? (end - begin) : 0u;
        if (elapsed > 0u && elapsed < best) {
            best = elapsed;
        }
    }

    if (best == UINT64_MAX) {
        return 0.0;
    }
    return ((double)LB_SELFTEST_ILP_ITERATIONS * (double)chains * 1.0e9) / (double)best;
}

static bool lb_selftest_ilp_check(const lb_cfg_t *cfg)
{
    const double rate_one = lb_selftest_ilp_rate(lb_selftest_ilp_k1, 1u);
    const double rate_four = lb_selftest_ilp_rate(lb_selftest_ilp_k4, LB_SELFTEST_ILP_CHAINS);
    double gain = 0.0;

    if (!(rate_one > 0.0) || !(rate_four > 0.0)) {
        tty_err("selftest: core ilp sanity could not be timed");
        return false;
    }

    gain = rate_four / rate_one;
    tty_header(cfg, "  Core ILP: K=1 %.1f Mops/s, K=%u %.1f Mops/s, gain %.2fx", rate_one / 1.0e6,
               LB_SELFTEST_ILP_CHAINS, rate_four / 1.0e6, gain);

    if (gain < LB_SELFTEST_ILP_GAIN_MIN) {
        if (!pal_ilp_scaling_expected()) {
            tty_header(cfg, "  Core ILP: no parallel scaling expected on this in-order core");
            return true;
        }
        tty_err("selftest: core ilp gain %.2fx is below %.1fx (chains serialized?)", gain,
                LB_SELFTEST_ILP_GAIN_MIN);
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
    if (!lb_selftest_branch_check(cfg)) {
        ok = false;
    }
    if (!lb_selftest_ilp_check(cfg)) {
        ok = false;
    }

    if (!ok) {
        tty_err("selftest FAILED");
        return LB_STATUS_ERROR;
    }

    tty_header(cfg, "selftest: PASS");
    return LB_STATUS_OK;
}

static void help_line(const lb_cfg_t *cfg, const char *label, const char *text)
{
    tty_header(cfg, HELP_LINE_FMT, label, text);
}

static bool module_has_arena(const char *name)
{
    return strcmp(name, "sys") == 0 || strcmp(name, "mem") == 0 || strcmp(name, "tlb") == 0 ||
           strcmp(name, "core") == 0 || strcmp(name, "dram") == 0;
}

static bool module_has_thp_policy(const char *name)
{
    return strcmp(name, "mem") == 0 || strcmp(name, "tlb") == 0 || strcmp(name, "core") == 0 ||
           strcmp(name, "dram") == 0;
}

static void module_subtest_summary(const lb_module_t *module, char *out, size_t capacity)
{
    size_t used = 0u;
    size_t i = 0u;

    if (module == NULL || out == NULL || capacity == 0u) {
        return;
    }

    out[0] = '\0';
    for (i = 0u; i < module->subtest_count; i++) {
        const int written = snprintf(out + used, capacity - used, "%s%s", (i == 0u) ? "" : ", ",
                                     module->subtests[i].name);

        if (written < 0 || (size_t)written >= capacity - used) {
            break;
        }
        used += (size_t)written;
    }
}

static void print_module_options(const lb_cfg_t *cfg, const lb_module_t *module)
{
    tty_section(cfg, "Options");
    help_line(cfg, "--cpu <id>", "pin worker to logical CPU <id>");
    help_line(cfg, "--batch-ms <ms>", "measurement batch duration (default: 20)");
    help_line(cfg, "--cool-ms <ms>", "cool-down between batches (default: 0)");

    if (module_has_arena(module->name)) {
        help_line(cfg, "--max-mem <MiB>", "cap arena size (default: 25% of RAM)");
    }
    if (module_has_thp_policy(module->name)) {
        help_line(cfg, "--no-thp / --thp",
                  "avoid or allow transparent huge pages (default: no-thp)");
    }
    if (strcmp(module->name, "sys") == 0) {
        help_line(cfg, "--qos <perf|bg>", "request perf or bg QoS");
        help_line(cfg, "--long", "extend sys throttle duration to 10 seconds");
    }
    if (strcmp(module->name, "mem") == 0) {
        help_line(cfg, "--write", "modify words during mem size walk (dirty write-back)");
    }
    if (strcmp(module->name, "topo") == 0) {
        help_line(cfg, "--mode <mode>", "handoff mode (cas|flag, default flag)");
    }
    if (strcmp(module->name, "os") == 0) {
        help_line(cfg, "--mode <mode>", "context switch mode (thread|proc, default both)");
    }

    help_line(cfg, "--csv", "emit CSV to stdout; diagnostics to stderr");
    help_line(cfg, "--no-color", "disable color output");
    help_line(cfg, "-h, --help", "display this help and exit");
    help_line(cfg, "-V, --version", "output version information and exit");
}

static void print_help(const lb_cfg_t *cfg)
{
    const lb_module_t *modules = NULL;
    size_t module_count = 0u;
    size_t i = 0u;

    modules = lb_bench_modules(&module_count);

    tty_section(cfg, "Usage");
    tty_header(cfg, "  latbench <module> <subtest> [OPTIONS]");
    tty_header(cfg, "  latbench <module> --help");
    tty_header(cfg, "  latbench auto [OPTIONS]");

    tty_section(cfg, "Commands");
    help_line(cfg, "auto", "run compact full-machine suite and print Passport");
    help_line(cfg, "selftest", "verify Sattolo, pointer-chase, branch, and ILP at -O2");

    tty_section(cfg, "Options");
    help_line(cfg, "--cpu <id>", "pin worker to logical CPU <id>");
    help_line(cfg, "--qos <perf|bg>", "request perf or bg QoS");
    help_line(cfg, "--max-mem <MiB>", "cap arena size (default: 25% of RAM)");
    help_line(cfg, "--batch-ms <ms>", "measurement batch duration (default: 20)");
    help_line(cfg, "--cool-ms <ms>", "cool-down between batches (default: 0)");
    help_line(cfg, "--no-thp / --thp", "avoid or allow transparent huge pages (default: no-thp)");
    help_line(cfg, "--write", "modify words during mem size walk (dirty write-back)");
    help_line(cfg, "--long", "extend sys throttle duration to 10 seconds");
    help_line(cfg, "--mode <mode>", "handoff/switch mode (cas|flag, thread|proc)");
    help_line(cfg, "--csv", "emit CSV to stdout; diagnostics to stderr");
    help_line(cfg, "--no-color", "disable color output");
    help_line(cfg, "-h, --help", "display this help and exit");
    help_line(cfg, "-V, --version", "output version information and exit");

    tty_section(cfg, "Modules");
    for (i = 0u; i < module_count; i++) {
        char summary[LB_LINE_MAX] = {0};

        module_subtest_summary(&modules[i], summary, sizeof(summary));
        help_line(cfg, modules[i].name, summary);
    }
}

static void print_module_help(const lb_cfg_t *cfg, const lb_module_t *module)
{
    size_t i = 0u;

    if (module == NULL) {
        return;
    }

    tty_section(cfg, "Usage");
    tty_header(cfg, "  latbench %s <subtest> [OPTIONS]", module->name);
    tty_header(cfg, "  latbench %s --help", module->name);

    tty_section(cfg, "Description");
    tty_header(cfg, "  %s", module->help);

    tty_section(cfg, "Subtests");
    for (i = 0u; i < module->subtest_count; i++) {
        help_line(cfg, module->subtests[i].name, module->subtests[i].help);
    }

    print_module_options(cfg, module);
}

static void print_command_help(const lb_cfg_t *cfg, const char *command)
{
    const bool is_auto = (strcmp(command, "auto") == 0);

    tty_section(cfg, "Usage");
    tty_header(cfg, "  latbench %s [OPTIONS]", command);

    tty_section(cfg, "Description");
    tty_header(cfg, "  %s",
               is_auto ? "run compact full-machine suite and print Passport"
                       : "verify Sattolo, pointer-chase, branch, and ILP at -O2");

    tty_section(cfg, "Options");
    help_line(cfg, "--cpu <id>", "pin worker to logical CPU <id>");
    help_line(cfg, "--batch-ms <ms>", "measurement batch duration (default: 20)");
    help_line(cfg, "--cool-ms <ms>", "cool-down between batches (default: 0)");
    help_line(cfg, "--max-mem <MiB>", "cap arena size (default: 25% of RAM)");
    help_line(cfg, "--no-thp / --thp", "avoid or allow transparent huge pages (default: no-thp)");
    help_line(cfg, "--csv", "emit CSV to stdout; diagnostics to stderr");
    help_line(cfg, "--no-color", "disable color output");
    help_line(cfg, "-h, --help", "display this help and exit");
    help_line(cfg, "-V, --version", "output version information and exit");
}

static void print_help_to_stderr(const lb_cfg_t *cfg)
{
    lb_cfg_t err_cfg = *cfg;

    err_cfg.csv = true;
    print_help(&err_cfg);
}

static void print_module_help_to_stderr(const lb_cfg_t *cfg, const lb_module_t *module)
{
    lb_cfg_t err_cfg = *cfg;

    err_cfg.csv = true;
    print_module_help(&err_cfg, module);
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
            break;
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
        if (strcmp(arg, "--long") == 0) {
            cfg->throttle_long = true;
            continue;
        }
        if (strcmp(arg, "--mode") == 0) {
            if (i + 1 >= argc) {
                tty_err("option %s requires a value", arg);
                return LB_STATUS_INVALID_ARG;
            }
            i++;
            if (strcmp(argv[i], "cas") == 0) {
                cfg->topo_pingpong_cas = true;
            } else if (strcmp(argv[i], "flag") == 0) {
                cfg->topo_pingpong_cas = false;
            } else if (strcmp(argv[i], "thread") == 0) {
                cfg->os_ctx_mode = LB_OS_CTX_THREAD;
            } else if (strcmp(argv[i], "proc") == 0) {
                cfg->os_ctx_mode = LB_OS_CTX_PROC;
            } else {
                tty_err("invalid value for --mode: '%s' (expected cas, flag, thread, or proc)",
                        argv[i]);
                return LB_STATUS_INVALID_ARG;
            }
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
    cfg.throttle_long = false;
    cfg.topo_pingpong_cas = false;
    cfg.fast = false;
    cfg.auto_mode = false;
    cfg.quiet = false;
    cfg.runtime_applied = false;
    cfg.os_ctx_mode = LB_OS_CTX_BOTH;

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

    if (module_name == NULL) {
        if (show_help || argc == 1) {
            print_help(&cfg);
            return 0;
        }
        tty_err("missing module name");
        print_help_to_stderr(&cfg);
        return 2;
    }

    if (strcmp(module_name, "selftest") == 0) {
        if (show_help) {
            print_command_help(&cfg, "selftest");
            return 0;
        }
        if (subtest_name != NULL) {
            tty_err("selftest does not take a subtest");
            print_help_to_stderr(&cfg);
            return 2;
        }
        return (lb_selftest_run(&cfg) == LB_STATUS_OK) ? 0 : 1;
    }

    if (strcmp(module_name, "auto") == 0) {
        if (show_help) {
            print_command_help(&cfg, "auto");
            return 0;
        }
        if (subtest_name != NULL) {
            tty_err("auto does not take a subtest");
            print_help_to_stderr(&cfg);
            return 2;
        }
        return (lb_auto_run(&cfg) == LB_STATUS_OK) ? 0 : 1;
    }

    module = find_module(module_name);
    if (module == NULL) {
        tty_err("unknown module '%s'", module_name);
        print_help_to_stderr(&cfg);
        return 2;
    }

    if (show_help || subtest_name == NULL) {
        print_module_help(&cfg, module);
        return 0;
    }

    subtest = find_subtest(module, subtest_name);
    if (subtest == NULL) {
        tty_err("unknown subtest '%s' for module '%s'", subtest_name, module_name);
        print_module_help_to_stderr(&cfg, module);
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
