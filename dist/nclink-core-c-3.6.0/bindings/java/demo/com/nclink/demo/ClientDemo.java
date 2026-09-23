// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink.demo;

import com.nclink.DeviceClient;
import com.nclink.Event;
import com.nclink.Json;
import com.nclink.Model;
import com.nclink.Nclink;
import com.nclink.NclinkException;
import com.nclink.Node;
import com.nclink.Sample;
import com.nclink.SampleColumn;

/**
 * Java 客户端示例：流程与 C / C++ / C# / Python 示例一致。
 *
 * <pre>
 *   java -Djava.library.path=bindings/java/native/bin -cp bindings/java/build/classes \
 *        com.nclink.demo.ClientDemo [broker] [设备SN] [秒数]
 * </pre>
 *
 * 先起设备端（另开一个窗口）：{@code build\examples\ncl_device_demo.exe D:\sim-java 30}
 */
public final class ClientDemo {
    private ClientDemo() {
    }

    public static void main(String[] args) {
        String uri = args.length > 0 ? args[0] : "tcp://127.0.0.1:1883";
        String sn = args.length > 1 ? args[1] : "V2023A7B762";
        int seconds = args.length > 2 ? Integer.parseInt(args[2]) : 6;

        Nclink.logInit();
        try {
            Nclink.init(uri);
            System.out.println("connected: " + uri + " (nclink " + Nclink.version() + ")");

            try (DeviceClient device = Nclink.getDevice(sn)) {
                try (Model model = device.probe()) {
                    System.out.println("probe: model root id=" + model.root().id()
                            + " name=" + model.root().name());
                    System.out.println("各轴的功率与振动（路径 含义）:");
                    for (Node deviceNode : model.root().devices()) {
                        printAxisQuantities(deviceNode);
                    }
                    System.out.println("GET /STATUS = " + device.getLong("/MACHINE/STATUS"));
                    try {
                        device.setValue("/MACHINE/STATUS", "42");
                        System.out.println("SET /STATUS = 42 ok");
                    } catch (NclinkException error) {
                        // 设备没把 /STATUS 绑到 setValue 上就会拒绝，这不是客户端的错
                        System.out.println("SET /STATUS 被拒绝：" + error.name());
                    }
                    String id = device.getId("/MACHINE/STATUS");
                    System.out.println("id(/STATUS) = " + id + ", path(" + id + ") = "
                            + device.getPath(id));

                    try (Json reply = device.methodCall("/plc/getCount", null, true, 5000)) {
                        System.out.println("methodCall(check) = " + reply.encode());
                    }
                }

                device.subscribeSamples(2, (topic, sample) -> printSample(sample));
                device.subscribeEvents(2, (topic, event) -> printEvent(event));
                System.out.println("subscribed: Sample/" + sn + "/# and Event/" + sn);

                for (int i = 0; i < seconds; i++) {
                    Thread.sleep(1000);
                }
                System.out.println("received " + device.sampleCount() + " samples, "
                        + device.eventCount() + " events");
                if (device.lastCallbackError() != null) {
                    System.out.println("callback error: " + device.lastCallbackError());
                }
            }
            Nclink.shutdown();
            Nclink.logShutdown();
        } catch (NclinkException error) {
            System.out.println("NC-Link error: " + error.getMessage());
            System.exit(1);
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
        }
    }

    /** 把每根轴下的 POWER / ACCELERATION 数据项打出来（含义按"轴 + 物理量"）。 */
    private static void printAxisQuantities(Node node) {
        if ("AXIS".equals(node.typeName())) {
            String axis = (node.name() == null || node.name().isEmpty()) ? "轴" : node.name();
            for (Node item : node.dataItems()) {
                if ("POWER".equals(item.typeName())) {
                    System.out.printf("%-24s %s功率%n", item.path(), axis);
                } else if ("ACCELERATION".equals(item.typeName())) {
                    System.out.printf("%-24s %s加速度%n", item.path(), axis);
                }
            }
        }
        for (Node child : node.devices()) {
            printAxisQuantities(child);
        }
        for (Node child : node.components()) {
            printAxisQuantities(child);
        }
    }

    private static void printSample(Sample sample) {
        System.out.printf("sample %s: id=%s interval=%sms upload=%sms columns=%d rows=%d%n",
                sample.topic(), sample.id(), sample.intervalMs(),
                sample.uploadIntervalMs(), sample.columns().size(), sample.rows());
        for (SampleColumn column : sample.columns()) {
            System.out.printf("  %s: %d 个槽位 × 每槽约 %d 点 = %d 点%s%n",
                    column.path(), column.slots(),
                    column.slots() > 0 ? column.points() / column.slots() : 0,
                    column.points(), column.isNested() ? "（批量）" : "");
        }

        // 按行消费：行数取数据最多的那一列；1 ms 的列在 0.25 ms 的 4 行里读到同一个点。
        int shown = Math.min(sample.rows(), 8);
        for (int row = 0; row < shown; row++) {
            StringBuilder cells = new StringBuilder();
            for (int col = 0; col < sample.columns().size(); col++) {
                if (col > 0) {
                    cells.append("  ");
                }
                cells.append(sample.columns().get(col).path()).append('=')
                        .append(sample.valueAt(row, col));
            }
            System.out.println("  行[" + row + "] " + cells);
        }
        if (sample.rows() > shown) {
            System.out.println("  ...（共 " + sample.rows() + " 行，这里只打前 " + shown + " 行）");
        }
    }

    private static void printEvent(Event event) {
        System.out.println("event " + event.topic() + ": key=" + event.key()
                + " value=" + event.value());
    }
}
