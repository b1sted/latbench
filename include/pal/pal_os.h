/* SPDX-License-Identifier: GPL-3.0 */
#ifndef PAL_PAL_OS_H
#define PAL_PAL_OS_H

/*
 * Operating-system latency probes. This header is strict ISO C99 and exposes
 * only flat result structures; every OS-specific call stays in src/platform/.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"

/*
 * Comparison between a fast userland clock read and a guaranteed kernel
 * transition. timer_ns and syscall_ns are totals over calls measured calls.
 */
typedef struct pal_os_syscall_result_t {
    uint64_t timer_ns;
    uint64_t syscall_ns;
    uint64_t calls;
} pal_os_syscall_result_t;

/* Two-party pipe ping-pong. switches counts handoffs (two per round trip). */
typedef struct pal_os_ctx_result_t {
    uint64_t elapsed_ns;
    uint64_t switches;
} pal_os_ctx_result_t;

/* Mutex and condition-variable scheduler ping-pong. */
typedef struct pal_os_wakeup_result_t {
    uint64_t elapsed_ns;
    uint64_t wakeups;
} pal_os_wakeup_result_t;

/*
 * mprotect() shootdown probe. single_ops and multi_ops count mprotect() calls
 * (two per permission flip). multi_skipped is true when the host has fewer than
 * two online CPUs, so the background-thread mode is not meaningful.
 */
typedef struct pal_os_shootdown_result_t {
    uint64_t single_ns;
    uint64_t single_ops;
    uint64_t multi_ns;
    uint64_t multi_ops;
    uint32_t background_threads;
    bool multi_skipped;
} pal_os_shootdown_result_t;

/* fork() + _exit(0) + waitpid() batch. */
typedef struct pal_os_fork_result_t {
    uint64_t elapsed_ns;
    uint64_t forks;
} pal_os_fork_result_t;

/* pthread_kill(pthread_self(), SIGUSR1) delivery. */
typedef struct pal_os_sig_result_t {
    uint64_t elapsed_ns;
    uint64_t signals;
} pal_os_sig_result_t;

/*
 * Swap-in latency probe. pages is the number of pages paged out and touched
 * again, major_faults and minor_faults are the deltas observed over the
 * measured pass, and swap_free_kib is the free swap visible at the start.
 */
typedef struct pal_os_swap_result_t {
    uint64_t elapsed_ns;
    uint64_t pages;
    uint64_t major_faults;
    uint64_t minor_faults;
    uint64_t swap_free_kib;
} pal_os_swap_result_t;

/*
 * Time calls round trips of pal_now_ns() and of a one-byte write() to
 * /dev/null. Both paths are warmed up before the measured region.
 */
lb_status_t pal_os_syscall_probe(uint32_t calls, pal_os_syscall_result_t *out);

/*
 * Context-switch ping-pong between two threads over two pipes. Each round trip
 * performs one request handoff and one response handoff.
 */
lb_status_t pal_os_ctx_thread(uint32_t round_trips, pal_os_ctx_result_t *out);

/*
 * Context-switch ping-pong between a parent and a forked child over two pipes.
 * The child terminates cleanly with _exit(0) and the parent reaps it.
 */
lb_status_t pal_os_ctx_proc(uint32_t round_trips, pal_os_ctx_result_t *out);

/*
 * Scheduler wakeup ping-pong between two threads using pthread_mutex and
 * pthread_cond_signal only, without any file-descriptor traffic.
 */
lb_status_t pal_os_wakeup(uint32_t round_trips, pal_os_wakeup_result_t *out);

/*
 * Measure mprotect(PROT_READ) and mprotect(PROT_READ | PROT_WRITE) pairs on one
 * page, first from a single thread and then with K background threads touching
 * the same address space. K is min(online CPUs - 1, 7). Every flip is followed
 * by a one-byte read and a one-byte write so the page stays in the TLB.
 */
lb_status_t pal_os_shootdown(uint32_t iterations, pal_os_shootdown_result_t *out);

/* Time forks batches of fork(), child _exit(0), and parent waitpid(). */
lb_status_t pal_os_fork_once(uint32_t forks, pal_os_fork_result_t *out);

/*
 * Install a SIGUSR1 handler, deliver signals with
 * pthread_kill(pthread_self(), SIGUSR1), and restore the previous handler.
 */
lb_status_t pal_os_sig_roundtrip(uint32_t signals, pal_os_sig_result_t *out);

/*
 * Sleep for target_us microseconds samples times and store the measured
 * wall-clock duration of each sleep in samples_ns. capacity must be at least
 * samples. out_count receives the number of valid entries.
 */
lb_status_t pal_os_sleep_us(uint32_t target_us, uint32_t samples, uint64_t *samples_ns,
                            size_t capacity, size_t *out_count);

/*
 * Page at most max_pages anonymous pages out with madvise(MADV_PAGEOUT) and
 * time one access to every page in a shuffled order. capacity must be at least
 * max_pages; samples_ns receives one latency per page and out_count the number
 * of valid entries. Returns LB_STATUS_UNSUPPORTED when the host has no usable
 * swap or the kernel cannot page anonymous memory out.
 */
lb_status_t pal_os_swap_probe(size_t max_pages, uint64_t *samples_ns, size_t capacity,
                              size_t *out_count, pal_os_swap_result_t *out);

#endif /* PAL_PAL_OS_H */
