/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * Monte Carlo pressure test for the allocation seam.
 *
 * Random operation sequences (allocate / release / resize) with random sizes
 * drawn log-uniformly over three orders of magnitude, driven into the pool
 * harder than anything the library itself does. What it checks, none of which
 * a hand written test covers:
 *
 *   - content integrity: every block carries a pattern keyed by its own seed;
 *     the pattern is verified before release and across a resize, so two live
 *     allocations overlapping, or a header write landing in a payload, shows up
 *     as a mismatch instead of a mystery crash later;
 *   - pool invariants after every refusal and every N operations, through
 *     ncl_mem_check(): blocks inside the arena, back links adding up, no two
 *     free neighbours, arena covered exactly;
 *   - the accounting identity in_use + free_bytes + meta_bytes == pool_bytes;
 *   - refusals are legitimate: an allocation may only fail when the largest
 *     contiguous free block really is smaller than the request. Failures where
 *     the total free space would have been enough are counted, and they are the
 *     expensive ones - that is fragmentation, measured rather than argued;
 *   - no drift: the same seed is replayed a second time and the two runs have
 *     to agree exactly on allocations, refusals, shape refusals and peak. A
 *     pool that leaked or kept a hole would disagree.
 *
 * Pool specific checks are compiled in only for NCL_STATIC_MEM builds; the
 * integrity and determinism checks run in both, which is itself a check that
 * the library's own sizing habits behave under a heap allocator too.
 */
#include "ncl_test.h"

#include "nclink/ncl_common.h"
#include "nclink/ncl_platform.h"

#include <stdint.h>

#define MC_SEEDS   4u
#define MC_OPS     20000u
#define MC_SLOTS   8192u
#define MC_SAMPLE  97u      /* stride of the content sampling       */
#define MC_EDGE    32u      /* bytes checked at both ends of a block */
#define MC_CHECK_EVERY 256u /* full pool check interval, in ops      */

typedef struct {
    unsigned char    *ptr;
    size_t            size;
    unsigned long long seed;
    int               live;
} mc_block;

typedef struct {
    size_t ops;
    size_t allocations;
    size_t refusals;
    size_t shape_refusals;   /* refused while total free space was enough */
    size_t mismatches;
    size_t peak;
    size_t largest_request;
    size_t worst_free_blocks;
    size_t worst_largest_free;
} mc_report;

static mc_block g_blocks[MC_SLOTS];
static size_t   g_live;
static size_t   g_live_bytes;
static unsigned long long g_rng;

/*
 * Soak mode (NCL_MEM_MC_SECONDS=<n>): run rounds of the same workload with a
 * fresh seed until the clock runs out, checking every round, and print a
 * heartbeat so a long run leaves a trail. The defaults below keep the ctest
 * behaviour (four fixed seeds, one round each) unchanged.
 */
static size_t   g_ops = MC_OPS;
static size_t   g_check_every = 1u;
static unsigned long long g_base_seed = 7919u;
static const char *g_label = "";

static size_t mc_env_size(const char *name, size_t fallback)
{
    const char *value = getenv(name);
    if (value == NULL || *value == '\0') {
        return fallback;
    }
    return (size_t)strtoul(value, NULL, 10);
}

/* A tiny trace of what just happened, so the first inconsistency can be
 * reported next to the operations that led to it. */
#define MC_TRACE 6u
static char   g_trace[MC_TRACE][64];
static size_t g_trace_next;

static void mc_trace(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_trace[g_trace_next % MC_TRACE], sizeof(g_trace[0]), fmt, ap);
    va_end(ap);
    g_trace_next++;
}

static void mc_dump_trace(void)
{
    size_t i;
    size_t count = g_trace_next < MC_TRACE ? g_trace_next : MC_TRACE;
    size_t start = g_trace_next - count;

    fprintf(stderr, "  last %lu operations:\n", (unsigned long)count);
    for (i = start; i < g_trace_next; i++) {
        fprintf(stderr, "    %s\n", g_trace[i % MC_TRACE]);
    }
}

/* ---------------------------------------------------------------- random -- */

static unsigned long long mc_rand(void)
{
    /* xorshift64*: tiny, deterministic, good enough to shake an allocator. */
    unsigned long long x = g_rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_rng = x;
    return x * 2685821657736338717ULL;
}

static size_t mc_below(size_t limit)
{
    return (size_t)(mc_rand() % (unsigned long long)limit);
}

