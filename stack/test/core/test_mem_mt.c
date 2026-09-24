/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * Multithreaded pressure test for the allocation seam.
 *
 * The single threaded Monte Carlo (test_mem_mc.c) proves that the pool keeps
 * its invariants under random traffic; this one proves that it keeps them when
 * several threads hammer it at the same time, which is how the library actually
 * runs (an MQTT reader thread, a server thread pool, sampling threads, the HTTP
 * and FTP accept threads all allocate from the same pool).
 *
 * Three things happen at once:
 *
 *   - every worker churns its own allocations (random sizes, random release and
 *     resize), with a pattern per block that is verified before the block is
 *     released: a block handed to two owners at once shows up as a mismatch;
 *   - every worker also passes ownership across threads through a shared ring,
 *     so a block allocated by one thread is released by another - the pattern is
 *     verified on the receiving side. This is the "allocated on the reader
 *     thread, released by the caller" shape the library uses for responses;
 *   - the main thread runs ncl_mem_check() the whole time. The check is taken
 *     under the pool lock, so every observation is a consistent snapshot, and it
 *     now also verifies the accounting identity from the blocks themselves; a
 *     nonzero result is a real defect, not a transient.
 *
 * After the workers stop, everything is drained and the pool has to be one free
 * block covering the arena again, with the refusal path behaving: a request the
 * size of the largest hole succeeds, the same request one alignment unit bigger
 * fails. NCL_MEM_MT_SECONDS=<n> turns it into a soak like the single threaded
 * one; NCL_MEM_MT_THREADS and NCL_MEM_MT_OPS size the round.
 */
#include "ncl_test.h"

#include "nclink/ncl_common.h"
#include "nclink/ncl_platform.h"

#include <stdint.h>

#define MT_SLOTS   512u   /* live blocks a worker keeps of its own        */
#define MT_RING    256u   /* hand-off slots between all workers           */
#define MT_EDGE    32u    /* bytes checked at both ends of a block        */
#define MT_SAMPLE  97u    /* stride of the content sampling               */
#define MT_MAX_THREADS 32u

typedef struct {
    unsigned char     *ptr;
    size_t             size;
    unsigned long long seed;
} mt_block;

typedef struct {
    size_t ops;
    size_t allocations;
    size_t refusals;
    size_t mismatches;
    size_t handed_off;
    size_t hand_offs_recv;
    size_t peak_live;
} mt_report;

typedef struct {
    size_t             index;
    size_t             ops;
    size_t             budget;      /* size ceiling for one request */
    size_t             live_target; /* bytes this worker aims to hold */
    size_t             live_count;
    size_t             live_bytes;
    unsigned long long rng;
    mt_block           live[MT_SLOTS];
    mt_report          report;
} mt_worker;

static mt_block        g_ring[MT_RING];
static size_t          g_ring_count;
static size_t          g_ring_head;
static size_t          g_ring_tail;
static ncl_mutex      *g_ring_mutex;
static volatile bool   g_stop;
static size_t          g_check_failures;  /* main thread only */
/* Worker tables live in .bss on purpose: in a static pool build allocating
 * them from the pool would need more room than the small pools have. */
static mt_worker       g_workers[MT_MAX_THREADS];
static ncl_thread     *g_handles[MT_MAX_THREADS];
static bool            g_done[MT_MAX_THREADS]; /* guarded by g_ring_mutex */

/* ---------------------------------------------------------------- random -- */

static unsigned long long mt_rand(unsigned long long *state)
{
    unsigned long long x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 2685821657736338717ULL;
}

static size_t mt_below(unsigned long long *state, size_t limit)
{
    return (size_t)(mt_rand(state) % (unsigned long long)limit);
}

static unsigned char mt_pattern(unsigned long long seed, size_t i)
{
    unsigned long long x = seed + 0x9E3779B97F4A7C15ULL * (i + 1u);
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 29;
    return (unsigned char)(x >> 32);
}

static void mt_fill(mt_block *b)
{
    size_t i;
    for (i = 0; i < b->size; i++) {
        b->ptr[i] = mt_pattern(b->seed, i);
    }
}

static size_t mt_verify(const mt_block *b)
{
    size_t bad = 0;
    size_t i;

    if (b->size == 0u) {
        return 0;
    }
    for (i = 0; i < b->size && i < MT_EDGE; i++) {
        if (b->ptr[i] != mt_pattern(b->seed, i)) {
            bad++;
        }
    }
    for (i = (b->size > MT_EDGE ? b->size - MT_EDGE : 0u); i < b->size; i++) {
        if (b->ptr[i] != mt_pattern(b->seed, i)) {
            bad++;
        }
    }
    for (i = 0; i < b->size; i += MT_SAMPLE) {
        if (b->ptr[i] != mt_pattern(b->seed, i)) {
            bad++;
        }
    }
    return bad;
}

