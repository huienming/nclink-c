/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - text file helpers (see adapter_text.h).
 */

#include "core/adapter_text.h"

#include <string.h>

#include "nclink/ncl_env.h"

/** Byte order mark, as it appears in a UTF-8 file. */
static const unsigned char kBom[] = {0xEF, 0xBB, 0xBF};

ncl_json *ncl_adapter_json_from_file(const char *path, ncl_strbuf *err)
{
    char *text = NULL;
    const char *body;
    ncl_json *document;
    ncl_err result;

    if (ncl_str_is_blank(path)) {
        return NULL;
    }
    result = ncl_file_read_all(path, &text, NULL);
    if (result != NCL_OK) {
        if (err != NULL) {
            (void)ncl_strbuf_printf(err, "cannot read \"%s\"", path);
        }
        return NULL;
    }
    body = text;
    if (memcmp(body, kBom, sizeof(kBom)) == 0) {
        body += sizeof(kBom);
    }
    document = ncl_json_parse_cstr(body, err);
    if (document == NULL && err != NULL && err->len == 0) {
        (void)ncl_strbuf_printf(err, "\"%s\" is not valid JSON", path);
    }
    ncl_free_safe(text);
    return document;
}
