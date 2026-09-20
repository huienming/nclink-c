// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * Java 绑定的自检（不需要 broker）。
 *
 * <pre>
 *   java -Djava.library.path=bindings/java/native/bin -cp bindings/java/build/classes \
 *        com.nclink.SelfTest
 * </pre>
 *
 * 对着真 broker 的端到端跑法见 bindings/java/README.md（设备端示例 +
 * demo.ClientDemo）。
 */
public final class SelfTest {
    private static final String SAMPLE_TOPIC = "Sample/V203243111F/s1";
    private static final String SAMPLE_PAYLOAD =
            "{\"@id\":\"m1\",\"paths\":[\"/STATUS\"],\"id\":\"s1\","
            + "\"beginTime\":\"1700000000000\",\"data\":[{\"data\":[1,2]}],"
            + "\"interval\":1000,\"uploadInterval\":2000}";
    private static final String EVENT_TOPIC = "Event/V203243111F";
    private static final String EVENT_PAYLOAD =
            "{\"@id\":\"m1\",\"id\":\"e1\",\"time\":\"1700000000000\","
            + "\"event\":{\"key\":\"PART_COUNT\",\"value\":7}}";

    private static int checks;
    private static int failures;

    private SelfTest() {
    }

    public static void main(String[] args) {
        version();
        json();
        model();
        messages();
        connection();
        server();
        http();
        files();

        System.out.println();
        System.out.println(checks + " 项检查，" + failures + " 失败");
        System.exit(failures == 0 ? 0 : 1);
    }

    // ------------------------------------------------------------ 用例 -- //

    private static void version() {
        String version = Nclink.version();
        check("version 是 3.x（" + version + "）",
                version != null && version.startsWith("3."));
    }

    private static void json() {
        Json value = Json.parse("{\"a\":1}");
        try {
            check("Json.encode 往返", "{\"a\":1}".equals(value.encode()));
            Map<?, ?> map = (Map<?, ?>) value.toJavaObject();
            check("Json.toJavaObject 对象", Long.valueOf(1).equals(map.get("a")));
        } finally {
            value.close();
        }
        value.close();                                  // 幂等
        check("Json.close 幂等", value.isClosed());
        try {
            value.encode();
            check("关闭后再用要抛异常", false);
        } catch (NclinkException expected) {
            check("关闭后再用要抛异常（" + expected.name() + "）", true);
        }

        try {
            Json.parse("{oops");
            check("非法 JSON 要抛异常", false);
        } catch (NclinkException error) {
            check("非法 JSON 要抛异常（" + error.name() + "）", "Json.parse".equals(error.op()));
        }

        // 中文往返：Java 字符串 -> JNI -> 库 -> JNI -> Java 字符串
        Json chinese = Json.parse("{\"s\":\"机床模型文件\"}");
        try {
            check("中文 JSON 往返", "机床模型文件".equals(chinese.get("s").asString()));
        } finally {
            chinese.close();
        }

        Json all = Json.parse("{\"s\":\"x\",\"i\":42,\"d\":1.5,\"b\":true,\"n\":null,"
                + "\"a\":[1,2]}");
        try {
            check("JsonType.OBJECT", all.type() == JsonType.OBJECT);
            check("对象成员个数", all.size() == 6);
            check("asString", "x".equals(all.get("s").asString()));
            check("asLong", all.get("i").asLong() == 42);
            check("asDouble", all.get("d").asDouble() == 1.5);
            check("asBool", all.get("b").asBool());
            check("isNull", all.get("n").isNull());
            check("数组长度", all.get("a").size() == 2);
            check("keys 顺序", all.keys().toString().equals("[s, i, d, b, n, a]"));
            check("取不存在的成员得 null", all.get("nope") == null);
            Json view = all.get("a");                   // 借用视图
            view.close();                               // 空操作
            check("借用视图 close 后仍可用", view.size() == 2);
            List<?> list = (List<?>) all.get("a").toJavaObject();
            check("数组元素是 Long", Long.valueOf(2).equals(list.get(1)));
        } finally {
            all.close();
        }
    }

