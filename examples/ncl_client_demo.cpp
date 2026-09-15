// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

// C++17 client example: connect, probe the model, read and write a value.
//
//   ./ncl_client_demo_cpp tcp://127.0.0.1:1883 V2023A7B762
//
#include <cstdio>
#include <exception>
#include <string>

#include "nclink/ncl.hpp"

int main(int argc, char **argv) {
    const std::string uri = argc > 1 ? argv[1] : "tcp://127.0.0.1:1883";
    const std::string sn = argc > 2 ? argv[2] : "V203243111F";

    try {
        ncl::Client::init(uri);
        std::printf("connected: %s (nclink %s)\n", uri.c_str(),
                    ncl::version().c_str());

        ncl::Client client(sn);
        ncl::Model model = client.probe();
        std::printf("probe: model root id=%s name=%s\n",
                    model.get()->id != nullptr ? model.get()->id : "?",
                    model.get()->name != nullptr ? model.get()->name : "?");

        ncl::Json status = client.value("/STATUS");
        std::printf("GET /STATUS = %s\n", status.dump().c_str());

        client.set("/STATUS", ncl::Json::parse("42"));
        std::printf("SET /STATUS = 42 ok\n");

        ncl::Client::shutdown();
    } catch (const ncl::Error &e) {
        std::printf("NC-Link error: %s\n", e.what());
        return 1;
    } catch (const std::exception &e) {
        std::printf("error: %s\n", e.what());
        return 1;
    }
    return 0;
}
