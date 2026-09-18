/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - common types: error codes, string helpers, string buffer,
 * pointer vector.
 *
 * Building blocks shared by every module: error codes, string helpers, a
 * growable text buffer and owning vectors.
 */
#ifndef NCL_COMMON_H
#define NCL_COMMON_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Library version. The NC-Link specification revision implemented by this
 * library is still 3.0.0 (GB/T 41970-2022).
 */
#define NCL_VERSION "3.3.0"

/* ------------------------------------------------------------------ error -- */
typedef int ncl_err;

#define NCL_OK 0

/* Infrastructure errors. */
#define NCL_ERR              (-1)  /**< generic failure            */
#define NCL_ERR_NOMEM        (-2)  /**< allocation failure         */
#define NCL_ERR_PARSE        (-3)  /**< malformed JSON / payload   */
#define NCL_ERR_TIMEOUT      (-4)  /**< request timed out          */
#define NCL_ERR_IO           (-5)  /**< file or socket error       */
#define NCL_ERR_NOT_FOUND    (-6)  /**< lookup miss                */
#define NCL_ERR_EXISTS       (-7)  /**< duplicate entry            */
#define NCL_ERR_NOT_SUPPORTED (-8) /**< feature not compiled in    */
#define NCL_ERR_INVALID_ARG  (-9)  /**< caller passed a bad pointer/enum */
#define NCL_ERR_STATE        (-10) /**< object not in a usable state */
#define NCL_ERR_RANGE        (-11) /**< index outside bounds       */
#define NCL_ERR_CONNECT      (-12) /**< transport connect failed   */
#define NCL_ERR_CLOSED       (-13) /**< object already closed      */

/* Domain validation errors, one per NC-Link validity rule. */
#define NCL_ERR_INVALID_CODE        (-100) /**< InvalidCodeException        */
#define NCL_ERR_INVALID_DATA_NAME   (-101) /**< InvalidDataNameException    */
#define NCL_ERR_INVALID_DATA_TYPE   (-102) /**< InvalidDataTypException     */
#define NCL_ERR_INVALID_DEVICE_ID   (-103) /**< InvalidDeviceIdException    */
#define NCL_ERR_INVALID_ENCODING    (-104) /**< InvalidEncodingException    */
#define NCL_ERR_INVALID_ID          (-105) /**< InvalidIdException          */
#define NCL_ERR_INVALID_INDEX_RANGE (-106) /**< InvalidIndexRangeException  */
#define NCL_ERR_INVALID_ITEM        (-107) /**< InvalidItemException        */
#define NCL_ERR_INVALID_KEY         (-108) /**< InvalidKeyException         */
#define NCL_ERR_INVALID_MESSAGE     (-109) /**< InvalidMessageException     */
#define NCL_ERR_INVALID_MESSAGE_ID  (-110) /**< InvalidMessageIdException   */
#define NCL_ERR_INVALID_MODEL       (-111) /**< InvalidModelException       */
#define NCL_ERR_INVALID_NODE        (-112) /**< InvalidNodeException        */
#define NCL_ERR_INVALID_NUMBER      (-113) /**< InvalidNumberException      */
#define NCL_ERR_INVALID_REQUEST     (-114) /**< InvalidRequestException     */
#define NCL_ERR_INVALID_TYPE        (-115) /**< InvalidTypeException        */
#define NCL_ERR_INVALID_VALUE       (-116) /**< InvalidValueException       */
#define NCL_ERR_INVALID_VERSION     (-117) /**< InvalidVersionException     */

/**
 * Stable, human readable name of an error code. The names are part of the
 * public API: log lines and REST failure messages quote them verbatim.
 */
const char *ncl_err_name(ncl_err err);

/* ------------------------------------------------------------- allocator -- */

/*
 * Every byte the library allocates goes through these four functions, which
 * is what makes a build without a heap possible:
 *
 *   default            the C runtime allocator (malloc/free/realloc/calloc)
 *   NCL_STATIC_MEM=1   one fixed pool carved out of a static array
 *
 * Pointers handed back through the public API (char * from ncl_strdup(), the
 * objects behind ncl_*_free()) are always owned by the *library* allocator, so
 * an application that shares them must release them through the matching
 * ncl_*_free() call and never through the C runtime's free() - in the static
 * pool build those two are different heaps.
 */

/** Allocate @p size bytes, or NULL when the allocator is exhausted. */
void *ncl_mem_alloc(size_t size);

/**
 * Alignment of a block in the static pool: every pointer the pool hands out is
 * a multiple of this. The heap build forwards to the C runtime, so there the
 * guarantee is whatever that allocator gives (16 bytes on x64, 8 bytes for a
 * small block on Win32) - the same thing a caller compiled against malloc()
 * would already assume.
 */
