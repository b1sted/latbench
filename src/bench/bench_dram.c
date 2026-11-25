/* SPDX-License-Identifier: GPL-3.0 */

#include "bench.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "defs.h"
#include "pal/compiler.h"
#include "pal/pal.h"
#include "passport.h"
#include "stats.h"
#include "tty.h"
#include "types.h"
#include "util.h"

/* Duration of each DVFS frequency-drift canary measurement. */
#define DRAM_CANARY_MS 20u

/* Arena policy when the platform cannot report the amount of physical RAM. */
#define DRAM_FALLBACK_BUDGET_BYTES (UINT64_C(4) * UINT64_C(1024) * UINT64_C(1024))

/* dram bw: sequential read, write, and copy over a working set above L3. */
#define DRAM_BW_MAX_BYTES (UINT64_C(128) * UINT64_C(1024) * UINT64_C(1024))
#define DRAM_BW_MIN_BYTES 4096u
#define DRAM_BW_REPEATS 3u

/* dram row: one node per 256 bytes, sixteen cache-line-sized nodes per page. */
#define DRAM_ROW_PAGE_BYTES 4096u
#define DRAM_ROW_NODES_PER_PAGE 16u
#define DRAM_ROW_NODE_BYTES (DRAM_ROW_PAGE_BYTES / DRAM_ROW_NODES_PER_PAGE)
#define DRAM_ROW_NODE_PAD (DRAM_ROW_NODE_BYTES - sizeof(void *))
#define DRAM_ROW_MAX_BYTES (UINT64_C(64) * UINT64_C(1024) * UINT64_C(1024))
#define DRAM_ROW_MIN_PAGES 2u
#define DRAM_ROW_ACCESSES UINT64_C(1048576)
#define DRAM_ROW_WARMUP UINT64_C(65536)
#define DRAM_ROW_REPEATS 3u

/* Compact auto mode: fewer accesses and repeats; arenas stay above L3. */
#define DRAM_FAST_MAX_BYTES (UINT64_C(64) * UINT64_C(1024) * UINT64_C(1024))
#define DRAM_BW_FAST_REPEATS 2u
#define DRAM_ROW_FAST_ACCESSES UINT64_C(262144)
#define DRAM_ROW_FAST_REPEATS 2u

typedef struct dram_node_t {
    struct dram_node_t *next;
    /* Structural padding: fixes the node stride at 256 bytes. */
    uint8_t pad[DRAM_ROW_NODE_PAD]; /* cppcheck-suppress unusedStructMember */
} dram_node_t;

/*
 * Volatile result sink. Every timed loop publishes its checksum here so the
 * optimizer cannot discard the dependent loads or the dirty stores.
 */
static volatile uint64_t g_dram_sink = 0u;

static size_t dram_arena_cap(const lb_cfg_t *cfg, uint64_t limit_bytes, bool reserve_indices)
{
    uint64_t budget = pal_safe_max_mem_bytes((uint64_t)cfg->max_mem_mb);

    if (budget == 0u) {
        budget = DRAM_FALLBACK_BUDGET_BYTES;
    }
    if (reserve_indices) {
        /* Leave one ninth of the budget for the permutation arrays. */
        budget = (budget / 9u) * 8u;
    }
    if (budget > limit_bytes) {
        budget = limit_bytes;
    }
#if SIZE_MAX < UINT64_MAX
    if (budget > (uint64_t)SIZE_MAX) {
        budget = (uint64_t)SIZE_MAX;
    }
#endif
    return (size_t)budget;
}

static uint32_t dram_arena_flags(const lb_cfg_t *cfg)
{
    return cfg->no_thp ? PAL_MEM_NO_THP : PAL_MEM_THP;
}

static void dram_apply_runtime(const lb_cfg_t *cfg)
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

static void dram_cool(const lb_cfg_t *cfg)
{
    if (cfg->cool_ms > 0u) {
        pal_sleep_ms(cfg->cool_ms);
    }
}

static void dram_canary_finish(const lb_cfg_t *cfg, uint64_t baseline)
{
    const uint64_t after = stats_canary_measure(DRAM_CANARY_MS);
    const double drift = stats_canary_drift_pct(baseline, after);

    tty_warn_drift(cfg, drift);
    tty_kv(cfg, "DVFS drift", "%+.1f%%", drift);
}