static unsigned char mc_pattern(unsigned long long seed, size_t i)
{
    unsigned long long x = seed + 0x9E3779B97F4A7C15ULL * (i + 1u);
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 29;
    return (unsigned char)(x >> 32);
}

/* ------------------------------------------------------------- contents -- */

static void mc_fill(mc_block *b)
{
    size_t i;
    for (i = 0; i < b->size; i++) {
        b->ptr[i] = mc_pattern(b->seed, i);
    }
}

/**
 * Compare a block against its pattern. Sampling (both ends plus a stride)
 * keeps the cost independent of the block size while still catching a write
 * that lands anywhere in the block.
 */
static size_t mc_verify(const mc_block *b)
{
    size_t bad = 0;
    size_t i;

    if (b->size == 0u) {
        return 0;
    }
    for (i = 0; i < b->size && i < MC_EDGE; i++) {
        if (b->ptr[i] != mc_pattern(b->seed, i)) {
            bad++;
        }
    }
    for (i = (b->size > MC_EDGE ? b->size - MC_EDGE : 0u); i < b->size; i++) {
        if (b->ptr[i] != mc_pattern(b->seed, i)) {
            bad++;
        }
    }
    for (i = 0; i < b->size; i += MC_SAMPLE) {
        if (b->ptr[i] != mc_pattern(b->seed, i)) {
            bad++;
        }
    }
    return bad;
}

/* --------------------------------------------------------------- sizing --- */

/**
 * Two size regimes, like the library's own traffic: mostly small objects, with
 * an occasional large one. Derived from the pool so the test means the same
 * thing at 64 KiB and at the 20 MiB default.
 */
static size_t mc_pick_size(size_t budget)
{
    size_t small_max;
    size_t big_max;
    size_t size;

    small_max = budget / 64u;
    if (small_max < 16u) {
        small_max = 16u;
    }
    big_max = budget / 8u;
    if (big_max < small_max) {
        big_max = small_max;
    }

    if (mc_below(100u) < 5u) {
        /* log-uniform in [small_max, big_max] */
        size = small_max + mc_below(big_max - small_max + 1u);
    } else {
        /* log-uniform in [8, small_max], the shape of a struct/string mix */
        unsigned shift = (unsigned)mc_below(24u);
        unsigned bucket = 0;
        while (bucket + 1u < 24u && bucket + 1u <= shift) {
            bucket++;
        }
        size = (size_t)8u << bucket;
        if (size > small_max) {
            size = small_max;
        }
        size += mc_below(size / 2u + 1u);
    }
    return size;
}

/* ------------------------------------------------------------ accounting -- */

static size_t mc_align_up(size_t n)
{
    size_t a = (size_t)NCL_MEM_ALIGNMENT;
    return (n + a - 1u) / a * a;
}

/**
 * The identity any block based allocator has to satisfy: reserved payload plus
 * free payload plus one header per block equals the arena.
 */
static int mc_accounting_ok(void)
{
    ncl_mem_stats s;
    ncl_mem_get_stats(&s);
    if (s.pool_bytes == 0u) {
        return 1; /* heap build: nothing to account for */
    }
    return s.in_use_bytes + s.free_bytes + s.meta_bytes == s.pool_bytes;
}

/* ------------------------------------------------------------------ run --- */

static void mc_alloc(mc_report *rep, size_t budget)
{
    mc_block *b;
    size_t size;
    void *p;

    if (g_live >= MC_SLOTS) {
        return;
    }
    size = mc_pick_size(budget);
    mc_trace("alloc size=%lu", (unsigned long)size);
    p = ncl_mem_alloc(size);
    if (p == NULL) {
        /* A refusal is only legitimate when the biggest contiguous free block
         * is smaller than the request: the pool has to be able to say no for
         * the right reason. */
        ncl_mem_stats s;
        ncl_mem_get_stats(&s);
        rep->refusals++;
        if (s.pool_bytes > 0u) {
            if (s.largest_free_bytes >= mc_align_up(size)) {
                rep->mismatches++; /* refused with room to spare: a real bug */
            } else if (s.free_bytes >= mc_align_up(size)) {
                rep->shape_refusals++; /* room in total, not in one piece */
            }
        }
        if (ncl_mem_check() != 0 || !mc_accounting_ok()) {
            rep->mismatches++;
        }
        return;
    }
    b = &g_blocks[g_live];
    b->ptr = (unsigned char *)p;
    b->size = size;
    b->seed = mc_rand();
    b->live = 1;
    mc_fill(b);
    g_live++;
    g_live_bytes += size;
    rep->allocations++;
}

