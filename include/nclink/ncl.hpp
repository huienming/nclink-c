/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - C++17 wrapper (header only).
 *
 * The C library stays the engine: this layer adds RAII, exceptions and the
 * usual C++ containers on top of exactly the same protocol code.
 *
 *   ncl::Json     - owning JSON value (parse / dump / typed access)
 *   ncl::Message  - owning protocol message (parse / serialise / helpers)
 *   ncl::Model    - owning device model (parse / serialise / lookup)
 *   ncl::Client   - one device client, connected through ncl_client_holder
 *   ncl::Server   - device side server
 *
 * Errors: every ncl_err becomes an exception derived from ncl::Error
 * (ncl::Error already carries the numeric code and ncl_err_name() text).
 * C++17 is required; no other dependency.
 */
#ifndef NCLINK_NCL_HPP
#define NCLINK_NCL_HPP

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "nclink/ncl_client.h"
#include "nclink/ncl_common.h"
#include "nclink/ncl_json.h"
#include "nclink/ncl_message.h"
#include "nclink/ncl_model.h"
#include "nclink/ncl_server.h"

namespace ncl {

/* ------------------------------------------------------------------ error -- */

/** Any failure reported by the C layer. */
class Error : public std::runtime_error {
public:
    Error(ncl_err code, const std::string &what)
        : std::runtime_error(what), code_(code) {}

    /** The ncl_err value (NCL_OK never reaches the user). */
    ncl_err code() const noexcept { return code_; }

private:
    ncl_err code_;
};

namespace detail {

inline std::string err_text(ncl_err code) {
    const char *name = ncl_err_name(code);
    return std::string(name != nullptr ? name : "NCL_ERR") + " (" +
           std::to_string(static_cast<int>(code)) + ")";
}

inline void check(ncl_err rc) {
    if (rc != NCL_OK) {
        throw Error(rc, err_text(rc));
    }
}

inline void check(ncl_err rc, const char *what) {
    if (rc != NCL_OK) {
        throw Error(rc, std::string(what) + ": " + err_text(rc));
    }
}

}  // namespace detail

/* ------------------------------------------------------------------- json -- */

/** Owning JSON value; moves are cheap, copies are deep. */
class Json {
public:
    Json() = default;
    explicit Json(ncl_json *owned) : value_(owned) {}

    static Json parse(const std::string &text) {
        ncl_json *value = ncl_json_parse_cstr(text.c_str(), nullptr);
        if (value == nullptr) {
            throw Error(NCL_ERR_PARSE, "text is not valid JSON");
        }
        return Json(value);
    }

    Json(const Json &other) : value_(ncl_json_clone(other.value_)) {}
    Json(Json &&other) noexcept : value_(other.value_) {
        other.value_ = nullptr;
    }
    Json &operator=(Json other) noexcept {  // copy and swap
        std::swap(value_, other.value_);
        return *this;
    }
    ~Json() { ncl_json_free(value_); }

    bool valid() const noexcept { return value_ != nullptr; }
    ncl_json *get() const noexcept { return value_; }

    /** Compact serialisation (the wire form). */
    std::string dump() const {
        char *text = value_ != nullptr ? ncl_json_write_string(value_) : nullptr;
        if (text == nullptr) {
            throw Error(NCL_ERR_STATE, "ncl::Json::dump on an empty value");
        }
        std::string out(text);
        free(text);
        return out;
    }

private:
    ncl_json *value_ = nullptr;
};

/* ---------------------------------------------------------------- message -- */

/** Owning protocol message. */
class Message {
public:
    Message() = default;
    explicit Message(ncl_message *owned) : value_(owned) {}

    /** Parse an MQTT payload that arrived on @p topic. */
    static Message parse(const std::string &topic, const std::string &payload) {
        ncl_message *msg =
            ncl_message_parse(topic.c_str(), payload.data(), payload.size());
        if (msg == nullptr) {
            throw Error(NCL_ERR_PARSE, "payload is not a valid NC-Link message");
        }
        return Message(msg);
    }

    static Message make(ncl_msg_type type) {
        ncl_message *msg = ncl_message_new(type);
        if (msg == nullptr) {
            throw Error(NCL_ERR_NOMEM, "ncl_message_new failed");
        }
        return Message(msg);
    }

