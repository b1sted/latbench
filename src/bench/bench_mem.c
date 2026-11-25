/* SPDX-License-Identifier: GPL-3.0 */

#include "bench.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "defs.h"
#include "pal/compiler.h"
#include "pal/pal.h"
#include "passport.h"
#include "stats.h"
#include "tty.h"
#include "types.h"
#include "util.h"

/* Cache lines are the natural granularity of the pointer-chase working set. */
#define MEM_NODE_BYTES 64u
#define MEM_NODE_PAD (MEM_NODE_BYTES - sizeof(void *) - sizeof(uint64_t))

/* Pointer hops between clock reads in the timed chase loops. */
#define MEM_CHASE_CHUNK 64u
#define MEM_CHASE_UNROLL 8u

/* Duration of each DVFS frequency-drift canary measurement. */
#define MEM_CANARY_MS 20u

/* Arena policy when the platform cannot report the amount of physical RAM. */
#define MEM_FALLBACK_BUDGET_BYTES (UINT64_C(4) * UINT64_C(1024) * UINT64_C(1024))

/* Smallest arena worth measuring in any subtest. */
#define MEM_MIN_ARENA_BYTES 4096u

/* mem size: hardware-aligned sweep from one page to the safe RAM ceiling. */
#define MEM_SIZE_MIN_BYTES (2u * (size_t)MEM_NODE_BYTES)
#define MEM_SIZE_MAX_POINTS 64u
#define MEM_SIZE_SAMPLES 5u

/*
 * Hardware-aligned working-set grid in KiB. The small steps resolve the L1d,
 * L1i, and L2 boundaries of embedded cores, while the MiB steps cover desktop
 * and mobile last-level caches. A --max-mem above 256 MiB continues in 1.5x
 * steps past the table.
 */
static const size_t MEM_SIZE_GRID_KIB[] = {
    1u,     2u,     4u,     8u,     12u,    16u,    20u,     24u,     28u,     32u,    36u,
    40u,    48u,    56u,    64u,    72u,    80u,    96u,     112u,    128u,    160u,   192u,
    224u,   256u,   320u,   384u,   448u,   512u,   640u,    768u,    896u,    1024u,  1280u,
    1536u,  2048u,  2560u,  3072u,  4096u,  5120u,  6144u,   8192u,   10240u,  12288u, 16384u,
    20480u, 24576u, 32768u, 49152u, 65536u, 98304u, 131072u, 196608u, 262144u,
};

/* Automatic mem size sweeps stop here so the last point stays in RAM. */
#define MEM_SIZE_AUTO_MAX_BYTES (UINT64_C(256) * UINT64_C(1024) * UINT64_C(1024))

/*
 * Compact auto mode: a fine 1.25x sweep with a short batch keeps the whole
 * passport run short while still resolving the 32 KiB L1d boundary.
 */
#define MEM_FAST_MAX_BYTES (UINT64_C(64) * UINT64_C(1024) * UINT64_C(1024))
#define MEM_FAST_BATCH_MS 5u
#define MEM_FAST_SAMPLES 3u
#define MEM_ASSOC_FAST_MAX_BYTES (UINT64_C(8) * UINT64_C(1024) * UINT64_C(1024))
#define MEM_ASSOC_FAST_ACCESSES 32768u
#define MEM_ASSOC_FAST_REPEATS 3u
#define MEM_PREFETCH_FAST_MAX_BYTES (UINT64_C(8) * UINT64_C(1024) * UINT64_C(1024))
#define MEM_PREFETCH_FAST_ACCESSES 262144u
#define MEM_PREFETCH_FAST_REPEATS 2u

/*
 * mem line: two dependent loads per hop over 512-byte blocks. The block start
 * holds the stride and the word at block + stride holds the next block, so a
 * stride below one cache line keeps both loads on the same line while larger
 * strides force a second line into the dependency chain.
 *
 * A 64 MiB arena holds 131072 blocks, so even the single line touched per block
 * at the smallest stride already covers 8 MiB and lands in RAM. That keeps the
 * whole sweep memory-resident and avoids an L3 capacity transition obscuring
 * the line-boundary jump. Each block base is rotated inside its 512-byte slot
 * by 0 or 128 bytes, which preserves 128-byte alignment for the 64 B vs 128 B
 * stride comparison.
 */
#define MEM_LINE_BLOCK_BYTES 512u
#define MEM_LINE_SKEW_BYTES 128u
#define MEM_LINE_SKEW_SLOTS 2u
#define MEM_LINE_SKEW_MAX ((size_t)MEM_LINE_SKEW_BYTES * (MEM_LINE_SKEW_SLOTS - 1u))
#define MEM_LINE_MAX_BYTES (UINT64_C(64) * UINT64_C(1024) * UINT64_C(1024))
#define MEM_LINE_ACCESSES 524288u
#define MEM_LINE_REPEATS 5u

/* mem assoc: N lines spaced stride bytes apart, repeated over one cycle. */
#define MEM_ASSOC_MAX_NODES 32u
#define MEM_ASSOC_MAX_BYTES (UINT64_C(128) * UINT64_C(1024) * UINT64_C(1024))
#define MEM_ASSOC_ACCESSES 65536u
#define MEM_ASSOC_REPEATS 3u
#define MEM_ASSOC_RATIO_MIN 1.65
#define MEM_ASSOC_SUSTAIN_RATIO 1.50
#define MEM_ASSOC_JUMP_MIN_NS 0.25

