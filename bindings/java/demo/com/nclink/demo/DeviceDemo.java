// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

// 设备端示例：这个 Java 进程就是一台机床。
//
//   java -Dfile.encoding=UTF-8 -Djava.library.path=bindings/java/native/bin \
//        -cp bindings/java/build/classes com.nclink.demo.DeviceDemo \
//        [broker] [sn] [seconds] [http-port]
//
//     broker    tcp://… / ssl://…；"-" = 离线（不接 MQTT，出站报文打控制台）；
//               省略 = 读 <root>/conf/mqtt.cfg（没有就先写一份默认的）
//     sn        省略或 "-" = <root>/bin/sn.txt（没有就生成 "V2" + 9 位十六进制）
//     seconds   0 或省略 = 一直运行到 Ctrl+C
//     http-port 省略 = 9008；0 = 系统分配的随机端口
//     安装根目录：环境变量 NCL_DEVICE_ROOT（省略 = 当前目录）
//
// **五个语言的设备端示例是同一台设备**：模型编译在库里（Nclink.deviceModel()，
// 与 C 示例共用 examples/device_model.c），工具方法 29 个、绑定 20 条，两个采样
// 通道与事件节拍也都一样。每个轴一个功率 /AXIS@<轴>/POWER@1 与三个加速度
// /AXIS@<轴>/ACCELERATION@X|Y|Z —— 振动信号在三个方向上的分量。

package com.nclink.demo;

import com.nclink.HttpEndpoint;
import com.nclink.Nclink;
import com.nclink.Operation;
import com.nclink.PublishSink;
import com.nclink.Server;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

public final class DeviceDemo {
    private DeviceDemo() {
    }

    private static final String[] AXES = {"X", "Y", "Z", "C", "S"};
    private static final String[] DIRS = {"X", "Y", "Z"};
    private static final String[] SCALARS = {
        "getValue", "setValue", "getCount", "getWarning", "getProgram",
        "getToolNumber", "getFeedOverride", "getMachiningMode", "getSpeedS"};
    private static final String[] SCALAR_PATHS = {
        "/MACHINE/STATUS", "/MACHINE/STATUS", "/MACHINE/PART_COUNT", "/MACHINE/CONTROLLER/WARNING",
        "/MACHINE/CONTROLLER/PROGRAM", "/MACHINE/CONTROLLER/TOOL_NUMBER", "/MACHINE/FEED_OVERRIDE",
        "/MACHINE/MACHINING_MODE", "/MACHINE/AXIS@S/SPEED"};
    private static final String DEFAULT_BROKER = "tcp://127.0.0.1:1883";

    /** 被工具方法读写的"机床状态"（真机里换成你的 PLC / 采集卡）。 */
    private static final class Machine {
        private volatile int status = 1;
        private volatile long partCount;
        private volatile int program = 1001;
        private volatile int toolNumber = 1;
        private volatile int feedOverride = 100;
        private volatile int spindleSpeed = 3600;
        private volatile int mode = 2;
        private final long[] powerTick = new long[AXES.length];
        private final long[][] vibrationTick = new long[AXES.length][DIRS.length];

        /** 功率（W）：每轴一个基值 + 0~300 W 的缓升，25 格一个锯齿。 */
        private synchronized double power(String axis) {
            int slot = indexOf(AXES, axis);
            long tick = powerTick[slot]++;
            return 800.0 + slot * 250.0 + ((tick + slot * 7) % 25) * 12.5;
        }

        /** 振动：一次查询给 4 个 0.25 ms 子采样（1 ms 槽位里的 4 kHz 波形）。 */
        private synchronized double[] vibration(String axis, String direction) {
            int a = indexOf(AXES, axis);
            int d = indexOf(DIRS, direction);
            int slot = a * DIRS.length + d;
            long step = vibrationTick[a][d];
            double[] block = new double[4];
            vibrationTick[a][d] += 4;
            for (int k = 0; k < block.length; k++) {
                block[k] = (((step + k + slot * 3) % 16) - 8) * 0.125;
            }
            return block;
        }

