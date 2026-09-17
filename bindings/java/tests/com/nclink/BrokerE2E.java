// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;
import java.util.Map;

/**
 * 对**真 broker** 的端到端自检（默认跳过）。
 *
 * <pre>
 *   NCLINK_TEST_BROKER=tcp://127.0.0.1:18830 java -Djava.library.path=... \
 *       -cp build/classes com.nclink.BrokerE2E
 * </pre>
 *
 * 设备端与客户端放在同一个进程里、报文真的过一遍 MQTT：probe、路径绑定读写、
 * methodCall、采样、事件，以及文件通道（上传 / 列目录 / 下载 / 建目录 / 删文件 /
 * 带文件参数的方法调用）。离线自检覆盖不到的"过 MQTT 的那一段"靠它守住。
 */
public final class BrokerE2E {
    private static final String SN = "V2JAVAE2E01";
    private static int checks;
    private static int failures;

    private BrokerE2E() {
    }

    public static void main(String[] args) throws Exception {
        String broker = System.getenv("NCLINK_TEST_BROKER");
        if (broker == null || broker.trim().isEmpty()) {
            System.out.println("跳过（设置 NCLINK_TEST_BROKER=tcp://host:port 才跑真 broker 用例）");
            return;
        }
        broker = broker.trim();

        String model = "{"
                + "\"name\":\"Java E2E 机床\",\"id\":\"01\",\"type\":\"NC_LINK_ROOT\","
                + "\"devices\":[{\"id\":\"02\",\"type\":\"MACHINE\",\"name\":\"模拟机床\","
                + "\"configs\":[{\"name\":\"采样通道\",\"id\":\"e2e_channel\","
                + "\"type\":\"SAMPLE_CHANNEL\",\"sampleInterval\":200,"
                + "\"uploadInterval\":200,\"ids\":[{\"id\":\"/STATUS\"}]}],"
                + "\"dataItems\":[{\"name\":\"状态\",\"id\":\"030001\","
                + "\"type\":\"STATUS\",\"settable\":true}]}]}";
        final long[] state = new long[] {1};
        final long[] seen = new long[] {-1};

        Nclink.logInit();
        File work = Files.createTempDirectory("nclink-java-e2e-").toFile();
        File local = new File(work, "report.txt");
        String content = "文件通道 e2e " + System.nanoTime() + "\n";
        Files.write(local.toPath(), content.getBytes(StandardCharsets.UTF_8));
        long size = local.length();
        String deviceMirror = new File(new File(Nclink.rootDirectory(), "uploadFile"),
                "data" + File.separator + "report.txt").getPath();
        final String[] give = new String[] {deviceMirror};

        boolean fileServer = true;
        try {
            Nclink.startFileServer();               // 幂等（init 里也会起）
        } catch (NclinkException error) {
            fileServer = false;
            System.out.println("跳过文件通道（FTP 2323 起不来: " + error.name() + "）");
        }
        try (Server device = new Server(SN, model, broker)) {
            device.registerTool(
                    "plc",
                    new String[] {"getStatus", "setStatus"},
                    new Server.Binding[] {
                        new Server.Binding("/STATUS", Operation.GET_VALUE, "getStatus"),
                        new Server.Binding("/STATUS", Operation.SET_VALUE, "setStatus")},
                    (method, params) -> {
                        if ("setStatus".equals(method)) {
                            state[0] = ((Number) ((Map<?, ?>) params.toJavaObject())
                                    .get("value")).longValue();
                        }
                        return Long.valueOf(state[0]);
                    });
            device.registerFileTool();
            device.setFilePeer("127.0.0.1", 2323, null, null);   // 显式指定对端
            device.registerTool("sink",
                    new String[] {"take", "give"},
                    null,
                    (method, params) -> {
                        if ("take".equals(method)) {
                            String token = String.valueOf(
                                    ((Map<?, ?>) params.toJavaObject()).get("blob"));
                            File file = new File(new File(Nclink.rootDirectory(), "uploadFile"),
                                    token.replace('/', File.separatorChar));
                            seen[0] = file.exists() ? file.length() : -1;
                            return Long.valueOf(seen[0]);
                        }
                        return Json.parse("{\"copy\":{\"@file\":\""
                                + give[0].replace("\\", "\\\\") + "\"}}");
                    });
            device.subscribe();

            // 顺手过一遍带 TLS 选项的 init：给了选项也要能连普通的 tcp://
            Nclink.init(broker, null, null, new TlsOptions());
            try (DeviceClient client = Nclink.getDevice(SN)) {
                check("带 TLS 选项的 init 已连上（TlsAvailable=" + Nclink.tlsAvailable()
                              + "）",
                      Nclink.isOpen());
                try (Model probed = client.probe()) {
                    check("真 broker：probe 到模型", "01".equals(probed.root().id()));
                }
                try (Json value = client.getValue("/STATUS")) {
                    check("真 broker：路径绑定取值",
                            value.toJavaObject().equals(Long.valueOf(1)));
                }
                client.setValue("/STATUS", "7");
                check("真 broker：路径绑定写值到达处理函数", state[0] == 7);
                try (Json reply = client.methodCall("/plc/getStatus")) {
                    check("真 broker：methodCall 应答解析成 JSON",
                            "OK".equals(((Map<?, ?>) reply.toJavaObject()).get("code")));
                }

                /* ---- 文件通道 ---- */
                if (fileServer) {
                    fileChannel(client, work, local, content, size, deviceMirror, seen);
                }
            } finally {
                Nclink.shutdown();
            }
        }
        Nclink.logShutdown();
        deleteTree(work);

        // TLS（可选）：设了这两个环境变量就再跑一遍 ssl://
        String tlsBroker = System.getenv("NCLINK_TEST_TLS_BROKER");
        String tlsCa = System.getenv("NCLINK_TEST_TLS_CA");
        if (tlsBroker != null && !tlsBroker.trim().isEmpty() && tlsCa != null
                && !tlsCa.trim().isEmpty()) {
            tlsRoundTrip(tlsBroker.trim(), tlsCa.trim(), model);
        }

        System.out.println();
        System.out.println(checks + " 项检查，" + failures + " 失败");
        if (failures != 0) {
            System.exit(1);
        }
    }