    private static void model() {
        try (Model model = Model.parse()) {
            Node root = model.root();
            check("默认模型根节点", root.type() == NodeType.ROOT
                    && "NC_LINK_ROOT".equals(root.typeName()));
            check("默认模型含 NC_LINK_ROOT", model.toJson().contains("NC_LINK_ROOT"));
        }

        try {
            Model.parse("{not a model}");
            check("非法模型要抛异常", false);
        } catch (NclinkException error) {
            check("非法模型要抛异常（" + error.name() + "）", true);
        }

        String text = fixture("model_nclink.json");
        if (text == null) {
            return;
        }
        try (Model model = Model.parse(text)) {
            Node root = model.root();
            check("fixture 根路径", "/NC_LINK_ROOT".equals(root.path()));
            check("fixture 根 id", "01".equals(root.id()));

            List<Node> items = new ArrayList<Node>();
            collect(root, items);
            check("fixture 有数据项", !items.isEmpty());
            boolean lookupOk = true;
            for (Node item : items) {
                Node found = model.findById(item.id());
                if (found == null || !found.path().equals(item.path())) {
                    lookupOk = false;
                    break;
                }
            }
            check("id 反查路径一致", lookupOk);
            check("查不到的 id 得 null", model.findById("does-not-exist") == null);

            List<Node> channels = new ArrayList<Node>();
            for (Node node : items) {
                if (node.isSampleChannel()) {
                    channels.add(node);
                }
            }
            collectChannels(root, channels);
            check("fixture 有采样通道", !channels.isEmpty());
            if (!channels.isEmpty()) {
                Node channel = channels.get(0);
                check("采样通道是 CONFIG", channel.type() == NodeType.CONFIG);
                check("采样周期 > 0", channel.sampleIntervalMs() > 0);
                check("采样项路径非空", !channel.sampleItemPaths().isEmpty());
            }
        }
    }

    private static void messages() {
        Object sample = Nclink.parse(SAMPLE_TOPIC,
                SAMPLE_PAYLOAD.getBytes(StandardCharsets.UTF_8));
        check("parse 采样报文", sample instanceof Sample);
        if (sample instanceof Sample) {
            Sample decoded = (Sample) sample;
            check("采样 id", "s1".equals(decoded.id()));
            check("采样周期", decoded.intervalMs() == 1000);
            check("上报周期", decoded.uploadIntervalMs() == 2000);
            check("行数", decoded.rows() == 2);
            check("列数", decoded.columns().size() == 1);
            check("表头路径", "/MACHINE/STATUS".equals(decoded.columns().get(0).path()));
            check("每列点数", decoded.columns().get(0).points() == 2);
            check("按行取值", Long.valueOf(1L).equals(decoded.valueAt(0, 0))
                    && Long.valueOf(2L).equals(decoded.valueAt(1, 0)));
            check("getLong / getDouble / getString",
                    decoded.getLong(0, 0) == 1 && decoded.getDouble(1, 0) == 2.0
                            && "2".equals(decoded.getString(1, 0)));
            check("表头一行", "/MACHINE/STATUS".equals(decoded.header(" ")));
            check("原始报文带 paths", decoded.rawJson() != null
                    && decoded.rawJson().contains("paths"));
        }

        Object event = Nclink.parse(EVENT_TOPIC,
                EVENT_PAYLOAD.getBytes(StandardCharsets.UTF_8));
        check("parse 事件报文", event instanceof Event);
        if (event instanceof Event) {
            Event decoded = (Event) event;
            check("事件字段", "e1".equals(decoded.id())
                    && "PART_COUNT".equals(decoded.key())
                    && Long.valueOf(7L).equals(decoded.value()));
        }

        Object ping = Nclink.parse("Ping/V203243111F",
                "{\"@id\":\"m1\",\"time\":\"1700000000000\"}".getBytes(StandardCharsets.UTF_8));
        check("parse 其它报文", ping instanceof Message
                && ((Message) ping).type() == MessageType.PING);

        try {
            Nclink.parse(SAMPLE_TOPIC, "not a message".getBytes(StandardCharsets.UTF_8));
            check("坏报文要抛异常", false);
        } catch (NclinkException error) {
            check("坏报文要抛异常（" + error.name() + "）", true);
        }
    }

