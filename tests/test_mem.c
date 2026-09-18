/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * Unit tests for the allocation seam (ncl_mem_*).
 *
 * The same test binary is expected to pass in both builds: the default one
 * forwards to the C runtime, the NCL_STATIC_MEM one serves every request from
 * a fixed pool. The pooling specific checks are compiled in only for the
 * static build (NCL_STATIC_MEM is defined for this target by CMake).
 */
#include "ncl_test.h"

#include "nclink/ncl_common.h"

#include <stdint.h>

/** Free bytes sitting on the size class free lists (arena holes are not one). */
static size_t class_free_bytes(const ncl_mem_stats *s)
{
    size_t i;
    size_t sum = 0;

    for (i = 0; i < sizeof(s->class_free) / sizeof(s->class_free[0]); i++) {
        sum += s->class_free[i] * s->class_size[i];
    }
    return sum;
}

static void test_basics(void)
{
    unsigned char *p;
    unsigned char *q;
    size_t i;

    NCL_TEST_CASE("allocate, touch, release");
    p = (unsigned char *)ncl_mem_alloc(64);
    NCL_CHECK(p != NULL);
    for (i = 0; i < 64; i++) {
        p[i] = (unsigned char)i;
    }
    ncl_mem_free(p);

    NCL_TEST_CASE("payload is aligned like malloc's");
    p = (unsigned char *)ncl_mem_alloc(1);
    NCL_CHECK(p != NULL);
    NCL_CHECK_EQ_INT((uintptr_t)p % NCL_MEM_ALIGNMENT, 0);
    ncl_mem_free(p);

    NCL_TEST_CASE("zero size still returns a unique pointer");
    p = (unsigned char *)ncl_mem_alloc(0);
    q = (unsigned char *)ncl_mem_alloc(0);
    NCL_CHECK(p != NULL && q != NULL && p != q);
    ncl_mem_free(p);
    ncl_mem_free(q);

    NCL_TEST_CASE("ncl_free_safe(NULL) is a no-op");
    ncl_mem_free(NULL);
}

static void test_calloc(void)
{
    unsigned char *p;
    size_t i;
    int zeroed = 1;

    NCL_TEST_CASE("calloc hands back zeroed memory");
    p = (unsigned char *)ncl_mem_calloc(100, 3);
    NCL_CHECK(p != NULL);
    for (i = 0; i < 300; i++) {
        if (p[i] != 0) {
            zeroed = 0;
        }
    }
    NCL_CHECK(zeroed);
    ncl_mem_free(p);

    NCL_TEST_CASE("calloc refuses an overflowing request");
    if (strcmp(ncl_mem_mode(), "heap") == 0) {
        /* The guard lives in the pool; in the heap build this would be a
         * libc calloc() with an overflowing size, which is exactly what the
         * sanitizers abort on. */
        printf("    (skipped: the overflow guard is part of the static pool)\n");
    } else {
        NCL_CHECK(ncl_mem_calloc((size_t)-1, 2) == NULL);
    }
}

