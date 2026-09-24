// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

// C++ wrapper test: RAII, moves, deep copies and exception mapping.
#include <cstdio>
#include <string>
#include <utility>

#include "nclink/ncl.hpp"

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("    FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);    \
            failures++;                                                      \
        }                                                                    \
    } while (0)

int main() {
    std::printf("running %s\n", __FILE__);

    ncl::Json json = ncl::Json::parse("{\"a\":1}");
    CHECK(json.valid());
    CHECK(json.dump() == "{\"a\":1}");

    ncl::Json copy = json;  // deep copy
    CHECK(copy.dump() == json.dump());
    ncl::Json moved = std::move(copy);
    CHECK(!copy.valid());
    CHECK(moved.dump() == json.dump());

    bool threw = false;
    try {
        (void)ncl::Json::parse("{oops");
    } catch (const ncl::Error &e) {
        threw = true;
        CHECK(e.code() == NCL_ERR_PARSE);
        CHECK(std::string(e.what()).size() > 0);
    }
    CHECK(threw);

    ncl::Message message = ncl::Message::make(NCL_MSG_PING);
    // finalise() validates first, and a Ping is only valid with an id.
    ncl_message_set_message_id(message.get(), "cpp-1");
    message.finalise();
    CHECK(message.valid());
    CHECK(message.dump().find("\"@id\"") != std::string::npos);
    threw = false;
    try {
        (void)ncl::Message::parse("Query/Request/V1", "not a message");
    } catch (const ncl::Error &e) {
        threw = true;
        CHECK(e.code() == NCL_ERR_PARSE);
    }
    CHECK(threw);

    ncl::Model model = ncl::Model::parse("");  // built in default model
    CHECK(model.valid());
    // find() is a depth first lookup *below* the node, so the root's own id
    // ("01") is not a hit: look the built in PLC device up instead.
    CHECK(model.find("01") == nullptr);
    CHECK(model.find("02") != nullptr);
    CHECK(model.dump().find("nclink") != std::string::npos);

    std::printf("%s: %d failures\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