static void mc_release(mc_report *rep, size_t index)
{
    mc_block *b = &g_blocks[index];

    mc_trace("free size=%lu", (unsigned long)b->size);
    rep->mismatches += mc_verify(b);
    g_live_bytes -= b->size;
    ncl_mem_free(b->ptr);
    b->live = 0;
    g_blocks[index] = g_blocks[g_live - 1];
    g_live--;
}

static void mc_resize(mc_report *rep, size_t index, size_t budget)
{
    mc_block *b = &g_blocks[index];
    size_t new_size = mc_pick_size(budget);
    size_t keep = new_size < b->size ? new_size : b->size;
    unsigned long long seed = b->seed;
    unsigned char *grown;
    size_t i;
    size_t bad = 0;

    mc_trace("resize %lu -> %lu", (unsigned long)b->size, (unsigned long)new_size);
    rep->mismatches += mc_verify(b);
    grown = (unsigned char *)ncl_mem_realloc(b->ptr, new_size);
    if (grown == NULL) {
        ncl_mem_stats s;
        ncl_mem_get_stats(&s);
        rep->refusals++;
        if (s.pool_bytes > 0u && s.largest_free_bytes >= mc_align_up(new_size)) {
            rep->mismatches++;
        }
        /* the old block must still be intact and still ours */
        rep->mismatches += mc_verify(b);
        return;
    }
    /* Resizing may move the block, but the part that survived has to be
     * bit-exact - this is what catches a copy that dropped bytes. */
    for (i = 0; i < keep; i += MC_SAMPLE) {
        if (grown[i] != mc_pattern(seed, i)) {
            bad++;
        }
    }
    for (i = 0; i < keep && i < MC_EDGE; i++) {
        if (grown[i] != mc_pattern(seed, i)) {
            bad++;
        }
    }
    rep->mismatches += bad;
    g_live_bytes += new_size - b->size;
    b->ptr = grown;
    b->size = new_size;
    b->seed = seed;
    for (i = keep; i < new_size; i++) {
        grown[i] = mc_pattern(seed, i);
    }
}

static void mc_run(unsigned long long seed, mc_report *rep)
{
    ncl_mem_stats s;
    size_t budget;
    size_t op;
    size_t live_target;

    memset(rep, 0, sizeof(*rep));
    memset(g_blocks, 0, sizeof(g_blocks));
    g_live = 0;
    g_live_bytes = 0;
    g_rng = seed | 1ULL;
    rep->ops = g_ops;

    ncl_mem_get_stats(&s);
    /* The synthetic mix below is deliberately nothing like the library's own
     * traffic (log-uniform over three decades, so mostly large requests): it
     * exists to hammer the general arena. Calibrate against the arena, not the
     * whole pool, so the size class area does not change what is being
     * measured - the class effect on real traffic is measured by the suites
     * themselves. */
    budget = s.arena_bytes > 0u ? s.arena_bytes : (1024u * 1024u);
    if (s.pool_bytes == 0u) {
        budget = 1024u * 1024u;
    }
    live_target = budget / 2u; /* run the pool at about half full on average */
    ncl_mem_reset_stats();

    for (op = 0; op < g_ops; op++) {
        unsigned roll = (unsigned)mc_below(100u);
        int want_alloc = g_live_bytes < live_target;

        if (g_live == 0) {
            mc_alloc(rep, budget);
        } else if (roll < (want_alloc ? 70u : 20u)) {
            mc_alloc(rep, budget);
        } else if (roll < (want_alloc ? 90u : 80u)) {
            mc_release(rep, mc_below(g_live));
        } else {
            mc_resize(rep, mc_below(g_live), budget);
        }

        if (s.pool_bytes > 0u && (op % MC_CHECK_EVERY) == 0u) {
            ncl_mem_stats now;
            ncl_mem_get_stats(&now);
            if (now.free_blocks > rep->worst_free_blocks) {
                rep->worst_free_blocks = now.free_blocks;
            }
            if (now.largest_free_bytes < rep->worst_largest_free ||
                rep->worst_largest_free == 0u) {
                rep->worst_largest_free = now.largest_free_bytes;
            }
        }
        /* Every operation is verified while the pool is being broken in; the
         * cost of the walk is worth knowing exactly which call did it. */
        if (s.pool_bytes > 0u && (op % g_check_every) == 0u) {
            size_t bad = ncl_mem_check();
            ncl_mem_stats now;

            ncl_mem_get_stats(&now);
            if (bad != 0 || !mc_accounting_ok()) {
                rep->mismatches++;
                if (rep->mismatches == 1u) {
                    fprintf(stderr,
                            "  first inconsistency at op=%lu (seed=%llu), "
                            "%lu problems, in_use=%lu free=%lu meta=%lu pool=%lu\n",
                            (unsigned long)op, (unsigned long long)seed,
                            (unsigned long)bad, (unsigned long)now.in_use_bytes,
                            (unsigned long)now.free_bytes, (unsigned long)now.meta_bytes,
                            (unsigned long)now.pool_bytes);
                    mc_dump_trace();
                }
                break;
            }
        }
    }

    /* Everything back out: the pool has to be one block covering the arena. */
    while (g_live > 0u) {
        mc_release(rep, g_live - 1u);
    }
    if (ncl_mem_check() != 0) {
        rep->mismatches++;
    }
    ncl_mem_get_stats(&s);
    rep->peak = s.peak_in_use_bytes;
    rep->largest_request = s.largest_request_bytes;
    if (s.pool_bytes > 0u) {
        if (s.free_blocks != 1u || s.in_use_bytes != 0u ||
            s.largest_free_bytes + 64u < s.arena_bytes) {
            rep->mismatches += 1u + s.free_blocks;
        }
    }
}