static double dram_gib_per_s(uint64_t bytes, uint64_t elapsed_ns)
{
    if (elapsed_ns == 0u) {
        return 0.0;
    }
    return ((double)bytes * 1.0e9) / ((double)elapsed_ns * 1073741824.0);
}

static double dram_best(const double *samples, size_t count)
{
    double best = 0.0;
    size_t i = 0u;

    for (i = 0u; i < count; i++) {
        if (samples[i] > best) {
            best = samples[i];
        }
    }
    return best;
}

/* ---------------------------------------------------------------- dram bw */

static PAL_NOINLINE uint64_t dram_bw_read(const uint64_t *data, size_t words)
{
    uint64_t a0 = 0u;
    uint64_t a1 = 0u;
    uint64_t a2 = 0u;
    uint64_t a3 = 0u;
    uint64_t a4 = 0u;
    uint64_t a5 = 0u;
    uint64_t a6 = 0u;
    uint64_t a7 = 0u;
    size_t i = 0u;

    for (i = 0u; (i + 8u) <= words; i += 8u) {
        a0 += data[i];
        a1 += data[i + 1u];
        a2 += data[i + 2u];
        a3 += data[i + 3u];
        a4 += data[i + 4u];
        a5 += data[i + 5u];
        a6 += data[i + 6u];
        a7 += data[i + 7u];
    }
    for (; i < words; i++) {
        a0 += data[i];
    }

    a0 += a1 + a2 + a3 + a4 + a5 + a6 + a7;
    PAL_KEEP_INT(a0);
    PAL_BARRIER();
    g_dram_sink = a0;
    return a0;
}

static PAL_NOINLINE uint64_t dram_bw_write(uint64_t *data, size_t words, uint64_t seed)
{
    uint64_t value = seed;
    size_t i = 0u;

    for (i = 0u; i < words; i++) {
        data[i] = value;
        value += UINT64_C(0x9e3779b97f4a7c15);
    }

    PAL_KEEP_INT(value);
    PAL_BARRIER();
    g_dram_sink = value;
    return value;
}

static PAL_NOINLINE uint64_t dram_bw_copy(uint64_t *dst, const uint64_t *src, size_t words)
{
    uint64_t acc = 0u;
    size_t i = 0u;

    for (i = 0u; i < words; i++) {
        const uint64_t value = src[i];

        dst[i] = value;
        acc ^= value;
    }

    PAL_KEEP_INT(acc);
    PAL_BARRIER();
    g_dram_sink = acc;
    return acc;
}

static double dram_bw_read_sample(const uint64_t *data, size_t words)
{
    const uint64_t bytes = (uint64_t)words * (uint64_t)sizeof(uint64_t);
    const uint64_t begin = pal_now_ns();
    const uint64_t sink = dram_bw_read(data, words);
    uint64_t end = pal_now_ns();

    g_dram_sink = sink;
    if (end <= begin) {
        end = begin + 1u;
    }
    return dram_gib_per_s(bytes, end - begin);
}

static double dram_bw_write_sample(uint64_t *data, size_t words)
{
    const uint64_t bytes = (uint64_t)words * (uint64_t)sizeof(uint64_t);
    const uint64_t begin = pal_now_ns();
    const uint64_t sink = dram_bw_write(data, words, UINT64_C(0x517cc1b727220a95));
    uint64_t end = pal_now_ns();

    g_dram_sink = sink;
    if (end <= begin) {
        end = begin + 1u;
    }
    return dram_gib_per_s(bytes, end - begin);
}

static double dram_bw_copy_sample(uint64_t *dst, const uint64_t *src, size_t words)
{
    const uint64_t bytes = (uint64_t)words * (uint64_t)sizeof(uint64_t);
    const uint64_t begin = pal_now_ns();
    const uint64_t sink = dram_bw_copy(dst, src, words);
    uint64_t end = pal_now_ns();

    g_dram_sink = sink;
    if (end <= begin) {
        end = begin + 1u;
    }
    return dram_gib_per_s(bytes, end - begin);
}

