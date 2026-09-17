// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink.demo;

import com.nclink.Json;
import com.nclink.Nclink;
import com.nclink.Operation;
import com.nclink.Server;

/**
 * Java 设备端示例：这个 Java 进程就是一台机床。
 *
 * <pre>
 *   java -Dfile.encoding=UTF-8 -Djava.library.path=bindings/java/native/bin \
 *        -cp bindings/java/build/classes \
 *        com.nclink.demo.DeviceDemo [broker] [设备SN] [秒数]
 * </pre>
 *
 * 它注册工具方法、绑定模型里的路径、启动采样通道、每秒推一条事件；然后用仓库里
 * **任何一个客户端**都能读它，例如 C 的示例：
 *
 * <pre>
 *   build\examples\ncl_client_demo.exe tcp://127.0.0.1:1883 V2JAVA00001 8
 * </pre>
 */
public final class DeviceDemo {
    private DeviceDemo() {
    }

    /** 设备模型（含一个采样通道）：换模型就是换这段 JSON。 */
    private static final String MODEL = "{"
            + "\"name\":\"Java 模拟机床\",\"id\":\"01\",\"type\":\"NC_LINK_ROOT\","
            + "\"devices\":[{\"id\":\"02\",\"type\":\"MACHINE\",\"name\":\"模拟机床\","
            + "\"version\":\"2.0\","
            + "\"configs\":[{\"name\":\"采样通道\",\"id\":\"java_channel\","
            + "\"type\":\"SAMPLE_CHANNEL\",\"sampleInterval\":1000,\"uploadInterval\":1000,"
            + "\"ids\":[{\"id\":\"/STATUS\"},{\"id\":\"/PART_COUNT\"},"
            + "{\"id\":\"/CONTROLLER/WARNNING\"}]}],"
            + "\"dataItems\":["
            + "{\"name\":\"状态\",\"id\":\"030001\",\"type\":\"STATUS\",\"settable\":false},"
            + "{\"name\":\"加工计件\",\"id\":\"030002\",\"type\":\"PART_COUNT\","
            + "\"settable\":true},"
            + "{\"name\":\"报警\",\"id\":\"030003\",\"type\":\"WARNNING\","
            + "\"source\":\"CONTROLLER\"}]}]}";

    /** 被工具方法读写的"机床状态"（真实设备里换成你的 PLC / 采集卡）。 */
    private static final class Machine {
        private volatile int partCount;
        private volatile int status = 1;

        Object handle(String method, Json params) {
            switch (method) {
                case "getStatus":
                    return Integer.valueOf(status);
                case "getCount":
                    return Integer.valueOf(partCount);
                case "setCount":
                    partCount = params == null ? 0
                            : ((Number) ((java.util.Map<?, ?>) params.toJavaObject())
                                    .get("value")).intValue();
                    return Integer.valueOf(partCount);
                case "getWarning":
                    return Integer.valueOf(0);
                default:
                    throw new IllegalStateException("没有方法 " + method);
            }
        }
    }

    public static void main(String[] args) throws Exception {
        String broker = args.length > 0 ? args[0] : "tcp://127.0.0.1:1883";
        String sn = args.length > 1 ? args[1] : "V2JAVA00001";
        int seconds = args.length > 2 ? Integer.parseInt(args[2]) : 0;

        Nclink.logInit();
        Machine machine = new Machine();
        try (Server device = new Server(sn, MODEL, broker)) {
            device.registerTool(
                    "plc",
                    new String[] {"getStatus", "getCount", "setCount", "getWarning"},
                    new Server.Binding[] {
                        new Server.Binding("/STATUS", Operation.GET_VALUE, "getStatus"),
                        new Server.Binding("/PART_COUNT", Operation.GET_VALUE, "getCount"),
                        new Server.Binding("/PART_COUNT", Operation.SET_VALUE, "setCount"),
                        new Server.Binding("/CONTROLLER/WARNNING", Operation.GET_VALUE,
                                           "getWarning")},
                    (method, params) -> machine.handle(method, params));
            device.registerBuiltinTool();       // addSample / removeSample
            device.subscribe();                 // 订阅 6 个请求主题
            device.initSamples();               // 启动模型里声明的采样通道
            System.out.println("设备端已就绪：SN=" + sn + " broker=" + broker
                    + "，工具 " + device.operationCount() + " 个操作，采样通道 "
                    + device.sampleCount() + " 个");
            System.out.println("（用仓库里的任意客户端读它，例如 build\\examples\\ncl_client_demo.exe）");

            long deadline = seconds > 0 ? System.currentTimeMillis() + seconds * 1000L : 0;
            while (deadline == 0 || System.currentTimeMillis() < deadline) {
                Thread.sleep(1000);
                int count = ++machine.partCount;
                device.pushEvent("010307", "{\"key\":\"PART_COUNT\",\"value\":" + count
                        + ",\"oldValue\":" + (count - 1) + "}");
                if (count % 5 == 0) {
                    System.out.println("已上报采样 " + device.sampleUploadCount() + " 次，事件 "
                            + device.eventCount() + " 条（计件 " + count + "）");
                }
            }
            System.out.println("设备端退出：采样上报 " + device.sampleUploadCount() + " 次，事件 "
                    + device.eventCount() + " 条");
        } finally {
            Nclink.logShutdown();
        }
    }
}