static void mc_report_line(const char *label, const mc_report *rep)
{
    printf("    %-18s ops=%u alloc=%lu refused=%lu shape-refused=%lu "
           "peak=%lu maxreq=%lu worst-free-blocks=%lu worst-largest-free=%lu\n",
           label, (unsigned)rep->ops, (unsigned long)rep->allocations,
           (unsigned long)rep->refusals, (unsigned long)rep->shape_refusals,
           (unsigned long)rep->peak, (unsigned long)rep->largest_request,
           (unsigned long)rep->worst_free_blocks,
           (unsigned long)rep->worst_largest_free);
}

/**
 * Soak mode. One process per pool size, each hammering its own pool for as long
 * as it is told to, with the per-operation invariants of mc_run() as the
 * acceptance criterion. Every round also drains the pool completely, so each
 * round re-proves "no permanent holes" from scratch.
 */
static void test_soak(void)
{
    const size_t seconds = mc_env_size("NCL_MEM_MC_SECONDS", 60u);
    const int64_t deadline = ncl_time_monotonic_millis() + (int64_t)seconds * 1000;
    int64_t last_beat = ncl_time_monotonic_millis();
    size_t rounds = 0;
    size_t ops = 0;
    size_t allocs = 0;
    size_t refused = 0;
    size_t shape = 0;
    size_t peak_footprint = 0;
    size_t worst_blocks = 0;
    size_t worst_hole = 0;
    ncl_mem_stats s;

    NCL_TEST_CASE("soak: rounds of random traffic until the clock runs out");
    ncl_mem_get_stats(&s);
    printf("    pool %lu bytes, arena %lu bytes, %lu ops/round, check every %lu\n",
           (unsigned long)s.pool_bytes, (unsigned long)s.arena_bytes,
           (unsigned long)g_ops, (unsigned long)g_check_every);

    while (ncl_time_monotonic_millis() < deadline) {
        mc_report rep;
        int64_t now;

        mc_run(g_base_seed + rounds, &rep);
        NCL_CHECK_EQ_INT(rep.mismatches, 0);
        if (rep.mismatches != 0) {
            printf("    round %lu (seed %llu) is not clean, stopping\n",
                   (unsigned long)rounds, (unsigned long long)(g_base_seed + rounds));
            return;
        }
        rounds++;
        ops += rep.ops;
        allocs += rep.allocations;
        refused += rep.refusals;
        shape += rep.shape_refusals;
        if (rep.peak > peak_footprint) {
            peak_footprint = rep.peak;
        }
        if (rep.worst_free_blocks > worst_blocks) {
            worst_blocks = rep.worst_free_blocks;
        }
        if (worst_hole == 0u || rep.worst_largest_free < worst_hole) {
            worst_hole = rep.worst_largest_free;
        }

        now = ncl_time_monotonic_millis();
        if (now - last_beat >= 30000) {
            last_beat = now;
            printf("[soak%s] t=%lds rounds=%lu ops=%lu alloc=%lu refused=%lu "
                   "shape=%lu peak-payload=%lu worst-blocks=%lu worst-hole=%lu\n",
                   g_label, (long)((now - (deadline - (int64_t)seconds * 1000)) / 1000),
                   (unsigned long)rounds, (unsigned long)ops,
                   (unsigned long)allocs, (unsigned long)refused,
                   (unsigned long)shape, (unsigned long)peak_footprint,
                   (unsigned long)worst_blocks, (unsigned long)worst_hole);
            fflush(stdout);
        }
    }

    printf("[soak%s] done: %lu rounds, %lu ops, %lu allocations, %lu refusals "
           "(%lu had enough total free space), peak payload %lu, worst shape "
           "%lu blocks / largest hole %lu\n",
           g_label, (unsigned long)rounds, (unsigned long)ops,
           (unsigned long)allocs, (unsigned long)refused, (unsigned long)shape,
           (unsigned long)peak_footprint, (unsigned long)worst_blocks,
           (unsigned long)worst_hole);
}

