/* SPDX-License-Identifier: GPL-3.0 */

#include "bench.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "bench_common.h"
#include "defs.h"
#include "pal/compiler.h"
#include "pal/pal.h"
#include "passport.h"
#include "stats.h"
#include "tty.h"
#include "types.h"
#include "util.h"

/*
 * Rotate the node offset inside every page by this many bytes. Two purposes:
 *  - every page holds exactly one node, so a chase of N nodes spans N pages;
 *  - consecutive pages change the L1d set index even when the cache line is
 *    128 bytes wide (Apple Silicon), avoiding data-cache set conflicts instead
 *    of measuring the TLB.
 */
#define TLB_NODE_OFFSET_STEP 128u

/* tlb size: hardware-aligned page-count sweep from four pages to the ceiling. */
#define TLB_SIZE_START_PAGES 4u
#define TLB_SIZE_MAX_PAGES 16384u
#define TLB_SIZE_MAX_POINTS 32u
#define TLB_SIZE_SAMPLES 5u

/*
 * Page counts seen in real L1 dTLB, L2 TLB, and STLB implementations. The dense
 * small steps resolve the 32/48/64-entry boundaries instead of landing between
 * them on a geometric grid.
 */
static const size_t TLB_SIZE_GRID_PAGES[] = {
    4u,  6u,   8u,   10u,  12u,  16u,  20u,  24u,   32u,   40u,   48u,   56u,   64u,   80u,
    96u, 128u, 192u, 256u, 384u, 512u, 768u, 1024u, 1536u, 2048u, 3072u, 4096u, 8192u, 16384u,
};

/* Pointer hops between clock reads in the timed chase loops. */
#define TLB_CHASE_CHUNK 64u
#define TLB_CHASE_UNROLL 8u

/* Upper bounds for the two arenas, also clipped by pal_safe_max_mem_bytes(). */
#define TLB_SIZE_MAX_BYTES (UINT64_C(256) * UINT64_C(1024) * UINT64_C(1024))
#define TLB_FAULT_MAX_BYTES (UINT64_C(64) * UINT64_C(1024) * UINT64_C(1024))

/* tlb fault: smallest arena worth measuring. */
#define TLB_FAULT_MIN_PAGES 4u

/* Compact auto mode: small arenas and fewer samples. */
#define TLB_FAST_MAX_BYTES (UINT64_C(8) * UINT64_C(1024) * UINT64_C(1024))
#define TLB_FAST_BATCH_MS 5u
#define TLB_FAST_SAMPLES 3u

typedef struct tlb_node_t {
    struct tlb_node_t *next;
} tlb_node_t;

static const char *const TLB_SIZE_LEVELS[] = {"L1 dTLB", "L2 TLB"};

/*
 * Volatile result sink. Every timed loop publishes its final pointer here so
 * the optimizer cannot discard the dependent loads.
 */
static volatile uint64_t g_tlb_sink = 0u;

static uint32_t tlb_batch_ms(const lb_cfg_t *cfg)
{
    uint32_t batch = 0u;

    if (cfg == NULL) {
        return 0u;
    }
    batch = cfg->batch_ms;
    if (!cfg->fast) {
        return batch;
    }
    if (batch == 0u || batch > TLB_FAST_BATCH_MS) {
        batch = TLB_FAST_BATCH_MS;
    }
    return batch;
}

/*
 * Address of node index: page index bytes apart, with a rotating offset inside
 * the page. The offset advances by TLB_NODE_OFFSET_STEP per page and wraps, so
 * adjacent nodes stay in different pages and in different L1d sets.
 */
static uint8_t *tlb_node_addr(uint8_t *base, size_t index, size_t page_size)
{
    size_t offset = 0u;

    if (page_size > TLB_NODE_OFFSET_STEP) {
        offset = (index * TLB_NODE_OFFSET_STEP) % page_size;
    }
    if (offset + sizeof(tlb_node_t) > page_size) {
        offset = 0u;
    }
    return base + (index * page_size) + offset;
}