        Object handle(String method, com.nclink.Json params) {
            if ("setValue".equals(method)) {
                status = ((Number) ((Map<?, ?>) params.toJavaObject()).get("value"))
                        .intValue();
                System.out.println("STATUS 被设置为 " + status);
                return Boolean.TRUE;
            }
            switch (method) {
                case "getValue": return Integer.valueOf(status);
                case "getCount": return Long.valueOf(partCount);
                case "getWarning": return Integer.valueOf(0);
                case "getProgram": return Integer.valueOf(program);
                case "getToolNumber": return Integer.valueOf(toolNumber);
                case "getFeedOverride": return Integer.valueOf(feedOverride);
                case "getMachiningMode": return Integer.valueOf(mode);
                case "getSpeedS": return Integer.valueOf(spindleSpeed);
                default: break;
            }
            if (method.startsWith("getPower")) {
                return Double.valueOf(power(method.substring("getPower".length())));
            }
            if (method.startsWith("getAcceleration")) {
                String axis = method.substring("getAcceleration".length(),
                                               "getAcceleration".length() + 1);
                String dir = method.substring("getAcceleration".length() + 1);
                return vibration(axis, dir);
            }
            throw new IllegalStateException("没有方法 " + method);
        }
    }

    private static int indexOf(String[] values, String value) {
        for (int i = 0; i < values.length; i++) {
            if (values[i].equals(value)) {
                return i;
            }
        }
        throw new IllegalArgumentException(value);
    }

    private static String readMqttCfg(String root) {
        Path path = Paths.get(root, "conf", "mqtt.cfg");
        try {
            for (String line : Files.readAllLines(path, StandardCharsets.UTF_8)) {
                if (line.trim().startsWith("url=")) {
                    String url = line.trim().substring(4).trim();
                    return url.isEmpty() ? DEFAULT_BROKER : url;
                }
            }
        } catch (IOException ignored) {
            // fall through
        }
        return DEFAULT_BROKER;
    }

    private static void bootstrap(String root) throws IOException {
        for (String sub : new String[] {"conf", "bin", "log"}) {
            Files.createDirectories(Paths.get(root, sub));
        }
        Path cfg = Paths.get(root, "conf", "mqtt.cfg");
        if (!Files.exists(cfg)) {
            Files.write(cfg, ("url=" + DEFAULT_BROKER
                    + "\r\nusername=\r\npassword=\r\n").getBytes(StandardCharsets.UTF_8));
            System.out.println("首次启动：写入 MQTT 配置（" + cfg + "）");
        }
    }

    /** <root>/bin/sn.txt；没有就按库的规则生成（"V2" + 9 位十六进制）。 */
    private static String readSn(String root) throws IOException {
        Path path = Paths.get(root, "bin", "sn.txt");
        if (Files.exists(path)) {
            String text = new String(Files.readAllBytes(path), StandardCharsets.UTF_8)
                    .trim();
            if (!text.isEmpty()) {
                return text;
            }
        }
        java.util.Random random = new java.util.Random();
        String digits = "0123456789ABCDEF";
        String sn;
        do {
            StringBuilder text = new StringBuilder("V2");
            for (int i = 0; i < 9; i++) {
                text.append(digits.charAt(random.nextInt(digits.length())));
            }
            sn = text.toString();
        } while (sn.replaceAll("[^A-F]", "").isEmpty());
        Files.write(path, sn.getBytes(StandardCharsets.UTF_8));
        System.out.println("首次启动：生成 SN（" + path + "）");
        return sn;
    }

    /** 模型编译在库里：现场 <root>/conf/model/nclink.json 优先，没有就写一份下来。 */
    private static String loadModel(String root) throws IOException {
        Path installed = Paths.get(root, "conf", "model", "nclink.json");
        if (Files.exists(installed)) {
            return new String(Files.readAllBytes(installed), StandardCharsets.UTF_8);
        }
        String text = Nclink.deviceModel();
        Files.createDirectories(installed.getParent());
        Files.write(installed, text.getBytes(StandardCharsets.UTF_8));
        System.out.println("首次启动：写入设备模型（" + installed + "，编译在库里的那份）");
        return text;
    }