    /** 文件通道那一段（FTP 端点起不来时整段跳过）。 */
    /** TLS 那一段：设备端与客户端都走 ssl://（要带 TLS 的 nclink_jni）。 */
    private static void tlsRoundTrip(String broker, String ca, String model)
            throws Exception {
        if (!Nclink.tlsAvailable()) {
            System.out.println("跳过 TLS 用例（这个 nclink_jni 没带 TLS）");
            return;
        }
        TlsOptions tls = new TlsOptions().caFile(ca).serverName("127.0.0.1");
        String sn = SN + "T";
        try (Server device = new Server(sn, model, broker, null, null, null, tls)) {
            device.registerTool("plc", new String[] {"getStatus"}, null,
                    (method, params) -> Integer.valueOf(42));
            device.subscribe();
            Nclink.init(broker, null, null, tls);
            try (DeviceClient client = Nclink.getDevice(sn)) {
                try (Json reply = client.methodCall("/plc/getStatus")) {
                    Map<?, ?> body = (Map<?, ?>) reply.toJavaObject();
                    check("TLS：设备端与客户端都过 ssl://（code=" + body.get("code")
                                  + "）",
                          "OK".equals(body.get("code"))
                          && body.get("data").equals(Long.valueOf(42)));
                }
            } finally {
                Nclink.shutdown();
            }
        }
    }

    private static void fileChannel(DeviceClient client, File work, File local,
                                    String content, long size, String deviceMirror,
                                    long[] seen) throws Exception {
                client.uploadLocalFile(local, "/data/report.txt");
                File mirror = new File(deviceMirror);
                check("文件通道：上传后设备侧有文件",
                        mirror.exists() && mirror.length() == size);

                List<FileInfo> listed = client.listFiles("/data");
                check("文件通道：列目录看到它（" + listed.size() + " 项）",
                        listed.size() == 1 && listed.get(0).fileSize() == size
                        && !listed.get(0).isDir());

                File back = new File(work, "back.txt");
                client.downloadTo("/data/report.txt", back);
                check("文件通道：下载回来的字节一致",
                        new String(Files.readAllBytes(back.toPath()),
                                   StandardCharsets.UTF_8).equals(content));

                client.makeDirectory("/docs");
                boolean hasDocs = false;
                for (FileInfo item : client.listFiles("/")) {
                    hasDocs = hasDocs || ("docs".equals(item.fileName()) && item.isDir());
                }
                check("文件通道：建目录", hasDocs);

                try (Json reply = client.methodCallFile("sink/take", "{\"blob\":\"\"}",
                                                        new String[] {"blob"},
                                                        new String[] {local.getPath()})) {
                    check("文件通道：方法参数带文件（设备收到 " + seen[0] + " 字节）",
                            seen[0] == size);
                }
                try (Json reply = client.methodCallFile("sink/give", "{}", null, null)) {
                    Map<?, ?> data = (Map<?, ?>) ((Map<?, ?>) reply.toJavaObject())
                            .get("data");
                    List<?> keys = (List<?>) data.get("fileKeys");
                    String got = String.valueOf(data.get("copy"));
                    check("文件通道：返回值里的文件已下载到本地",
                            keys != null && keys.contains("copy") && !"null".equals(got)
                            && new String(Files.readAllBytes(new File(got).toPath()),
                                          StandardCharsets.UTF_8).equals(content));
                }

                client.deleteFile("/data/report.txt");
                check("文件通道：删掉之后列不到了", client.listFiles("/data").isEmpty());
    }

    private static void check(String name, boolean ok) {
        checks++;
        if (ok) {
            System.out.println("ok   " + name);
        } else {
            failures++;
            System.out.println("FAIL " + name);
        }
    }

    private static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteTree(child);
            }
        }
        file.delete();
    }
}