static lb_status_t tlb_build_chain(uint8_t *base, size_t count, size_t page_size, size_t *indices,
                                   uint64_t seed)
{
    size_t i = 0u;
    lb_status_t status = LB_STATUS_OK;

    if (base == NULL || indices == NULL || count == 0u || page_size == 0u) {
        return LB_STATUS_INVALID_ARG;
    }

    status = util_sattolo_indices(indices, count, seed);
    if (status != LB_STATUS_OK) {
        return status;
    }

    for (i = 0u; i < count; i++) {
        tlb_node_t *const node = (tlb_node_t *)(void *)tlb_node_addr(base, i, page_size);
        tlb_node_t *const next = (tlb_node_t *)(void *)tlb_node_addr(base, indices[i], page_size);

        node->next = next;
    }
    return LB_STATUS_OK;
}

static PAL_NOINLINE void tlb_chase_until(const tlb_node_t *start, uint64_t deadline,
                                         uint64_t *hops_out)
{
    const tlb_node_t *p = start;
    uint64_t total = 0u;

    for (;;) {
        size_t chunk = 0u;

        for (chunk = 0u; chunk < TLB_CHASE_CHUNK; chunk++) {
            LB_UNROLL_8(p = p->next);
        }
        total += (uint64_t)TLB_CHASE_CHUNK * (uint64_t)TLB_CHASE_UNROLL;
        if (pal_now_ns() >= deadline) {
            break;
        }
    }

    PAL_BARRIER();
    g_tlb_sink = (uint64_t)(uintptr_t)p;
    *hops_out = total;
}

static double tlb_size_sample(const tlb_node_t *start, uint32_t batch_ms)
{
    const uint64_t begin = pal_now_ns();
    const uint64_t batch_ns = (uint64_t)batch_ms * UINT64_C(1000000);
    const uint64_t deadline = begin + batch_ns;
    uint64_t hops = 0u;
    uint64_t end = 0u;

    tlb_chase_until(start, deadline, &hops);

    end = pal_now_ns();
    if (hops == 0u) {
        return 0.0;
    }
    if (end <= begin) {
        end = begin + 1u;
    }
    return (double)(end - begin) / (double)hops;
}