/** Log-uniform-ish sizes: mostly small, occasionally a big one. */
static size_t mt_pick_size(mt_worker *w)
{
    size_t small_max = w->budget / 64u;
    size_t size;
    unsigned bucket;

    if (small_max < 16u) {
        small_max = 16u;
    }
    if (mt_below(&w->rng, 100u) < 5u) {
        size = small_max + mt_below(&w->rng, w->budget / 8u + 1u);
    } else {
        bucket = (unsigned)mt_below(&w->rng, 24u);
        size = (size_t)8u << bucket;
        if (size > small_max) {
            size = small_max;
        }
        size += mt_below(&w->rng, size / 2u + 1u);
    }
    return size;
}

/* ------------------------------------------------------------------ ring -- */

static bool ring_try_push(const mt_block *block)
{
    bool pushed = false;

    ncl_mutex_lock(g_ring_mutex);
    if (g_ring_count < MT_RING) {
        g_ring[g_ring_tail] = *block;
        g_ring_tail = (g_ring_tail + 1u) % MT_RING;
        g_ring_count++;
        pushed = true;
    }
    ncl_mutex_unlock(g_ring_mutex);
    return pushed;
}

static bool ring_try_pop(mt_block *block)
{
    bool popped = false;

    ncl_mutex_lock(g_ring_mutex);
    if (g_ring_count > 0u) {
        *block = g_ring[g_ring_head];
        g_ring_head = (g_ring_head + 1u) % MT_RING;
        g_ring_count--;
        popped = true;
    }
    ncl_mutex_unlock(g_ring_mutex);
    return popped;
}

/* ---------------------------------------------------------------- worker -- */

static void mt_free_own(mt_worker *w, size_t index)
{
    mt_block *b = &w->live[index];

    w->report.mismatches += mt_verify(b);
    w->live_bytes -= b->size;
    ncl_mem_free(b->ptr);
    w->live_count--;
    w->live[index] = w->live[w->live_count];
}

static void mt_op_alloc(mt_worker *w)
{
    size_t size = mt_pick_size(w);
    void *p = ncl_mem_alloc(size);

    if (p == NULL) {
        w->report.refusals++;
        return;
    }
    if (w->live_count < MT_SLOTS) {
        mt_block *b = &w->live[w->live_count++];
        b->ptr = (unsigned char *)p;
        b->size = size;
        b->seed = mt_rand(&w->rng);
        mt_fill(b);
        w->live_bytes += size;
        w->report.allocations++;
        if (w->live_bytes > w->report.peak_live) {
            w->report.peak_live = w->live_bytes;
        }
        return;
    }
    ncl_mem_free(p); /* table full: nothing to keep it for */
}

static void mt_op_free(mt_worker *w)
{
    if (w->live_count == 0u) {
        return;
    }
    mt_free_own(w, mt_below(&w->rng, w->live_count));
}

static void mt_op_resize(mt_worker *w)
{
    size_t index;
    size_t new_size;
    size_t keep;
    unsigned long long seed;
    unsigned char *grown;
    size_t i;
    size_t bad = 0;
    mt_block *b;

    if (w->live_count == 0u) {
        return;
    }
    index = mt_below(&w->rng, w->live_count);
    b = &w->live[index];
    new_size = mt_pick_size(w);
    keep = new_size < b->size ? new_size : b->size;
    seed = b->seed;

    w->report.mismatches += mt_verify(b);
    grown = (unsigned char *)ncl_mem_realloc(b->ptr, new_size);
    if (grown == NULL) {
        w->report.refusals++;
        return;
    }
    for (i = 0; i < keep; i += MT_SAMPLE) {  /* the surviving bytes must match */
        if (grown[i] != mt_pattern(seed, i)) {
            bad++;
        }
    }
    w->report.mismatches += bad;
    w->live_bytes += new_size - b->size;
    b->ptr = grown;
    b->size = new_size;
    b->seed = seed;
    for (i = keep; i < new_size; i++) {
        grown[i] = mt_pattern(seed, i);
    }
    if (w->live_bytes > w->report.peak_live) {
        w->report.peak_live = w->live_bytes;
    }
}

/** Hand a fresh block to another thread (or take one over and release it). */
static void mt_op_handoff(mt_worker *w)
{
    mt_block block;
    size_t size;
    void *p;

    if (mt_below(&w->rng, 100u) < 50u) {
        size = mt_pick_size(w);
        p = ncl_mem_alloc(size);
        if (p == NULL) {
            w->report.refusals++;
            return;
        }
        block.ptr = (unsigned char *)p;
        block.size = size;
        block.seed = mt_rand(&w->rng);
        mt_fill(&block);
        if (ring_try_push(&block)) {
            w->report.handed_off++;
        } else {
            ncl_mem_free(block.ptr); /* ring full: keep it local */
        }
        return;
    }
    if (ring_try_pop(&block)) {
        w->report.mismatches += mt_verify(&block);
        w->report.hand_offs_recv++;
        ncl_mem_free(block.ptr);
    }
}

