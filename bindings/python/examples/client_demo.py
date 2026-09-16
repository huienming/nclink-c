#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""Python 客户端示例：流程与 C / C++ / C# 示例一致。

    python examples/client_demo.py [broker] [设备SN] [秒数]
    # 默认：tcp://127.0.0.1:1883、V2023A7B762、6 秒

先起设备端（另开一个窗口）：

    build\\examples\\ncl_device_demo.exe D:\\sim-py 30

probe 模型 → 打印各轴功率/振动含义 → 读 / 写 /STATUS → 订阅采样与事件
→ 按行消费采样（前 8 行）→ 等若干秒 → 打印计数。
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import nclink  # noqa: E402


def print_axis_quantities(node):
    """把每根轴下的 POWER / ACCELERATION 数据项打出来（含义按"轴 + 物理量"）。"""
    if node.type_name == "AXIS":
        axis = node.name or "轴"
        for item in node.data_items:
            if item.type_name == "POWER":
                print("%-24s %s功率" % (item.path, axis))
            elif item.type_name == "ACCELERATION":
                print("%-24s %s加速度" % (item.path, axis))
    for child in node.devices:
        print_axis_quantities(child)
    for child in node.components:
        print_axis_quantities(child)


def print_sample(sample):
    print("sample %s: id=%s interval=%sms upload=%sms columns=%d rows=%d" % (
        sample.topic, sample.id, sample.interval_ms, sample.upload_interval_ms,
        len(sample.columns), sample.rows))
    for column in sample.columns:
        points_per_slot = column.points // column.slots if column.slots else 0
        print("  %s: %d 个槽位 × 每槽约 %d 点 = %d 点%s" % (
            column.path, column.slots, points_per_slot, column.points,
            "（批量）" if column.is_nested else ""))

    # 按行消费：行数取数据最多的那一列；1 ms 的列在 0.25 ms 的 4 行里读到同一个点。
    shown = min(sample.rows, 8)
    for row in range(shown):
        cells = ["%s=%s" % (column.path, sample.value_at(row, col))
                 for col, column in enumerate(sample.columns)]
        print("  行[%d] %s" % (row, "  ".join(cells)))
    if sample.rows > shown:
        print("  ...（共 %d 行，这里只打前 %d 行）" % (sample.rows, shown))


def print_event(event):
    print("event %s: key=%s value=%s" % (event.topic, event.key, event.value))


def main(argv):
    uri = argv[1] if len(argv) > 1 else "tcp://127.0.0.1:1883"
    sn = argv[2] if len(argv) > 2 else "V2023A7B762"
    seconds = int(argv[3]) if len(argv) > 3 else 6

    nclink.log_init()
    nclink.init(uri)
    print("connected: %s (nclink %s)" % (uri, nclink.version()))

    with nclink.get_device(sn) as device:
        with device.probe() as model:
            print("probe: model root id=%s name=%s" % (model.root.id, model.root.name))
            print("各轴的功率与振动（路径 含义）:")
            for device_node in model.root.devices:
                print_axis_quantities(device_node)

            print("GET /STATUS = %s" % device.get_long("/STATUS"))
            device.set_value("/STATUS", 42)
            print("SET /STATUS = 42 ok")
            print("id(/STATUS) = %s, path(%s) = %s" % (
                device.get_id("/STATUS"), device.get_id("/STATUS"),
                device.get_path(device.get_id("/STATUS"))))

        device.subscribe_samples(2, lambda topic, sample: print_sample(sample))
        device.subscribe_events(2, lambda topic, event: print_event(event))
        print("subscribed: Sample/%s/# and Event/%s" % (sn, sn))

        time.sleep(seconds)
        print("received %d samples, %d events" % (device.sample_count, device.event_count))
        if device.last_callback_error is not None:
            print("callback error: %s" % device.last_callback_error)

    nclink.shutdown()
    nclink.log_shutdown()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except nclink.NclinkError as error:
        print("NC-Link error: %s" % error)
        sys.exit(1)