lb_status_t tlb_size_run(const lb_cfg_t *cfg)
{
    pal_mem_t *arena = NULL;
    uint8_t *base = NULL;
    size_t *indices = NULL;
    stats_series_t *series = NULL;
    stats_steps_t steps = {0};
    double samples[TLB_SIZE_SAMPLES] = {0.0};
    size_t sample_count = 0u;
    uint32_t batch_ms = 0u;
    const size_t page_size = pal_page_size();
    size_t arena_bytes = 0u;
    size_t max_pages = 0u;
    size_t count = 0u;
    size_t grid_index = 0u;
    size_t points = 0u;
    uint64_t baseline = 0u;
    lb_status_t status = LB_STATUS_NOMEM;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    sample_count = cfg->fast ? TLB_FAST_SAMPLES : TLB_SIZE_SAMPLES;
    batch_ms = tlb_batch_ms(cfg);
    if (page_size == 0u) {
        tty_err("cannot determine the system page size");
        return LB_STATUS_UNSUPPORTED;
    }

    lb_bench_apply_runtime(cfg);

    arena_bytes =
        lb_bench_arena_cap(cfg, cfg->fast ? TLB_FAST_MAX_BYTES : TLB_SIZE_MAX_BYTES, true);
    max_pages = arena_bytes / page_size;
    if (max_pages > TLB_SIZE_MAX_PAGES) {
        max_pages = TLB_SIZE_MAX_PAGES;
    }
    if (max_pages < TLB_SIZE_START_PAGES) {
        tty_err("memory budget is too small for tlb size");
        return LB_STATUS_NOMEM;
    }
    arena_bytes = max_pages * page_size;

    arena = pal_mem_alloc(arena_bytes, lb_bench_arena_flags(cfg));
    if (arena == NULL) {
        tty_err("cannot allocate a %zu-byte tlb size arena", arena_bytes);
        goto cleanup;
    }
    base = pal_mem_data(arena);
    if (base == NULL) {
        goto cleanup;
    }

    indices = malloc(max_pages * sizeof(*indices));
    if (indices == NULL) {
        goto cleanup;
    }

    series = stats_series_create("tlb size", TLB_SIZE_MAX_POINTS);
    if (series == NULL) {
        goto cleanup;
    }

    {
        char human[LB_NAME_MAX] = {0};

        (void)tty_format_bytes((uint64_t)arena_bytes, human, sizeof(human));
        tty_kv(cfg, "Page size", "%zu bytes", page_size);
        tty_kv(cfg, "Arena limit", "%s", human);
        tty_kv(cfg, "Pages", "%u .. %zu", TLB_SIZE_START_PAGES, max_pages);
        tty_kv(cfg, "Batch / samples", "%" PRIu32 " ms / %zu", batch_ms, sample_count);
    }

    baseline = stats_canary_baseline(LB_BENCH_CANARY_MS);

    for (;;) {
        uint64_t seed = 0u;
        size_t sample = 0u;

        if (grid_index >= LB_ARRAY_SIZE(TLB_SIZE_GRID_PAGES)) {
            break;
        }
        count = TLB_SIZE_GRID_PAGES[grid_index];
        if (count > max_pages) {
            break;
        }
        seed = UINT64_C(0x517cc1b727220a95) ^ ((uint64_t)count * UINT64_C(0x2545f4914f6cdd1d));

        status = tlb_build_chain(base, count, page_size, indices, seed);
        if (status != LB_STATUS_OK) {
            goto cleanup;
        }

        for (sample = 0u; sample < sample_count; sample++) {
            samples[sample] = tlb_size_sample((const tlb_node_t *)(const void *)base, batch_ms);
        }
        lb_bench_cool(cfg);

        status = stats_series_push(series, count * page_size, samples, sample_count);
        if (status != LB_STATUS_OK) {
            goto cleanup;
        }
        points++;

        if (count >= max_pages) {
            break;
        }
        if ((points + 1u) >= TLB_SIZE_MAX_POINTS) {
            break;
        }
        grid_index++;
    }

    lb_bench_canary_finish(cfg, baseline, true);

    status = stats_series_detect_steps(series, &steps);
    if (status != LB_STATUS_OK) {
        goto cleanup;
    }
    if (steps.count > 0u && page_size > 0u) {
        const uint64_t entries = (uint64_t)steps.items[0].x / (uint64_t)page_size;

        if (entries <= (uint64_t)UINT32_MAX) {
            lb_passport_mutable()->l1dtlb_entries = (uint32_t)entries;
        }
    }
    status = tty_plot_series(cfg, series, &steps, TLB_SIZE_LEVELS, LB_ARRAY_SIZE(TLB_SIZE_LEVELS),
                             page_size);

cleanup:
    stats_series_free(series);
    free(indices);
    pal_mem_free(arena);
    return status;
}

/*
 * Touch one byte per page. The pointer is volatile so neither the write nor its
 * page fault can be removed by the optimizer. value only changes the stored
 * byte between the first and the second pass.
 */
static PAL_NOINLINE void tlb_fault_touch(volatile uint8_t *base, size_t page_size, size_t pages,
                                         uint8_t value)
{
    size_t i = 0u;

    for (i = 0u; i < pages; i++) {
        size_t offset = 0u;

        if (page_size > TLB_NODE_OFFSET_STEP) {
            offset = (i * TLB_NODE_OFFSET_STEP) % page_size;
        }
        base[(i * page_size) + offset] = (uint8_t)(value + (uint8_t)(i & 0xffu));
    }

    PAL_BARRIER();
}

static double tlb_fault_ns_per_page(uint64_t total_ns, size_t pages)
{
    if (pages == 0u) {
        return 0.0;
    }
    return (double)total_ns / (double)pages;
}