lb_status_t dram_bw_run(const lb_cfg_t *cfg)
{
    pal_mem_t *arena_a = NULL;
    pal_mem_t *arena_b = NULL;
    uint64_t *data_a = NULL;
    uint64_t *data_b = NULL;
    const size_t page_size = pal_page_size();
    size_t bytes = 0u;
    size_t words = 0u;
    double read_samples[DRAM_BW_REPEATS] = {0.0};
    double write_samples[DRAM_BW_REPEATS] = {0.0};
    double copy_samples[DRAM_BW_REPEATS] = {0.0};
    double read_gibs = 0.0;
    double write_gibs = 0.0;
    double copy_gibs = 0.0;
    size_t repeats = 0u;
    uint64_t baseline = 0u;
    size_t repeat = 0u;
    lb_status_t status = LB_STATUS_NOMEM;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }
    repeats = cfg->fast ? DRAM_BW_FAST_REPEATS : DRAM_BW_REPEATS;
    if (page_size == 0u) {
        tty_err("cannot determine the system page size");
        return LB_STATUS_UNSUPPORTED;
    }

    dram_apply_runtime(cfg);

    bytes = dram_arena_cap(cfg, cfg->fast ? DRAM_FAST_MAX_BYTES : DRAM_BW_MAX_BYTES, false);
    if (bytes < DRAM_BW_MIN_BYTES) {
        tty_err("memory budget is too small for dram bw");
        return LB_STATUS_NOMEM;
    }
    /*
     * Copy bandwidth needs two buffers, so the shared budget is split in half.
     * On a 32 MiB board a single full-budget buffer per side would otherwise
     * exceed the free memory and trigger the OOM killer.
     */
    bytes /= 2u;
    bytes = (bytes / page_size) * page_size;
    if (bytes < DRAM_BW_MIN_BYTES) {
        tty_err("memory budget is too small for dram bw");
        return LB_STATUS_NOMEM;
    }
    words = bytes / sizeof(uint64_t);

    arena_a = pal_mem_alloc(bytes, dram_arena_flags(cfg));
    if (arena_a == NULL) {
        tty_err("cannot allocate a %zu-byte dram bw buffer", bytes);
        goto cleanup;
    }
    arena_b = pal_mem_alloc(bytes, dram_arena_flags(cfg));
    if (arena_b == NULL) {
        tty_err("cannot allocate a second %zu-byte dram bw buffer", bytes);
        goto cleanup;
    }
    data_a = pal_mem_data(arena_a);
    data_b = pal_mem_data(arena_b);
    if (data_a == NULL || data_b == NULL) {
        goto cleanup;
    }

    {
        char human[LB_NAME_MAX] = {0};

        (void)tty_format_bytes((uint64_t)bytes, human, sizeof(human));
        tty_section(cfg, "DRAM bandwidth");
        tty_kv(cfg, "Buffer", "%s (copy uses two buffers)", human);
        tty_kv(cfg, "Words", "%zu", words);
        tty_kv(cfg, "Repeats", "%zu", repeats);
    }
    tty_header(cfg, "  %-8s %12s", "Pattern", "GiB/s");
    tty_csv_row(cfg, "dram,bw,pattern,gib_per_s");

    baseline = stats_canary_baseline(DRAM_CANARY_MS);

    /* Warm the pages, the TLB entries, and the prefetchers before timing. */
    (void)dram_bw_read(data_a, words);
    (void)dram_bw_write(data_a, words, UINT64_C(0x517cc1b727220a95));
    (void)dram_bw_copy(data_b, data_a, words);

    for (repeat = 0u; repeat < repeats; repeat++) {
        read_samples[repeat] = dram_bw_read_sample(data_a, words);
    }
    dram_cool(cfg);
    for (repeat = 0u; repeat < repeats; repeat++) {
        write_samples[repeat] = dram_bw_write_sample(data_a, words);
    }
    dram_cool(cfg);
    for (repeat = 0u; repeat < repeats; repeat++) {
        copy_samples[repeat] = dram_bw_copy_sample(data_b, data_a, words);
    }
    dram_cool(cfg);

    read_gibs = dram_best(read_samples, repeats);
    write_gibs = dram_best(write_samples, repeats);
    copy_gibs = dram_best(copy_samples, repeats);
    lb_passport_mutable()->dram_read_gibs = read_gibs;
    lb_passport_mutable()->dram_write_gibs = write_gibs;

    tty_header(cfg, "  %-8s %12.2f", "read", read_gibs);
    tty_header(cfg, "  %-8s %12.2f", "write", write_gibs);
    tty_header(cfg, "  %-8s %12.2f", "copy", copy_gibs);
    tty_csv_row(cfg, "dram,bw,read,%.2f", read_gibs);
    tty_csv_row(cfg, "dram,bw,write,%.2f", write_gibs);
    tty_csv_row(cfg, "dram,bw,copy,%.2f", copy_gibs);

    dram_canary_finish(cfg, baseline);
    status = LB_STATUS_OK;