    private static void connection() {
        try {
            Nclink.getDevice("V203243111F");
            check("没连 broker 时 getDevice 要抛异常", false);
        } catch (NclinkException error) {
            check("没连 broker 时 getDevice 要抛异常（" + error.op() + "）",
                    "getDevice".equals(error.op()));
        }
        try {
            // 端口 0 非法：库立刻报错，不碰网络。（别用"没人监听的端口"测：某些
            // 环境里本机端口会被代理黑洞掉，connect 既不失败也不超时。）
            Nclink.init("tcp://127.0.0.1:0");
            check("连不上要抛异常", false);
        } catch (NclinkException error) {
            check("连不上要抛异常（" + error.op() + "）", "init".equals(error.op()));
        }
        check("失败后仍是未连接", !Nclink.isOpen());
        try {
            Nclink.init("");
            check("空 uri 要抛 IllegalArgumentException", false);
        } catch (IllegalArgumentException expected) {
            check("空 uri 要抛 IllegalArgumentException", true);
        }
    }

    // ------------------------------------------------------------ 设备端 -- //

    private static void server() {
        // 建一个设备端（不接 broker，离线驱动）
        try (Server device = new Server("V2TEST00001")) {
            check("Server.sn", "V2TEST00001".equals(device.sn()));
            check("Server 默认模型", device.model() != null
                    && "01".equals(device.model().root().id()));

            device.registerTool(
                    "plc",
                    new String[] {"getValue", "getCount"},
                    new Server.Binding[] {
                        new Server.Binding("/MACHINE/STATUS", Operation.GET_VALUE, "getValue")},
                    (method, params) -> {
                        if ("getValue".equals(method)) {
                            return Integer.valueOf(42);
                        }
                        return java.util.Collections.singletonMap("n", Integer.valueOf(7));
                    });
            check("工具计数", device.operationCount() == 2 && device.bindingCount() >= 1);

            String query = "{\"@id\":\"q1\",\"ids\":[{\"id\":\"/STATUS\","
                    + "\"params\":{\"operation\":\"get_value\"}}]}";
            Object reply = device.dispatch("Query/Request/V2TEST00001",
                    query.getBytes(StandardCharsets.UTF_8));
            check("dispatch 返回 QUERY_RESPONSE", reply instanceof Message
                    && ((Message) reply).type() == MessageType.QUERY_RESPONSE);
            Map<?, ?> body = (Map<?, ?>) ((Message) reply).toJavaObject();
            Map<?, ?> first = (Map<?, ?>) ((List<?>) body.get("values")).get(0);
            check("路径绑定应答 code=OK", "OK".equals(first.get("code")));
            check("路径绑定应答的值", ((List<?>) first.get("values")).get(0).equals(Long.valueOf(42)));

            String missing = "{\"@id\":\"q2\",\"ids\":[{\"id\":\"/NOPE\","
                    + "\"params\":{\"operation\":\"get_value\"}}]}";
            Map<?, ?> missingBody = (Map<?, ?>) ((Message) device.dispatch(
                    "Query/Request/V2TEST00001", missing.getBytes(StandardCharsets.UTF_8)))
                    .toJavaObject();
            Map<?, ?> missingFirst = (Map<?, ?>) ((List<?>) missingBody.get("values")).get(0);
            check("没绑定的路径应答 NG", !"OK".equals(missingFirst.get("code")));

            Map<?, ?> call = (Map<?, ?>) ((Message) device.invokeMethodCall("getCount"))
                    .toJavaObject();
            check("invokeMethodCall code=OK", "OK".equals(call.get("code")));
            check("invokeMethodCall data", String.valueOf(call.get("data")).contains("7"));

            Map<?, ?> checked = (Map<?, ?>) ((Message) device.checkMethodCall("getCount",
                    "{\"a\":1}")).toJavaObject();
            check("没有 schema 的方法 check 只收空参数（NG）",
                    "NG".equals(checked.get("code")) && checked.get("reason") != null);
            Map<?, ?> checkedOk = (Map<?, ?>) ((Message) device.checkMethodCall("getValue"))
                    .toJavaObject();
            check("check 空参数 OK", "OK".equals(checkedOk.get("code")));

            check("openapi 文档含方法路径", device.openapiJson("http://localhost:9008/api")
                    .contains("/plc/getCount"));

            check("加/删采样通道", addAndRemoveSample());
        } catch (NclinkException error) {
            check("设备端基础用例（" + error.getMessage() + "）", false);
        }

        // 带 schema 的方法：check 会按 schema 校验
        try (Server device = new Server("V2TEST00001")) {
            device.registerTool(
                    "plc",
                    java.util.Collections.singletonMap("setValue",
                            "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"integer\"}},"
                            + "\"required\":[\"value\"]}"),
                    null,
                    (method, params) -> ((Map<?, ?>) params.toJavaObject()).get("value"));
            Map<?, ?> ok = (Map<?, ?>) ((Message) device.checkMethodCall("setValue",
                    "{\"value\":42}")).toJavaObject();
            check("schema 校验通过", "OK".equals(ok.get("code")));
            Map<?, ?> bad = (Map<?, ?>) ((Message) device.checkMethodCall("setValue",
                    "{\"value\":\"nope\"}")).toJavaObject();
            check("schema 校验不过", "NG".equals(bad.get("code")));
            Map<?, ?> called = (Map<?, ?>) ((Message) device.invokeMethodCall("setValue",
                    "{\"value\":42}")).toJavaObject();
            check("schema 方法调用取到值", called.get("data").equals(Long.valueOf(42)));
        }

        // 处理函数抛异常：应答 NG + reason，异常记在 lastCallbackError 上
        try (Server device = new Server("V2TEST00001")) {
            device.registerTool("plc", new String[] {"getValue"},
                    new Server.Binding[] {
                        new Server.Binding("/MACHINE/STATUS", Operation.GET_VALUE, "getValue")},
                    (method, params) -> {
                        throw new IllegalStateException("坏掉了");
                    });
            String query = "{\"@id\":\"q1\",\"ids\":[{\"id\":\"/STATUS\","
                    + "\"params\":{\"operation\":\"get_value\"}}]}";
            Map<?, ?> body = (Map<?, ?>) ((Message) device.dispatch("Query/Request/V2TEST00001",
                    query.getBytes(StandardCharsets.UTF_8))).toJavaObject();
            Map<?, ?> first = (Map<?, ?>) ((List<?>) body.get("values")).get(0);
            check("处理函数抛异常 -> NG", "NG".equals(first.get("code")));
            check("异常文本进 reason", String.valueOf(first.get("reason")).contains("坏掉了"));
            check("异常记在 lastCallbackError",
                    device.lastCallbackError() instanceof IllegalStateException);
        }

        // 没有处理函数就注册不了
        try (Server device = new Server("V2TEST00001")) {
            try {
                device.registerTool("plc", new String[] {"getValue"}, null, null);
                check("缺处理函数要抛异常", false);
            } catch (IllegalStateException expected) {
                check("缺处理函数要抛异常", true);
            }
        }

        // 自研传输：事件交给 PublishSink
        final List<String> topics = new ArrayList<String>();
        final List<byte[]> payloads = new ArrayList<byte[]>();
        try (Server device = new Server("V2TEST00002", null, null, null, null,
                (topic, payload) -> {
                    topics.add(topic);
                    payloads.add(payload);
                })) {
            device.pushEvent("010307", "{\"key\":\"PART_COUNT\",\"value\":7}");
            check("事件进自研传输", topics.size() == 1
                    && "Event/V2TEST00002".equals(topics.get(0)));
            check("事件计数", device.eventCount() == 1);
            if (!payloads.isEmpty()) {
                Json event = Json.parse(new String(payloads.get(0), StandardCharsets.UTF_8));
                try {
                    Map<?, ?> map = (Map<?, ?>) event.toJavaObject();
                    check("事件报文体", "010307".equals(map.get("id"))
                            && String.valueOf(map.get("event")).contains("PART_COUNT"));
                } finally {
                    event.close();
                }
            }
        }

        // close 可重复，关掉之后再用要抛异常
        Server device = new Server("V2TEST00003");
        device.close();
        device.close();
        try {
            device.sn();
            check("关闭后再用要抛异常", false);
        } catch (NclinkException expected) {
            check("关闭后再用要抛异常（" + expected.name() + "）", true);
        }
    }

