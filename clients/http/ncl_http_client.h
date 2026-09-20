/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the HTTP GET client the drivers that speak HTTP share.
 *
 * Two of them do: MTConnect's agent (XML documents) and KND's REST interface
 * (JSON objects). Both want the same thing - one GET, take the body - so the
 * client lives on its own rather than inside either driver. It is deliberately
 * small: one request per call, no keep-alive, no POST, and the three body
 * shapes a device may use (Content-Length, chunked, or read until close).
 */
#ifndef NCL_HTTP_CLIENT_H
#define NCL_HTTP_CLIENT_H

#include <stddef.h>

#include "nclink/ncl_common.h"
#include "nclink_adapter/ncl_driver.h" /* the tiered transport/protocol codes */

#ifdef __cplusplus
extern "C" {
#endif

/** One GET. All strings are borrowed and must outlive the call. */
typedef struct {
    const char *host;
    unsigned    port;     /**< 80 when 0                                    */
    const char *path;     /**< "/status", "/KEDE/CNC/GNC62/STATUS", ...     */
    const char *user;     /**< optional, for Basic authentication           */
    const char *password;
    unsigned    timeout_ms; /**< per socket operation (default 3000)        */
    size_t      max_body;   /**< refuse a body larger than this             */
} ncl_http_request;

/**
 * GET @p request->path and hand back the response body.
 * *body is a heap buffer (NUL terminated, so it can be scanned as text);
 * the caller releases it with ncl_free_safe().
 */
ncl_err ncl_http_get(const ncl_http_request *request, char **body,
                     size_t *body_len, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif /* NCL_HTTP_CLIENT_H */
