/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - the allocation seam.
 *
 * This file is the only place in the library that knows where bytes come from.
 * Everything else calls ncl_mem_alloc() / ncl_mem_calloc() / ncl_mem_realloc()
 * / ncl_mem_free(), which resolve to one of two implementations:
 *
 *   default              the C runtime allocator, zero overhead, unchanged
 *                        behaviour on Windows / Linux / macOS
 *   NCL_STATIC_MEM=1     one fixed pool inside a static array: the library
 *                        never calls malloc(), and every object it hands out
 *                        (models, messages, JSON documents, MQTT packet
 *                        buffers, the server's tables) lives in that array
 *
 * Two regions
 * -----------
 * The array is split once, on first use, into a small-object part and a general
 * arena:
 *
 *     +----------+----------+--- ... ---+---------------------------------+
 *     | class 16 | class 32 |   ...     | general arena (headers + blocks)|
 *     +----------+----------+--- ... ---+---------------------------------+
 *      <------------ NCL_MEM_CLASS_BYTES ----------->
 *
 * Size classes (16/32/64/128/256 bytes) exist because the library's traffic is
 * almost entirely small: measured over the whole test suite, more than 98% of
 * requests are 128 bytes or less, and about half fall in the 33..64 byte band
 * (a struct, a key, a short string). A class block is fixed size, so
 *   - it needs no header at all: releasing a pointer is a matter of finding out
 *     which region it lies in, which is a range check, not a lookup;
 *   - releasing and reusing are O(1) (a push/pop on that class's free list);
 *   - external fragmentation inside a class is impossible: every block in it is
 *     the same size, so a free block always fits the next request of that size.
 * The price is internal fragmentation - a 40 byte request occupies a 64 byte
 * block - which is still cheaper than the 96 bytes (40 rounded to 48 plus a
 * 32 byte header) the arena would spend on it.
 *
 * Region sizes are weighted by the measured request mix (see g_class_share) and
 * can be overridden. When a class runs out, the request falls back to the arena,
 * so an under-provisioned class costs speed, never correctness: there is no new
 * way for an allocation to fail.
 *
 * The general arena
 * -----------------
 * Everything else - and every small request whose class is exhausted - comes
 * from a first/best fit arena with an in band header:
 *
 *     +--------+----------------+--------+----------------+
 *     | header | payload        | header | payload        |
 *     +--------+----------------+--------+----------------+
 *       total   NCL_MEM_HDR      ...
 *
 * Blocks start on a 16 byte boundary, so the payload has the alignment malloc()
 * guarantees. Free blocks carry the size of their predecessor, which is what
 * makes releasing a block merge it with the neighbours on both sides.
 *
 * Fragmentation
 * -------------
 * Two properties are required of an allocator that must run for weeks on a
 * device, and both are tested in tests/test_mem.c:
 *
 *   1. no permanent holes - when a workload returns to its idle state, the
 *      space the workload used is one free block again, cycle after cycle.
 *      That comes from merging both neighbours on release, not from any
 *      compaction (there is none: pointers handed out stay valid).
 *   2. refusals are about size, not shape - ncl_mem_get_stats() reports
 *      free_bytes next to largest_free_bytes. When a request is refused, the
 *      pool records both numbers (failure_free_bytes,
 *      failure_largest_free_bytes) so a field engineer can tell "the pool is
 *      genuinely too small" from "the free space is chopped up".
 *
 * What is left is inherent to any non compacting allocator: a *single* request
 * larger than the biggest contiguous free block cannot be served even when the
 * total free space would be enough. largest_request_bytes keeps the worst
 * request ever seen, which is the floor the pool has to be sized above.
 *
 * The arena search is best fit (the smallest free block that still fits) rather
 * than first fit: it costs one comparison more per free block, and it keeps the
 * big holes big, which is what the occasional large request (a 16 KiB file
 * chunk) depends on. An exact size match ends the search early, so a workload
 * that repeatedly allocates the same size pays almost nothing for the policy.
 * NCL_MEM_FIRST_FIT=1 switches to first fit, which exists so the two policies
 * can be compared on identical random traffic (tests/test_mem_mc.c).
 *
 * Exhaustion is reported, never worked around: the call fails (NULL), the
 * counter ncl_mem_stats.failures goes up, and the library turns it into
 * NCL_ERR_NOMEM like any other allocation failure. Nothing here falls back to
 * the heap.
 *
 * Configuration
 * -------------
 *   NCL_STATIC_MEM=1            serve every allocation from the static pool
 *   NCL_MEM_POOL_BYTES=<n>      pool size, default 20 MiB (20 * 1024 * 1024)
 *   NCL_MEM_CLASS_BYTES=<n>     size class area, default pool / 4, 0 disables
 *   NCL_MEM_SINGLE_THREAD=1     drop the lock: for bare metal builds where the
 *                               library is driven from one context only
 *   NCL_MEM_STRICT=1            abort on a free() of a pointer the pool never
 *                               handed out, or of a block that is already free
 *   NCL_MEM_FIRST_FIT=1         arena uses first fit instead of best fit
 *   NCL_MEM_REPORT=1            print the pool high water mark at exit, which
 *                               is how the pool gets sized for a device: run
 *                               the real traffic once, read the peak
 */
#include "nclink/ncl_common.h"
#include "nclink/ncl_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(NCL_STATIC_MEM)

/* ------------------------------------------------------- runtime allocator -- */

void *ncl_mem_alloc(size_t size)
{
    return malloc(size);
}

void *ncl_mem_calloc(size_t count, size_t size)
{
    return calloc(count, size);
}

void *ncl_mem_realloc(void *ptr, size_t size)
{
    return realloc(ptr, size);
}

void ncl_mem_free(void *ptr)
{
    free(ptr);
}

const char *ncl_mem_mode(void)
{
    return "heap";
}

void ncl_mem_get_stats(ncl_mem_stats *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}

void ncl_mem_reset_stats(void)
{
}

size_t ncl_mem_check(void)
{
    return 0;
}

#else /* NCL_STATIC_MEM */

/* ----------------------------------------------------------- static pool -- */

#ifndef NCL_MEM_POOL_BYTES
#define NCL_MEM_POOL_BYTES (20u * 1024u * 1024u)
#endif

/** Size of the small-object area; 0 turns the size classes off entirely. */
#ifndef NCL_MEM_CLASS_BYTES
#define NCL_MEM_CLASS_BYTES (NCL_MEM_POOL_BYTES / 4u)
#endif

#if NCL_MEM_POOL_BYTES < 4096
#error "NCL_MEM_POOL_BYTES is too small to hold the library's bookkeeping"
#endif

/** Alignment of every block, and therefore of every payload pointer. */
#define NCL_MEM_ALIGN ((size_t)NCL_MEM_ALIGNMENT)

/*
 * Size classes, ascending, all multiples of the alignment. The steps are finer
 * than a power of two on purpose: a class of size C only pays off for requests
 * above C - 32, because the arena would charge the aligned request plus a 32
 * byte header. With C = 128 the requests from 65 to 96 bytes would actually be
 * cheaper in the arena (96 + 32 = 128, i.e. a tie) and from 97 on the class
 * wins; a 48 and a 96 step keep those loss windows small.
 */
#define NCL_MEM_CLASS_COUNT 8u
static const size_t g_class_block[NCL_MEM_CLASS_COUNT] = {
    16u, 32u, 48u, 64u, 96u, 128u, 192u, 256u
};

/*
 * Share of the class area per class, in percent. Derived from the request mix
 * measured over the test suite (request bytes, not request count: the 33..64
 * bucket carries about half of them and nearly two thirds of the bytes). An
 * under-provisioned class falls back to the arena, so these only decide how
 * much of the small-object traffic gets the cheap path; ncl_mem_stats reports
 * per class usage (-MemReport prints it) for retuning on real traffic.
 */
static const unsigned g_class_share[NCL_MEM_CLASS_COUNT] = {
    13u, 2u, 27u, 36u, 8u, 11u, 1u, 2u
};

/**
 * Block header, padded to a whole number of alignment units so that the
 * payload after it starts on an aligned address as well. The byte array is the
 * padding: a union is as large as its largest member, and a block header is
 * never bigger than 32 bytes (three fields: two size_t and a flag).
 */
typedef union ncl_mem_block {
    struct {
        size_t   total;      /**< whole block, header included            */
        size_t   prev_total; /**< total of the preceding block, 0 = first */
        unsigned free_flag;  /**< 1 when the block is on the free list    */
    } h;
    unsigned char header_size[NCL_MEM_ALIGNMENT * 2u];
} ncl_mem_block;

typedef char ncl_mem_block_header_is_aligned
        [((sizeof(ncl_mem_block) % NCL_MEM_ALIGNMENT) == 0u) ? 1 : -1];

#define NCL_MEM_HDR (sizeof(ncl_mem_block))

/** One fixed-size region per size class, plus its free list of free blocks. */
typedef struct {
    unsigned char *begin;       /**< first block of the region               */
    unsigned char *end;         /**< one past the last block                 */
    size_t         block;       /**< payload bytes per block                 */
    size_t         live;        /**< blocks handed out                       */
    size_t         free_blocks; /**< blocks on the free list                 */
    void          *head;        /**< free list; a free block holds the next  */
} ncl_mem_class;

/*
 * The arena, plus one alignment unit of slack: the usable area starts at the
 * next aligned address inside the array, which is computed at run time so the
 * pool needs no compiler specific alignment attribute.
 */
static unsigned char g_pool[NCL_MEM_POOL_BYTES + NCL_MEM_ALIGN];

static ncl_mem_class  g_class[NCL_MEM_CLASS_COUNT];
static unsigned char *g_arena_begin; /* set once, by pool_init_locked() */

static bool   g_ready;
static size_t g_in_use;   /* payload bytes currently reserved (both regions) */
static size_t g_peak;
static size_t g_footprint;      /* payload + headers, i.e. what live objects cost */
static size_t g_peak_footprint;
static size_t g_live_blocks;
static size_t g_allocations;
static size_t g_failures;
static size_t g_foreign_frees;
static size_t g_bad_links;            /* headers whose back link did not add up */
static size_t g_largest_request;      /* biggest request ever served/asked      */
static size_t g_failure_free;         /* free bytes when the last refusal happened */
static size_t g_failure_largest_free; /* largest free block at that moment      */
static size_t g_size_hist[16];        /* request sizes, log2 buckets            */

#if defined(NCL_MEM_SINGLE_THREAD)

static void pool_lock(void) {}
static void pool_unlock(void) {}

#elif defined(NCL_OS_WINDOWS)

static CRITICAL_SECTION g_cs;
static INIT_ONCE        g_cs_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK pool_cs_init(PINIT_ONCE once, PVOID param, PVOID *context)
{
    (void)once;
    (void)param;
    (void)context;
    InitializeCriticalSection(&g_cs);
    return TRUE;
}

static void pool_lock(void)
{
    InitOnceExecuteOnce(&g_cs_once, pool_cs_init, NULL, NULL);
    EnterCriticalSection(&g_cs);
}

static void pool_unlock(void)
{
    LeaveCriticalSection(&g_cs);
}

#else

#include <pthread.h>

/* Statically initialised: the pool must be usable before anything ran, and it
 * must never allocate to protect itself. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void pool_lock(void)
{
    pthread_mutex_lock(&g_lock);
}

static void pool_unlock(void)
{
    pthread_mutex_unlock(&g_lock);
}

#endif

/** Bytes of the array that are usable as blocks (a multiple of the alignment). */
static size_t pool_total(void)
{
    return (size_t)(NCL_MEM_POOL_BYTES / NCL_MEM_ALIGN) * NCL_MEM_ALIGN;
}

#if defined(NCL_MEM_REPORT)
/** Diagnostic: how big the pool would have to be for the traffic just run. */
static void pool_report_at_exit(void)
{
    ncl_mem_stats s;
    size_t i;

    ncl_mem_get_stats(&s);
    fprintf(stderr,
            "ncl_mem: static-pool peak %lu of %lu bytes, live %lu blocks, "
            "%lu allocations, %lu failures, %lu foreign frees; "
            "free %lu bytes in %lu blocks (largest %lu), largest request %lu, "
            "peak footprint %lu\n",
            (unsigned long)s.peak_in_use_bytes, (unsigned long)s.pool_bytes,
            (unsigned long)s.live_blocks, (unsigned long)s.allocations,
            (unsigned long)s.failures, (unsigned long)s.foreign_frees,
            (unsigned long)s.free_bytes, (unsigned long)s.free_blocks,
            (unsigned long)s.largest_free_bytes,
            (unsigned long)s.largest_request_bytes,
            (unsigned long)s.peak_footprint_bytes);
    fprintf(stderr, "ncl_mem: size classes (block bytes, live/free, region bytes):");
    for (i = 0; i < sizeof(s.class_bytes) / sizeof(s.class_bytes[0]); i++) {
        if (s.class_size[i] > 0u) {
            fprintf(stderr, " %lu:%lu/%lu(%luB)", (unsigned long)s.class_size[i],
                    (unsigned long)s.class_live[i], (unsigned long)s.class_free[i],
                    (unsigned long)s.class_bytes[i]);
        }
    }
    fprintf(stderr, "\n");
    if (s.failures > 0u) {
        /* Fragmentation check: a small largest-free next to a large total-free
         * means the pool was refused for shape, not for size. */
        fprintf(stderr,
                "ncl_mem: last refusal happened with %lu free bytes in the pool, "
                "largest contiguous block %lu bytes\n",
                (unsigned long)s.failure_free_bytes,
                (unsigned long)s.failure_largest_free_bytes);
    }
    if (s.bad_links > 0u) {
        fprintf(stderr, "ncl_mem: %lu block links did not add up (pool corruption)\n",
                (unsigned long)s.bad_links);
    }
    fprintf(stderr, "ncl_mem: %lu bytes in arena block headers (32 per block)\n",
            (unsigned long)s.meta_bytes);
    fprintf(stderr, "ncl_mem: request sizes (bytes -> count):");
    for (i = 0; i < sizeof(s.size_hist) / sizeof(s.size_hist[0]); i++) {
        if (s.size_hist[i] > 0u) {
            fprintf(stderr, " <=%lu:%lu", (unsigned long)((size_t)2u << i),
                    (unsigned long)s.size_hist[i]);
        }
    }
    fprintf(stderr, "\n");
}
#endif

static unsigned char *pool_begin(void)
{
    uintptr_t base = (uintptr_t)g_pool;
    return (unsigned char *)((base + (NCL_MEM_ALIGN - 1u)) & ~(uintptr_t)(NCL_MEM_ALIGN - 1u));
}

static unsigned char *pool_end(void)
{
    return pool_begin() + pool_total();
}

static size_t pool_align_up(size_t n)
{
    size_t a = NCL_MEM_ALIGN;
    return (n + a - 1u) / a * a;
}

/** The size class a request of @p size bytes belongs to, or -1 for the arena. */
static int pool_class_for(size_t size)
{
    size_t want = pool_align_up(size == 0u ? 1u : size);
    size_t i;

    for (i = 0; i < NCL_MEM_CLASS_COUNT; i++) {
        if (g_class[i].block == 0u) {
            continue;
        }
        if (want <= g_class[i].block) {
            return (int)i;
        }
    }
    return -1;
}

/** Which class region @p ptr lies in, or -1 when it is not a class block. */
static int pool_class_of(const void *ptr)
{
    const unsigned char *p = (const unsigned char *)ptr;
    size_t i;

    if (p == NULL) {
        return -1;
    }
    for (i = 0; i < NCL_MEM_CLASS_COUNT; i++) {
        if (g_class[i].block != 0u && p >= g_class[i].begin && p < g_class[i].end &&
            ((size_t)(p - g_class[i].begin) % g_class[i].block) == 0u) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * The block starting at @p p, or NULL when @p p is not the start of a block
 * that fits inside the arena. Every walk and every link follows through here,
 * so a header that was overwritten by a buffer overrun in the caller cannot
 * send the allocator off into memory it does not own.
 */
static ncl_mem_block *pool_block_at(const unsigned char *p)
{
    unsigned char *end = pool_end();
    ncl_mem_block *b;

    if (g_arena_begin == NULL || p < g_arena_begin || p + NCL_MEM_HDR > end) {
        return NULL;
    }
    if (((size_t)p % NCL_MEM_ALIGN) != 0u) {
        return NULL;
    }
    b = (ncl_mem_block *)p;
    if (b->h.total < NCL_MEM_HDR || b->h.total > (size_t)(end - p)) {
        return NULL;
    }
    return b;
}

/** The block behind a live arena payload pointer, or NULL when @p ptr is not one. */
static ncl_mem_block *pool_block_of(const void *ptr)
{
    const unsigned char *p;
    ncl_mem_block *b;

    if (ptr == NULL || g_arena_begin == NULL) {
        return NULL;
    }
    p = (const unsigned char *)ptr;
    if (p < g_arena_begin + NCL_MEM_HDR || p >= pool_end()) {
        return NULL;
    }
    if (((size_t)p % NCL_MEM_ALIGN) != 0u) {
        return NULL;
    }
    b = pool_block_at(p - NCL_MEM_HDR);
    if (b == NULL || b->h.free_flag) {
        return NULL;
    }
    return b;
}

/** Carve the class regions, then hand what is left to the arena. */
static void pool_init_locked(void)
{
    ncl_mem_block *first;
    unsigned char *cursor;
    unsigned char *limit;
    size_t         area;
    size_t         i;

    if (g_ready) {
        return;
    }
    cursor = pool_begin();
    limit = pool_end();
    area = (size_t)NCL_MEM_CLASS_BYTES;
    /* Keep at least half of the pool for the arena: the classes are an
     * optimisation for small objects, never the whole story. */
    if (area > pool_total() / 2u) {
        area = pool_total() / 2u;
    }

    for (i = 0; i < NCL_MEM_CLASS_COUNT && area > 0u; i++) {
        size_t block = g_class_block[i];
        size_t want = area * g_class_share[i] / 100u;
        size_t blocks;
        size_t j;

        want = want / block * block; /* whole blocks only */
        blocks = want / block;
        /* The arena needs room for one block behind the classes. */
        if (blocks == 0u || (size_t)(limit - cursor) < want + NCL_MEM_HDR + NCL_MEM_ALIGN) {
            continue;
        }
        g_class[i].begin = cursor;
        g_class[i].end = cursor + want;
        g_class[i].block = block;
        g_class[i].live = 0;
        g_class[i].free_blocks = blocks;
        g_class[i].head = NULL;
        /* Thread the region onto its free list from the back, so the first
         * allocation hands out the lowest address and the region is walked
         * forwards over time. */
        for (j = blocks; j > 0u; j--) {
            void **slot = (void **)(cursor + (j - 1u) * block);
            *slot = g_class[i].head;
            g_class[i].head = slot;
        }
        cursor += want;
    }

    cursor = (unsigned char *)(((uintptr_t)cursor + (NCL_MEM_ALIGN - 1u)) &
                               ~(uintptr_t)(NCL_MEM_ALIGN - 1u));
    g_arena_begin = cursor;
    first = (ncl_mem_block *)g_arena_begin;
    first->h.total = (size_t)(limit - g_arena_begin);
    first->h.prev_total = 0;
    first->h.free_flag = 1;
    g_ready = true;
#if defined(NCL_MEM_REPORT)
    atexit(pool_report_at_exit);
#endif
}

static size_t pool_payload(const ncl_mem_block *b)
{
    return b->h.total - NCL_MEM_HDR;
}

/**
 * Cut @p b down to @p need bytes, keeping the remainder as a free block. The
 * remainder is only kept when it could serve an allocation of its own, so the
 * pool does not fill up with slivers too small to ever be used again.
 */
static void pool_split(ncl_mem_block *b, size_t need);

/**
 * Tell the block behind @p b where its predecessor now starts. Any operation
 * that changes a block's size has to call this, because the next block's back
 * link is what a later release uses to find its neighbour.
 */
static void pool_fix_follower(const ncl_mem_block *b)
{
    ncl_mem_block *after = pool_block_at((unsigned char *)b + b->h.total);

    if (after != NULL) {
        after->h.prev_total = b->h.total;
    }
}

/**
 * Absorb every free block behind @p b, in one walk. Two jobs in one place:
 *
 *   - the back link: a merge changes where the block behind the pair starts, so
 *     that block has to be told, otherwise a later release would merge into the
 *     wrong address;
 *   - the loop: a single step is not enough. Splitting a block can leave the
 *     new free tail next to a free neighbour (realloc shrink is the easy way to
 *     get there), and stopping after one merge would leave two free blocks
 *     side by side - space that is free but split, which is exactly the
 *     fragmentation this allocator is supposed to avoid.
 */
static void pool_merge_forward(ncl_mem_block *b)
{
    ncl_mem_block *next;

    for (;;) {
        next = pool_block_at((unsigned char *)b + b->h.total);
        if (next == NULL || !next->h.free_flag) {
            return;
        }
        b->h.total += next->h.total;
        pool_fix_follower(b);
    }
}

static void pool_split(ncl_mem_block *b, size_t need)
{
    size_t rest = b->h.total - need;
    ncl_mem_block *tail;

    if (rest < NCL_MEM_HDR + NCL_MEM_ALIGN) {
        return;
    }
    tail = (ncl_mem_block *)((unsigned char *)b + need);
    tail->h.total = rest;
    tail->h.prev_total = need;
    tail->h.free_flag = 1;
    pool_merge_forward(tail);
    /* When the follower is allocated, merge_forward above stops immediately and
     * this is the only place that link gets fixed. */
    pool_fix_follower(tail);
    b->h.total = need;
}

/** Take one block from size class @p index; NULL when that class ran out. */
static void *pool_class_take(int index)
{
    ncl_mem_class *c = &g_class[index];
    void *block;

    if (c->head == NULL) {
        return NULL;
    }
    block = c->head;
    c->head = *(void **)block;
    c->free_blocks--;
    c->live++;
    g_in_use += c->block;
    g_footprint += c->block;
    if (g_in_use > g_peak) {
        g_peak = g_in_use;
    }
    if (g_footprint > g_peak_footprint) {
        g_peak_footprint = g_footprint;
    }
    g_live_blocks++;
    g_allocations++;
    return block;
}

/** Give a block back to size class @p index. */
static void pool_class_give(int index, void *ptr)
{
    ncl_mem_class *c = &g_class[index];

#if defined(NCL_MEM_STRICT)
    {
        /* Headerless blocks cannot tell a double free from a first one by
         * themselves; this walks the (short) free list to say so out loud. */
        void *slot = c->head;
        size_t guard = c->live + c->free_blocks + 1u;
        while (slot != NULL && guard-- > 0u) {
            if (slot == ptr) {
                fprintf(stderr, "ncl_mem_free: %p is already free (class %lu)\n",
                        ptr, (unsigned long)c->block);
                abort();
            }
            slot = *(void **)slot;
        }
    }
#endif
    *(void **)ptr = c->head;
    c->head = ptr;
    c->free_blocks++;
    c->live--;
    g_in_use -= c->block;
    g_footprint -= c->block;
    g_live_blocks--;
}

/**
 * Walk both regions and report how the free space is shaped: the biggest
 * contiguous arena piece, the total of all free space (classes included), the
 * number of arena holes, and how many blocks the arena has.
 */
static void pool_measure_locked(size_t *largest, size_t *total, size_t *holes,
                               size_t *arena_blocks)
{
    ncl_mem_block *b = pool_block_at(g_arena_begin);
    size_t best = 0;
    size_t sum = 0;
    size_t count = 0;
    size_t all = 0;
    size_t i;

    while (b != NULL) {
        all++;
        if (b->h.free_flag) {
            size_t payload = pool_payload(b);
            if (payload > best) {
                best = payload;
            }
            sum += payload;
            count++;
        }
        b = pool_block_at((unsigned char *)b + b->h.total);
    }
    for (i = 0; i < NCL_MEM_CLASS_COUNT; i++) {
        sum += g_class[i].free_blocks * g_class[i].block;
    }
    if (largest != NULL) {
        *largest = best;
    }
    if (total != NULL) {
        *total = sum;
    }
    if (holes != NULL) {
        *holes = count;
    }
    if (arena_blocks != NULL) {
        *arena_blocks = all;
    }
}

/** Record why an allocation was refused, for diagnosing fragmentation. */
static void pool_note_failure_locked(void)
{
    size_t largest = 0;
    size_t total = 0;

    pool_measure_locked(&largest, &total, NULL, NULL);
    g_failure_largest_free = largest;
    g_failure_free = total;
}

/** Count a request in the log2 histogram that sizes the pool. */
static void pool_note_request(size_t size)
{
    unsigned i = 0;
    size_t   bound = 2u;

    while (i < 15u && size > bound) {
        i++;
        bound *= 2u;
    }
    g_size_hist[i]++;
}

void *ncl_mem_alloc(size_t size)
{
    ncl_mem_block *b;
    ncl_mem_block *best = NULL;
    size_t need;
    int    class_index;
    void  *out = NULL;

    if (size == 0u) {
        size = 1u;
    }

    pool_lock();
    pool_init_locked();
    /* Refuse anything the pool could never hold before rounding up, so a
     * request close to SIZE_MAX cannot wrap into a small one. */
    if (size > pool_total()) {
        g_failures++;
        pool_note_failure_locked();
        pool_unlock();
        return NULL;
    }
    if (size > g_largest_request) {
        g_largest_request = size;
    }
    pool_note_request(size);

    class_index = pool_class_for(size);
    if (class_index >= 0) {
        out = pool_class_take(class_index);
        if (out != NULL) {
            pool_unlock();
            return out;
        }
        /* The class is exhausted: fall through to the arena rather than
         * refusing, so a size class can only ever cost speed. */
    }

    need = NCL_MEM_HDR + pool_align_up(size);
    b = pool_block_at(g_arena_begin);
    while (b != NULL) {
        if (b->h.free_flag && b->h.total >= need) {
#if defined(NCL_MEM_FIRST_FIT)
            best = b; /* first fit: the first block that fits wins */
            break;
#else
            if (best == NULL || b->h.total < best->h.total) {
                best = b;
                if (b->h.total == need) {
                    break; /* exact fit: nothing can be better */
                }
            }
#endif
        }
        b = pool_block_at((unsigned char *)b + b->h.total);
    }
    b = best;
    if (b != NULL) {
        pool_split(b, need);
        b->h.free_flag = 0;
        g_in_use += pool_payload(b);
        g_footprint += b->h.total;
        if (g_in_use > g_peak) {
            g_peak = g_in_use;
        }
        if (g_footprint > g_peak_footprint) {
            g_peak_footprint = g_footprint;
        }
        g_live_blocks++;
        g_allocations++;
        out = (void *)((unsigned char *)b + NCL_MEM_HDR);
    } else {
        g_failures++;
        pool_note_failure_locked();
    }
    pool_unlock();
    return out;
}

void *ncl_mem_calloc(size_t count, size_t size)
{
    void *out;

    if (count != 0u && size > (size_t)-1 / count) {
        pool_lock();
        g_failures++;
        pool_note_failure_locked();
        pool_unlock();
        return NULL;
    }
    out = ncl_mem_alloc(count * size);
    if (out != NULL) {
        memset(out, 0, count * size);
    }
    return out;
}

void ncl_mem_free(void *ptr)
{
    ncl_mem_block *b;
    ncl_mem_block *prev;
    int            class_index;

    if (ptr == NULL) {
        return;
    }

    pool_lock();
    pool_init_locked();

    class_index = pool_class_of(ptr);
    if (class_index >= 0) {
        pool_class_give(class_index, ptr);
        pool_unlock();
        return;
    }

    b = pool_block_of(ptr);
    if (b == NULL) {
        /* Not ours, already released, or an interior pointer: releasing it
         * would corrupt the pool, so it is counted and dropped. */
        g_foreign_frees++;
#if defined(NCL_MEM_STRICT)
        fprintf(stderr, "ncl_mem_free: %p is not a live pool block\n", ptr);
        abort();
#endif
        pool_unlock();
        return;
    }
    g_in_use -= pool_payload(b);
    g_footprint -= b->h.total;
    g_live_blocks--;
    b->h.free_flag = 1;
    pool_merge_forward(b);
    if (b->h.prev_total != 0u) {
        prev = (ncl_mem_block *)pool_block_at((unsigned char *)b - b->h.prev_total);
        /* Never trust the back link blindly: a header that does not add up
         * would send the merge to an arbitrary address. */
        if (prev == NULL || prev->h.total != b->h.prev_total) {
            g_bad_links++;
        } else if (prev->h.free_flag) {
            pool_merge_forward(prev);
        }
    }
    pool_unlock();
}

void *ncl_mem_realloc(void *ptr, size_t size)
{
    ncl_mem_block *b;
    ncl_mem_block *next;
    void          *fresh;
    size_t         need;
    size_t         copy;
    size_t         old_bytes;
    int            class_index;

    if (ptr == NULL) {
        return ncl_mem_alloc(size);
    }
    if (size == 0u) {
        ncl_mem_free(ptr);
        return NULL;
    }

    pool_lock();
    pool_init_locked();

    class_index = pool_class_of(ptr);
    if (class_index >= 0) {
        size_t block = g_class[class_index].block;

        if (size > g_largest_request) {
            g_largest_request = size;
        }
        pool_note_request(size);
        if (pool_align_up(size) <= block) {
            pool_unlock();
            return ptr; /* still fits its block: nothing to do, not even a copy */
        }
        copy = block < size ? block : size;
        pool_unlock();

        fresh = ncl_mem_alloc(size);
        if (fresh == NULL) {
            return NULL;
        }
        memcpy(fresh, ptr, copy);
        ncl_mem_free(ptr);
        return fresh;
    }

    b = pool_block_of(ptr);
    if (b == NULL) {
        g_foreign_frees++;
#if defined(NCL_MEM_STRICT)
        fprintf(stderr, "ncl_mem_realloc: %p is not a live pool block\n", ptr);
        abort();
#endif
        pool_unlock();
        return NULL;
    }
    if (size > pool_total()) {
        g_failures++;
        pool_note_failure_locked();
        pool_unlock();
        return NULL;
    }
    if (size > g_largest_request) {
        g_largest_request = size;
    }
    pool_note_request(size);
    need = NCL_MEM_HDR + pool_align_up(size);

    if (need <= b->h.total) {
        size_t before = b->h.total;
        /* Shrink in place; the tail returns to the pool. */
        pool_split(b, need);
        g_in_use -= before - b->h.total;
        g_footprint -= before - b->h.total;
        pool_unlock();
        return ptr;
    }

    /* Grow in place when the next block is free and big enough together. */
    next = pool_block_at((unsigned char *)b + b->h.total);
    if (next != NULL && next->h.free_flag && b->h.total + next->h.total >= need) {
        size_t before = b->h.total;
        pool_merge_forward(b);
        pool_split(b, need);
        g_in_use += b->h.total - before;
        g_footprint += b->h.total - before;
        pool_unlock();
        return ptr;
    }
    old_bytes = pool_payload(b);
    copy = size < old_bytes ? size : old_bytes;
    pool_unlock();

    fresh = ncl_mem_alloc(size);
    if (fresh == NULL) {
        return NULL;
    }
    memcpy(fresh, ptr, copy);
    ncl_mem_free(ptr);
    return fresh;
}

const char *ncl_mem_mode(void)
{
    return "static-pool";
}

void ncl_mem_get_stats(ncl_mem_stats *out)
{
    size_t largest = 0;
    size_t free_total = 0;
    size_t holes = 0;
    size_t arena_blocks = 0;
    size_t class_bytes = 0;
    size_t i;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    pool_lock();
    pool_init_locked();
    pool_measure_locked(&largest, &free_total, &holes, &arena_blocks);
    out->pool_bytes = pool_total();
    out->in_use_bytes = g_in_use;
    out->peak_in_use_bytes = g_peak;
    out->footprint_bytes = g_footprint;
    out->peak_footprint_bytes = g_peak_footprint;
    out->live_blocks = g_live_blocks;
    out->largest_free_bytes = largest;
    out->free_bytes = free_total;
    out->free_blocks = holes;
    out->meta_bytes = (size_t)NCL_MEM_HDR * arena_blocks;
    memcpy(out->size_hist, g_size_hist, sizeof(g_size_hist));
    for (i = 0; i < NCL_MEM_CLASS_COUNT &&
                i < sizeof(out->class_bytes) / sizeof(out->class_bytes[0]); i++) {
        if (g_class[i].block > 0u) {
            out->class_size[i] = g_class[i].block;
            out->class_bytes[i] = g_class[i].block * (g_class[i].live + g_class[i].free_blocks);
            out->class_live[i] = g_class[i].live;
            out->class_free[i] = g_class[i].free_blocks;
            class_bytes += out->class_bytes[i];
        }
    }
    out->arena_bytes = pool_total() - class_bytes;
    out->allocations = g_allocations;
    out->failures = g_failures;
    out->foreign_frees = g_foreign_frees;
    out->largest_request_bytes = g_largest_request;
    out->failure_free_bytes = g_failure_free;
    out->failure_largest_free_bytes = g_failure_largest_free;
    out->bad_links = g_bad_links;
    pool_unlock();
}

void ncl_mem_reset_stats(void)
{
    size_t i;

    pool_lock();
    g_peak = g_in_use;
    g_peak_footprint = g_footprint;
    g_allocations = 0;
    g_failures = 0;
    g_failure_free = 0;
    g_failure_largest_free = 0;
    g_bad_links = 0;
    for (i = 0; i < sizeof(g_size_hist) / sizeof(g_size_hist[0]); i++) {
        g_size_hist[i] = 0;
    }
    pool_unlock();
}

size_t ncl_mem_check(void)
{
    const unsigned char *p;
    size_t problems = 0;
    size_t covered = 0;
    size_t used_payload = 0;   /* what is handed out, from the blocks themselves */
    size_t free_payload = 0;   /* what is free, from the blocks themselves      */
    size_t live_blocks = 0;
    size_t arena_block_count = 0;
    size_t prev_total = 0;
    bool   prev_free = false;
    const unsigned char *prev_block = NULL;
    ncl_mem_block *b;
    size_t reported = 0;
    size_t i;

    pool_lock();
    pool_init_locked();

    /* Size classes: the free list has to stay a bounded, in-region list, and
     * every block of the region is either handed out or on that list. */
    for (i = 0; i < NCL_MEM_CLASS_COUNT; i++) {
        ncl_mem_class *c = &g_class[i];
        const unsigned char *slot;
        size_t seen = 0;
        size_t region_blocks;
        size_t guard;

        if (c->block == 0u) {
            if (c->begin != NULL || c->live != 0u || c->free_blocks != 0u) {
                problems++;
            }
            continue;
        }
        region_blocks = (size_t)(c->end - c->begin) / c->block;
        if (c->live + c->free_blocks != region_blocks) {
            problems++;
            if (reported++ < 8u) {
                fprintf(stderr,
                        "ncl_mem_check: class %lu has %lu live + %lu free, "
                        "region holds %lu\n",
                        (unsigned long)c->block, (unsigned long)c->live,
                        (unsigned long)c->free_blocks, (unsigned long)region_blocks);
            }
        }
        slot = (const unsigned char *)c->head;
        guard = region_blocks + 1u;
        while (slot != NULL && guard-- > 0u) {
            if (slot < c->begin || slot >= c->end ||
                ((size_t)(slot - c->begin) % c->block) != 0u) {
                problems++;
                if (reported++ < 8u) {
                    fprintf(stderr, "ncl_mem_check: class %lu free list holds %p\n",
                            (unsigned long)c->block, (const void *)slot);
                }
                break;
            }
            seen++;
            slot = *(const unsigned char *const *)slot;
        }
        if (slot != NULL && guard == (size_t)-1) {
            problems++; /* the list loops */
            if (reported++ < 8u) {
                fprintf(stderr, "ncl_mem_check: class %lu free list loops\n",
                        (unsigned long)c->block);
            }
        }
        if (seen != c->free_blocks) {
            problems++;
            if (reported++ < 8u) {
                fprintf(stderr, "ncl_mem_check: class %lu free list has %lu entries, "
                        "counter says %lu\n", (unsigned long)c->block,
                        (unsigned long)seen, (unsigned long)c->free_blocks);
            }
        }
        /* The counters have to agree with the region, not the other way round. */
        used_payload += (region_blocks - seen) * c->block;
        free_payload += seen * c->block;
        live_blocks += region_blocks - seen;
        covered += (size_t)(c->end - c->begin);
    }

    /* The arena. */
    if (g_arena_begin == NULL) {
        problems++; /* the pool does not even hold one arena block */
    }
    b = pool_block_at(g_arena_begin);
    while (b != NULL) {
        if (b->h.prev_total != prev_total) {
            problems++;
            if (reported++ < 8u) {
                fprintf(stderr,
                        "ncl_mem_check: block %p total=%lu says prev=%lu, walk says %lu\n",
                        (void *)b, (unsigned long)b->h.total,
                        (unsigned long)b->h.prev_total, (unsigned long)prev_total);
            }
        }
        if (b->h.free_flag && prev_free) {
            problems++; /* two free neighbours that should have been merged */
            if (reported++ < 8u) {
                fprintf(stderr,
                        "ncl_mem_check: %p (total=%lu) and %p are both free\n",
                        (void *)prev_block, (unsigned long)b->h.total, (void *)b);
            }
        }
        covered += b->h.total;
        arena_block_count++;
        if (b->h.free_flag) {
            free_payload += pool_payload(b);
        } else {
            used_payload += pool_payload(b);
            live_blocks++;
        }
        if (covered > pool_total()) {
            problems++; /* the chain runs past the end of the arena */
            if (reported++ < 8u) {
                fprintf(stderr, "ncl_mem_check: block %p runs past the arena end\n",
                        (void *)b);
            }
            break;
        }
        prev_block = (const unsigned char *)b;
        prev_total = b->h.total;
        prev_free = b->h.free_flag == 1u;
        p = (const unsigned char *)b + b->h.total;
        b = pool_block_at(p);
    }
    if (covered != pool_total()) {
        problems++;
        fprintf(stderr, "ncl_mem_check: regions cover %lu of %lu bytes\n",
                (unsigned long)covered, (unsigned long)pool_total());
    }
    /*
     * The accounting identity, computed from the blocks rather than from the
     * counters, and compared under the same lock - so a concurrent caller
     * cannot make it look inconsistent. This is what makes the check usable as
     * a multithreaded invariant: every allocation, release and resize has to
     * leave these three numbers telling the truth.
     */
    if (used_payload != g_in_use || live_blocks != g_live_blocks) {
        problems++;
        if (reported++ < 8u) {
            fprintf(stderr,
                    "ncl_mem_check: blocks say %lu bytes in %lu blocks, "
                    "counters say %lu in %lu\n",
                    (unsigned long)used_payload, (unsigned long)live_blocks,
                    (unsigned long)g_in_use, (unsigned long)g_live_blocks);
        }
    }
    if (used_payload + free_payload + (size_t)NCL_MEM_HDR * arena_block_count !=
        pool_total()) {
        problems++;
        if (reported++ < 8u) {
            fprintf(stderr,
                    "ncl_mem_check: %lu used + %lu free + %lu header bytes != "
                    "%lu pool bytes\n",
                    (unsigned long)used_payload, (unsigned long)free_payload,
                    (unsigned long)((size_t)NCL_MEM_HDR * arena_block_count),
                    (unsigned long)pool_total());
        }
    }
    if (problems > reported) {
        fprintf(stderr, "ncl_mem_check: %lu more problems not shown\n",
                (unsigned long)(problems - reported));
    }
    pool_unlock();
    return problems;
}

#endif /* NCL_STATIC_MEM */
