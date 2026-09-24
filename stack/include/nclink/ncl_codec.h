/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - payload codecs.
 *
 * Two independent codec families:
 *   hex      - two upper case characters per byte, the textual form NC-Link
 *              used for binary payloads such as images.
 *   compress - zlib (deflate) container, used for bulk payloads.
 *
 * The compress codec needs zlib. Configure with -DNCLINK_WITH_ZLIB=ON to enable
 * it; otherwise those calls report NCL_ERR_NOT_SUPPORTED.
 */
#ifndef NCL_CODEC_H
#define NCL_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "nclink/ncl_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Owning byte buffer returned by the codecs. */
typedef struct {
    unsigned char *data;
    size_t         len;
} ncl_buffer;

void ncl_buffer_free(ncl_buffer *buf);

/* ---------------------------------------------------------- hex codec ----- */

/**
 * Hex encode: two upper case characters per input byte.
 */
ncl_err ncl_codec_encode_hex(const unsigned char *src, size_t src_len,
                             ncl_buffer *out);

/**
 * Hex decode. Accepts the characters '0'-'9' and 'A'-'F' only; anything else
 * yields NCL_ERR_INVALID_VALUE. An odd number of input bytes yields
 * NCL_ERR_INVALID_REQUEST rather than silently dropping the final nibble.
 */
ncl_err ncl_codec_decode_hex(const unsigned char *src, size_t src_len,
                             ncl_buffer *out);

/* ------------------------------------------------------- compress codec --- */

/** Deflate (zlib container) @p src into @p out. */
ncl_err ncl_codec_encode_compress(const unsigned char *src, size_t src_len,
                                  ncl_buffer *out);

/** Inflate (zlib container) @p src into @p out. */
ncl_err ncl_codec_decode_compress(const unsigned char *src, size_t src_len,
                                  ncl_buffer *out);

/** True when the compress codec was built in (-DNCLINK_WITH_ZLIB=ON). */
bool ncl_codec_compress_available(void);

/* ------------------------------------------------------ dispatch helpers -- */

typedef enum {
    NCL_CODEC_DEFAULT = 0, /**< hex codec */
    NCL_CODEC_COMPRESS     /**< zlib deflate/inflate */
} ncl_codec_kind;

/** Encode with the codec named by @p kind. */
ncl_err ncl_codec_encode(ncl_codec_kind kind, const unsigned char *src,
                         size_t src_len, ncl_buffer *out);

/** Decode with the codec named by @p kind. */
ncl_err ncl_codec_decode(ncl_codec_kind kind, const unsigned char *src,
                         size_t src_len, ncl_buffer *out);

#ifdef __cplusplus
}
#endif

#endif /* NCL_CODEC_H */
