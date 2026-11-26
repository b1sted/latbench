/* SPDX-License-Identifier: GPL-3.0 */
#ifndef PAL_PAL_H
#define PAL_PAL_H

/*
 * Portable platform abstraction interface. This header is strict ISO C99 and
 * must never include OS-specific headers.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"

/* Opaque arena handle. Storage is page-aligned when the OS supports mmap. */
/* NOLINTNEXTLINE(readability-identifier-naming): the public tag stays pal_mem. */
typedef struct pal_mem pal_mem_t;

/* Opaque joinable thread handle. */
/* NOLINTNEXTLINE(readability-identifier-naming): the public tag stays pal_thread. */
typedef struct pal_thread pal_thread_t;

/* Thread entry point. The argument is the value passed to pal_thread_spawn. */
typedef void (*pal_thread_fn_t)(void *arg);

/* Arena allocation flags. PAL_MEM_NO_THP wins when both bits are set. */
#define PAL_MEM_NO_THP 0x01u
#define PAL_MEM_THP 0x02u

/* Monotonic nanosecond clock. */
uint64_t pal_now_ns(void);

/* Sleep for at least milliseconds; a zero duration is a no-op. */
void pal_sleep_ms(uint32_t milliseconds);

/*
 * True when stdout (use_stderr == false) or stderr (use_stderr == true) is
 * attached to a terminal. Used to decide whether ANSI styling is safe.
 */
bool pal_is_tty(bool use_stderr);

/* System page size in bytes. */
size_t pal_page_size(void);

/* Total physical RAM in bytes, or 0 when the platform cannot report it. */
uint64_t pal_total_ram_bytes(void);

/*
 * Safe arena budget in bytes. A user_limit_mb of 0 selects the automatic
 * policy, which never exceeds 25% of physical RAM. A non-zero limit is treated
 * as an upper bound and is still clipped to that 25% ceiling.
 */
uint64_t pal_safe_max_mem_bytes(uint64_t user_limit_mb);

/* Number of online CPUs, always at least 1. */
uint32_t pal_cpu_count(void);

/*
 * Name of the architecture targeted by this binary ("x86_64", "i386", "arm64",
 * "aarch64", "riscv64", "armhf", "armv7", or "unknown"). Never returns NULL.
 */
const char *pal_arch_name(void);

/* Pin the calling thread to one CPU. Returns LB_STATUS_UNSUPPORTED when denied. */
lb_status_t pal_pin_cpu(int32_t cpu_id);

/* Request interactive or background QoS. Unsupported platforms return UNSUPPORTED. */
lb_status_t pal_set_qos(bool high_perf);

/*
 * True when the platform exposes a per-thread CPU affinity request. Linux and
 * Android report true even though a container or cpuset may still deny the
 * actual pin; macOS has no stable hard-affinity API and reports false.
 */
bool pal_pin_supported(void);

/*
 * True when independent integer chains are expected to overlap in the pipeline.
 * Out-of-order cores report true; in-order 32-bit ARMv7 cores report false so
 * the ILP selftest does not demand scaling that the microarchitecture cannot
 * provide.
 */
bool pal_ilp_scaling_expected(void);

/*
 * Start a joinable thread. pin_cpu >= 0 asks the new thread to pin itself to
 * that logical CPU before fn runs; pass -1 to leave affinity untouched. When
 * qos_perf is true the new thread requests interactive/high-performance QoS
 * before fn runs. Both requests are best-effort on platforms that lack them.
 *
 * Returns NULL when fn is NULL, allocation fails, or the OS refuses to create
 * the thread. Every non-NULL handle must be passed to pal_thread_join exactly
 * once.
 */
pal_thread_t *pal_thread_spawn(pal_thread_fn_t fn, void *arg, int32_t pin_cpu, bool qos_perf);

/*
 * Wait for a spawned thread to finish and release its handle. When pin_status
 * is not NULL it receives the outcome of the affinity request made by
 * pal_thread_spawn(): LB_STATUS_OK when no pin was requested or the pin was
 * applied, otherwise the status returned by pal_pin_cpu().
 */
lb_status_t pal_thread_join_ex(pal_thread_t *t, lb_status_t *pin_status);

/*
 * Wait for a spawned thread to finish and release its handle. Passing NULL is
 * an error. On success the handle is invalid and must not be used again; on
 * failure the caller still owns the handle. Equivalent to
 * pal_thread_join_ex(t, NULL).
 */
lb_status_t pal_thread_join(pal_thread_t *t);

/* Allocate a page-aligned arena of at least size bytes. */
pal_mem_t *pal_mem_alloc(size_t size, uint32_t flags);

/* Return the arena's data pointer, or NULL when mem is NULL. */
void *pal_mem_data(const pal_mem_t *mem);

/* Return the requested arena size in bytes, or 0 when mem is NULL. */
size_t pal_mem_size(const pal_mem_t *mem);

/* Release an arena. Safe to call with NULL. */
void pal_mem_free(pal_mem_t *mem);

#endif /* PAL_PAL_H */
