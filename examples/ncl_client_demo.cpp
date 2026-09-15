// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

// C++17 client example: connect, probe the model, read/write a value, then
// subscribe to the device's sample reports and events.
//
//   ./ncl_client_demo_cpp tcp://127.0.0.1:1883 V2023A7B762
//
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#include "nclink/ncl.hpp"

// 采样回调：每个上报窗口调用一次，打印表头与本轮各列取值。
// msg 是借用的，回调返回后立即释放，不要保存指针。
static void on_sample(const char *topic, const ncl_message *msg) {
    size_t items = ncl_message_item_count(msg);
    char *header = ncl_message_sample_header(msg, ";");
    size_t i;

    std::printf("sample %s: id=%s interval=%lldms upload=%lldms items=%u\n",
                topic, msg->as.sample.id != NULL ? msg->as.sample.id : "?",
                (long long)msg->as.sample.interval,
                (long long)msg->as.sample.upload_interval, (unsigned)items);
    std::printf("  header: %s\n", header != NULL ? header : "(none)");
    free(header);

    for (i = 0; i < items; i++) {
        const ncl_sample_item *item =
            (const ncl_sample_item *)ncl_message_item_at(msg, i);
        const ncl_json *first =
            item != NULL ? ncl_sample_item_value_at(item, 0) : NULL;
        char *text = first != NULL ? ncl_json_write_string(first) : NULL;
        const char *path =
            ncl_strvec_len(&msg->as.sample.paths) > i
                ? ncl_strvec_at(&msg->as.sample.paths, i)
                : "?";

        std::printf("  [%u] %s = %s (%u values)\n", (unsigned)i, path,
                    text != NULL ? text : "?",
                    (unsigned)(item != NULL ? ncl_sample_item_value_count(item)
                                            : 0));
        free(text);
    }
}

// 事件回调：设备主动推送（Event/<sn>）。
static void on_event(const char *topic, const ncl_message *msg) {
    const char *key = ncl_json_obj_get_string(msg->as.event.event, "key");
    const ncl_json *value = ncl_json_obj_get(msg->as.event.event, "value");
    char *text = value != NULL ? ncl_json_write_string(value) : NULL;

    std::printf("event %s: key=%s value=%s\n", topic, key != NULL ? key : "?",
                text != NULL ? text : "?");
    free(text);
}

int main(int argc, char **argv) {
    const std::string uri = argc > 1 ? argv[1] : "tcp://127.0.0.1:1883";
    const std::string sn = argc > 2 ? argv[2] : "V203243111F";
    const int seconds = argc > 3 ? std::atoi(argv[3]) : 6;

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

        // 采样与事件：订阅一次覆盖该设备的所有采样通道。
        client.on_sample(on_sample);
        client.on_event(on_event);
        if (ncl_client_subscribe_samples(client.get(), 2) != NCL_OK) {
            throw ncl::Error(NCL_ERR_IO, "subscribe to the sample channels failed");
        }
        ncl_client_subscribe_events(client.get(), 2);
        std::printf("subscribed: Sample/%s/# and Event/%s\n", sn.c_str(),
                    sn.c_str());

        // 等设备按 uploadInterval 上报若干个窗口（默认 6 秒）。
        for (int i = 0; i < seconds; i++) {
            ncl_sleep_millis(1000);
        }
        std::printf("received %u samples, %u events\n",
                    (unsigned)ncl_client_sample_count(client.get()),
                    (unsigned)ncl_client_event_count(client.get()));

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