#define NCL_MEM_ALIGNMENT 16u

/** Allocate @p count * @p size zeroed bytes, or NULL (with overflow check). */
void *ncl_mem_calloc(size_t count, size_t size);

/** Resize @p ptr (NULL behaves as ncl_mem_alloc(), 0 as ncl_mem_free()). */
void *ncl_mem_realloc(void *ptr, size_t size);

/** Release @p ptr; NULL is a no-op. */
void ncl_mem_free(void *ptr);

/** Which allocator is compiled in: "heap" or "static-pool". */
const char *ncl_mem_mode(void);

/** Counters of the allocation layer; all zero in the "heap" build. */
typedef struct {
    size_t pool_bytes;      /**< size of the pool (0 in the "heap" build)   */
    size_t in_use_bytes;    /**< payload bytes currently handed out         */
    size_t peak_in_use_bytes; /**< high water mark since the last reset     */
    /**
     * What the pool has actually committed to live objects: payload plus the
     * 32 byte header of every arena block, and the whole block for a size class
     * object (a class block has no header). This is the number to size the pool
     * against - in_use_bytes alone flatters the arena, because it leaves the
     * headers out.
     */
    size_t footprint_bytes;
    size_t peak_footprint_bytes; /**< high water mark of footprint_bytes     */
    size_t live_blocks;     /**< blocks currently handed out                */
    size_t largest_free_bytes; /**< largest single free block               */
    size_t free_bytes;      /**< total free space, holes included            */
    size_t free_blocks;     /**< number of free blocks (1 = no holes)        */
    /**
     * Bytes spent on block headers. This is the price of an allocator that can
     * release individual objects: 32 bytes per live (or free) block. In a small
     * pool with many small objects it is a real part of the budget, so it is
     * reported instead of being hidden.
     */
    size_t meta_bytes;
    /**
     * Small object size classes, index 0 upwards while class_size[i] is
     * non-zero. A class block is fixed size and carries no header, so its
     * region serves requests up to class_size[i] in O(1) and cannot fragment.
     * class_live/class_free say how well the region is sized for the traffic
     * that just ran (a class that is always empty was over-provisioned, one
     * that is never free was under-provisioned - it falls back to the arena,
     * which costs speed, never correctness).
     */
    size_t class_size[8];   /**< block size per class, 0 = class unused   */
    size_t class_bytes[8];  /**< region size per class                    */
    size_t class_live[8];   /**< blocks in use per class                  */
    size_t class_free[8];   /**< blocks on the class free list            */
    /**
     * Size of the general arena inside the pool: pool_bytes minus the size
     * class regions. largest_free_bytes and free_blocks describe this part,
     * because a size class has no holes to speak of.
     */
    size_t arena_bytes;
    /**
     * Request size histogram: bucket i counts requests of (2^i, 2^(i+1)] bytes,
     * bucket 0 everything up to 2 bytes, bucket 15 everything above 64 KiB.
     * This is what tells an integrator whether a pool of size classes would
     * pay off, or whether one arena is the right shape.
     */
    size_t size_hist[16];
    size_t allocations;     /**< successful allocations                     */
    size_t failures;        /**< refused allocations (pool exhausted)       */
    size_t foreign_frees;   /**< free() of a pointer the pool never handed out */
    /**
     * Biggest single request ever made. A pool has to be at least this plus
     * the long lived set, no matter how well it coalesces - that is the floor
     * below which no fragmentation policy can help.
     */
    size_t largest_request_bytes;
    /**
     * Snapshot taken when the last allocation was refused: how much was free
     * in total, and how big the largest contiguous piece was. When the first
     * number is much larger than the second, the refusal was fragmentation,
     * not a pool that is genuinely too small.
     */
    size_t failure_free_bytes;
    size_t failure_largest_free_bytes;
    /**
     * Block headers whose back link did not add up when a block was released.
     * Always zero in a healthy pool; a non-zero value means something wrote
     * past the end of an allocation.
     */
    size_t bad_links;
} ncl_mem_stats;

/** Snapshot the allocator counters (safe to call from any thread). */
void ncl_mem_get_stats(ncl_mem_stats *out);

/** Clear peak/allocations/failures counters, keeping in_use and pool_bytes. */
void ncl_mem_reset_stats(void);

/**
 * Walk the whole pool and verify its invariants: every block lies inside the
 * arena, the back links add up, no two free blocks are left unmerged, and the
 * blocks cover the arena exactly. Returns 0 when the pool is consistent, or
 * the number of problems found.
 *
 * A bring up aid: call it from a test after each phase, or from a field build
 * when something writes past its own allocation. Always 0 for the "heap" mode.
 */
size_t ncl_mem_check(void);

/* ------------------------------------------------------------ string utils -- */