static void test_realloc(void)
{
    char *p;
    char *grown;

    NCL_TEST_CASE("realloc(NULL, n) behaves like alloc");
    p = (char *)ncl_mem_realloc(NULL, 32);
    NCL_CHECK(p != NULL);
    memset(p, 'a', 31);
    p[31] = '\0';

    NCL_TEST_CASE("growing keeps the old bytes");
    grown = (char *)ncl_mem_realloc(p, 4096);
    NCL_CHECK(grown != NULL);
    NCL_CHECK_EQ_STR(grown, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    memset(grown, 'b', 4095);
    grown[4095] = '\0';

    NCL_TEST_CASE("shrinking keeps the prefix");
    p = (char *)ncl_mem_realloc(grown, 8);
    NCL_CHECK(p != NULL);
    p[7] = '\0';
    NCL_CHECK_EQ_STR(p, "bbbbbbb");

    NCL_TEST_CASE("realloc(p, 0) releases p");
    NCL_CHECK(ncl_mem_realloc(p, 0) == NULL);
}

static void test_strings(void)
{
    char *s;

    NCL_TEST_CASE("library strings come from the same allocator");
    s = ncl_strdup("NC-Link");
    NCL_CHECK(s != NULL);
    NCL_CHECK_EQ_STR(s, "NC-Link");
    ncl_free_safe(s);

    NCL_TEST_CASE("a variant cannot be served at all");
    if (strcmp(ncl_mem_mode(), "heap") == 0) {
        /* libc malloc(SIZE_MAX) is undefined-ish: ASan calls it
         * "allocation-size-too-big". The pool's own guard is what is under
         * test here, so this only runs against the pool. */
        printf("    (skipped: the size guard is part of the static pool)\n");
    } else {
        NCL_CHECK(ncl_mem_alloc((size_t)-1) == NULL);
    }
}

#if defined(NCL_STATIC_MEM)

/*
 * Coalescing: fill the pool with equal blocks until it refuses, release them
 * all, and check that the free space came back as one big block. A pool that
 * fragmented or leaked would end up with a much smaller largest free block.
 */
static void test_pool_reuse(void)
{
    enum { MAX_BLOCKS = 1024 };
    static void *blocks[MAX_BLOCKS];
    ncl_mem_stats before;
    ncl_mem_stats after;
    size_t count = 0;
    size_t i;
    size_t block_size;

    NCL_TEST_CASE("pool reports its size");
    ncl_mem_get_stats(&before);
    NCL_CHECK(before.pool_bytes >= 4096);
    NCL_CHECK(before.in_use_bytes == 0);
    NCL_CHECK_EQ_STR(ncl_mem_mode(), "static-pool");

    NCL_TEST_CASE("pool refuses what does not fit and says so");
    NCL_CHECK_EQ_INT(ncl_mem_check(), 0);
    NCL_CHECK(ncl_mem_alloc(before.pool_bytes) == NULL);
    ncl_mem_get_stats(&after);
    NCL_CHECK_EQ_INT(after.failures, before.failures + 1);
    NCL_CHECK_EQ_INT(ncl_mem_check(), 0);

    NCL_TEST_CASE("pool hands out until it is full, then refuses");
    /* Size the request from the pool so the loop reaches exhaustion whether
     * the pool is 64 KiB or 20 MiB. */
    block_size = before.pool_bytes / 256u;
    if (block_size < 64u) {
        block_size = 64u;
    }
    while (count < MAX_BLOCKS) {
        void *p = ncl_mem_alloc(block_size);
        if (p == NULL) {
            break;
        }
        blocks[count++] = p;
    }
    NCL_CHECK(count > 8);
    NCL_CHECK(count < MAX_BLOCKS); /* it stopped because the pool said no */
    ncl_mem_get_stats(&after);
    NCL_CHECK(after.in_use_bytes > 0);
    NCL_CHECK(after.peak_in_use_bytes >= after.in_use_bytes);
    NCL_CHECK_EQ_INT(after.live_blocks, count);
    NCL_CHECK(after.failures > before.failures);
    NCL_CHECK_EQ_INT(ncl_mem_check(), 0);

    NCL_TEST_CASE("releasing everything coalesces back into one block");
    for (i = 0; i < count; i++) {
        ncl_mem_free(blocks[i]);
    }
    NCL_CHECK_EQ_INT(ncl_mem_check(), 0);
    ncl_mem_get_stats(&after);
    NCL_CHECK_EQ_INT(after.in_use_bytes, 0);
    NCL_CHECK_EQ_INT(after.live_blocks, 0);
    /* The arena - not the whole pool: the size class regions are part of the
     * pool but have no holes to speak of. */
    NCL_CHECK(after.largest_free_bytes + 4096 >= after.arena_bytes);

    NCL_TEST_CASE("the same workload fits again");
    for (i = 0; i < count; i++) {
        blocks[i] = ncl_mem_alloc(block_size);
        if (blocks[i] == NULL) {
            NCL_CHECK(blocks[i] != NULL);
            break;
        }
    }
    NCL_CHECK_EQ_INT(ncl_mem_check(), 0);
    ncl_mem_get_stats(&after);
    NCL_CHECK_EQ_INT(after.live_blocks, count);
    for (i = 0; i < count; i++) {
        ncl_mem_free(blocks[i]);
    }
}

static void test_pool_stress(void)
{
    enum { ROUNDS = 2000 };
    ncl_mem_stats start;
    ncl_mem_stats end;
    size_t i;

    NCL_TEST_CASE("mixed sizes recycle without drifting");
    ncl_mem_get_stats(&start);
    for (i = 0; i < ROUNDS; i++) {
        void *a = ncl_mem_alloc(16 + (i % 97));
        void *b = ncl_mem_alloc(200 + (i % 13));
        void *c = ncl_mem_realloc(a, 32 + (i % 41));
        NCL_CHECK(b != NULL);
        NCL_CHECK(c != NULL);
        ncl_mem_free(b);
        ncl_mem_free(c);
    }
    ncl_mem_get_stats(&end);
    NCL_CHECK_EQ_INT(end.in_use_bytes, start.in_use_bytes);
    NCL_CHECK_EQ_INT(end.live_blocks, start.live_blocks);
    NCL_CHECK_EQ_INT(end.failures, start.failures);
    NCL_CHECK(end.peak_in_use_bytes > 0);
}

/*
 * Fragmentation. Two things have to hold when churn happens around a set of
 * long lived blocks (that is what a running device looks like: the model and
 * the server tables stay, messages come and go):
 *
 *   1. the churn is never refused while the pool as a whole has room,
 *   2. once the churn is released the free space is one single block again -
 *      no permanent holes - and that stays true cycle after cycle.
 *
 * Sizes are derived from the pool so the test means the same thing at 64 KiB
 * and at the 20 MiB default.
 */
static void test_pool_fragmentation(void)
{
    enum { PERSISTENT = 8, CYCLES = 6, ROUNDS = 300 };
    static void *fixed[PERSISTENT];
    ncl_mem_stats s;
    size_t unit;
    size_t big;
    size_t baseline_largest;
    size_t i;
    size_t cycle;
    size_t round;

    NCL_TEST_CASE("long lived blocks age without the churn carving holes");
    ncl_mem_get_stats(&s);
    unit = s.pool_bytes / 128u;
    if (unit < 64u) {
        unit = 64u;
    }
    big = unit * 8u;

    for (i = 0; i < PERSISTENT; i++) {
        fixed[i] = ncl_mem_alloc(unit + i * 32u);
        NCL_CHECK(fixed[i] != NULL);
    }
    ncl_mem_get_stats(&s);
    baseline_largest = s.largest_free_bytes;

    for (cycle = 0; cycle < CYCLES; cycle++) {
        ncl_mem_stats before;
        ncl_mem_stats after;

        ncl_mem_get_stats(&before);
        for (round = 0; round < ROUNDS; round++) {
            void *a = ncl_mem_alloc(unit + (round * 37u) % big);
            void *b = ncl_mem_alloc(unit / 4u + (round * 91u) % (unit / 2u));
            void *grown = NULL;

            if (a != NULL) {
                grown = ncl_mem_realloc(a, unit + (round * 53u) % (big * 2u));
            }
            NCL_CHECK(a != NULL);
            NCL_CHECK(b != NULL);
            NCL_CHECK(grown != NULL);
            ncl_mem_free(b);
            ncl_mem_free(grown);
        }
        ncl_mem_get_stats(&after);

        /* Nothing was refused, the churn is gone, and the free space came
         * back as one block of exactly the size it had before the cycle. */
        NCL_CHECK_EQ_INT(after.failures, before.failures);
        NCL_CHECK_EQ_INT(after.live_blocks, before.live_blocks);
        NCL_CHECK_EQ_INT(after.free_blocks, 1);
        NCL_CHECK_EQ_INT(after.largest_free_bytes, baseline_largest);
        /* The arena holds one hole, so the rest of the free space has to be the
         * size class free lists - nothing else can be unaccounted for. */
        NCL_CHECK_EQ_INT(after.free_bytes,
                         after.largest_free_bytes + class_free_bytes(&after));
    }

    for (i = 0; i < PERSISTENT; i++) {
        ncl_mem_free(fixed[i]);
    }
    ncl_mem_get_stats(&s);
    NCL_CHECK_EQ_INT(s.in_use_bytes, 0);
    NCL_CHECK_EQ_INT(s.live_blocks, 0);
    NCL_CHECK_EQ_INT(s.free_blocks, 1);
    NCL_CHECK(s.largest_free_bytes + 64u >= s.arena_bytes);
}

#endif /* NCL_STATIC_MEM */

/*
 * Size classes: small objects come from a fixed-size region with no header, so
 * they cost less than an arena block, they cannot fragment, and a resize that
 * still fits the block does not move the object.
 */
#if defined(NCL_STATIC_MEM)
static void test_size_classes(void)
{
    ncl_mem_stats before;
    ncl_mem_stats after;
    static void *objs[512];
    size_t take;
    size_t i;
    size_t class_index = 0u; /* resolved below: the 64 byte class */
    int    found = 0;

    NCL_TEST_CASE("small requests are served by a size class");
    ncl_mem_get_stats(&before);
    for (i = 0; i < sizeof(before.class_size) / sizeof(before.class_size[0]); i++) {
        if (before.class_size[i] == 64u) {
            class_index = i;
            found = 1;
            break;
        }
    }
    if (!found || before.class_free[class_index] < 4u) {
        printf("    (size classes are not configured in this build)\n");
        return;
    }
    NCL_CHECK_EQ_INT(before.class_size[class_index], 64);

    take = before.class_free[class_index];
    if (take > 200u) {
        take = 200u;
    }
    NCL_CHECK(take > 1u);
    for (i = 0; i < take; i++) {
        objs[i] = ncl_mem_alloc(56); /* fits the 64 byte class */
        NCL_CHECK(objs[i] != NULL);
        if (objs[i] == NULL) {
            break;
        }
    }
    ncl_mem_get_stats(&after);
    NCL_CHECK_EQ_INT(after.class_live[class_index],
                     before.class_live[class_index] + take);
    NCL_CHECK_EQ_INT(after.meta_bytes, before.meta_bytes); /* no new headers */
    /* The saving being bought here: 56 bytes costs 64 in its class, against a
     * 32 byte header plus 64 bytes of payload = 96 in the arena. */
    NCL_CHECK_EQ_INT(after.in_use_bytes - before.in_use_bytes, take * 64u);

    NCL_TEST_CASE("a resize that still fits its block does not move the object");
    NCL_CHECK(ncl_mem_realloc(objs[0], 64) == objs[0]);
    NCL_CHECK(ncl_mem_realloc(objs[0], 8) == objs[0]);

    NCL_TEST_CASE("an exhausted class falls back to the arena, never fails");
    /*
     * Draining the class has to be done by hand, so this runs only where the
     * free list fits in the table below - the small pools, which is where the
     * fallback matters. In a pool with tens of thousands of free class blocks
     * the same path is covered by the Monte Carlo suite.
     */
    if (before.class_free[class_index] <= 300u) {
        size_t extra = before.class_free[class_index] - take + 16u;
        size_t got = 0;

        for (i = 0; i < extra && take + got < 512u; i++) {
            void *p = ncl_mem_alloc(60);
            if (p == NULL) {
                break;
            }
            objs[take + got] = p;
            got++;
        }
        ncl_mem_get_stats(&after);
        NCL_CHECK_EQ_INT(after.class_free[class_index], 0); /* class emptied */
        NCL_CHECK_EQ_INT(got, extra); /* the arena took every one of them */
        NCL_CHECK(after.meta_bytes > before.meta_bytes);    /* ... with headers */
        take += got;
    } else {
        printf("    (class is large here; the fallback is covered by mem_mc)\n");
    }

    NCL_TEST_CASE("releasing everything leaves both regions consistent");
    for (i = 0; i < take; i++) {
        ncl_mem_free(objs[i]);
    }
    NCL_CHECK_EQ_INT(ncl_mem_check(), 0);
    ncl_mem_get_stats(&after);
    NCL_CHECK_EQ_INT(after.in_use_bytes, before.in_use_bytes);
    NCL_CHECK_EQ_INT(after.class_live[class_index], before.class_live[class_index]);
    NCL_CHECK_EQ_INT(after.free_blocks, before.free_blocks);
    NCL_CHECK(after.largest_free_bytes + 4096 >= after.arena_bytes);
}
#endif /* NCL_STATIC_MEM */

NCL_TEST_MAIN_BEGIN()
    test_basics();
    test_calloc();
    test_realloc();
    test_strings();
#if defined(NCL_STATIC_MEM)
    test_pool_reuse();
    test_pool_stress();
    test_pool_fragmentation();
    test_size_classes();
#endif
NCL_TEST_MAIN_END()