    /** 离线加一个采样通道再删掉（不启动采样线程，只验证注册）。 */
    private static boolean addAndRemoveSample() {
        Server device = new Server("V2TEST00004");
        try {
            device.addSample("{\"name\":\"测试通道\",\"id\":\"ch1\","
                    + "\"type\":\"SAMPLE_CHANNEL\",\"sampleInterval\":1000,"
                    + "\"uploadInterval\":1000,\"ids\":[{\"id\":\"/STATUS\"}]}");
            if (device.sampleCount() != 1) {
                return false;
            }
            device.removeSample("ch1");
            return device.sampleCount() == 0;
        } finally {
            device.stopAllSamples();
            device.close();
        }
    }

    /* ----------------------------------------------------------- files -- */

    /** 文件小工具（离线：本地文件，不需要 broker 与设备）。 */
    private static void files() {
        File dir = new File(System.getProperty("java.io.tmpdir"),
                            "nclink-selftest-" + System.nanoTime());
        if (!dir.mkdirs()) {
            check("建临时目录", false);
            return;
        }
        File file = new File(dir, "hello.txt");
        String content = "文件通道 hello\n";
        try {
            fileServerCustomPort();
            writeText(file, content);
            check("文件工具：文本类要压缩", Nclink.fileNeedCompression("a.txt")
                    && Nclink.fileNeedCompression("model.json"));
            check("文件工具：二进制类不压缩", !Nclink.fileNeedCompression("a.bin"));
            check("文件工具：分片数（256 KB 一片）",
                    Nclink.fileTotalChunks(0) == 0 && Nclink.fileTotalChunks(1) == 1
                    && Nclink.fileTotalChunks(256 * 1024) == 1
                    && Nclink.fileTotalChunks(256 * 1024 + 1) == 2);

            byte[] bytes = java.nio.file.Files.readAllBytes(file.toPath());
            java.security.MessageDigest sha =
                    java.security.MessageDigest.getInstance("SHA-256");
            StringBuilder hex = new StringBuilder();
            for (byte value : sha.digest(bytes)) {
                hex.append(String.format("%02x", Byte.valueOf(value)));
            }
            check("文件工具：SHA-256 与 JDK 算的一致",
                    hex.toString().equals(Nclink.fileChecksum(file.getPath())));

            FileInfo info = Nclink.fileAttribute(file.getPath(), dir.getPath());
            check("文件工具：属性（名字/大小/片数/不是目录）", info != null
                    && "hello.txt".equals(info.fileName())
                    && info.fileSize() == bytes.length && info.totalChunks() == 1
                    && !info.isDir() && info.compressed());
            FileInfo folder = Nclink.fileAttribute(dir.getPath());
            check("文件工具：目录属性（fileType=1）",
                    folder != null && folder.isDir() && folder.fileSize() == 0);
            check("文件工具：不存在的路径返回 null",
                    Nclink.fileAttribute(new File(dir, "nope.bin").getPath()) == null);
        } catch (Exception error) {
            check("文件工具用例（" + error + "）", false);
        } finally {
            deleteTree(dir);
        }
    }

