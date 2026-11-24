/* SPDX-License-Identifier: GPL-3.0 */
#ifndef LB_TYPES_H
#define LB_TYPES_H

#include <stdbool.h>
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
    bool csv;
    bool no_color;
    bool write;                   /* --write: modify words during the memory-size walk */
    bool throttle_long;           /* --long: extend sys throttle from 5 s to 10 s */
    bool topo_pingpong_cas;       /* --mode cas: CAS token handoff for topo pingpong */
    lb_os_ctx_mode_t os_ctx_mode; /* --mode thread|proc: os ctx selection */
} lb_cfg_t;

/* One measured latency sample. */
typedef struct lb_point_t {
    uint64_t seq;
    uint64_t ns;
} lb_point_t;

#endif /* LB_TYPES_H */
