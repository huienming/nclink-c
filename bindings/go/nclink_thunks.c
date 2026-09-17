// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

/*
 * C trampolines for the Go binding.
 *
 * Two things can only be done on the C side: the C API identifies a tool
 * method by its function pointer (ncl_tool_fn receives the instance, not the
 * method name), so the device side needs one trampoline per method; and a Go
 * function value reaches C as a void*, so the cast to the callback type has to
 * happen here.
 *
 * They live in a .c file rather than in a cgo preamble because a file that
 * uses //export may only carry declarations there.
 */

#include <stddef.h>
#include <stdlib.h>

#include "nclink/ncl_http.h"
#include "nclink/ncl_mqtt.h"
#include "nclink/ncl_server.h"

/* Exported by server.go. */
extern ncl_err nclinkGoToolThunk(int index, void *instance, ncl_json *params,
                                 ncl_json **result, char **reason);
extern void nclinkGoRouteThunk(int index, ncl_http_request *request,
                               ncl_http_response *response, void *user);
extern ncl_err nclinkGoPublishThunk(void *user, char *topic, char *payload,
                                    size_t length);
extern void nclinkGoMessageThunk(void *user, ncl_mqtt_publish *publish);

#define NCLINK_TOOL_THUNKS 32
#define NCLINK_ROUTE_THUNKS 32

#define NCLINK_DEFINE_TOOL_THUNK(i)                                           \
    static ncl_err nclink_tool_thunk_##i(void *instance,                      \
                                         const ncl_json *params,              \
                                         ncl_json **result, char **reason)    \
    {                                                                         \
        return nclinkGoToolThunk(i, instance, (ncl_json *)params, result,     \
                                 reason);                                     \
    }

NCLINK_DEFINE_TOOL_THUNK(0)  NCLINK_DEFINE_TOOL_THUNK(1)
NCLINK_DEFINE_TOOL_THUNK(2)  NCLINK_DEFINE_TOOL_THUNK(3)
NCLINK_DEFINE_TOOL_THUNK(4)  NCLINK_DEFINE_TOOL_THUNK(5)
NCLINK_DEFINE_TOOL_THUNK(6)  NCLINK_DEFINE_TOOL_THUNK(7)
NCLINK_DEFINE_TOOL_THUNK(8)  NCLINK_DEFINE_TOOL_THUNK(9)
NCLINK_DEFINE_TOOL_THUNK(10) NCLINK_DEFINE_TOOL_THUNK(11)
NCLINK_DEFINE_TOOL_THUNK(12) NCLINK_DEFINE_TOOL_THUNK(13)
NCLINK_DEFINE_TOOL_THUNK(14) NCLINK_DEFINE_TOOL_THUNK(15)
NCLINK_DEFINE_TOOL_THUNK(16) NCLINK_DEFINE_TOOL_THUNK(17)
NCLINK_DEFINE_TOOL_THUNK(18) NCLINK_DEFINE_TOOL_THUNK(19)
NCLINK_DEFINE_TOOL_THUNK(20) NCLINK_DEFINE_TOOL_THUNK(21)
NCLINK_DEFINE_TOOL_THUNK(22) NCLINK_DEFINE_TOOL_THUNK(23)
NCLINK_DEFINE_TOOL_THUNK(24) NCLINK_DEFINE_TOOL_THUNK(25)
NCLINK_DEFINE_TOOL_THUNK(26) NCLINK_DEFINE_TOOL_THUNK(27)
NCLINK_DEFINE_TOOL_THUNK(28) NCLINK_DEFINE_TOOL_THUNK(29)
NCLINK_DEFINE_TOOL_THUNK(30) NCLINK_DEFINE_TOOL_THUNK(31)

#define NCLINK_DEFINE_ROUTE_THUNK(i)                                          \
    static void nclink_route_thunk_##i(ncl_http_request *request,             \
                                       ncl_http_response *response,           \
                                       void *user)                            \
    {                                                                         \
        nclinkGoRouteThunk(i, request, response, user);                       \
    }

NCLINK_DEFINE_ROUTE_THUNK(0)  NCLINK_DEFINE_ROUTE_THUNK(1)
NCLINK_DEFINE_ROUTE_THUNK(2)  NCLINK_DEFINE_ROUTE_THUNK(3)
NCLINK_DEFINE_ROUTE_THUNK(4)  NCLINK_DEFINE_ROUTE_THUNK(5)
NCLINK_DEFINE_ROUTE_THUNK(6)  NCLINK_DEFINE_ROUTE_THUNK(7)
NCLINK_DEFINE_ROUTE_THUNK(8)  NCLINK_DEFINE_ROUTE_THUNK(9)
NCLINK_DEFINE_ROUTE_THUNK(10) NCLINK_DEFINE_ROUTE_THUNK(11)
NCLINK_DEFINE_ROUTE_THUNK(12) NCLINK_DEFINE_ROUTE_THUNK(13)
NCLINK_DEFINE_ROUTE_THUNK(14) NCLINK_DEFINE_ROUTE_THUNK(15)
NCLINK_DEFINE_ROUTE_THUNK(16) NCLINK_DEFINE_ROUTE_THUNK(17)
NCLINK_DEFINE_ROUTE_THUNK(18) NCLINK_DEFINE_ROUTE_THUNK(19)
NCLINK_DEFINE_ROUTE_THUNK(20) NCLINK_DEFINE_ROUTE_THUNK(21)
NCLINK_DEFINE_ROUTE_THUNK(22) NCLINK_DEFINE_ROUTE_THUNK(23)
NCLINK_DEFINE_ROUTE_THUNK(24) NCLINK_DEFINE_ROUTE_THUNK(25)
NCLINK_DEFINE_ROUTE_THUNK(26) NCLINK_DEFINE_ROUTE_THUNK(27)
NCLINK_DEFINE_ROUTE_THUNK(28) NCLINK_DEFINE_ROUTE_THUNK(29)
NCLINK_DEFINE_ROUTE_THUNK(30) NCLINK_DEFINE_ROUTE_THUNK(31)

