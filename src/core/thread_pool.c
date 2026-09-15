/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* NC-Link core - thread pool and TTL cache. */
#include "nclink/ncl_thread.h"

#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_logger.h"

/* ============================================================ thread pool = */

typedef struct {
    ncl_thread_fn fn;
    void         *arg;
} ncl_task;

typedef struct {
    ncl_thread *thread;
    bool        joined;
} ncl_worker_slot;

struct ncl_thread_pool {
    ncl_mutex  *mutex;
    ncl_cond   *cond;
    ncl_task   *queue;
    size_t      queue_len;
    size_t      queue_capacity;
    size_t      queue_head; /**< ring buffer index */
    int         core_threads;
    int         max_threads;
    int         workers;
    unsigned    idle_timeout_ms;
    bool        shutting_down;
    ncl_worker_slot *slots;
    size_t      slot_count;
};

void ncl_thread_pool_options_default(ncl_thread_pool_options *options)
{
    if (options == NULL) {
        return;
    }
    options->core_threads = 5;
    options->max_threads = 10;
    options->queue_capacity = 100;
    options->idle_timeout_ms = 60000;
}

static ncl_task ncl_pool_pop(ncl_thread_pool *pool)
{
    ncl_task task;
    task.fn = NULL;
    task.arg = NULL;
    if (pool->queue_len == 0) {
        return task;
    }
    task = pool->queue[pool->queue_head];
    pool->queue_head = (pool->queue_head + 1) % pool->queue_capacity;
    pool->queue_len--;
    return task;
}

static void ncl_pool_push(ncl_thread_pool *pool, ncl_task task)
{
    size_t tail = (pool->queue_head + pool->queue_len) % pool->queue_capacity;
    pool->queue[tail] = task;
    pool->queue_len++;
}

static void ncl_pool_worker(void *arg)
{
    ncl_thread_pool *pool = (ncl_thread_pool *)arg;

    ncl_mutex_lock(pool->mutex);
    for (;;) {
        if (pool->queue_len > 0) {
            ncl_task task = ncl_pool_pop(pool);
            ncl_mutex_unlock(pool->mutex);
            task.fn(task.arg);
            ncl_mutex_lock(pool->mutex);
            continue;
        }
        if (pool->shutting_down) {
            break;
        }
        {
            bool signalled =
                ncl_cond_wait_timeout(pool->cond, pool->mutex, pool->idle_timeout_ms);
            if (!signalled && pool->queue_len == 0 && !pool->shutting_down &&
                pool->workers > pool->core_threads) {
                /* Idle beyond the keep-alive window and above the core size. */
                pool->workers--;
                break;
            }
        }
    }
    ncl_mutex_unlock(pool->mutex);
}

/* Spawn a worker while the pool lock is held. */
static void ncl_pool_add_worker(ncl_thread_pool *pool)
{
    ncl_thread *thread;

    if (pool->workers >= pool->max_threads) {
        return;
    }
    thread = ncl_thread_start(ncl_pool_worker, pool);
    if (thread == NULL) {
        return;
    }
    if (pool->slot_count == (size_t)pool->max_threads) {
        /* Reuse the slot of a worker that has already been reaped. */
        size_t i;
        for (i = 0; i < pool->slot_count; i++) {
            if (pool->slots[i].joined) {
                pool->slots[i].thread = thread;
                pool->slots[i].joined = false;
                pool->workers++;
                return;
            }
        }
        ncl_thread_detach(thread);
        pool->workers++;
        return;
    }
    pool->slots[pool->slot_count].thread = thread;
    pool->slots[pool->slot_count].joined = false;
    pool->slot_count++;
    pool->workers++;
}