    Message(const Message &) = delete;
    Message &operator=(const Message &) = delete;
    Message(Message &&other) noexcept : value_(other.value_) {
        other.value_ = nullptr;
    }
    Message &operator=(Message &&other) noexcept {
        if (this != &other) {
            ncl_message_free(value_);
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }
    ~Message() { ncl_message_free(value_); }

    bool valid() const noexcept { return value_ != nullptr; }
    ncl_message *get() const noexcept { return value_; }
    ncl_message *release() noexcept {
        ncl_message *msg = value_;
        value_ = nullptr;
        return msg;
    }

    /** Fill in "@id" when missing; throws when the message is not valid. */
    Message &finalise() {
        detail::check(ncl_message_finalise(value_), "ncl_message_finalise");
        return *this;
    }

    std::string dump() const {
        char *text =
            value_ != nullptr ? ncl_message_write_string(value_) : nullptr;
        if (text == nullptr) {
            throw Error(NCL_ERR_STATE, "ncl::Message::dump on an empty message");
        }
        std::string out(text);
        free(text);
        return out;
    }

private:
    ncl_message *value_ = nullptr;
};

/* ------------------------------------------------------------------ model -- */

/** Owning device model (root node of the tree). */
class Model {
public:
    Model() = default;
    explicit Model(ncl_node *owned) : value_(owned) {}

    /** Parse a model document; an empty string yields the built in model. */
    static Model parse(const std::string &text) {
        ncl_node *root = ncl_root_node_parse(text.empty() ? nullptr : text.c_str());
        if (root == nullptr) {
            throw Error(NCL_ERR_INVALID_MODEL, "model document is not valid");
        }
        return Model(root);
    }

    Model(const Model &) = delete;
    Model &operator=(const Model &) = delete;
    Model(Model &&other) noexcept : value_(other.value_) {
        other.value_ = nullptr;
    }
    Model &operator=(Model &&other) noexcept {
        if (this != &other) {
            ncl_node_free(value_);
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }
    ~Model() { ncl_node_free(value_); }

    bool valid() const noexcept { return value_ != nullptr; }
    ncl_node *get() const noexcept { return value_; }
    ncl_node *release() noexcept {
        ncl_node *root = value_;
        value_ = nullptr;
        return root;
    }

    /** Look a node up by id, or nullptr. */
    ncl_node *find(const std::string &id) const {
        return value_ != nullptr ? ncl_node_find_by_id(value_, id.c_str())
                                 : nullptr;
    }

    std::string dump() const {
        char *text = value_ != nullptr ? ncl_node_write_string(value_) : nullptr;
        if (text == nullptr) {
            throw Error(NCL_ERR_STATE, "ncl::Model::dump on an empty model");
        }
        std::string out(text);
        free(text);
        return out;
    }

private:
    ncl_node *value_ = nullptr;
};

/* ----------------------------------------------------------------- client -- */

/**
 * One device client. The process wide connection is owned by
 * ncl_client_holder; connecting is a separate, explicit step so a caller can
 * keep several clients of the same process.
 */
class Client {
public:
    /** Connect the process wide client to @p uri (tcp:// or ssl://). */
    static void init(const std::string &uri, const std::string &username = {},
                     const std::string &password = {}) {
        detail::check(ncl_client_holder_init(
                          uri.c_str(),
                          username.empty() ? nullptr : username.c_str(),
                          password.empty() ? nullptr : password.c_str()),
                      "ncl_client_holder_init");
    }

    static void shutdown() noexcept { ncl_client_holder_shutdown(); }

    /** Fetch (creating on demand) the client of @p sn. */
    explicit Client(const std::string &sn) {
        value_ = ncl_client_holder_get(sn.c_str());
        if (value_ == nullptr) {
            throw Error(NCL_ERR_NOT_FOUND, "no client for serial number " + sn);
        }
    }

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;
    Client(Client &&other) noexcept : value_(other.value_) {
        other.value_ = nullptr;
    }
    ~Client() = default; /* owned by the holder */

    ncl_client *get() const noexcept { return value_; }

    /** get_value: the value of @p path, or an exception. */
    Json value(const std::string &path, unsigned timeout_ms = 5000) {
        ncl_json *out = nullptr;
        detail::check(
            ncl_client_get_value(value_, path.c_str(), timeout_ms, &out),
            "ncl_client_get_value");
        return Json(out);
    }

    /** set_value; throws when the device rejects the write. */
    void set(const std::string &path, const Json &value,
             unsigned timeout_ms = 5000) {
        detail::check(ncl_client_set_value(value_, path.c_str(),
                                           ncl_json_clone(value.get()),
                                           timeout_ms),
                      "ncl_client_set_value");
    }