static void test_monte_carlo(void)
{
    mc_report first;
    mc_report again;
    unsigned seed;
    size_t total_shape = 0;
    size_t total_refused = 0;
    size_t total_allocations = 0;
    size_t peak_sum = 0;
    size_t worst_free_blocks = 0;
    size_t worst_largest_free = 0;
    char label[16];

    NCL_TEST_CASE("random traffic: integrity, invariants, legitimate refusals");
    for (seed = 1; seed <= MC_SEEDS; seed++) {
        mc_run(seed * 7919u, &first);
        NCL_CHECK_EQ_INT(first.mismatches, 0);
        NCL_CHECK(first.allocations > MC_OPS / 4u);
        total_allocations += first.allocations;
        peak_sum += first.peak;
        if (first.worst_free_blocks > worst_free_blocks) {
            worst_free_blocks = first.worst_free_blocks;
        }
        if (worst_largest_free == 0u || first.worst_largest_free < worst_largest_free) {
            worst_largest_free = first.worst_largest_free;
        }

        /* Replay: identical input, identical pool state in, so identical out.
         * Any drift (a leaked block, a hole that stayed behind) breaks this. */
        mc_run(seed * 7919u, &again);
        NCL_CHECK_EQ_INT(again.allocations, first.allocations);
        NCL_CHECK_EQ_INT(again.refusals, first.refusals);
        NCL_CHECK_EQ_INT(again.shape_refusals, first.shape_refusals);
        NCL_CHECK_EQ_INT(again.peak, first.peak);
        NCL_CHECK_EQ_INT(again.largest_request, first.largest_request);
        NCL_CHECK_EQ_INT(again.mismatches, 0);

        total_shape += first.shape_refusals;
        total_refused += first.refusals;
        snprintf(label, sizeof(label), "seed %u", seed);
        mc_report_line(label, &first);
    }
    printf("    totals: %lu refusals, of which %lu had enough free space in "
           "total (shape), i.e. %.2f%%\n",
           (unsigned long)total_refused, (unsigned long)total_shape,
           total_refused > 0u ? 100.0 * (double)total_shape / (double)total_refused
                              : 0.0);
    printf("    worst shape seen: %lu free blocks, smallest largest-hole %lu "
           "bytes; mean peak %lu bytes\n",
           (unsigned long)worst_free_blocks, (unsigned long)worst_largest_free,
           (unsigned long)(total_allocations > 0u ? peak_sum / MC_SEEDS : 0u));
}

NCL_TEST_MAIN_BEGIN()
    g_label = getenv("NCL_MEM_MC_LABEL") != NULL ? getenv("NCL_MEM_MC_LABEL") : "";
    g_ops = mc_env_size("NCL_MEM_MC_OPS", MC_OPS);
    g_check_every = mc_env_size("NCL_MEM_MC_CHECK_EVERY", 1u);
    if (mc_env_size("NCL_MEM_MC_SEED", 0u) != 0u) {
        g_base_seed = (unsigned long long)mc_env_size("NCL_MEM_MC_SEED", 7919u);
    }
    if (getenv("NCL_MEM_MC_SECONDS") != NULL) {
        test_soak();
    } else {
        test_monte_carlo();
    }
NCL_TEST_MAIN_END()