ncl_thread_pool *ncl_thread_pool_create(const ncl_thread_pool_options *options)
{
    ncl_thread_pool_options defaults;
    ncl_thread_pool *pool;
    int i;

    if (options == NULL) {
        ncl_thread_pool_options_default(&defaults);
        options = &defaults;
    }
    if (options->core_threads < 0 || options->max_threads < 1 ||
        options->max_threads < options->core_threads ||
        options->queue_capacity < 1) {
        return NULL;
    }

    pool = (ncl_thread_pool *)calloc(1, sizeof(ncl_thread_pool));
    if (pool == NULL) {
        return NULL;
    }
    pool->queue_capacity = options->queue_capacity;
    pool->core_threads = options->core_threads;
    pool->max_threads = options->max_threads;
    pool->idle_timeout_ms = options->idle_timeout_ms;
    pool->queue = (ncl_task *)calloc(pool->queue_capacity, sizeof(ncl_task));
    pool->slots = (ncl_worker_slot *)calloc((size_t)options->max_threads,
                                            sizeof(ncl_worker_slot));
    pool->mutex = ncl_mutex_create();
    pool->cond = ncl_cond_create();
    if (pool->queue == NULL || pool->slots == NULL || pool->mutex == NULL ||
        pool->cond == NULL) {
        ncl_thread_pool_shutdown(pool, false);
        return NULL;
    }

    ncl_mutex_lock(pool->mutex);
    for (i = 0; i < pool->core_threads; i++) {
        ncl_pool_add_worker(pool);
    }
    ncl_mutex_unlock(pool->mutex);
    return pool;
}