lb_status_t tlb_fault_run(const lb_cfg_t *cfg)
{
    pal_mem_t *arena = NULL;
    volatile uint8_t *base = NULL;
    const size_t page_size = pal_page_size();
    size_t arena_bytes = 0u;
    size_t pages = 0u;
    uint64_t first_begin = 0u;
    uint64_t first_end = 0u;
    uint64_t first_ns = 0u;
    uint64_t second_begin = 0u;
    uint64_t second_end = 0u;
    uint64_t second_ns = 0u;
    double first_per_page = 0.0;
    double second_per_page = 0.0;
    double ratio = 0.0;
    char first_total[LB_NAME_MAX] = {0};
    char second_total[LB_NAME_MAX] = {0};
    lb_status_t status = LB_STATUS_NOMEM;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }
    if (page_size == 0u) {
        tty_err("cannot determine the system page size");
        return LB_STATUS_UNSUPPORTED;
    }

    lb_bench_apply_runtime(cfg);

    arena_bytes =
        lb_bench_arena_cap(cfg, cfg->fast ? TLB_FAST_MAX_BYTES : TLB_FAULT_MAX_BYTES, false);
    pages = arena_bytes / page_size;
    if (pages < TLB_FAULT_MIN_PAGES) {
        tty_err("memory budget is too small for tlb fault");
        return LB_STATUS_NOMEM;
    }
    arena_bytes = pages * page_size;

    /*
     * Deliberate exception to STYLEGUIDE section 7: the arena must stay
     * untouched so the first pass measures genuine first-touch page faults.
     * Do not memset() or otherwise initialize it before the timed region.
     */
    arena = pal_mem_alloc(arena_bytes, lb_bench_arena_flags(cfg));
    if (arena == NULL) {
        tty_err("cannot allocate a %zu-byte tlb fault arena", arena_bytes);
        goto cleanup;
    }
    base = (volatile uint8_t *)pal_mem_data(arena);
    if (base == NULL) {
        goto cleanup;
    }

    {
        char human[LB_NAME_MAX] = {0};

        (void)tty_format_bytes((uint64_t)arena_bytes, human, sizeof(human));
        tty_kv(cfg, "Page size", "%zu bytes", page_size);
        tty_kv(cfg, "Arena", "%s", human);
        tty_kv(cfg, "Pages", "%zu", pages);
        tty_kv(cfg, "THP policy", "%s", cfg->no_thp ? "no-thp" : "thp");
    }

    tty_csv_row(cfg, "module,subtest,metric,value,unit");

    first_begin = pal_now_ns();
    tlb_fault_touch(base, page_size, pages, 1u);
    first_end = pal_now_ns();
    first_ns = (first_end > first_begin) ? (first_end - first_begin) : 0u;

    second_begin = pal_now_ns();
    tlb_fault_touch(base, page_size, pages, 2u);
    second_end = pal_now_ns();
    second_ns = (second_end > second_begin) ? (second_end - second_begin) : 0u;

    first_per_page = tlb_fault_ns_per_page(first_ns, pages);
    second_per_page = tlb_fault_ns_per_page(second_ns, pages);
    if (second_per_page > 0.0) {
        ratio = first_per_page / second_per_page;
    }

    (void)tty_format_ns(first_ns, first_total, sizeof(first_total));
    (void)tty_format_ns(second_ns, second_total, sizeof(second_total));

    tty_section(cfg, "Page faults");
    tty_header(cfg, "  %-18s %14s %14s", "Pass", "total", "ns/page");
    tty_header(cfg, "  %-18s %14s %14.1f", "first touch", first_total, first_per_page);
    tty_header(cfg, "  %-18s %14s %14.2f", "second pass", second_total, second_per_page);
    tty_header(cfg, "  %-18s %14.2fx", "first / second", ratio);

    tty_csv_row(cfg, "tlb,fault,first_touch,%.2f,ns/page", first_per_page);
    tty_csv_row(cfg, "tlb,fault,second_pass,%.2f,ns/page", second_per_page);
    tty_csv_row(cfg, "tlb,fault,first_over_second,%.2f,x", ratio);
    status = LB_STATUS_OK;

cleanup:
    pal_mem_free(arena);
    return status;
}
