/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* Unit tests for the thread pool, the shared pool and the TTL cache. */
#include "ncl_test.h"

#include "nclink/ncl_thread.h"

static ncl_mutex *g_counter_mutex = NULL;
static volatile long g_counter = 0;

static void bump(void *arg)
{
    long amount = (long)(intptr_t)arg;
    ncl_mutex_lock(g_counter_mutex);
    g_counter += amount;
    ncl_mutex_unlock(g_counter_mutex);
}

/* Gate state shared with the blocking task (see the saturation test). */
static ncl_mutex *g_gate_mutex = NULL;
static ncl_cond  *g_gate_cond = NULL;
static bool       g_gate_open = false;
static long       g_gate_started = 0;

static void blocking_task(void *arg)
{
    (void)arg;
    ncl_mutex_lock(g_gate_mutex);
    g_gate_started++;
    ncl_cond_broadcast(g_gate_cond);
    while (!g_gate_open) {
        ncl_cond_wait(g_gate_cond, g_gate_mutex);
    }
    ncl_mutex_unlock(g_gate_mutex);
}

static void test_pool_runs_tasks(void)
{
    ncl_thread_pool_options options;
    ncl_thread_pool *pool;
    int i;

    ncl_thread_pool_options_default(&options);
    NCL_TEST_CASE("defaults are 5 core / 10 max / 100 queue");
    NCL_CHECK_EQ_INT(options.core_threads, 5);
    NCL_CHECK_EQ_INT(options.max_threads, 10);
    NCL_CHECK_EQ_INT(options.queue_capacity, 100);

    g_counter_mutex = ncl_mutex_create();
    g_counter = 0;
    pool = ncl_thread_pool_create(&options);
    NCL_CHECK(pool != NULL);
    if (pool == NULL) {
        return;
    }
    NCL_CHECK_EQ_INT(ncl_thread_pool_worker_count(pool), 5);

    NCL_TEST_CASE("submitted tasks all run");
    for (i = 0; i < 50; i++) {
        ncl_thread_pool_submit(pool, bump, (void *)(intptr_t)1);
    }
    for (i = 0; i < 500 && g_counter < 50; i++) {
        ncl_sleep_millis(10);
    }
    NCL_CHECK_EQ_INT(g_counter, 50);
    ncl_thread_pool_shutdown(pool, true);
    ncl_mutex_destroy(g_counter_mutex);
    g_counter_mutex = NULL;
}

static void test_pool_caller_runs_when_saturated(void)
{
    ncl_thread_pool_options options;
    ncl_thread_pool *pool;
    int i;

    /* A gate lets the single worker be parked deterministically, so the
     * caller-runs path can be observed without relying on timing. */
    g_gate_mutex = ncl_mutex_create();
    g_gate_cond = ncl_cond_create();
    g_gate_open = false;
    g_gate_started = 0;

    options.core_threads = 1;
    options.max_threads = 1;
    options.queue_capacity = 1;
    options.idle_timeout_ms = 1000;

    g_counter_mutex = ncl_mutex_create();
    g_counter = 0;
    pool = ncl_thread_pool_create(&options);
    NCL_CHECK(pool != NULL);
    if (pool == NULL) {
        return;
    }

    NCL_TEST_CASE("a saturated pool runs the overflow task on the calling thread");
    /* Park the only worker on the gate. */
    ncl_mutex_lock(g_gate_mutex);
    NCL_CHECK_EQ_INT(ncl_thread_pool_submit(pool, blocking_task, NULL), NCL_OK);
    for (i = 0; i < 500 && g_gate_started == 0; i++) {
        ncl_cond_wait_timeout(g_gate_cond, g_gate_mutex, 10);
    }
    NCL_CHECK_EQ_INT(g_gate_started, 1); /* worker is blocked inside task 1 */

    /* Fill the single queue slot; the worker cannot pick it up yet. */
    NCL_CHECK_EQ_INT(ncl_thread_pool_submit(pool, bump, (void *)(intptr_t)1), NCL_OK);
    NCL_CHECK_EQ_INT(g_counter, 0);

    /* Queue is full and the pool is at max_threads: this must run inline. */
    NCL_CHECK_EQ_INT(ncl_thread_pool_submit(pool, bump, (void *)(intptr_t)1), NCL_OK);
    NCL_CHECK_EQ_INT(g_counter, 1);

    /* Release the worker and let everything drain. */
    g_gate_open = true;
    ncl_cond_broadcast(g_gate_cond);
    for (i = 0; i < 500 && g_counter < 2; i++) {
        ncl_mutex_unlock(g_gate_mutex);
        ncl_sleep_millis(10);
        ncl_mutex_lock(g_gate_mutex);
    }
    ncl_mutex_unlock(g_gate_mutex);
    NCL_CHECK_EQ_INT(g_counter, 2);

    /* All three submissions ran exactly once. */
    NCL_CHECK_EQ_INT(ncl_thread_pool_worker_count(pool), 1);
    ncl_thread_pool_shutdown(pool, true);
    ncl_mutex_destroy(g_counter_mutex);
    g_counter_mutex = NULL;
    ncl_cond_destroy(g_gate_cond);
    ncl_mutex_destroy(g_gate_mutex);
    g_gate_cond = NULL;
    g_gate_mutex = NULL;
}