static void mt_worker_main(void *arg)
{
    mt_worker *w = (mt_worker *)arg;
    size_t op;

    w->report.ops = w->ops;
    for (op = 0; op < w->ops; op++) {
        if ((op & 0xFFu) == 0u) {
            /* The stop flag is read under the same lock that publishes it, so
             * this test has no races of its own for a sanitizer to find. */
            bool stop;
            ncl_mutex_lock(g_ring_mutex);
            stop = g_stop;
            ncl_mutex_unlock(g_ring_mutex);
            if (stop) {
                break;
            }
        }
        unsigned roll = (unsigned)mt_below(&w->rng, 100u);
        int want_alloc = w->live_bytes < w->live_target;

        if (roll < 30u) {
            mt_op_handoff(w);
        } else if (w->live_count == 0u || roll < (want_alloc ? 70u : 25u)) {
            mt_op_alloc(w);
        } else if (roll < (want_alloc ? 88u : 70u)) {
            mt_op_free(w);
        } else {
            mt_op_resize(w);
        }
    }
    while (w->live_count > 0u) {
        mt_free_own(w, w->live_count - 1u);
    }
    ncl_mutex_lock(g_ring_mutex);
    g_done[w->index] = true;
    ncl_mutex_unlock(g_ring_mutex);
}

/* ----------------------------------------------------------------- round -- */

static void mt_drain_ring(size_t *mismatches)
{
    mt_block block;

    while (ring_try_pop(&block)) {
        *mismatches += mt_verify(&block);
        ncl_mem_free(block.ptr);
    }
}

static int mt_round(size_t threads, size_t ops, mt_report *total)
{
    ncl_mem_stats s;
    size_t budget;
    size_t i;
    size_t mismatches = 0;
    int ok = 1;

    ncl_mem_get_stats(&s);
    budget = s.arena_bytes > 0u ? s.arena_bytes : (1024u * 1024u);
    if (s.pool_bytes == 0u) {
        budget = 1024u * 1024u;
    }

    memset(g_workers, 0, sizeof(g_workers));
    memset(g_handles, 0, sizeof(g_handles));
    memset(g_done, 0, sizeof(g_done));
    memset(g_ring, 0, sizeof(g_ring));
    g_ring_count = 0;
    g_ring_head = 0;
    g_ring_tail = 0;
    g_stop = false;
    g_ring_mutex = ncl_mutex_create();
    if (g_ring_mutex == NULL) {
        return 0;
    }

    for (i = 0; i < threads; i++) {
        g_workers[i].index = i;
        g_workers[i].ops = ops;
        g_workers[i].budget = budget;
        /* Aim for about half the arena being live across all workers. */
        g_workers[i].live_target = budget / (2u * threads);
        g_workers[i].rng = (i + 1u) * 0x9E3779B97F4A7C15ULL;
        g_handles[i] = ncl_thread_start(mt_worker_main, &g_workers[i]);
        if (g_handles[i] == NULL) {
            g_done[i] = true;
            ok = 0;
        }
    }

    /*
     * While the workers run, the main thread keeps asking the pool whether it
     * is still consistent. Each answer is taken under the pool lock, so it is
     * a snapshot of a state that is supposed to be consistent at every instant:
     * a nonzero result is a defect, not a race in the test.
     */
    for (;;) {
        size_t done = 0u;
        size_t bad;

        ncl_mutex_lock(g_ring_mutex);
        for (i = 0; i < threads; i++) {
            if (g_done[i]) {
                done++;
            }
        }
        ncl_mutex_unlock(g_ring_mutex);
        if (done == threads) {
            break;
        }
        bad = ncl_mem_check();
        if (bad != 0u) {
            g_check_failures += bad;
            fprintf(stderr, "  concurrent ncl_mem_check found %lu problems\n",
                    (unsigned long)bad);
            ok = 0;
            ncl_mutex_lock(g_ring_mutex);
            g_stop = true;
            ncl_mutex_unlock(g_ring_mutex);
        }
        ncl_sleep_millis(20);
    }

    for (i = 0; i < threads; i++) {
        if (g_handles[i] != NULL) {
            ncl_thread_join(g_handles[i]);
        }
    }
    mt_drain_ring(&mismatches);

    for (i = 0; i < threads; i++) {
        mismatches += g_workers[i].report.mismatches;
        total->ops += g_workers[i].report.ops;
        total->allocations += g_workers[i].report.allocations;
        total->refusals += g_workers[i].report.refusals;
        total->handed_off += g_workers[i].report.handed_off;
        total->hand_offs_recv += g_workers[i].report.hand_offs_recv;
        if (g_workers[i].report.peak_live > total->peak_live) {
            total->peak_live = g_workers[i].report.peak_live;
        }
    }

    /* The ring mutex itself lives in the pool (ncl_mutex_create allocates), so
     * it has to go back before the pool is expected to be empty. */
    ncl_mutex_destroy(g_ring_mutex);
    g_ring_mutex = NULL;

    /* Quiescent checks: the pool is back to one block, and the refusal path
     * answers correctly for a request the size of the largest hole. */
    if (ncl_mem_check() != 0u) {
        ok = 0;
    }
    ncl_mem_get_stats(&s);
    if (s.pool_bytes > 0u) {
        if (s.in_use_bytes != 0u || s.live_blocks != 0u) {
            printf("    leaked %lu bytes in %lu blocks after the round\n",
                   (unsigned long)s.in_use_bytes, (unsigned long)s.live_blocks);
            ok = 0;
        }
        if (s.free_blocks != 1u || s.largest_free_bytes + 64u < s.arena_bytes) {
            printf("    arena did not coalesce: %lu free blocks, largest %lu of %lu\n",
                   (unsigned long)s.free_blocks, (unsigned long)s.largest_free_bytes,
                   (unsigned long)s.arena_bytes);
            ok = 0;
        }
        if (s.largest_free_bytes > 32u) {
            void *exact = ncl_mem_alloc(s.largest_free_bytes);
            void *over = NULL;

            if (exact == NULL) {
                printf("    a request the size of the largest hole was refused\n");
                ok = 0;
            } else {
                ncl_mem_free(exact);
            }
            over = ncl_mem_alloc(s.largest_free_bytes + 64u);
            if (over != NULL) {
                printf("    a request larger than the pool was granted\n");
                ncl_mem_free(over);
                ok = 0;
            }
            if (ncl_mem_check() != 0u) {
                ok = 0;
            }
        }
    }
    total->mismatches += mismatches;
    if (mismatches != 0u) {
        printf("    %lu content mismatches\n", (unsigned long)mismatches);
        ok = 0;
    }

    return ok;
}