cleanup:
    pal_mem_free(arena_b);
    pal_mem_free(arena_a);
    return status;
}

/* --------------------------------------------------------------- dram row */

static void dram_row_order_same(size_t *order, const size_t *page_order, const size_t *inner,
                                size_t page_count)
{
    size_t pos = 0u;
    size_t page = 0u;

    for (page = 0u; page < page_count; page++) {
        const size_t base = page_order[page] * (size_t)DRAM_ROW_NODES_PER_PAGE;

        for (size_t slot = 0u; slot < DRAM_ROW_NODES_PER_PAGE; slot++) {
            order[pos] = base + inner[slot];
            pos++;
        }
    }
}

static void dram_row_link_sequence(dram_node_t *nodes, const size_t *order, size_t node_count)
{
    size_t i = 0u;

    for (i = 0u; i < node_count; i++) {
        const size_t next = (i + 1u < node_count) ? (i + 1u) : 0u;

        nodes[order[i]].next = &nodes[order[next]];
    }
}

static void dram_row_link_map(dram_node_t *nodes, const size_t *indices, size_t node_count)
{
    size_t i = 0u;

    for (i = 0u; i < node_count; i++) {
        nodes[i].next = &nodes[indices[i]];
    }
}

static PAL_NOINLINE uint64_t dram_row_chase(const dram_node_t *start, uint64_t accesses)
{
    const dram_node_t *p = start;
    uint64_t done = 0u;

    while ((done + UINT64_C(8)) <= accesses) {
        LB_UNROLL_8(p = p->next);
        done += UINT64_C(8);
    }
    while (done < accesses) {
        p = p->next;
        done++;
    }

    PAL_BARRIER();
    g_dram_sink = (uint64_t)(uintptr_t)p;
    return g_dram_sink;
}

static double dram_row_sample(const dram_node_t *start, uint64_t accesses)
{
    const uint64_t begin = pal_now_ns();
    const uint64_t sink = dram_row_chase(start, accesses);
    uint64_t end = pal_now_ns();

    g_dram_sink = sink;
    if (end <= begin || accesses == 0u) {
        return 0.0;
    }
    return (double)(end - begin) / (double)accesses;
}

