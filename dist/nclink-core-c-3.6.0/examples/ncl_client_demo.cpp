// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

// C++17 client example: connect, probe the model, read/write a value, then
// subscribe to the device's sample reports and events.
//
//   ./ncl_client_demo_cpp tcp://127.0.0.1:1883 V2023A7B762
//
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include "nclink/ncl.hpp"

// 采样回调：每个上报窗口调用一次。
//
// 消费方式：**按行**（和 C 示例一致）。
//   行数        ncl_message_sample_point_count(msg) —— 数据最多的那一列的点数
//   第 row 行   ncl_message_sample_value_at(msg, row, column)
// 采样率更低的列在同一个段里会连着几行读到同一个点（覆盖该行的第一个点）——
// 比如 1 ms 一列配 0.25 ms 一列时，前者 4 行共用同一个值。
//
// msg 是借用的，回调返回后立即释放，不要保存指针。
static void on_sample(const char *topic, const ncl_message *msg) {
    if (!ncl_message_sample_is_complete(msg)) {
        std::printf("sample %s: 报文不完整，已忽略\n", topic);
        return;
    }

    const size_t items = ncl_message_item_count(msg);
    const size_t path_count = ncl_strvec_len(&msg->as.sample.paths);
    const size_t rows = ncl_message_sample_point_count(msg);
    const size_t shown = rows < 8 ? rows : 8;
    char *header = ncl_message_sample_header(msg, ";");

    std::printf("sample %s: id=%s interval=%lldms upload=%lldms items=%u rows=%u\n",
                topic, msg->as.sample.id != NULL ? msg->as.sample.id : "?",
                (long long)msg->as.sample.interval,
                (long long)msg->as.sample.upload_interval, (unsigned)items,
                (unsigned)rows);
    std::printf("  header: %s\n", header != NULL ? header : "(none)");
    ncl_free_safe(header);

    // 每列的形态：几个槽位、每槽几个点、合计几个点（批量列 = 亚毫秒采样）。
    for (size_t i = 0; i < items; i++) {
        const ncl_sample_item *item =
            (const ncl_sample_item *)ncl_message_item_at(msg, i);
        const char *path = i < path_count ? ncl_strvec_at(&msg->as.sample.paths, i)
                                          : "?";
        const size_t slots = item != NULL ? ncl_json_arr_len(item->data) : 0;
        const size_t points = ncl_sample_item_value_count(item);
        const ncl_json *first = ncl_sample_item_value_at(item, 0);
        char *text = first != NULL ? ncl_json_write_string(first) : NULL;

        std::printf("  [%u] %s: %u 个槽位 × 每槽约 %u 点 = %u 点%s，首个=%s\n",
                    (unsigned)i, path, (unsigned)slots,
                    (unsigned)(slots > 0 ? points / slots : 0),
                    (unsigned)points,
                    ncl_sample_item_is_nested(item) ? "（批量）" : "",
                    text != NULL ? text : "null");
        ncl_free_safe(text);
    }

    // 按行遍历：每行把各列的值摆在一起（前 8 行）。
    for (size_t row = 0; row < shown; row++) {
        std::string line;

        for (size_t i = 0; i < items; i++) {
            const char *path = i < path_count
                                   ? ncl_strvec_at(&msg->as.sample.paths, i)
                                   : "?";
            const ncl_json *value = ncl_message_sample_value_at(msg, row, i);
            char *text = value != NULL ? ncl_json_write_string(value) : NULL;

            line += path;
            line += "=";
            line += text != NULL ? text : "null";
            line += "  ";
            ncl_free_safe(text);
        }
        std::printf("  行[%u] %s\n", (unsigned)row, line.c_str());
    }
    if (rows > shown) {
        std::printf("  ...（共 %u 行，这里只打前 %u 行）\n", (unsigned)rows,
                    (unsigned)shown);
    }
}

// 事件回调：设备主动推送（Event/<sn>）。
static void on_event(const char *topic, const ncl_message *msg) {
    const char *key = ncl_json_obj_get_string(msg->as.event.event, "key");
    const ncl_json *value = ncl_json_obj_get(msg->as.event.event, "value");
    char *text = value != NULL ? ncl_json_write_string(value) : NULL;

    std::printf("event %s: key=%s value=%s\n", topic, key != NULL ? key : "?",
                text != NULL ? text : "?");
    ncl_free_safe(text);
}

// 轴下面这两类数据项的中文含义（T/CMTBA 1008.4—2020 表4 物理量数据项）。
//
// 含义按"对象 + 物理量"的说法写，和"主轴振动"（主轴 + 振动）是同一种写法：
//
//     POWER        -> 功率    （瓦特 W）
//     SPEED        -> 转速    （转每分 r/min）
//     ACCELERATION -> 加速度  （毫米每秒平方 mm/s²）
//
// 打印时再把轴补在前面，于是有"X轴功率""主轴转速""主轴加速度"；数据项带 number
// （一个部件多路传感器）时再跟一个 #<number>。返回 nullptr 表示不是这里关心的数据项。
static const char *axis_quantity_meaning_cn(const char *type) {
    if (type == nullptr) {
        return nullptr;
    }
    if (std::strcmp(type, "POWER") == 0) {
        return "功率";
    }
    if (std::strcmp(type, "SPEED") == 0) {
        return "转速";
    }
    if (std::strcmp(type, "ACCELERATION") == 0) {
        return "加速度";
    }
    return nullptr;
}

// 打印模型里每个轴的功率、转速与加速度，一行一条，格式为：
//     路径 中文含义
// 含义是"轴 + 物理量"的说法，例：/AXIS@X/POWER 是"X轴功率"、/AXIS@S/SPEED 是
// "主轴转速"、/AXIS@S/ACCELERATION 是"主轴加速度"。路径就是设备端取值用的
// 路径，可以直接拿去 getValue，也可以写进采样通道的 ids 里当采样项。
static void print_axis_quantities(const ncl_node *node) {
    if (node == nullptr) {
        return;
    }
    // 只认组件里的 AXIS；功率/加速度是挂在轴下面的数据项。
    if (node->type == NCL_NODE_COMPONENT && node->node_type_name != nullptr &&
        std::strcmp(node->node_type_name, "AXIS") == 0) {
        // 轴的中文名（X轴 / 主轴）就是含义里的那个"对象"。
        const char *axis_name = node->name != nullptr ? node->name : "轴";

        for (size_t i = 0; ncl_node_data_item_at(node, i) != nullptr; i++) {
            const ncl_node *item = ncl_node_data_item_at(node, i);
            const char *meaning = axis_quantity_meaning_cn(item->node_type_name);

            if (meaning != nullptr) {
                // 一个部件多路传感器时（数据项带 number）含义后面跟 #<number>
                std::printf("%-24s %s%s%s%s\n", ncl_node_path(item),
                            axis_name, meaning,
                            item->number != nullptr ? " #" : "",
                            item->number != nullptr ? item->number : "");
            }
        }
    }
    for (size_t i = 0; ncl_node_device_at(node, i) != nullptr; i++) {
        print_axis_quantities(ncl_node_device_at(node, i));
    }
    for (size_t i = 0; ncl_node_component_at(node, i) != nullptr; i++) {
        print_axis_quantities(ncl_node_component_at(node, i));
    }
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

        std::printf("各轴的功率、转速与加速度（路径 含义）:\n");
        print_axis_quantities(model.get());

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