ncl_err ncl_thread_pool_submit(ncl_thread_pool *pool, ncl_thread_fn fn, void *arg)
{
    ncl_task task;

    if (pool == NULL || fn == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    task.fn = fn;
    task.arg = arg;

    ncl_mutex_lock(pool->mutex);
    if (pool->shutting_down) {
        ncl_mutex_unlock(pool->mutex);
        return NCL_ERR_CLOSED;
    }
    if (pool->queue_len < pool->queue_capacity) {
        ncl_pool_push(pool, task);
        ncl_cond_signal(pool->cond);
        ncl_mutex_unlock(pool->mutex);
        return NCL_OK;
    }
    /* Queue saturated: grow towards max_threads and give the fresh worker a
     * moment to drain a slot, otherwise the task runs on the calling thread. */
    if (pool->workers < pool->max_threads) {
        int i;
        ncl_pool_add_worker(pool);
        ncl_cond_broadcast(pool->cond);
        for (i = 0; i < 10 && pool->queue_len == pool->queue_capacity; i++) {
            ncl_cond_wait_timeout(pool->cond, pool->mutex, 5);
        }
        if (pool->queue_len < pool->queue_capacity) {
            ncl_pool_push(pool, task);
            ncl_cond_signal(pool->cond);
            ncl_mutex_unlock(pool->mutex);
            return NCL_OK;
        }
    }
    ncl_mutex_unlock(pool->mutex);
    fn(arg);
    return NCL_OK;
}

void ncl_thread_pool_shutdown(ncl_thread_pool *pool, bool wait)
{
    size_t i;

    if (pool == NULL) {
        return;
    }
    if (pool->mutex != NULL) {
        ncl_mutex_lock(pool->mutex);
        pool->shutting_down = true;
        if (wait) {
            /* Drain queued work before stopping. */
            while (pool->queue_len > 0) {
                ncl_cond_broadcast(pool->cond);
                ncl_mutex_unlock(pool->mutex);
                ncl_sleep_millis(5);
                ncl_mutex_lock(pool->mutex);
            }
        }
        ncl_cond_broadcast(pool->cond);
        ncl_mutex_unlock(pool->mutex);
    }

    for (i = 0; i < pool->slot_count; i++) {
        if (pool->slots[i].thread != NULL && !pool->slots[i].joined) {
            ncl_thread_join(pool->slots[i].thread);
            pool->slots[i].joined = true;
        }
    }

    ncl_cond_destroy(pool->cond);
    ncl_mutex_destroy(pool->mutex);
    free(pool->queue);
    free(pool->slots);
    free(pool);
}

size_t ncl_thread_pool_pending(const ncl_thread_pool *pool)
{
    size_t pending;
    ncl_thread_pool *mutable_pool = (ncl_thread_pool *)pool;
    if (pool == NULL) {
        return 0;
    }
    ncl_mutex_lock(mutable_pool->mutex);
    pending = mutable_pool->queue_len;
    ncl_mutex_unlock(mutable_pool->mutex);
    return pending;
}

int ncl_thread_pool_worker_count(const ncl_thread_pool *pool)
{
    int workers;
    ncl_thread_pool *mutable_pool = (ncl_thread_pool *)pool;
    if (pool == NULL) {
        return 0;
    }
    ncl_mutex_lock(mutable_pool->mutex);
    workers = mutable_pool->workers;
    ncl_mutex_unlock(mutable_pool->mutex);
    return workers;
}

static ncl_thread_pool *g_thread_service = NULL;
static ncl_mutex       *g_thread_service_mutex = NULL;

ncl_thread_pool *ncl_thread_service(void)
{
    if (g_thread_service_mutex == NULL) {
        g_thread_service_mutex = ncl_mutex_create();
    }
    ncl_mutex_lock(g_thread_service_mutex);
    if (g_thread_service == NULL) {
        ncl_thread_pool_options options;
        ncl_thread_pool_options_default(&options);
        g_thread_service = ncl_thread_pool_create(&options);
    }
    ncl_mutex_unlock(g_thread_service_mutex);
    return g_thread_service;
}

void ncl_thread_service_shutdown(void)
{
    if (g_thread_service_mutex == NULL) {
        return;
    }
    ncl_mutex_lock(g_thread_service_mutex);
    if (g_thread_service != NULL) {
        ncl_thread_pool_shutdown(g_thread_service, true);
        g_thread_service = NULL;
    }
    ncl_mutex_unlock(g_thread_service_mutex);
    ncl_mutex_destroy(g_thread_service_mutex);
    g_thread_service_mutex = NULL;
}

/* ================================================================ cache === */

typedef struct {
    char   *key;
    void   *value;
    int64_t expires_at; /**< absolute monotonic millis, 0 = never */
} ncl_cache_entry;

struct ncl_cache {
    ncl_mutex        *mutex;
    ncl_cache_entry  *entries;
    size_t            len;
    size_t            cap;
    unsigned          ttl_ms;
    bool              expire_after_access;
    ncl_cache_free_fn free_fn;
};

static void ncl_cache_entry_release(ncl_cache *cache, ncl_cache_entry *entry)
{
    free(entry->key);
    if (cache->free_fn != NULL && entry->value != NULL) {
        cache->free_fn(entry->value);
    }
}

ncl_cache *ncl_cache_create(unsigned ttl_ms, bool expire_after_access,
                            ncl_cache_free_fn free_fn)
{
    ncl_cache *cache = (ncl_cache *)calloc(1, sizeof(ncl_cache));
    if (cache == NULL) {
        return NULL;
    }
    cache->mutex = ncl_mutex_create();
    if (cache->mutex == NULL) {
        free(cache);
        return NULL;
    }
    cache->ttl_ms = ttl_ms;
    cache->expire_after_access = expire_after_access;
    cache->free_fn = free_fn;
    return cache;
}

static size_t ncl_cache_find(const ncl_cache *cache, const char *key)
{
    size_t i;
    for (i = 0; i < cache->len; i++) {
        if (strcmp(cache->entries[i].key, key) == 0) {
            return i;
        }
    }
    return (size_t)-1;
}

static void ncl_cache_remove_at(ncl_cache *cache, size_t index)
{
    ncl_cache_entry_release(cache, &cache->entries[index]);
    memmove(&cache->entries[index], &cache->entries[index + 1],
            (cache->len - index - 1) * sizeof(ncl_cache_entry));
    cache->len--;
}

void ncl_cache_purge_expired(ncl_cache *cache)
{
    size_t i;
    int64_t now;

    if (cache == NULL) {
        return;
    }
    ncl_mutex_lock(cache->mutex);
    now = ncl_time_monotonic_millis();
    for (i = 0; i < cache->len;) {
        if (cache->entries[i].expires_at != 0 &&
            cache->entries[i].expires_at <= now) {
            ncl_cache_remove_at(cache, i);
        } else {
            i++;
        }
    }
    ncl_mutex_unlock(cache->mutex);
}

ncl_err ncl_cache_put(ncl_cache *cache, const char *key, void *value)
{
    size_t index;
    int64_t expires_at = 0;

    if (cache == NULL || key == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (cache->ttl_ms > 0) {
        expires_at = ncl_time_monotonic_millis() + (int64_t)cache->ttl_ms;
    }

    ncl_mutex_lock(cache->mutex);
    index = ncl_cache_find(cache, key);
    if (index != (size_t)-1) {
        ncl_cache_entry_release(cache, &cache->entries[index]);
        cache->entries[index].key = ncl_strdup(key);
        cache->entries[index].value = value;
        cache->entries[index].expires_at = expires_at;
        ncl_mutex_unlock(cache->mutex);
        return NCL_OK;
    }

    if (cache->len == cache->cap) {
        size_t cap = cache->cap == 0 ? 16 : cache->cap * 2;
        ncl_cache_entry *grown =
            (ncl_cache_entry *)realloc(cache->entries, cap * sizeof(ncl_cache_entry));
        if (grown == NULL) {
            ncl_mutex_unlock(cache->mutex);
            return NCL_ERR_NOMEM;
        }
        cache->entries = grown;
        cache->cap = cap;
    }
    cache->entries[cache->len].key = ncl_strdup(key);
    if (cache->entries[cache->len].key == NULL) {
        ncl_mutex_unlock(cache->mutex);
        return NCL_ERR_NOMEM;
    }
    cache->entries[cache->len].value = value;
    cache->entries[cache->len].expires_at = expires_at;
    cache->len++;
    ncl_mutex_unlock(cache->mutex);
    return NCL_OK;
}

void *ncl_cache_get(ncl_cache *cache, const char *key)
{
    size_t index;
    void *value = NULL;

    if (cache == NULL || key == NULL) {
        return NULL;
    }
    ncl_mutex_lock(cache->mutex);
    index = ncl_cache_find(cache, key);
    if (index != (size_t)-1) {
        int64_t now = ncl_time_monotonic_millis();
        if (cache->entries[index].expires_at != 0 &&
            cache->entries[index].expires_at <= now) {
            ncl_cache_remove_at(cache, index);
        } else {
            if (cache->expire_after_access && cache->ttl_ms > 0) {
                cache->entries[index].expires_at = now + (int64_t)cache->ttl_ms;
            }
            value = cache->entries[index].value;
        }
    }
    ncl_mutex_unlock(cache->mutex);
    return value;
}

bool ncl_cache_remove(ncl_cache *cache, const char *key)
{
    size_t index;
    bool removed = false;

    if (cache == NULL || key == NULL) {
        return false;
    }
    ncl_mutex_lock(cache->mutex);
    index = ncl_cache_find(cache, key);
    if (index != (size_t)-1) {
        ncl_cache_remove_at(cache, index);
        removed = true;
    }
    ncl_mutex_unlock(cache->mutex);
    return removed;
}

void *ncl_cache_take(ncl_cache *cache, const char *key)
{
    size_t index;
    void *value = NULL;

    if (cache == NULL || key == NULL) {
        return NULL;
    }
    ncl_mutex_lock(cache->mutex);
    index = ncl_cache_find(cache, key);
    if (index != (size_t)-1) {
        int64_t now = ncl_time_monotonic_millis();
        if (cache->entries[index].expires_at != 0 &&
            cache->entries[index].expires_at <= now) {
            ncl_cache_remove_at(cache, index);
        } else {
            /* Detach the value, then release the key without the destructor. */
            value = cache->entries[index].value;
            free(cache->entries[index].key);
            memmove(&cache->entries[index], &cache->entries[index + 1],
                    (cache->len - index - 1) * sizeof(ncl_cache_entry));
            cache->len--;
        }
    }
    ncl_mutex_unlock(cache->mutex);
    return value;
}

void ncl_cache_clear(ncl_cache *cache)
{
    if (cache == NULL) {
        return;
    }
    ncl_mutex_lock(cache->mutex);
    while (cache->len > 0) {
        ncl_cache_remove_at(cache, cache->len - 1);
    }
    ncl_mutex_unlock(cache->mutex);
}

size_t ncl_cache_size(ncl_cache *cache)
{
    size_t size;
    if (cache == NULL) {
        return 0;
    }
    ncl_mutex_lock(cache->mutex);
    size = cache->len;
    ncl_mutex_unlock(cache->mutex);
    return size;
}

void ncl_cache_free(ncl_cache *cache)
{
    if (cache == NULL) {
        return;
    }
    ncl_cache_clear(cache);
    free(cache->entries);
    ncl_mutex_destroy(cache->mutex);
    free(cache);
}
