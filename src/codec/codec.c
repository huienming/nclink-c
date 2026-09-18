/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* NC-Link core - hex and (optional) zlib payload codecs. */
#include "nclink/ncl_codec.h"

#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_logger.h"

#if defined(NCL_HAVE_ZLIB)
#  include <zlib.h>
#endif

void ncl_buffer_free(ncl_buffer *buf)
{
    if (buf == NULL) {
        return;
    }
    ncl_mem_free(buf->data);
    buf->data = NULL;
    buf->len = 0;
}

static ncl_err ncl_buffer_alloc(ncl_buffer *out, size_t len)
{
    if (out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    out->data = NULL;
    out->len = 0;
    if (len == 0) {
        return NCL_OK;
    }
    out->data = (unsigned char *)ncl_mem_alloc(len);
    if (out->data == NULL) {
        return NCL_ERR_NOMEM;
    }
    out->len = len;
    return NCL_OK;
}

/* --------------------------------------------------------------- hex ------ */

/* Upper case "00".."FF". */
static const char ncl_hex_digits[] = "0123456789ABCDEF";

ncl_err ncl_codec_encode_hex(const unsigned char *src, size_t src_len,
                             ncl_buffer *out)
{
    size_t i;
    ncl_err rc;

    if (src == NULL && src_len > 0) {
        return NCL_ERR_INVALID_ARG;
    }
    if (src_len > ((size_t)-1) / 2) {
        return NCL_ERR_NOMEM;
    }
    rc = ncl_buffer_alloc(out, src_len * 2);
    if (rc != NCL_OK) {
        return rc;
    }
    for (i = 0; i < src_len; i++) {
        out->data[2 * i] = (unsigned char)ncl_hex_digits[(src[i] >> 4) & 0x0F];
        out->data[2 * i + 1] = (unsigned char)ncl_hex_digits[src[i] & 0x0F];
    }
    return NCL_OK;
}

static int ncl_hex_value(unsigned char c, bool *ok)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    *ok = false;
    return 0;
}

ncl_err ncl_codec_decode_hex(const unsigned char *src, size_t src_len,
                             ncl_buffer *out)
{
    size_t i;
    ncl_err rc;

    if (src == NULL && src_len > 0) {
        return NCL_ERR_INVALID_ARG;
    }
    if (src_len == 0) {
        return ncl_buffer_alloc(out, 0);
    }
    if (src_len % 2 != 0) {
        /* An odd byte count is refused rather than silently dropping a nibble. */
        return NCL_ERR_INVALID_REQUEST;
    }

    rc = ncl_buffer_alloc(out, src_len / 2);
    if (rc != NCL_OK) {
        return rc;
    }
    for (i = 0; i < src_len / 2; i++) {
        bool ok = true;
        int hi = ncl_hex_value(src[2 * i], &ok);
        int lo = ok ? ncl_hex_value(src[2 * i + 1], &ok) : 0;
        if (!ok) {
            ncl_buffer_free(out);
            return NCL_ERR_INVALID_VALUE;
        }
        out->data[i] = (unsigned char)((hi << 4) | lo);
    }
    return NCL_OK;
}

/* ------------------------------------------------------------ compress ---- */

#if defined(NCL_HAVE_ZLIB)

/**
 * Run a zlib stream to completion, growing the output buffer as needed.
 * The input is pumped through in 1 KiB blocks.
 */
