/* SPDX-License-Identifier: GPL-3.0 */
#ifndef LB_TYPES_H
#define LB_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Common return codes for public module interfaces. */
typedef enum lb_status_t {
    LB_STATUS_OK = 0,
    LB_STATUS_ERROR = 1,
    LB_STATUS_INVALID_ARG = 2,
    LB_STATUS_NOMEM = 3,
    LB_STATUS_IO = 4,
    LB_STATUS_UNSUPPORTED = 5
} lb_status_t;

/* Transparent huge page policy reported by the kernel. */
typedef enum lb_thp_mode_t {
    LB_THP_UNSUPPORTED = 0,
    LB_THP_ALWAYS = 1,
    LB_THP_MADVISE = 2,
    LB_THP_NEVER = 3
} lb_thp_mode_t;

/* Context-switch probe selection for "os ctx". */
typedef enum lb_os_ctx_mode_t {
    LB_OS_CTX_BOTH = 0,
    LB_OS_CTX_THREAD = 1,
    LB_OS_CTX_PROC = 2
} lb_os_ctx_mode_t;

/* Flat CLI and run configuration. Declared with {0} before use. */
typedef struct lb_cfg_t {
    int32_t cpu_id; /* -1 means no --cpu request */
    bool qos_perf;
    bool qos_set; /* true when --qos was given explicitly */
    uint32_t max_mem_mb;
    uint32_t batch_ms;
    uint32_t cool_ms;
    bool no_thp;
    bool thp_set; /* true when --thp or --no-thp was given explicitly */
    bool csv;
    bool no_color;
    bool write;                   /* --write: modify words during the memory-size walk */
    bool throttle_long;           /* --long: extend sys throttle from 5 s to 10 s */
    bool topo_pingpong_cas;       /* --mode cas: CAS token handoff for topo pingpong */
    bool selftest_no_timing;      /* --no-timing: skip hardware ratio checks in selftest */
    bool fast;                    /* compact probe mode used by "auto" */
    bool auto_mode;               /* internal: collect a passport, hide probe detail */
    bool quiet;                   /* internal: suppress all informational output */
    bool suppress_drift_warn;     /* internal: big.LITTLE migration makes the canary noisy */
    bool runtime_applied;         /* internal: --cpu/--qos already applied once by "auto" */
    lb_os_ctx_mode_t os_ctx_mode; /* --mode thread|proc: os ctx selection */
} lb_cfg_t;

/*
 * Flat machine passport collected by the "auto" profile. Probes publish their
 * key results here; zero means "not measured" for every field. The struct is
 * POD and may live on the stack, so it is safe to copy and render.
 */
typedef struct lb_passport_t {
    const char *arch;
    size_t page_size;
    uint64_t ram_bytes;
    uint32_t cpu_count;

    uint64_t l1d_bytes;
    double l1d_ns;
    uint32_t l1d_ways;
    uint64_t l2_bytes;
    double l2_ns;
    uint64_t l3_bytes;
    double l3_ns;
    double ram_ns;
    uint32_t coherence_line_bytes;
    uint32_t l1dtlb_entries;

    uint32_t alu_ilp_chains;
    uint32_t fpu_ilp_chains;
    double alu_ilp_speedup;
    double fpu_ilp_speedup;
    double stlf_stall_ns;
    const char *stlf_variant;
    uint64_t rsb_frames;
    uint64_t btb_targets;
    double mispredict_ns;

    double dram_read_gibs;
    double dram_write_gibs;
    double row_delta_ns;
    double row_delta_pct;
    uint64_t pp_min_ns;
    uint64_t pp_med_ns;
    uint64_t pp_max_ns;

    double os_syscall_ns;
    double os_ctx_thread_us;
    double os_ctx_proc_us;
    double os_wakeup_us;
    double os_shootdown_single_us;
    double os_shootdown_multi_us;
    double os_fork_us;
    double os_sleep_1us_p50_us;
} lb_passport_t;

/* One measured latency sample. */
typedef struct lb_point_t {
    uint64_t seq;
    uint64_t ns;
} lb_point_t;

#endif /* LB_TYPES_H */
