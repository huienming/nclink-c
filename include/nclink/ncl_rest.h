/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - REST plumbing for the HTTP interface.
 *
 * Every handler answer is wrapped in the same envelope:
 *
 *     {"status": true,  "data": ...}     success
 *     {"status": false, "data": "..."}   failure, "data" carries the message
 *
 * "data" is omitted when there is nothing to report. This module provides that
 * envelope plus the endpoints that can be generated from the NC-Link server
 * state (the OpenAPI document and a minimal Swagger UI page).
 *
 * The concrete configuration handlers (the cfg / method / edgeUrl groups) are
 * registered by the application through ncl_http_server_route() using the
 * helpers below.
 */
#ifndef NCL_REST_H
#define NCL_REST_H

#include "nclink/ncl_common.h"
#include "nclink/ncl_http.h"
#include "nclink/ncl_json.h"
#include "nclink/ncl_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Success answer: takes ownership of @p data (may be NULL). */
ncl_json *ncl_result_success(ncl_json *data);

/** Failure answer carrying @p message as "data". */
ncl_json *ncl_result_failed(const char *message);

/** Success answer carrying a boolean. */
ncl_json *ncl_result_success_bool(bool value);

/** Success answer carrying a string. */
ncl_json *ncl_result_success_string(const char *value);

/** Reply with a Result envelope in one call. */
void ncl_rest_reply(ncl_http_response *response, int status, ncl_json *data);
void ncl_rest_reply_error(ncl_http_response *response, int status,
                          const char *message);

/**
 * Register the endpoints that are derived from the NC-Link server state:
 *   GET  /api/schema      OpenAPI 3.0 document describing the methods
 *   GET  /swagger-ui      a small page that renders the document
 * @p base_url fills servers[0].url; pass NULL to use "/api".
 */
ncl_err ncl_rest_attach(ncl_http_server *http, ncl_server *server);

/**
 * Register the device configuration endpoints:
 *
 *   GET  /api/cfg/getSn          GET  /api/cfg/getModel
 *   GET  /api/cfg/getDriver      GET  /api/getMqttUrl
 *   GET  /api/method/getServerList
 *   POST /api/cfg/init           POST /api/cfg/setModel
 *   POST /api/cfg/setDriver      POST /api/setMqttUrl
 *   POST /api/method/setServer
 *
 * POST bodies carry the argument of the corresponding call: a serial number, a
 * model/driver/server document, or the {"url","username","password"} object for
 * setMqttUrl. Answers use the envelope above.
 */
ncl_err ncl_rest_attach_config(ncl_http_server *http);

#ifdef __cplusplus
}
#endif

#endif /* NCL_REST_H */