/*
 * Below this stride the L1d set index is dominated by the offset inside a
 * single line, so the knee is not a clean associativity signal. The passport
 * consensus therefore uses only the larger strides, where the knee settles.
 */
#define MEM_ASSOC_CONSENSUS_MIN_STRIDE 8192u

/*
 * A knee below this jump factor is usually an L1-to-L2 transition on a stride
 * that aliases out of the L1d, not the associativity limit. Such knees would
 * otherwise drag the consensus towards a small way count on Intel cores whose
 * large strides skip a level.
 */
#define MEM_ASSOC_CONSENSUS_RATIO_MIN 2.50

/* mem prefetch: sequential, strided, and Sattolo-random line walks. */
#define MEM_PREFETCH_MAX_BYTES (UINT64_C(32) * UINT64_C(1024) * UINT64_C(1024))
#define MEM_PREFETCH_ACCESSES 1048576u
#define MEM_PREFETCH_REPEATS 3u
#define MEM_PREFETCH_CASES 5u

/*
 * The 64-bit payload comes first so the pointer that follows it never needs
 * implicit alignment padding. On 32-bit ARM, placing the pointer first would
 * insert four bytes before the uint64_t and grow the node to 72 bytes, while
 * the arena and index arrays are sized in 64-byte units.
 */
typedef struct mem_node_t {
    uint64_t payload;
    struct mem_node_t *next;
    /* Structural padding: fixes the node stride at one cache line. */
    uint8_t pad[MEM_NODE_PAD]; /* cppcheck-suppress unusedStructMember */
} mem_node_t;

typedef struct mem_pattern_t {
    const char *name;
    size_t step;
    bool backward;
    bool random;
} mem_pattern_t;

static const char *const MEM_SIZE_LEVELS[] = {"L1", "L2", "L3"};

static const size_t MEM_ASSOC_STRIDES[] = {
    2048u,   4096u,   8192u,   16384u,   32768u,   65536u,
    131072u, 262144u, 524288u, 1048576u, 2097152u, 4194304u,
};

static const size_t MEM_ASSOC_COLUMNS[] = {1u, 2u, 4u, 8u, 16u, 32u};

static const size_t MEM_LINE_STEPS[] = {8u, 16u, 32u, 64u, 128u, 256u};

/*
 * Volatile result sink. Every timed loop publishes its checksum here so the
 * optimizer cannot discard the dependent loads or the dirty stores.
 */
static volatile uint64_t g_mem_sink = 0u;