static ncl_tool_fn const nclink_tool_thunks[NCLINK_TOOL_THUNKS] = {
    nclink_tool_thunk_0,  nclink_tool_thunk_1,  nclink_tool_thunk_2,
    nclink_tool_thunk_3,  nclink_tool_thunk_4,  nclink_tool_thunk_5,
    nclink_tool_thunk_6,  nclink_tool_thunk_7,  nclink_tool_thunk_8,
    nclink_tool_thunk_9,  nclink_tool_thunk_10, nclink_tool_thunk_11,
    nclink_tool_thunk_12, nclink_tool_thunk_13, nclink_tool_thunk_14,
    nclink_tool_thunk_15, nclink_tool_thunk_16, nclink_tool_thunk_17,
    nclink_tool_thunk_18, nclink_tool_thunk_19, nclink_tool_thunk_20,
    nclink_tool_thunk_21, nclink_tool_thunk_22, nclink_tool_thunk_23,
    nclink_tool_thunk_24, nclink_tool_thunk_25, nclink_tool_thunk_26,
    nclink_tool_thunk_27, nclink_tool_thunk_28, nclink_tool_thunk_29,
    nclink_tool_thunk_30, nclink_tool_thunk_31
};

static ncl_http_handler const nclink_route_thunks[NCLINK_ROUTE_THUNKS] = {
    nclink_route_thunk_0,  nclink_route_thunk_1,  nclink_route_thunk_2,
    nclink_route_thunk_3,  nclink_route_thunk_4,  nclink_route_thunk_5,
    nclink_route_thunk_6,  nclink_route_thunk_7,  nclink_route_thunk_8,
    nclink_route_thunk_9,  nclink_route_thunk_10, nclink_route_thunk_11,
    nclink_route_thunk_12, nclink_route_thunk_13, nclink_route_thunk_14,
    nclink_route_thunk_15, nclink_route_thunk_16, nclink_route_thunk_17,
    nclink_route_thunk_18, nclink_route_thunk_19, nclink_route_thunk_20,
    nclink_route_thunk_21, nclink_route_thunk_22, nclink_route_thunk_23,
    nclink_route_thunk_24, nclink_route_thunk_25, nclink_route_thunk_26,
    nclink_route_thunk_27, nclink_route_thunk_28, nclink_route_thunk_29,
    nclink_route_thunk_30, nclink_route_thunk_31
};

/* Slot lookup used by RegisterTool / HTTPEndpoint.Route: the callback pointer
 * is what the C API stores, so every method or route needs its own slot. */
ncl_tool_fn nclink_tool_thunk_at(int index)
{
    if (index < 0 || index >= NCLINK_TOOL_THUNKS) {
        return NULL;
    }
    return nclink_tool_thunks[index];
}

ncl_http_handler nclink_route_thunk_at(int index)
{
    if (index < 0 || index >= NCLINK_ROUTE_THUNKS) {
        return NULL;
    }
    return nclink_route_thunks[index];
}

/* Casting a Go function value (a void*) to the callback type. */
void nclink_mqtt_options_set_on_message(ncl_mqtt_client_options *options,
                                        void *fn, void *user)
{
    options->on_message = (ncl_mqtt_message_fn)fn;
    options->user = user;
}

void nclink_server_options_set_publish(ncl_server_options *options, void *fn,
                                       void *user)
{
    options->publish = (ncl_server_publish_fn)fn;
    options->publish_user = user;
}

/*
 * The option structs carry the callback user data, i.e. a pointer to a Go
 * handle box. cgo refuses to hand C a Go pointer that points at Go memory
 * holding Go pointers, so the struct itself must live in C memory: these
 * allocate it (and fill the defaults in) on that side.
 */
ncl_mqtt_client_options *nclink_mqtt_options_new(void)
{
    ncl_mqtt_client_options *options =
        (ncl_mqtt_client_options *)calloc(1, sizeof(*options));

    if (options != NULL) {
        ncl_mqtt_client_options_default(options);
    }
    return options;
}

ncl_server_options *nclink_server_options_new(void)
{
    return (ncl_server_options *)calloc(1, sizeof(ncl_server_options));
}

void nclink_options_free(void *options)
{
    free(options);
}