    public static void main(String[] args) throws Exception {
        String brokerArg = args.length > 0 ? args[0] : "";
        String snArg = args.length > 1 ? args[1] : "";
        int seconds = args.length > 2 ? Integer.parseInt(args[2]) : 0;
        int httpPort = args.length > 3 ? Integer.parseInt(args[3]) : 9008;
        String root = System.getenv("NCL_DEVICE_ROOT");
        root = root == null || root.isEmpty() ? "." : root;
        boolean offline = "-".equals(brokerArg);

        bootstrap(root);
        Nclink.setRootDirectory(root);
        Nclink.logInit();
        String sn = snArg.isEmpty() || "-".equals(snArg) ? readSn(root) : snArg;
        String broker = offline ? null
                : (brokerArg.isEmpty() ? readMqttCfg(root) : brokerArg);
        String model = loadModel(root);

        // 方法表与绑定表：9 个标量 + 5 个轴功率 + 15 个方向加速度（与其它语言一致）。
        List<String> methods = new ArrayList<String>();
        List<Server.Binding> bindings = new ArrayList<Server.Binding>();
        for (int i = 0; i < SCALARS.length; i++) {
            methods.add(SCALARS[i]);
            bindings.add(new Server.Binding(SCALAR_PATHS[i],
                    "setValue".equals(SCALARS[i]) ? Operation.SET_VALUE
                                                  : Operation.GET_VALUE,
                    SCALARS[i]));
        }
        for (String axis : AXES) {
            methods.add("getPower" + axis);
            bindings.add(new Server.Binding("/MACHINE/AXIS@" + axis + "/MACHINE/POWER@1",
                    Operation.GET_VALUE, "getPower" + axis));
        }
        // 振动只留主轴：模型里 X/Y/Z/C 四轴已经没有 ACCELERATION 数据项了。
        for (String axis : new String[] {"S"}) {
            for (String dir : DIRS) {
                methods.add("getAcceleration" + axis + dir);
                bindings.add(new Server.Binding(
                        "/MACHINE/AXIS@" + axis + "/ACCELERATION@" + dir,
                        Operation.GET_VALUE, "getAcceleration" + axis + dir));
            }
        }

        Machine machine = new Machine();
        final boolean[] stop = {false};
        Runtime.getRuntime().addShutdownHook(new Thread(new Runnable() {
            @Override
            public void run() {
                stop[0] = true;
            }
        }));
        PublishSink sink = offline ? new PublishSink() {
            @Override
            public void publish(String topic, byte[] payload) {
                System.out.println("out  " + topic + "  "
                        + new String(payload, StandardCharsets.UTF_8));
            }
        } : null;

        try (Server device = new Server(sn, model, broker, null, null, sink)) {
            device.registerTool("plc", methods.toArray(new String[0]),
                    bindings.toArray(new Server.Binding[0]),
                    (method, params) -> machine.handle(method, params));
            device.registerBuiltinTool();
            device.registerFileTool();
            if (!offline) {
                device.subscribe();
            }
            device.initSamples();
            HttpEndpoint http = device.startHttp(httpPort, true);
            http.route("GET", "/api/hello", (method, path, query, body) ->
                    HttpEndpoint.Reply.json("{\"sn\":\"" + sn + "\",\"status\":"
                            + machine.status + ",\"parts\":" + machine.partCount
                            + "}"));

            System.out.println("设备 SN: " + sn);
            System.out.println("MQTT: " + (offline
                    ? "离线模式（出站报文打到控制台）" : broker));
            System.out.println("HTTP: " + http.url()
                    + "/swagger-ui（自定义路由 /api/hello）");
            System.out.println("工具 " + device.operationCount() + " 个操作，采样通道 "
                    + device.sampleCount() + " 个");
            System.out.println("运行 " + (seconds > 0
                    ? seconds + " 秒（Ctrl+C 可随时退出）" : "直到 Ctrl+C"));

            long deadline = seconds > 0
                    ? System.currentTimeMillis() + seconds * 1000L : 0;
            long i = 0;
            while (!stop[0]
                    && (deadline == 0 || System.currentTimeMillis() < deadline)) {
                Thread.sleep(100);
                i++;
                machine.partCount++;
                machine.feedOverride = 60 + (int) (i % 7) * 10;
                machine.spindleSpeed = 3000 + (int) (i % 5) * 300;
                if (i % 20 == 0) {
                    machine.program++;
                    machine.toolNumber = 1 + machine.program % 8;
                    machine.mode = machine.program % 2 == 0 ? 1 : 2;
                }
                if (i % 10 == 0) {
                    device.pushEvent("010307", "{\"key\":\"PART_COUNT\",\"value\":"
                            + machine.partCount + ",\"oldValue\":"
                            + (machine.partCount - 1) + "}");
                    System.out.println("事件 PART_COUNT=" + machine.partCount
                            + "；采样上报 " + device.sampleUploadCount() + " 次，状态 "
                            + machine.status + "，刀号 " + machine.toolNumber);
                }
            }
            System.out.println("设备端退出统计：采样上报 " + device.sampleUploadCount()
                    + " 次，事件 " + device.eventCount() + " 条");
        } finally {
            Nclink.logShutdown();
        }
    }
}