lb_status_t dram_row_run(const lb_cfg_t *cfg)
{
    pal_mem_t *arena = NULL;
    dram_node_t *nodes = NULL;
    size_t *order = NULL;
    size_t *page_order = NULL;
    size_t inner[DRAM_ROW_NODES_PER_PAGE] = {0u};
    const size_t page_size = pal_page_size();
    size_t arena_bytes = 0u;
    size_t page_count = 0u;
    size_t node_count = 0u;
    double same_samples[DRAM_ROW_REPEATS] = {0.0};
    double cross_samples[DRAM_ROW_REPEATS] = {0.0};
    double same_ns = 0.0;
    double cross_ns = 0.0;
    double delta_ns = 0.0;
    double delta_pct = 0.0;
    uint64_t accesses = 0u;
    size_t repeats = 0u;
    uint64_t baseline = 0u;
    size_t repeat = 0u;
    lb_status_t status = LB_STATUS_NOMEM;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }
    accesses = cfg->fast ? DRAM_ROW_FAST_ACCESSES : DRAM_ROW_ACCESSES;
    repeats = cfg->fast ? DRAM_ROW_FAST_REPEATS : DRAM_ROW_REPEATS;
    if (page_size == 0u) {
        tty_err("cannot determine the system page size");
        return LB_STATUS_UNSUPPORTED;
    }
    if (page_size < (size_t)DRAM_ROW_PAGE_BYTES) {
        tty_err("dram row requires at least %u-byte pages (found %zu)", DRAM_ROW_PAGE_BYTES,
                page_size);
        return LB_STATUS_UNSUPPORTED;
    }

    dram_apply_runtime(cfg);

    arena_bytes = dram_arena_cap(cfg, DRAM_ROW_MAX_BYTES, true);
    arena_bytes = (arena_bytes / page_size) * page_size;
    page_count = arena_bytes / page_size;
    if (page_count < DRAM_ROW_MIN_PAGES) {
        tty_err("memory budget is too small for dram row");
        return LB_STATUS_NOMEM;
    }
    node_count = page_count * (size_t)DRAM_ROW_NODES_PER_PAGE;
    arena_bytes = page_count * page_size;

    arena = pal_mem_alloc(arena_bytes, dram_arena_flags(cfg));
    if (arena == NULL) {
        tty_err("cannot allocate a %zu-byte dram row arena", arena_bytes);
        goto cleanup;
    }
    nodes = pal_mem_data(arena);
    if (nodes == NULL) {
        goto cleanup;
    }

    order = malloc(node_count * sizeof(*order));
    page_order = malloc(page_count * sizeof(*page_order));
    if (order == NULL || page_order == NULL) {
        goto cleanup;
    }

    {
        char human[LB_NAME_MAX] = {0};

        (void)tty_format_bytes((uint64_t)arena_bytes, human, sizeof(human));
        tty_section(cfg, "DRAM row locality");
        tty_kv(cfg, "Arena", "%s", human);
        tty_kv(cfg, "Pages / nodes", "%zu / %zu", page_count, node_count);
        tty_kv(cfg, "Node stride", "%u bytes", DRAM_ROW_NODE_BYTES);
        tty_kv(cfg, "Accesses / repeats", "%" PRIu64 " / %zu", accesses, repeats);
    }
    tty_header(cfg, "  %-10s %14s", "Pattern", "ns/access");
    tty_csv_row(cfg, "dram,row,pattern,ns_per_access");

    baseline = stats_canary_baseline(DRAM_CANARY_MS);

    /* same-row: sixteen random nodes inside a page, then a random page jump. */
    status = util_sattolo_indices(page_order, page_count, UINT64_C(0x6a09e667f3bcc909));
    if (status != LB_STATUS_OK) {
        goto cleanup;
    }
    status = util_sattolo_indices(inner, DRAM_ROW_NODES_PER_PAGE, UINT64_C(0xbb67ae8584caa73b));
    if (status != LB_STATUS_OK) {
        goto cleanup;
    }
    dram_row_order_same(order, page_order, inner, page_count);
    dram_row_link_sequence(nodes, order, node_count);

    (void)dram_row_chase(&nodes[order[0]], DRAM_ROW_WARMUP / 4u);
    for (repeat = 0u; repeat < repeats; repeat++) {
        same_samples[repeat] = dram_row_sample(&nodes[order[0]], accesses);
    }
    dram_cool(cfg);

    /* cross-row: one global Sattolo cycle over the same nodes. */
    status = util_sattolo_indices(order, node_count, UINT64_C(0x3c6ef372fe94f82b));
    if (status != LB_STATUS_OK) {
        goto cleanup;
    }
    dram_row_link_map(nodes, order, node_count);

    (void)dram_row_chase(&nodes[0], DRAM_ROW_WARMUP / 4u);
    for (repeat = 0u; repeat < repeats; repeat++) {
        cross_samples[repeat] = dram_row_sample(&nodes[0], accesses);
    }
    dram_cool(cfg);

    same_ns = stats_percentile(same_samples, repeats, STATS_NOISE_PERCENTILE);
    cross_ns = stats_percentile(cross_samples, repeats, STATS_NOISE_PERCENTILE);
    delta_ns = cross_ns - same_ns;
    if (same_ns > 0.0) {
        delta_pct = (delta_ns / same_ns) * 100.0;
    }
    lb_passport_mutable()->row_delta_ns = delta_ns;
    lb_passport_mutable()->row_delta_pct = delta_pct;

    tty_header(cfg, "  %-10s %14.2f", "same-row", same_ns);
    tty_header(cfg, "  %-10s %14.2f", "cross-row", cross_ns);
    tty_header(cfg, "  %-10s %14.2f (%+.1f%%)", "difference", delta_ns, delta_pct);
    tty_csv_row(cfg, "dram,row,same-row,%.2f", same_ns);
    tty_csv_row(cfg, "dram,row,cross-row,%.2f", cross_ns);
    tty_csv_row(cfg, "dram,row,difference,%.2f", delta_ns);

    dram_canary_finish(cfg, baseline);
    status = LB_STATUS_OK;

cleanup:
    free(page_order);
    free(order);
    pal_mem_free(arena);
    return status;
}