static size_t mem_arena_cap(const lb_cfg_t *cfg, uint64_t limit_bytes, bool reserve_indices)
{
    uint64_t budget = pal_safe_max_mem_bytes((uint64_t)cfg->max_mem_mb);

    if (budget == 0u) {
        budget = MEM_FALLBACK_BUDGET_BYTES;
    }

    if (reserve_indices) {
        /* Leave one eighth of the budget for Sattolo index arrays. */
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

static uint32_t mem_arena_flags(const lb_cfg_t *cfg)
{
    return cfg->no_thp ? PAL_MEM_NO_THP : PAL_MEM_THP;
}

static void mem_apply_runtime(const lb_cfg_t *cfg)
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

static void mem_cool(const lb_cfg_t *cfg)
{
    if (cfg->cool_ms > 0u) {
        pal_sleep_ms(cfg->cool_ms);
    }
}

static uint32_t mem_batch_ms(const lb_cfg_t *cfg)
{
    uint32_t batch = 0u;

    if (cfg == NULL) {
        return 0u;
    }
    batch = cfg->batch_ms;
    if (!cfg->fast) {
        return batch;
    }
    if (batch == 0u || batch > MEM_FAST_BATCH_MS) {
        batch = MEM_FAST_BATCH_MS;
    }
    return batch;
}

static void mem_publish_cache_steps(const stats_steps_t *steps)
{
    lb_passport_t *passport = lb_passport_mutable();

    if (steps == NULL) {
        return;
    }
    if (steps->count > 0u) {
        passport->l1d_bytes = (uint64_t)steps->items[0].x;
    }
    if (steps->count > 1u) {
        passport->l2_bytes = (uint64_t)steps->items[1].x;
    }
    if (steps->count > 2u) {
        passport->l3_bytes = (uint64_t)steps->items[2].x;
    }
}

static void mem_canary_finish(const lb_cfg_t *cfg, uint64_t baseline)
{
    const uint64_t after = stats_canary_measure(MEM_CANARY_MS);
    const double drift = stats_canary_drift_pct(baseline, after);

    tty_warn_drift(cfg, drift);
    tty_kv(cfg, "DVFS drift", "%+.1f%%", drift);
}

static lb_status_t mem_build_chain(mem_node_t *nodes, size_t count, size_t *indices, uint64_t seed)
{
    size_t i = 0u;
    lb_status_t status = LB_STATUS_OK;

    if (nodes == NULL || count == 0u) {
        return LB_STATUS_INVALID_ARG;
    }

    if (count == 1u) {
        nodes[0].next = &nodes[0];
        nodes[0].payload = 0u;
        return LB_STATUS_OK;
    }
    if (indices == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    status = util_sattolo_indices(indices, count, seed);
    if (status != LB_STATUS_OK) {
        return status;
    }

    for (i = 0u; i < count; i++) {
        nodes[i].next = &nodes[indices[i]];
        nodes[i].payload = (uint64_t)i;
    }
    return LB_STATUS_OK;
}

/*
 * One pointer-chase hop in write mode. The payload update dirties the node's
 * cache line, forcing a write-back when the line is evicted.
 */
static inline mem_node_t *mem_advance_write(mem_node_t *node)
{
    mem_node_t *const next = node->next;

    next->payload += 1u;
    return next;
}

static PAL_NOINLINE void mem_chase_read_until(const mem_node_t *start, uint64_t deadline,
                                              uint64_t *hops_out)
{
    const mem_node_t *p = start;
    uint64_t total = 0u;

    for (;;) {
        size_t chunk = 0u;

        for (chunk = 0u; chunk < MEM_CHASE_CHUNK; chunk++) {
            LB_UNROLL_8(p = p->next);
        }
        total += (uint64_t)MEM_CHASE_CHUNK * (uint64_t)MEM_CHASE_UNROLL;
        if (pal_now_ns() >= deadline) {
            break;
        }
    }

    PAL_BARRIER();
    g_mem_sink = (uint64_t)(uintptr_t)p;
    *hops_out = total;
}

static PAL_NOINLINE void mem_chase_write_until(mem_node_t *start, uint64_t deadline,
                                               uint64_t *hops_out)
{
    mem_node_t *p = start;
    uint64_t total = 0u;

    for (;;) {
        size_t chunk = 0u;

        for (chunk = 0u; chunk < MEM_CHASE_CHUNK; chunk++) {
            LB_UNROLL_8(p = mem_advance_write(p));
        }
        total += (uint64_t)MEM_CHASE_CHUNK * (uint64_t)MEM_CHASE_UNROLL;
        if (pal_now_ns() >= deadline) {
            break;
        }
    }

    PAL_BARRIER();
    g_mem_sink = (uint64_t)(uintptr_t)p;
    *hops_out = total;
}

static PAL_NOINLINE uint64_t mem_chase_hops(const mem_node_t *start, size_t hops)
{
    const mem_node_t *p = start;
    size_t done = 0u;

    while ((done + MEM_CHASE_UNROLL) <= hops) {
        LB_UNROLL_8(p = p->next);
        done += MEM_CHASE_UNROLL;
    }
    while (done < hops) {
        p = p->next;
        done++;
    }

    PAL_BARRIER();
    g_mem_sink = (uint64_t)(uintptr_t)p;
    return g_mem_sink;
}

static double mem_size_sample(mem_node_t *start, uint32_t batch_ms, bool write)
{
    const uint64_t begin = pal_now_ns();
    const uint64_t batch_ns = (uint64_t)batch_ms * UINT64_C(1000000);
    const uint64_t deadline = begin + batch_ns;
    uint64_t hops = 0u;
    uint64_t end = 0u;

    if (write) {
        mem_chase_write_until(start, deadline, &hops);
    } else {
        mem_chase_read_until(start, deadline, &hops);
    }

    end = pal_now_ns();
    if (hops == 0u) {
        return 0.0;
    }
    if (end <= begin) {
        end = begin + 1u;
    }
    return (double)(end - begin) / (double)hops;
}

/*
 * Return the working-set size of grid point index. Indices past the table are
 * generated as 1.5x extensions of the last table entry so an explicit
 * --max-mem above 256 MiB keeps a bounded number of large steps.
 */
static bool mem_size_grid_point(size_t index, size_t *bytes)
{
    const size_t table_count = LB_ARRAY_SIZE(MEM_SIZE_GRID_KIB);
    size_t kib = 0u;

    if (bytes == NULL) {
        return false;
    }
    if (index < table_count) {
        kib = MEM_SIZE_GRID_KIB[index];
    } else {
        size_t steps = (index - table_count) + 1u;

        kib = MEM_SIZE_GRID_KIB[table_count - 1u];
        while (steps > 0u) {
            const size_t next = kib + (kib / 2u);

            if (next <= kib) {
                return false;
            }
            kib = next;
            steps--;
        }
    }

    if (kib > SIZE_MAX / 1024u) {
        return false;
    }
    *bytes = kib * 1024u;
    return true;
}

lb_status_t mem_size_run(const lb_cfg_t *cfg)
{
    pal_mem_t *arena = NULL;
    mem_node_t *nodes = NULL;
    size_t *indices = NULL;
    stats_series_t *series = NULL;
    stats_steps_t steps = {0};
    double samples[MEM_SIZE_SAMPLES] = {0.0};
    size_t sample_count = 0u;
    uint32_t batch_ms = 0u;
    uint64_t size_limit = 0u;
    size_t arena_bytes = 0u;
    size_t max_nodes = 0u;
    size_t size = 0u;
    size_t grid_index = 0u;
    size_t points = 0u;
    uint64_t baseline = 0u;
    lb_status_t status = LB_STATUS_NOMEM;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    sample_count = cfg->fast ? MEM_FAST_SAMPLES : MEM_SIZE_SAMPLES;
    batch_ms = mem_batch_ms(cfg);
    size_limit = cfg->fast ? MEM_FAST_MAX_BYTES
                           : ((cfg->max_mem_mb == 0u) ? MEM_SIZE_AUTO_MAX_BYTES : UINT64_MAX);

    mem_apply_runtime(cfg);

    arena_bytes = mem_arena_cap(cfg, size_limit, true);
    arena_bytes = (arena_bytes / MEM_NODE_BYTES) * MEM_NODE_BYTES;
    if (arena_bytes < MEM_SIZE_MIN_BYTES) {
        tty_err("memory budget is too small for mem size");
        return LB_STATUS_NOMEM;
    }
    max_nodes = arena_bytes / MEM_NODE_BYTES;

    arena = pal_mem_alloc(arena_bytes, mem_arena_flags(cfg));
    if (arena == NULL) {
        tty_err("cannot allocate a %zu-byte mem size arena", arena_bytes);
        goto cleanup;
    }
    nodes = pal_mem_data(arena);
    if (nodes == NULL) {
        goto cleanup;
    }

    indices = malloc(max_nodes * sizeof(*indices));
    if (indices == NULL) {
        goto cleanup;
    }

    series = stats_series_create("mem size", 32u);
    if (series == NULL) {
        goto cleanup;
    }

    {
        char human[LB_NAME_MAX] = {0};

        (void)tty_format_bytes((uint64_t)arena_bytes, human, sizeof(human));
        tty_kv(cfg, "Walk mode", "%s", cfg->write ? "write (dirty)" : "read-only");
        tty_kv(cfg, "Arena limit", "%s", human);
        tty_kv(cfg, "Batch / samples", "%" PRIu32 " ms / %zu", batch_ms, sample_count);
    }

    baseline = stats_canary_baseline(MEM_CANARY_MS);

    for (;;) {
        size_t grid_size = 0u;
        size_t count = 0u;
        uint64_t seed = 0u;
        size_t sample = 0u;

        if (!mem_size_grid_point(grid_index, &grid_size)) {
            break;
        }
        size = grid_size;
        if (size > arena_bytes) {
            /*
             * The arena is smaller than the next hardware step. Measure the
             * arena itself once so a tiny target still reports a real plateau.
             */
            size = arena_bytes;
        }
        if (size < MEM_SIZE_MIN_BYTES) {
            break;
        }

        count = size / MEM_NODE_BYTES;
        seed = UINT64_C(0x9e3779b97f4a7c15) ^ ((uint64_t)count * UINT64_C(0x2545f4914f6cdd1d));

        status = mem_build_chain(nodes, count, indices, seed);
        if (status != LB_STATUS_OK) {
            goto cleanup;
        }

        for (sample = 0u; sample < sample_count; sample++) {
            samples[sample] = mem_size_sample(nodes, batch_ms, cfg->write);
        }
        mem_cool(cfg);

        status = stats_series_push(series, size, samples, sample_count);
        if (status != LB_STATUS_OK) {
            goto cleanup;
        }
        points++;

        if (size >= arena_bytes) {
            break;
        }
        if ((points + 1u) >= MEM_SIZE_MAX_POINTS) {
            break;
        }
        grid_index++;
    }

    mem_canary_finish(cfg, baseline);

    status = stats_series_detect_steps(series, &steps);
    if (status != LB_STATUS_OK) {
        goto cleanup;
    }
    mem_publish_cache_steps(&steps);
    status =
        tty_plot_series(cfg, series, &steps, MEM_SIZE_LEVELS, LB_ARRAY_SIZE(MEM_SIZE_LEVELS), 0u);

cleanup:
    stats_series_free(series);
    free(indices);
    pal_mem_free(arena);
    return status;
}

static inline size_t mem_next_index(size_t index, size_t step, size_t length, bool backward)
{
    if (backward) {
        if (index >= step) {
            return index - step;
        }
        return length - (step - index);
    }

    index += step;
    if (index >= length) {
        return index - length;
    }
    return index;
}

static lb_status_t mem_line_build(uint8_t *buf, size_t block_count, size_t stride, size_t *indices,
                                  uint64_t seed)
{
    size_t i = 0u;
    lb_status_t status = LB_STATUS_INVALID_ARG;

    if (buf == NULL || indices == NULL || block_count < 2u) {
        return LB_STATUS_INVALID_ARG;
    }
    if ((stride < sizeof(uintptr_t)) ||
        ((stride + sizeof(uintptr_t) + MEM_LINE_SKEW_MAX) > MEM_LINE_BLOCK_BYTES)) {
        return LB_STATUS_INVALID_ARG;
    }
    if ((stride % sizeof(uintptr_t)) != 0u) {
        return LB_STATUS_INVALID_ARG;
    }

    status = util_sattolo_indices(indices, block_count, seed);
    if (status != LB_STATUS_OK) {
        return status;
    }

    for (i = 0u; i < block_count; i++) {
        uint8_t *const block = buf + (i * (size_t)MEM_LINE_BLOCK_BYTES) +
                               ((i % MEM_LINE_SKEW_SLOTS) * MEM_LINE_SKEW_BYTES);
        const uint8_t *const next = buf + (indices[i] * (size_t)MEM_LINE_BLOCK_BYTES) +
                                    ((indices[i] % MEM_LINE_SKEW_SLOTS) * MEM_LINE_SKEW_BYTES);

        *(uintptr_t *)(void *)block = (uintptr_t)stride;
        *(uintptr_t *)(void *)(block + stride) = (uintptr_t)next;
    }
    return LB_STATUS_OK;
}

/*
 * Two dependent loads per hop: the offset at the block start selects the word
 * that holds the next block address. Both words stay inside one cache line
 * while stride < cache_line, and the second load moves to a new line once the
 * stride crosses it.
 */
static PAL_NOINLINE uint64_t mem_line_chase(const uint8_t *start, size_t hops)
{
    const uint8_t *p = start;
    size_t done = 0u;

    PAL_NO_VECTORIZE_LOOP
    while (done < hops) {
        const uintptr_t off = *(const uintptr_t *)(const void *)p;

        p = (const uint8_t *)(*(const uintptr_t *)(const void *)(p + off));
        done++;
    }

    PAL_BARRIER();
    g_mem_sink = (uint64_t)(uintptr_t)p;
    return g_mem_sink;
}

lb_status_t mem_line_run(const lb_cfg_t *cfg)
{
    pal_mem_t *arena = NULL;
    uint8_t *buf = NULL;
    size_t *indices = NULL;
    stats_series_t *series = NULL;
    double samples[MEM_LINE_REPEATS] = {0.0};
    size_t buffer_bytes = 0u;
    size_t block_count = 0u;
    size_t step_index = 0u;
    uint64_t baseline = 0u;
    lb_status_t status = LB_STATUS_NOMEM;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    mem_apply_runtime(cfg);

    buffer_bytes = mem_arena_cap(cfg, MEM_LINE_MAX_BYTES, false);
    if (buffer_bytes < MEM_MIN_ARENA_BYTES) {
        tty_err("memory budget is too small for mem line");
        return LB_STATUS_NOMEM;
    }
    buffer_bytes = (buffer_bytes / MEM_LINE_BLOCK_BYTES) * MEM_LINE_BLOCK_BYTES;
    block_count = buffer_bytes / MEM_LINE_BLOCK_BYTES;
    if (block_count < 2u) {
        tty_err("memory budget is too small for mem line");
        return LB_STATUS_NOMEM;
    }

    arena = pal_mem_alloc(buffer_bytes, mem_arena_flags(cfg));
    if (arena == NULL) {
        tty_err("cannot allocate a %zu-byte mem line buffer", buffer_bytes);
        goto cleanup;
    }
    buf = pal_mem_data(arena);
    if (buf == NULL) {
        goto cleanup;
    }

    indices = malloc(block_count * sizeof(*indices));
    if (indices == NULL) {
        goto cleanup;
    }

    series = stats_series_create("mem line", LB_ARRAY_SIZE(MEM_LINE_STEPS));
    if (series == NULL) {
        goto cleanup;
    }

    {
        char human[LB_NAME_MAX] = {0};
        char block[LB_NAME_MAX] = {0};

        (void)tty_format_bytes((uint64_t)buffer_bytes, human, sizeof(human));
        (void)tty_format_bytes((uint64_t)MEM_LINE_BLOCK_BYTES, block, sizeof(block));
        tty_kv(cfg, "Buffer", "%s", human);
        tty_kv(cfg, "Block size", "%s (%zu blocks)", block, block_count);
        tty_kv(cfg, "Accesses / repeats", "%u / %u", MEM_LINE_ACCESSES, MEM_LINE_REPEATS);
    }

    baseline = stats_canary_baseline(MEM_CANARY_MS);

    for (step_index = 0u; step_index < LB_ARRAY_SIZE(MEM_LINE_STEPS); step_index++) {
        const size_t step = MEM_LINE_STEPS[step_index];
        const uint64_t seed =
            UINT64_C(0x452821e638d01377) ^ ((uint64_t)step * UINT64_C(0x2545f4914f6cdd1d));
        size_t repeat = 0u;

        status = mem_line_build(buf, block_count, step, indices, seed);
        if (status != LB_STATUS_OK) {
            goto cleanup;
        }

        for (repeat = 0u; repeat < MEM_LINE_REPEATS; repeat++) {
            const uint64_t begin = pal_now_ns();
            const uint64_t sink = mem_line_chase(buf, MEM_LINE_ACCESSES);
            uint64_t end = pal_now_ns();

            g_mem_sink = sink;
            if (end <= begin) {
                end = begin + 1u;
            }
            samples[repeat] = (double)(end - begin) / (double)(size_t)MEM_LINE_ACCESSES;
        }
        mem_cool(cfg);

        status = stats_series_push(series, step, samples, MEM_LINE_REPEATS);
        if (status != LB_STATUS_OK) {
            goto cleanup;
        }
    }

    mem_canary_finish(cfg, baseline);

    status = tty_plot_series(cfg, series, NULL, NULL, 0u, 0u);

cleanup:
    stats_series_free(series);
    free(indices);
    pal_mem_free(arena);
    return status;
}

/*
 * Find the first sustained exit from the N=1..2 base plateau. The global
 * next/prev maximum used to lock onto a late L2-to-RAM transition (N=26 on
 * Cortex-A7), so the knee is now the first N whose latency clears the base by
 * 1.65x while the following N stays at least 1.50x above the base.
 */
static size_t mem_assoc_knee(const double *row, size_t limit, double *ratio_out)
{
    double base = 0.0;
    size_t column = 0u;

    if (ratio_out != NULL) {
        *ratio_out = 0.0;
    }
    if (row == NULL || limit < 4u) {
        return 0u;
    }

    base = row[0];
    if (row[1] > base) {
        base = row[1];
    }
    if (!(base > 0.0)) {
        return 0u;
    }

    for (column = 2u; (column + 1u) < limit; column++) {
        const double upper = row[column];
        const double sustained = row[column + 1u];

        if ((upper - base) < MEM_ASSOC_JUMP_MIN_NS) {
            continue;
        }
        if (upper < (base * MEM_ASSOC_RATIO_MIN)) {
            continue;
        }
        if (sustained < (base * MEM_ASSOC_SUSTAIN_RATIO)) {
            continue;
        }
        if (ratio_out != NULL) {
            *ratio_out = upper / base;
        }
        return column + 1u;
    }
    return 0u;
}

/*
 * Publish the associativity consensus over the larger strides. The knee is the
 * first conflicting N, so the last fitting N is knee - 1. A lower median keeps
 * one noisy stride from moving the passport.
 */
static void mem_sort_ways(uint32_t *values, size_t count)
{
    size_t i = 0u;

    for (i = 1u; i < count; i++) {
        const uint32_t value = values[i];
        size_t j = i;

        while (j > 0u && values[j - 1u] > value) {
            values[j] = values[j - 1u];
            j--;
        }
        values[j] = value;
    }
}

/*
 * Publish the associativity consensus over the larger strides. The knee is the
 * first conflicting N, so the last fitting N is knee - 1. Knees with a weak
 * jump are L1-to-L2 transitions on aliasing strides, not an associativity
 * limit, so the consensus prefers the convincing knees and falls back to every
 * valid high-stride knee when too few are strong.
 */
static void mem_publish_assoc(double latencies[][MEM_ASSOC_MAX_NODES], const size_t limits[],
                              size_t stride_count)
{
    lb_passport_t *passport = lb_passport_mutable();
    uint32_t ways_all[LB_ARRAY_SIZE(MEM_ASSOC_STRIDES)] = {0u};
    uint32_t ways_strong[LB_ARRAY_SIZE(MEM_ASSOC_STRIDES)] = {0u};
    size_t all_count = 0u;
    size_t strong_count = 0u;
    size_t stride_index = 0u;

    for (stride_index = 0u; stride_index < stride_count; stride_index++) {
        double ratio = 0.0;
        size_t ways = 0u;

        if (MEM_ASSOC_STRIDES[stride_index] < MEM_ASSOC_CONSENSUS_MIN_STRIDE) {
            continue;
        }
        ways = mem_assoc_knee(latencies[stride_index], limits[stride_index], &ratio);
        if (ways > 1u && ways <= MEM_ASSOC_MAX_NODES) {
            const uint32_t last_fitting = (uint32_t)(ways - 1u);

            ways_all[all_count] = last_fitting;
            all_count++;
            if (ratio >= MEM_ASSOC_CONSENSUS_RATIO_MIN) {
                ways_strong[strong_count] = last_fitting;
                strong_count++;
            }
        }
    }

    if (strong_count >= 2u) {
        mem_sort_ways(ways_strong, strong_count);
        passport->l1d_ways = ways_strong[(strong_count - 1u) / 2u];
    } else if (all_count > 0u) {
        mem_sort_ways(ways_all, all_count);
        passport->l1d_ways = ways_all[(all_count - 1u) / 2u];
    } else {
        passport->l1d_ways = 0u;
    }
}

static void mem_assoc_table(const lb_cfg_t *cfg, double latencies[][MEM_ASSOC_MAX_NODES],
                            const size_t limits[], size_t stride_count)
{
    size_t stride_index = 0u;

    tty_section(cfg, "Associativity");
    tty_header(cfg, "  %-10s %8s %8s %8s %8s %8s %8s  %s", "Stride", "N=1", "N=2", "N=4", "N=8",
               "N=16", "N=32", "Knee");

    for (stride_index = 0u; stride_index < stride_count; stride_index++) {
        const size_t stride = MEM_ASSOC_STRIDES[stride_index];
        const size_t limit = limits[stride_index];
        char stride_text[LB_NAME_MAX] = {0};
        char cells[LB_ARRAY_SIZE(MEM_ASSOC_COLUMNS)][16] = {{0}};
        char knee[LB_NAME_MAX] = {0};
        char line[LB_LINE_MAX] = {0};
        size_t best_node = 0u;
        double best_ratio = 0.0;
        size_t column = 0u;

        (void)tty_format_bytes((uint64_t)stride, stride_text, sizeof(stride_text));

        for (column = 0u; column < LB_ARRAY_SIZE(MEM_ASSOC_COLUMNS); column++) {
            const size_t nodes = MEM_ASSOC_COLUMNS[column];

            if (nodes <= limit) {
                (void)snprintf(cells[column], sizeof(cells[column]), "%.2f",
                               latencies[stride_index][nodes - 1u]);
            } else {
                (void)snprintf(cells[column], sizeof(cells[column]), "-");
            }
        }

        best_node = mem_assoc_knee(latencies[stride_index], limit, &best_ratio);

        if (best_node > 0u) {
            (void)snprintf(knee, sizeof(knee), "N=%zu x%.2f", best_node, best_ratio);
        } else {
            (void)snprintf(knee, sizeof(knee), "none");
        }

        (void)snprintf(line, sizeof(line), "  %-10s %8s %8s %8s %8s %8s %8s  %s", stride_text,
                       cells[0], cells[1], cells[2], cells[3], cells[4], cells[5], knee);
        tty_header(cfg, "%s", line);
    }
}

lb_status_t mem_assoc_run(const lb_cfg_t *cfg)
{
    pal_mem_t *arena = NULL;
    uint8_t *base = NULL;
    double latencies[LB_ARRAY_SIZE(MEM_ASSOC_STRIDES)][MEM_ASSOC_MAX_NODES] = {{0.0}};
    size_t limits[LB_ARRAY_SIZE(MEM_ASSOC_STRIDES)] = {0u};
    size_t accesses = 0u;
    size_t repeats = 0u;
    size_t buffer_bytes = 0u;
    size_t stride_count = LB_ARRAY_SIZE(MEM_ASSOC_STRIDES);
    size_t stride_index = 0u;
    uint64_t baseline = 0u;
    lb_status_t status = LB_STATUS_NOMEM;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    accesses = cfg->fast ? MEM_ASSOC_FAST_ACCESSES : MEM_ASSOC_ACCESSES;
    repeats = cfg->fast ? MEM_ASSOC_FAST_REPEATS : MEM_ASSOC_REPEATS;

    mem_apply_runtime(cfg);

    buffer_bytes =
        mem_arena_cap(cfg, cfg->fast ? MEM_ASSOC_FAST_MAX_BYTES : MEM_ASSOC_MAX_BYTES, false);
    if (buffer_bytes < MEM_MIN_ARENA_BYTES) {
        tty_err("memory budget is too small for mem assoc");
        return LB_STATUS_NOMEM;
    }

    arena = pal_mem_alloc(buffer_bytes, mem_arena_flags(cfg));
    if (arena == NULL) {
        tty_err("cannot allocate a %zu-byte mem assoc buffer", buffer_bytes);
        goto cleanup;
    }
    base = pal_mem_data(arena);
    if (base == NULL) {
        goto cleanup;
    }

    {
        char human[LB_NAME_MAX] = {0};

        (void)tty_format_bytes((uint64_t)buffer_bytes, human, sizeof(human));
        tty_kv(cfg, "Buffer", "%s", human);
        tty_kv(cfg, "Accesses / repeats", "%zu / %zu", accesses, repeats);
    }

    tty_csv_row(cfg, "mem,assoc,stride_bytes,nodes,ns_per_access");

    baseline = stats_canary_baseline(MEM_CANARY_MS);

    for (stride_index = 0u; stride_index < stride_count; stride_index++) {
        const size_t stride = MEM_ASSOC_STRIDES[stride_index];
        size_t nodes = buffer_bytes / stride;

        if (nodes > MEM_ASSOC_MAX_NODES) {
            nodes = MEM_ASSOC_MAX_NODES;
        }
        limits[stride_index] = nodes;

        if (nodes == 0u) {
            continue;
        }

        {
            size_t node_count = 1u;

            for (node_count = 1u; node_count <= nodes; node_count++) {
                size_t order[MEM_ASSOC_MAX_NODES] = {0u};
                double samples[MEM_ASSOC_REPEATS] = {0.0};
                size_t i = 0u;
                size_t repeat = 0u;
                const uint64_t seed =
                    UINT64_C(0x243f6a8885a308d3) ^ ((uint64_t)stride << 24u) ^ (uint64_t)node_count;

                status = util_sattolo_indices(order, node_count, seed);
                if (status != LB_STATUS_OK) {
                    goto cleanup;
                }
                for (i = 0u; i < node_count; i++) {
                    mem_node_t *const node = (mem_node_t *)(void *)(base + (i * stride));
                    mem_node_t *const next = (mem_node_t *)(void *)(base + (order[i] * stride));

                    node->next = next;
                    node->payload = (uint64_t)i;
                }

                for (repeat = 0u; repeat < repeats; repeat++) {
                    const uint64_t begin = pal_now_ns();
                    const uint64_t sink = mem_chase_hops((mem_node_t *)(void *)base, accesses);
                    uint64_t end = pal_now_ns();

                    g_mem_sink = sink;
                    if (end <= begin) {
                        end = begin + 1u;
                    }
                    samples[repeat] = (double)(end - begin) / (double)accesses;
                }
                mem_cool(cfg);

                latencies[stride_index][node_count - 1u] =
                    stats_percentile(samples, repeats, STATS_NOISE_PERCENTILE);

                tty_csv_row(cfg, "mem,assoc,%zu,%zu,%.3f", stride, node_count,
                            latencies[stride_index][node_count - 1u]);
            }
        }
    }

    mem_canary_finish(cfg, baseline);
    mem_publish_assoc(latencies, limits, stride_count);
    mem_assoc_table(cfg, latencies, limits, stride_count);
    status = LB_STATUS_OK;

cleanup:
    pal_mem_free(arena);
    return status;
}

static PAL_NOINLINE uint64_t mem_stream_probe(const uint8_t *buf, size_t length, size_t step,
                                              bool backward, size_t accesses)
{
    uint64_t sum = 0u;
    size_t index = backward ? (length - 1u) : 0u;
    size_t done = 0u;

    PAL_NO_VECTORIZE_LOOP
    while (done < accesses) {
        sum += (uint64_t)buf[index];
        index = mem_next_index(index, step, length, backward);
        done++;
    }

    PAL_BARRIER();
    return sum;
}

static double mem_pattern_ns_per_access(const uint8_t *buf, size_t length,
                                        const mem_pattern_t *pattern, size_t accesses)
{
    const uint64_t begin = pal_now_ns();
    const uint64_t sum = mem_stream_probe(buf, length, pattern->step, pattern->backward, accesses);
    uint64_t end = pal_now_ns();

    g_mem_sink = sum;
    if (end <= begin) {
        end = begin + 1u;
    }
    return (double)(end - begin) / (double)accesses;
}

lb_status_t mem_prefetch_run(const lb_cfg_t *cfg)
{
    pal_mem_t *arena = NULL;
    uint8_t *buf = NULL;
    mem_node_t *nodes = NULL;
    size_t *indices = NULL;
    double results[MEM_PREFETCH_CASES] = {0.0};
    double samples[MEM_PREFETCH_REPEATS] = {0.0};
    size_t accesses = 0u;
    size_t repeats = 0u;
    mem_pattern_t patterns[MEM_PREFETCH_CASES] = {{0}};
    size_t buffer_bytes = 0u;
    size_t node_count = 0u;
    size_t page_step = 0u;
    size_t case_index = 0u;
    uint64_t baseline = 0u;
    lb_status_t status = LB_STATUS_NOMEM;

    if (cfg == NULL) {
        return LB_STATUS_INVALID_ARG;
    }

    accesses = cfg->fast ? MEM_PREFETCH_FAST_ACCESSES : MEM_PREFETCH_ACCESSES;
    repeats = cfg->fast ? MEM_PREFETCH_FAST_REPEATS : MEM_PREFETCH_REPEATS;

    mem_apply_runtime(cfg);

    buffer_bytes =
        mem_arena_cap(cfg, cfg->fast ? MEM_PREFETCH_FAST_MAX_BYTES : MEM_PREFETCH_MAX_BYTES, true);
    if (buffer_bytes < MEM_MIN_ARENA_BYTES) {
        tty_err("memory budget is too small for mem prefetch");
        return LB_STATUS_NOMEM;
    }

    arena = pal_mem_alloc(buffer_bytes, mem_arena_flags(cfg));
    if (arena == NULL) {
        tty_err("cannot allocate a %zu-byte mem prefetch buffer", buffer_bytes);
        goto cleanup;
    }
    buf = pal_mem_data(arena);
    if (buf == NULL) {
        goto cleanup;
    }

    node_count = buffer_bytes / MEM_NODE_BYTES;
    if (node_count < 2u) {
        goto cleanup;
    }
    nodes = (mem_node_t *)(void *)buf;
    indices = malloc(node_count * sizeof(*indices));
    if (indices == NULL) {
        goto cleanup;
    }
    status = mem_build_chain(nodes, node_count, indices,
                             UINT64_C(0x13198a2e03707344) ^ (uint64_t)node_count);
    if (status != LB_STATUS_OK) {
        goto cleanup;
    }

    page_step = pal_page_size() + MEM_NODE_BYTES;
    if (page_step >= buffer_bytes / 2u) {
        page_step = buffer_bytes / 2u;
    }
    if (page_step == 0u) {
        page_step = 1u;
    }

    patterns[0].name = "sequential";
    patterns[0].step = 1u;
    patterns[1].name = "stride-64";
    patterns[1].step = MEM_NODE_BYTES;
    patterns[2].name = "page-cross";
    patterns[2].step = page_step;
    patterns[3].name = "page-cross-back";
    patterns[3].step = page_step;
    patterns[3].backward = true;
    patterns[4].name = "sattolo-random";
    patterns[4].random = true;

    {
        char human[LB_NAME_MAX] = {0};

        (void)tty_format_bytes((uint64_t)buffer_bytes, human, sizeof(human));
        tty_kv(cfg, "Buffer", "%s", human);
        tty_kv(cfg, "Accesses / repeats", "%zu / %zu", accesses, repeats);
    }

    tty_csv_row(cfg, "mem,prefetch,variant,ns_per_access");

    baseline = stats_canary_baseline(MEM_CANARY_MS);

    for (case_index = 0u; case_index < MEM_PREFETCH_CASES; case_index++) {
        size_t repeat = 0u;

        for (repeat = 0u; repeat < repeats; repeat++) {
            if (patterns[case_index].random) {
                const uint64_t begin = pal_now_ns();
                const uint64_t sink = mem_chase_hops(nodes, accesses);
                uint64_t end = pal_now_ns();

                g_mem_sink = sink;
                if (end <= begin) {
                    end = begin + 1u;
                }
                samples[repeat] = (double)(end - begin) / (double)accesses;
            } else {
                samples[repeat] =
                    mem_pattern_ns_per_access(buf, buffer_bytes, &patterns[case_index], accesses);
            }
        }
        mem_cool(cfg);

        results[case_index] = stats_percentile(samples, repeats, STATS_NOISE_PERCENTILE);
        tty_csv_row(cfg, "mem,prefetch,%s,%.3f", patterns[case_index].name, results[case_index]);
    }

    mem_canary_finish(cfg, baseline);

    tty_section(cfg, "Prefetcher");
    tty_header(cfg, "  %-22s %12s", "Pattern", "ns/access");
    for (case_index = 0u; case_index < MEM_PREFETCH_CASES; case_index++) {
        tty_header(cfg, "  %-22s %12.3f", patterns[case_index].name, results[case_index]);
    }
    status = LB_STATUS_OK;

cleanup:
    free(indices);
    pal_mem_free(arena);
    return status;
}
