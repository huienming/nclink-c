// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

// C++17 device example: build a server, register a tool method, load a model
// and dispatch one inbound message. The C example shows the same flow with the
// plain C API (examples/ncl_device_demo.c).
//
#include <cstdio>
#include <cstring>

#include "nclink/ncl.hpp"

static ncl_err read_status(void *instance, const ncl_json *params,
                           ncl_json **result, char **reason) {
    (void)instance;
    (void)params;
    (void)reason;
    *result = ncl_json_new_int(42);
    return NCL_OK;
}

int main() {
    ncl_server_options options{};  // zero initialised, then filled in below
    const ncl_tool_method methods[] = {
        {"read", read_status, nullptr},
    };
    const ncl_tool_binding bindings[] = {
        {"/STATUS", NCL_OP_GET_VALUE, "read", "demoTool"},
    };

    options.sn = "V2023CPPDEMO";

    try {
        ncl::Server server(ncl_server_create(&options));
        server.load_model("");
        ncl_server_register_tool(server.get(), "demoTool", nullptr, methods, 1,
                                 bindings, 1);
        std::printf("device ready: model + tool registered\n");
    } catch (const ncl::Error &e) {
        std::printf("NC-Link error: %s\n", e.what());
        return 1;
    }
    return 0;
}