static void test_shared_service(void)
{
    ncl_thread_pool *pool = ncl_thread_service();

    NCL_TEST_CASE("the shared pool is a singleton");
    NCL_CHECK(pool != NULL);
    NCL_CHECK(ncl_thread_service() == pool);
    ncl_thread_service_shutdown();
}

static void test_cache(void)
{
    ncl_cache *cache;
    char *value = ncl_strdup("first");

    NCL_TEST_CASE("cache stores and returns values");
    cache = ncl_cache_create(0, false, ncl_mem_free);
    NCL_CHECK(cache != NULL);
    NCL_CHECK_EQ_INT(ncl_cache_put(cache, "k1", value), NCL_OK);
    NCL_CHECK_EQ_STR((const char *)ncl_cache_get(cache, "k1"), "first");
    NCL_CHECK(ncl_cache_get(cache, "missing") == NULL);
    NCL_CHECK_EQ_INT(ncl_cache_size(cache), 1);

    NCL_TEST_CASE("put replaces and frees the previous value");
    NCL_CHECK_EQ_INT(ncl_cache_put(cache, "k1", ncl_strdup("second")), NCL_OK);
    NCL_CHECK_EQ_STR((const char *)ncl_cache_get(cache, "k1"), "second");
    NCL_CHECK_EQ_INT(ncl_cache_size(cache), 1);

    NCL_TEST_CASE("remove deletes entries");
    NCL_CHECK(ncl_cache_remove(cache, "k1"));
    NCL_CHECK(!ncl_cache_remove(cache, "k1"));
    NCL_CHECK_EQ_INT(ncl_cache_size(cache), 0);
    ncl_cache_free(cache);

    NCL_TEST_CASE("entries expire after the TTL");
    cache = ncl_cache_create(150, false, ncl_mem_free);
    ncl_cache_put(cache, "e1", ncl_strdup("v"));
    NCL_CHECK(ncl_cache_get(cache, "e1") != NULL);
    ncl_sleep_millis(400);
    NCL_CHECK(ncl_cache_get(cache, "e1") == NULL);
    NCL_CHECK_EQ_INT(ncl_cache_size(cache), 0);
    ncl_cache_free(cache);

    NCL_TEST_CASE("reading refreshes the deadline");
    /* Generous margins: the assertions must not depend on how closely a sleep
     * matches the requested duration on a loaded machine. */
    cache = ncl_cache_create(1000, true, ncl_mem_free);
    ncl_cache_put(cache, "a1", ncl_strdup("v"));
    ncl_sleep_millis(400);
    NCL_CHECK(ncl_cache_get(cache, "a1") != NULL); /* refresh: deadline -> +1000 */
    ncl_sleep_millis(400);
    NCL_CHECK(ncl_cache_get(cache, "a1") != NULL); /* ~400 ms since refresh */
    /* Every successful read pushes the deadline out by the full TTL, so the
     * final wait must exceed 1000 ms without touching the entry. */
    ncl_sleep_millis(1400);
    NCL_CHECK(ncl_cache_get(cache, "a1") == NULL);
    ncl_cache_free(cache);
}

NCL_TEST_MAIN_BEGIN()
    test_pool_runs_tasks();
    test_pool_caller_runs_when_saturated();
    test_shared_service();
    test_cache();
NCL_TEST_MAIN_END()