/** Heap copy of @p s (NULL safe). Caller frees. */
char *ncl_strdup(const char *s);

/** Heap copy of the first @p len bytes of @p s (NUL terminated). */
char *ncl_strndup(const char *s, size_t len);

/**
 * printf into a freshly allocated buffer. *out is always set to a heap string
 * or NULL on failure. Returns NCL_OK / NCL_ERR_NOMEM.
 */
ncl_err ncl_asprintf(char **out, const char *fmt, ...);
ncl_err ncl_vasprintf(char **out, const char *fmt, va_list ap);

/** Case-insensitive ASCII equality. */
bool ncl_streq_ignore_case(const char *a, const char *b);

bool ncl_str_starts_with(const char *s, const char *prefix);
bool ncl_str_ends_with(const char *s, const char *suffix);

/** True when @p s is NULL or empty. */
bool ncl_str_is_empty(const char *s);

/** True when @p s is NULL, empty, or contains only whitespace. */
bool ncl_str_is_blank(const char *s);

/**
 * Duplicate @p s with leading and trailing ASCII whitespace removed.
 */
char *ncl_str_trim_dup(const char *s);

/** Free a heap pointer and set the variable to NULL. */
void ncl_free_safe(void *ptr);

/**
 * Format a random version 4 UUID into @p out in lower case (8-4-4-4-12).
 * @p out must hold at least 37 bytes. Returns NCL_OK / NCL_ERR.
 */
ncl_err ncl_uuid4(char *out, size_t out_len);

/* ------------------------------------------------------------- ncl_strbuf -- */

/** Growable byte/string buffer used by every writer in the library. */
typedef struct {
    char  *data; /**< always NUL terminated once non-NULL */
    size_t len;  /**< length excluding the terminator    */
    size_t cap;  /**< allocated capacity including room for NUL */
} ncl_strbuf;

void   ncl_strbuf_init(ncl_strbuf *sb);
void   ncl_strbuf_free(ncl_strbuf *sb);
void   ncl_strbuf_reset(ncl_strbuf *sb);
ncl_err ncl_strbuf_reserve(ncl_strbuf *sb, size_t additional);
ncl_err ncl_strbuf_append(ncl_strbuf *sb, const char *data, size_t len);
ncl_err ncl_strbuf_puts(ncl_strbuf *sb, const char *s);
ncl_err ncl_strbuf_putc(ncl_strbuf *sb, char c);
ncl_err ncl_strbuf_printf(ncl_strbuf *sb, const char *fmt, ...);

/** Detach the buffer contents; caller frees. Resets @p sb to empty. */
char *ncl_strbuf_detach(ncl_strbuf *sb);

/** NUL terminated view of the buffer (never NULL for an initialised buffer). */
const char *ncl_strbuf_cstr(ncl_strbuf *sb);

/* ---------------------------------------------------------------- ptrvec -- */

/**
 * Vector of owned pointers with an optional element destructor, used for the
 * list valued fields of the message and model objects.
 */
typedef void (*ncl_free_fn)(void *element);

typedef struct {
    void      **items;
    size_t      len;
    size_t      cap;
    ncl_free_fn free_fn; /**< may be NULL when elements are not owned */
} ncl_ptrvec;

void   ncl_ptrvec_init(ncl_ptrvec *v, ncl_free_fn free_fn);
void   ncl_ptrvec_free(ncl_ptrvec *v);
void   ncl_ptrvec_clear(ncl_ptrvec *v);
ncl_err ncl_ptrvec_push(ncl_ptrvec *v, void *item);
ncl_err ncl_ptrvec_push_owned(ncl_ptrvec *v, void *item);
void  *ncl_ptrvec_at(const ncl_ptrvec *v, size_t index);
size_t ncl_ptrvec_len(const ncl_ptrvec *v);
/** Detach ownership of element @p index; slot is removed. */
void  *ncl_ptrvec_take(ncl_ptrvec *v, size_t index);

/* --------------------------------------------------------------- strvec -- */

/** Vector of owned C strings. */
typedef struct {
    char  **items;
    size_t  len;
    size_t  cap;
} ncl_strvec;

void    ncl_strvec_init(ncl_strvec *v);
void    ncl_strvec_free(ncl_strvec *v);
void    ncl_strvec_clear(ncl_strvec *v);
/** Copies @p s; returns NCL_OK / NCL_ERR_NOMEM. */
ncl_err ncl_strvec_push(ncl_strvec *v, const char *s);
const char *ncl_strvec_at(const ncl_strvec *v, size_t index);
size_t  ncl_strvec_len(const ncl_strvec *v);
bool    ncl_strvec_contains(const ncl_strvec *v, const char *s);

#ifdef __cplusplus
}
#endif

#endif /* NCL_COMMON_H */