    /** 进程级 FTP 端点换端口 / 换根目录也能起（对端找的就是这个端点）。 */
    private static void fileServerCustomPort() {
        int port = 24124;
        Nclink.stopFileServer();
        try {
            Nclink.startFileServer(port, Nclink.rootDirectory(), null, null);
            java.net.Socket probe = new java.net.Socket("127.0.0.1", port);
            try {
                probe.setSoTimeout(3000);
                byte[] buffer = new byte[64];
                int got = probe.getInputStream().read(buffer);
                String greeting = new String(buffer, 0, Math.max(got, 0),
                                             StandardCharsets.US_ASCII).trim();
                check("文件端点：自定义端口在听（" + port + "：" + greeting + "）",
                        greeting.startsWith("220"));
            } finally {
                probe.close();
            }
        } catch (Exception error) {
            check("文件端点：自定义端口用例（" + error + "）", false);
        } finally {
            Nclink.stopFileServer();
            try {
                Nclink.startFileServer();           // 恢复默认（2323）
            } catch (NclinkException expected) {
                /* 2323 被占就算了 */
            }
        }
    }

    private static void writeText(File file, String text) throws java.io.IOException {
        java.io.OutputStream out = new java.io.FileOutputStream(file);
        try {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        } finally {
            out.close();
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

    /* ------------------------------------------------------------ http -- */

    /** 一次 HTTP 往返的结果。 */
    private static final class Reply {
        int status;
        String body;
    }

    /**
     * HTTP / REST 端点：全部走本机回环，不需要 broker。
     */
    private static void http() {
        try (Server device = new Server("V2TEST00005")) {
            device.registerTool("plc", new String[] {"getCount"}, null,
                    (method, params) -> Integer.valueOf(42));

            HttpEndpoint endpoint = device.startHttp(0, true);
            check("HTTP 端点端口（0 = 随机）", endpoint.port() > 0);
            check("HTTP 端点 URL",
                    ("http://localhost:" + endpoint.port()).equals(endpoint.url()));

            Reply schema = fetch(endpoint, "GET", "/api/schema", null);
            check("GET /api/schema 是 200", schema.status == 200);
            check("OpenAPI 3.0 文档", schema.body.contains("3.0.0"));
            check("OpenAPI 里有工具方法", schema.body.contains("/plc/getCount"));

            Reply call = fetch(endpoint, "POST", "/api/plc/getCount", "{}");
            check("POST /api/<工具>/<方法> 是 200", call.status == 200);
            check("REST 调用等价于 methodCall（走 Result 信封）",
                    call.body.contains("\"status\":true") && call.body.contains("42"));

            // 配置端点：没装模型文件时是 NG，但端点必须在
            Reply config = fetch(endpoint, "GET", "/api/cfg/getModel", null);
            check("配置端点必须在（200）", config.status == 200);
            check("配置端点有 status 字段", config.body.contains("status"));

            Reply ui = fetch(endpoint, "GET", "/swagger-ui", null);
            check("GET /swagger-ui 是 200", ui.status == 200);
            check("请求计数（已发 4 条，计数 " + endpoint.requestCount() + "）",
                    endpoint.requestCount() >= 4);

            endpoint.route("GET", "/hello",
                    (method, path, query, body) -> HttpEndpoint.Reply.json(
                            "{\"path\":\"" + path + "\",\"query\":\"" + query + "\"}"));
            endpoint.route("POST", "/echo",
                    (method, path, query, body) -> HttpEndpoint.Reply.of(201,
                            "text/plain; charset=utf-8", "echo:" + body));
            endpoint.route("GET", "/none", (method, path, query, body) -> null);
            endpoint.route("GET", "/give-up", (method, path, query, body) ->
                    HttpEndpoint.Reply.status(204));
            endpoint.route("GET", "/boom", (method, path, query, body) -> {
                throw new IllegalStateException("炸了");
            });

            Reply hello = fetch(endpoint, "GET", "/hello?x=1", null);
            check("自定义 GET 路由是 200", hello.status == 200);
            check("自定义路由能拿到 query",
                    hello.body.contains("x=1") && hello.body.contains("/hello"));

            Reply echo = fetch(endpoint, "POST", "/echo", "hi");
            check("自定义路由能给状态码与报文", echo.status == 201 && "echo:hi".equals(echo.body));

            Reply none = fetch(endpoint, "GET", "/none", null);
            check("处理函数返回 null -> 404", none.status == 404);

            Reply giveUp = fetch(endpoint, "GET", "/give-up", null);
            check("只回状态码的路由", giveUp.status == 204);

            Reply boom = fetch(endpoint, "GET", "/boom", null);
            check("处理函数抛异常 -> 500", boom.status == 500);
            check("异常文本进报文", boom.body != null && boom.body.contains("炸了"));
            check("异常记在 lastCallbackError",
                    device.lastCallbackError() instanceof IllegalStateException);

            Reply missing = fetch(endpoint, "GET", "/no-such-path", null);
            check("没挂过的路径是 404", missing.status == 404);

            endpoint.setCors(false);
            endpoint.setCors(true);
            check("setCors 可切换", true);

            endpoint.close();
            endpoint.close();
            check("HTTP 端点 close 幂等", true);
            try {
                endpoint.route("GET", "/after-close", (method, path, query, body) -> null);
                check("关了之后挂路由要抛异常", false);
            } catch (NclinkException expected) {
                check("关了之后挂路由要抛异常（" + expected.name() + "）", true);
            }
        } catch (NclinkException error) {
            check("HTTP 端点用例（" + error.getMessage() + "）", false);
        }

        // server.close() 也会收掉没关的 HTTP 端点
        Server device = new Server("V2TEST00006");
        HttpEndpoint endpoint = device.startHttp(0, false);
        device.close();
        endpoint.close();                       // 再关一次也不该炸
        check("server.close 收掉 HTTP 端点，端点再关也不炸", true);
    }

    /** 打一次本机 HTTP；4xx/5xx 也当正常返回（读它的报文）。 */
    private static Reply fetch(HttpEndpoint endpoint, String method, String path,
                               String body) {
        Reply reply = new Reply();
        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection) new URL(endpoint.url() + path)
                    .openConnection();
            connection.setRequestMethod(method);
            connection.setConnectTimeout(5000);
            connection.setReadTimeout(5000);
            if (body != null) {
                byte[] data = body.getBytes(StandardCharsets.UTF_8);
                connection.setDoOutput(true);
                connection.setRequestProperty("Content-Type", "application/json");
                connection.setFixedLengthStreamingMode(data.length);
                OutputStream out = connection.getOutputStream();
                try {
                    out.write(data);
                } finally {
                    out.close();
                }
            }
            reply.status = connection.getResponseCode();
            InputStream stream = reply.status >= 400 ? connection.getErrorStream()
                                                     : connection.getInputStream();
            reply.body = stream != null ? read(stream) : "";
            return reply;
        } catch (Exception error) {
            check("HTTP 请求失败（" + method + " " + path + "）：" + error, false);
            reply.status = 0;
            reply.body = "";
            return reply;
        } finally {
            if (connection != null) {
                connection.disconnect();
            }
        }
    }

