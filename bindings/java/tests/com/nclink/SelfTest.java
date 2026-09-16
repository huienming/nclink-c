// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

import java.io.File;
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
            check("表头路径", "/STATUS".equals(decoded.columns().get(0).path()));
            check("每列点数", decoded.columns().get(0).points() == 2);
            check("按行取值", Long.valueOf(1L).equals(decoded.valueAt(0, 0))
                    && Long.valueOf(2L).equals(decoded.valueAt(1, 0)));
            check("getLong / getDouble / getString",
                    decoded.getLong(0, 0) == 1 && decoded.getDouble(1, 0) == 2.0
                            && "2".equals(decoded.getString(1, 0)));
            check("表头一行", "/STATUS".equals(decoded.header(" ")));
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
            Nclink.init("tcp://127.0.0.1:1");            // 1 号端口不会有 broker
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