static ncl_err ncl_zlib_run(const unsigned char *src, size_t src_len,
                            ncl_buffer *out, bool compress)
{
    z_stream stream;
    size_t capacity;
    size_t produced = 0;
    unsigned char *buffer;
    ncl_err rc = NCL_OK;

    if (out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    out->data = NULL;
    out->len = 0;
    if (src_len > 0xFFFFFFFFu) {
        return NCL_ERR_INVALID_ARG; /* zlib takes a 32 bit input length */
    }

    capacity = src_len > 128 ? src_len * 2 : 256;
    buffer = (unsigned char *)ncl_mem_alloc(capacity);
    if (buffer == NULL) {
        return NCL_ERR_NOMEM;
    }

    memset(&stream, 0, sizeof(stream));
    if (compress ? deflateInit(&stream, Z_DEFAULT_COMPRESSION) != Z_OK
                 : inflateInit(&stream) != Z_OK) {
        ncl_mem_free(buffer);
        return NCL_ERR;
    }

    stream.next_in = (Bytef *)src;
    stream.avail_in = (uInt)src_len;

    for (;;) {
        int status;

        if (produced == capacity) {
            size_t next = capacity * 2;
            unsigned char *grown;
            if (next <= capacity) {
                rc = NCL_ERR_NOMEM;
                break;
            }
            grown = (unsigned char *)ncl_mem_realloc(buffer, next);
            if (grown == NULL) {
                rc = NCL_ERR_NOMEM;
                break;
            }
            buffer = grown;
            capacity = next;
        }

        stream.next_out = buffer + produced;
        stream.avail_out = (uInt)(capacity - produced);
        status = compress ? deflate(&stream, Z_FINISH) : inflate(&stream, Z_NO_FLUSH);
        produced = capacity - (size_t)stream.avail_out;

        if (status == Z_STREAM_END) {
            break;
        }
        if (status == Z_OK || status == Z_BUF_ERROR) {
            /* Z_BUF_ERROR with no input left means the stream is truncated. */
            if (status == Z_BUF_ERROR && stream.avail_in == 0 &&
                produced != capacity) {
                rc = compress ? NCL_ERR : NCL_ERR_PARSE;
                break;
            }
            if (produced != capacity) {
                /* Made progress but not finished; keep pumping. */
                continue;
            }
            continue; /* buffer full: the loop head grows it */
        }
        rc = compress ? NCL_ERR : NCL_ERR_PARSE;
        break;
    }

    if (compress) {
        deflateEnd(&stream);
    } else {
        inflateEnd(&stream);
    }

    if (rc == NCL_OK) {
        out->data = buffer;
        out->len = produced;
    } else {
        ncl_mem_free(buffer);
    }
    return rc;
}

ncl_err ncl_codec_encode_compress(const unsigned char *src, size_t src_len,
                                  ncl_buffer *out)
{
    if (src == NULL && src_len > 0) {
        return NCL_ERR_INVALID_ARG;
    }
    return ncl_zlib_run(src, src_len, out, true);
}

ncl_err ncl_codec_decode_compress(const unsigned char *src, size_t src_len,
                                  ncl_buffer *out)
{
    if (src == NULL && src_len > 0) {
        return NCL_ERR_INVALID_ARG;
    }
    return ncl_zlib_run(src, src_len, out, false);
}

bool ncl_codec_compress_available(void)
{
    return true;
}

#else /* !NCL_HAVE_ZLIB */

ncl_err ncl_codec_encode_compress(const unsigned char *src, size_t src_len,
                                  ncl_buffer *out)
{
    (void)src;
    (void)src_len;
    (void)out;
    return NCL_ERR_NOT_SUPPORTED;
}

ncl_err ncl_codec_decode_compress(const unsigned char *src, size_t src_len,
                                  ncl_buffer *out)
{
    (void)src;
    (void)src_len;
    (void)out;
    return NCL_ERR_NOT_SUPPORTED;
}

bool ncl_codec_compress_available(void)
{
    return false;
}

#endif /* NCL_HAVE_ZLIB */

/* ------------------------------------------------------------- dispatch --- */

ncl_err ncl_codec_encode(ncl_codec_kind kind, const unsigned char *src,
                         size_t src_len, ncl_buffer *out)
{
    switch (kind) {
    case NCL_CODEC_DEFAULT:
        return ncl_codec_encode_hex(src, src_len, out);
    case NCL_CODEC_COMPRESS:
        return ncl_codec_encode_compress(src, src_len, out);
    default:
        return NCL_ERR_NOT_SUPPORTED;
    }
}

ncl_err ncl_codec_decode(ncl_codec_kind kind, const unsigned char *src,
                         size_t src_len, ncl_buffer *out)
{
    switch (kind) {
    case NCL_CODEC_DEFAULT:
        return ncl_codec_decode_hex(src, src_len, out);
    case NCL_CODEC_COMPRESS:
        return ncl_codec_decode_compress(src, src_len, out);
    default:
        return NCL_ERR_NOT_SUPPORTED;
    }
}