    private static String read(InputStream stream) throws java.io.IOException {
        ByteArrayOutputStream buffer = new ByteArrayOutputStream();
        byte[] chunk = new byte[4096];
        int read;
        while ((read = stream.read(chunk)) > 0) {
            buffer.write(chunk, 0, read);
        }
        stream.close();
        return new String(buffer.toByteArray(), StandardCharsets.UTF_8);
    }

    // ------------------------------------------------------------ 工具 -- //

    private static void collect(Node node, List<Node> items) {
        for (Node child : node.children(0)) {
            items.add(child);
        }
        for (int kind = 1; kind <= 3; kind++) {
            for (Node child : node.children(kind)) {
                collect(child, items);
            }
        }
    }

    private static void collectChannels(Node node, List<Node> channels) {
        if (node.isSampleChannel() && !channels.contains(node)) {
            channels.add(node);
        }
        for (int kind = 0; kind <= 3; kind++) {
            for (Node child : node.children(kind)) {
                collectChannels(child, channels);
            }
        }
    }

    /** 从当前目录往上找 tests/data/<name>；找不到返回 null（跳过用例）。 */
    private static String fixture(String name) {
        File dir = new File(System.getProperty("user.dir", ".")).getAbsoluteFile();
        for (int depth = 0; dir != null && depth < 6; depth++) {
            File candidate = new File(new File(new File(dir, "tests"), "data"), name);
            if (candidate.isFile()) {
                try {
                    return new String(Files.readAllBytes(candidate.toPath()),
                            StandardCharsets.UTF_8);
                } catch (Exception error) {
                    return null;
                }
            }
            dir = dir.getParentFile();
        }
        return null;
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
}