    /** get_length: the collected length of @p path. */
    long long length(const std::string &path, unsigned timeout_ms = 5000) {
        long long out = 0;
        detail::check(ncl_client_get_length(value_, path.c_str(), timeout_ms, &out),
                      "ncl_client_get_length");
        return out;
    }

    /** methodCall: takes the request over, returns the response. */
    Message method_call(Message request, unsigned timeout_ms = 5000) {
        ncl_message *response = nullptr;
        detail::check(ncl_client_method_call(value_, request.release(),
                                             timeout_ms, &response),
                      "ncl_client_method_call");
        return Message(response);
    }

    /** Sample and event callbacks; the C callbacks are adapted with
     *  std::function, one small holder per client (replacing a handler frees
     *  the previous one). */
    using SampleHandler = std::function<void(const char *, const ncl_message *)>;
    using EventHandler = std::function<void(const char *, const ncl_message *)>;

    void on_sample(SampleHandler handler) {
        install(Sample, std::move(handler));
        ncl_client_set_sample_handler(value_, &Client::sample_thunk,
                                      holder(Sample).get());
    }

    void on_event(EventHandler handler) {
        install(Event, std::move(handler));
        ncl_client_set_event_handler(value_, &Client::event_thunk,
                                     holder(Event).get());
    }

    /** Probe the device and install its model. */
    Model probe(unsigned timeout_ms = 5000) {
        ncl_message *response = nullptr;
        detail::check(ncl_client_probe(value_, timeout_ms, &response),
                      "ncl_client_probe");
        Message message(response);
        ncl_node *model = ncl_message_take_model(message.get());
        if (model == nullptr) {
            throw Error(NCL_ERR_INVALID_MESSAGE, "probe response carries no model");
        }
        return Model(model);
    }

private:
    enum Slot { Sample = 0, Event = 1 };

    static std::shared_ptr<SampleHandler> &holder(Slot slot) {
        static std::mutex mutex;
        static std::unordered_map<int, std::shared_ptr<SampleHandler>> handlers;
        (void)mutex;
        return handlers[slot];
    }

    void install(Slot slot, SampleHandler handler) {
        holder(slot) = std::make_shared<SampleHandler>(std::move(handler));
    }

    static void sample_thunk(ncl_client *client, const char *topic,
                             const ncl_message *message, void *user) {
        (void)client;
        auto *fn = static_cast<SampleHandler *>(user);
        if (fn != nullptr && *fn) {
            (*fn)(topic, message);
        }
    }

    static void event_thunk(ncl_client *client, const char *topic,
                            const ncl_message *message, void *user) {
        sample_thunk(client, topic, message, user);
    }

    ncl_client *value_ = nullptr;
};

/* ----------------------------------------------------------------- server -- */

/**
 * Device side server (RAII owner). Create it with ncl_server_create() - the
 * options struct stays in C hands - and hand it over:
 *
 *   ncl_server_options options;
 *   ncl_server_options_default(&options);
 *   ncl::Server server(ncl_server_create(&options));
 */
class Server {
public:
    /** Create a server for @p sn (options zero initialised, sn filled in). */
    static Server create(const std::string &sn) {
        ncl_server_options options{};
        options.sn = sn.c_str();
        return Server(ncl_server_create(&options));
    }

    explicit Server(ncl_server *owned) : value_(owned) {
        if (value_ == nullptr) {
            throw Error(NCL_ERR_INVALID_ARG, "ncl::Server needs a server object");
        }
    }

    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;
    Server(Server &&other) noexcept : value_(other.value_) {
        other.value_ = nullptr;
    }
    ~Server() { ncl_server_free(value_); }

    ncl_server *get() const noexcept { return value_; }

    /** Load a model document (post-construction included). */
    Server &load_model(const std::string &text) {
        detail::check(ncl_server_load_model(value_, text.c_str()),
                      "ncl_server_load_model");
        return *this;
    }

    /** Subscribe to this serial number's request topics. */
    Server &subscribe() {
        detail::check(ncl_server_subscribe(value_), "ncl_server_subscribe");
        return *this;
    }

    void on_message(const std::string &topic, Message message) {
        ncl_server_on_message(value_, topic.c_str(), message.release());
    }

private:
    ncl_server *value_ = nullptr;
};

/** Version string of the C library. */
inline std::string version() { return std::string(NCL_VERSION); }

}  // namespace ncl

#endif /* NCLINK_NCL_HPP */
