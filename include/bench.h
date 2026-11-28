/* SPDX-License-Identifier: GPL-3.0 */
#ifndef LB_BENCH_H
#define LB_BENCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"

/*
 * Repeat one statement eight times. The statement must be a plain expression
 * without control flow; benchmark hot loops use it to amortise loop overhead
 * while keeping the timing structure visible in the source.
 */
#define LB_UNROLL_8(stmt)                                                                          \
    do {                                                                                           \
        stmt;                                                                                      \
        stmt;                                                                                      \
        stmt;                                                                                      \
        stmt;                                                                                      \
        stmt;                                                                                      \
        stmt;                                                                                      \
        stmt;                                                                                      \
        stmt;                                                                                      \
    } while (0)

/* Entry point signature shared by every benchmark subcommand. */
/* NOLINTNEXTLINE(readability-identifier-naming): the public name follows the PAL API style. */
typedef lb_status_t (*lb_subtest_fn)(const lb_cfg_t *cfg);

/* One runnable subtest inside a benchmark module (for example "sys timer"). */
typedef struct lb_subtest_t {
    const char *name;
    const char *help;
    lb_subtest_fn run;
} lb_subtest_t;

/* A benchmark module groups related subtests (for example "sys"). */
typedef struct lb_module_t {
    const char *name;
    const char *help;
    const lb_subtest_t *subtests;
    size_t subtest_count;
} lb_module_t;

/*
 * Return the module registry. The returned array is static and must not be
 * modified. When count is non-NULL it receives the number of modules.
 */
const lb_module_t *lb_bench_modules(size_t *count);

/* System calibration and clock subtests, implemented in src/bench/bench_sys.c. */
lb_status_t sys_info_run(const lb_cfg_t *cfg);
lb_status_t sys_timer_run(const lb_cfg_t *cfg);
lb_status_t sys_throttle_run(const lb_cfg_t *cfg);

/* Cache subtests, implemented in src/bench/bench_mem.c. */
lb_status_t mem_size_run(const lb_cfg_t *cfg);
lb_status_t mem_line_run(const lb_cfg_t *cfg);
lb_status_t mem_assoc_run(const lb_cfg_t *cfg);
lb_status_t mem_prefetch_run(const lb_cfg_t *cfg);

/* TLB and page-fault subtests, implemented in src/bench/bench_tlb.c. */
lb_status_t tlb_size_run(const lb_cfg_t *cfg);
lb_status_t tlb_fault_run(const lb_cfg_t *cfg);

/* Pipeline and memory-level parallelism subtests, implemented in bench_core.c. */
lb_status_t core_ilp_run(const lb_cfg_t *cfg);
lb_status_t core_mlp_run(const lb_cfg_t *cfg);
lb_status_t core_align_run(const lb_cfg_t *cfg);
lb_status_t core_stlf_run(const lb_cfg_t *cfg);

/* Branch predictor, RSB, and BTB subtests, implemented in bench_branch.c. */
lb_status_t br_hist_run(const lb_cfg_t *cfg);
lb_status_t br_rsb_run(const lb_cfg_t *cfg);
lb_status_t br_btb_run(const lb_cfg_t *cfg);

/* Memory bandwidth and DRAM row-buffer subtests, implemented in bench_dram.c. */
lb_status_t dram_bw_run(const lb_cfg_t *cfg);
lb_status_t dram_row_run(const lb_cfg_t *cfg);
lb_status_t dram_lat_run(const lb_cfg_t *cfg);

/* Topology atomics, false sharing, and ping-pong, implemented in bench_topo.c. */
lb_status_t topo_atomic_run(const lb_cfg_t *cfg);
lb_status_t topo_falseshare_run(const lb_cfg_t *cfg);
lb_status_t topo_pingpong_run(const lb_cfg_t *cfg);

/* Operating-system probes, implemented in bench_os.c. */
lb_status_t os_syscall_run(const lb_cfg_t *cfg);
lb_status_t os_ctx_run(const lb_cfg_t *cfg);
lb_status_t os_wakeup_run(const lb_cfg_t *cfg);
lb_status_t os_shootdown_run(const lb_cfg_t *cfg);
lb_status_t os_fork_run(const lb_cfg_t *cfg);
lb_status_t os_sig_run(const lb_cfg_t *cfg);
lb_status_t os_sleep_run(const lb_cfg_t *cfg);
lb_status_t os_swap_run(const lb_cfg_t *cfg);

/*
 * Pure estimator helpers exported for unit testing. They return zero or false
 * on invalid or flat inputs without performing I/O.
 */
size_t mem_line_estimate_p10(const double *p10_ns, const size_t *steps, size_t count);
size_t mem_assoc_knee(const double *row, size_t limit, double *ratio_out);
bool br_rsb_detect_threshold(const uint64_t *depths, const double *ns_per_call, size_t count,
                             size_t *base_index_out, size_t *threshold_index_out);
uint32_t topo_fs_estimate_line_offsets(const size_t *offsets, const double *samples, size_t count);

#endif /* LB_BENCH_H */