static size_t mt_env_size(const char *name, size_t fallback)
{
    const char *value = getenv(name);
    if (value == NULL || *value == '\0') {
        return fallback;
    }
    return (size_t)strtoul(value, NULL, 10);
}

static void mt_print_total(const char *when, const mt_report *total)
{
    printf("[mt%s] %s: %lu ops, %lu allocations, %lu refusals, %lu hand-offs "
           "(%lu received), peak live per worker %lu\n",
           getenv("NCL_MEM_MT_LABEL") != NULL ? getenv("NCL_MEM_MT_LABEL") : "",
           when, (unsigned long)total->ops, (unsigned long)total->allocations,
           (unsigned long)total->refusals, (unsigned long)total->handed_off,
           (unsigned long)total->hand_offs_recv, (unsigned long)total->peak_live);
    fflush(stdout);
}

static void test_multithreaded(void)
{
    size_t threads = mt_env_size("NCL_MEM_MT_THREADS", 4u);
    size_t ops = mt_env_size("NCL_MEM_MT_OPS", 40000u);
    size_t seconds = mt_env_size("NCL_MEM_MT_SECONDS", 0u);
    mt_report total;
    int64_t deadline;
    size_t rounds = 0;

#if defined(NCL_MEM_SINGLE_THREAD)
    NCL_TEST_CASE("multithreaded traffic (skipped: single thread build)");
    printf("    this build was configured with NCL_MEM_SINGLE_THREAD\n");
    return;
#else
    if (threads < 2u) {
        threads = 2u;
    }
    if (threads > 32u) {
        threads = 32u;
    }

    NCL_TEST_CASE("concurrent random traffic on one pool");
    printf("    %lu threads, %lu ops each, %s\n", (unsigned long)threads,
           (unsigned long)ops,
           seconds > 0u ? "soak until the clock runs out" : "one round");
    memset(&total, 0, sizeof(total));
    deadline = ncl_time_monotonic_millis() + (int64_t)seconds * 1000;

    do {
        int ok = mt_round(threads, ops, &total);
        NCL_CHECK_EQ_INT(ok, 1);
        NCL_CHECK_EQ_INT(g_check_failures, 0);
        rounds++;
        if (!ok) {
            return;
        }
    } while (seconds > 0u && ncl_time_monotonic_millis() < deadline);

    mt_print_total("done", &total);
    printf("    %lu round(s), %lu invariant violations\n",
           (unsigned long)rounds, (unsigned long)g_check_failures);
#endif
}

NCL_TEST_MAIN_BEGIN()
    test_multithreaded();
NCL_TEST_MAIN_END()
