/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - thread pool and TTL cache.
 *
 * Two utilities that the client and the server both rely on:
 *   ncl_thread_pool - one process wide pool (ncl_thread_service()): 5 core
 *                     threads, 10 max, a 100 slot queue, 60 s idle timeout;
 *                     a full queue runs the task on the calling thread.
 *   ncl_cache       - TTL map used for the client side response cache (5 min
 *                     after write) and the client registry (30 min after
 *                     access).
 */
#ifndef NCL_THREAD_H
#define NCL_THREAD_H

#include <stdbool.h>

#include "nclink/ncl_common.h"
#include "nclink/ncl_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================ thread pool = */

typedef struct ncl_thread_pool ncl_thread_pool;

typedef struct {
    int    core_threads;    /**< default 5                                */
    int    max_threads;     /**< default 10                               */
    size_t queue_capacity;  /**< default 100                              */
    unsigned idle_timeout_ms; /**< default 60 s                           */
} ncl_thread_pool_options;

/** Fill @p options with the defaults listed above. */
void ncl_thread_pool_options_default(ncl_thread_pool_options *options);

ncl_thread_pool *ncl_thread_pool_create(const ncl_thread_pool_options *options);

/**
 * Queue @p fn for execution. When the queue is full and the pool cannot grow,
 * the task runs on the calling thread.
 */
ncl_err ncl_thread_pool_submit(ncl_thread_pool *pool, ncl_thread_fn fn, void *arg);

/** Stop accepting work; when @p wait is true, join every worker. */
void ncl_thread_pool_shutdown(ncl_thread_pool *pool, bool wait);

size_t ncl_thread_pool_pending(const ncl_thread_pool *pool);
int    ncl_thread_pool_worker_count(const ncl_thread_pool *pool);

/** Process wide pool, created on first use. */
ncl_thread_pool *ncl_thread_service(void);

/** Shut the shared pool down (call once during process teardown). */
void ncl_thread_service_shutdown(void);

/** Default timeout of one submitted operation, milliseconds. */
#define NCL_THREAD_OPERATION_TIMEOUT 5000

/* ================================================================ cache === */

typedef struct ncl_cache ncl_cache;
typedef void (*ncl_cache_free_fn)(void *value);

/**
 * Create a cache.
 * @param ttl_ms                entry lifetime; 0 means "never expires".
 * @param expire_after_access   refresh the deadline on every read, otherwise
 *                              entries expire at a fixed time after insertion.
 * @param free_fn               called on evicted values, may be NULL.
 */
ncl_cache *ncl_cache_create(unsigned ttl_ms, bool expire_after_access,
                            ncl_cache_free_fn free_fn);

void ncl_cache_free(ncl_cache *cache);
void ncl_cache_clear(ncl_cache *cache);

/** Insert or replace @p key. Ownership of @p value transfers to the cache. */
ncl_err ncl_cache_put(ncl_cache *cache, const char *key, void *value);

/** Look up @p key; returns NULL when absent or expired. */
void *ncl_cache_get(ncl_cache *cache, const char *key);

/** Remove @p key; returns true when an entry was removed. */
bool ncl_cache_remove(ncl_cache *cache, const char *key);

/**
 * Remove @p key and hand the value to the caller without invoking the cache's
 * free function. Returns NULL when the key is absent or expired. Use this
 * instead of get()+remove() when the value must outlive the entry.
 */
void *ncl_cache_take(ncl_cache *cache, const char *key);

size_t ncl_cache_size(ncl_cache *cache);

/** Drop expired entries (also done lazily by get/put). */
void ncl_cache_purge_expired(ncl_cache *cache);

#ifdef __cplusplus
}
#endif

#endif /* NCL_THREAD_H */
