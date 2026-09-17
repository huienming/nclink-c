// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * 设备端（ncl_server）：这个进程就是一台机床。
 *
 * <pre>{@code
 * try (Server device = new Server("V2JAVA00001", modelJson, "tcp://127.0.0.1:1883")) {
 *     device.registerTool("plc",
 *         new String[] {"getStatus", "getCount"},
 *         new Server.Binding[] {
 *             new Server.Binding("/STATUS", Operation.GET_VALUE, "getStatus"),
 *             new Server.Binding("/PART_COUNT", Operation.GET_VALUE, "getCount")},
 *         (method, params) -> "getStatus".equals(method) ? 1 : 42);
 *     device.registerBuiltinTool();
 *     device.subscribe();          // 订阅 6 个请求主题
 *     device.initSamples();        // 启动模型里声明的采样通道
 *     device.pushEvent("010307", "{\"key\":\"PART_COUNT\",\"value\":7}");
 * }
 * }</pre>
 *
 * <p>工具方法回调跑在库自己的线程池上（不是 MQTT 读取线程），JNI 会把该线程挂到
 * JVM 上、回调结束再摘下来；回调里抛出的异常会被记成应答的 reason，不穿回原生层。
 *
 * <p>不接 broker 也能用（broker 传 null）：用 {@link #dispatch} 离线驱动请求，或者
 * 给一个 {@link PublishSink} 自己当传输。
 */
public final class Server implements AutoCloseable {
    /** 一条 "<operation>#<path>" 路径绑定。 */
    public static final class Binding {
        private final String path;
        private final Operation operation;
        private final String method;

        public Binding(String path, Operation operation, String method) {
            this.path = path;
            this.operation = operation;
            this.method = method;
        }
    }

    private long handle;
    private long publishHost;
    private PublishSink publishSink;
    private final List<Long> toolHosts = new ArrayList<Long>();
    private final List<HttpEndpoint> httpEndpoints = new ArrayList<HttpEndpoint>();
    private final Map<String, HttpEndpoint.RouteHandler> routeHandlers =
            new LinkedHashMap<String, HttpEndpoint.RouteHandler>();
    private final Map<String, ToolHandler> handlers = new LinkedHashMap<String, ToolHandler>();
    private volatile Throwable lastCallbackError;

    public Server(String sn) {
        this(sn, null, null, null, null, null);
    }

    public Server(String sn, String modelJson, String broker) {
        this(sn, modelJson, broker, null, null, null);
    }

    /**
     * @param sn        设备序列号（必填；做 MQTT clientId 与主题地址）
     * @param modelJson 模型文档；null = 库内置的那份（NC_LINK_ROOT）
     * @param broker    MQTT 地址（如 tcp://127.0.0.1:1883）；null = 不接 MQTT
     * @param username  用户名；null/空 = 匿名
     * @param password  密码；null/空 = 匿名
     * @param sink      自研传输；给了它就不再走 MQTT 发布
     */
    public Server(String sn, String modelJson, String broker, String username,
                  String password, PublishSink sink) {
        if (sn == null || sn.isEmpty()) {
            throw new IllegalArgumentException("sn 不能为空");
        }
        String model = modelJson;
        if (model == null) {
            Model fallback = Model.parse();          // 设备端没有模型就没法采样
            try {
                model = fallback.toJson();
            } finally {
                fallback.close();
            }
        }
        this.publishSink = sink;
        long[] created = new long[1];
        long[] host = new long[1];
        NclinkException.check(Native.serverCreate(sn, model, broker, username, password,
                                                  sink == null ? null : this, created, host),
                              "Server");
        handle = created[0];
        publishHost = host[0];
    }

    // ------------------------------------------------------------ 基本 -- //

    public String sn() {
        return Native.serverSn(requireOpen());
    }

    /** 设备模型（借用视图：服务器活着它就有效）。 */
    public Model model() {
        long node = Native.serverModel(requireOpen());
        return node == 0 ? null : Model.borrowed(node, this);
    }

    public String modelJson() {
        return Native.serverModelJson(requireOpen());
    }

    public String openapiJson(String baseUrl) {
        return Native.serverOpenapiJson(requireOpen(), baseUrl);
    }

    /** 已注册的 "<operation>#<path>" 绑定数（每个方法名本身也算一条）。 */
    public int bindingCount() {
        return Native.serverBindingCount(requireOpen());
    }

    /** 可调用的 (工具, 方法) 对数。 */
    public int operationCount() {
        return Native.serverOperationCount(requireOpen());
    }

    /** 已注册的采样通道数。 */
    public int sampleCount() {
        return Native.serverSampleCount(requireOpen());
    }

    /** 采样上报次数（诊断用）。 */
    public int sampleUploadCount() {
        return Native.serverSampleUploadCount(requireOpen());
    }

    /** 已发布事件数。 */
    public int eventCount() {
        return Native.serverEventCount(requireOpen());
    }

    /** 工具回调里抛出的异常（没抛过就是 null）。 */
    public Throwable lastCallbackError() {
        return lastCallbackError;
    }

    private long requireOpen() {
        if (handle == 0) {
            throw new NclinkException(-13, "Server", "设备端已关闭");
        }
        return handle;
    }

    // ------------------------------------------------------------ 工具 -- //

    /** 注册工具（方法都没有 schema）。 */
    public void registerTool(String tool, String[] methods, Binding[] bindings,
                             ToolHandler handler) {
        Map<String, String> methodsMap = new LinkedHashMap<String, String>();
        if (methods != null) {
            for (String method : methods) {
                methodsMap.put(method, null);
            }
        }
        registerTool(tool, methodsMap, bindings, handler);
    }

    /**
     * 注册工具。
     *
     * @param tool     工具名（形如 "plc"）
     * @param methods  `{方法名: 参数 schema JSON 文本}`（null 表示没有 schema：这种
     *                 方法的 `check` 只接受空参数，是库的规则）
     * @param bindings 路径绑定；可以为 null
     * @param handler  处理函数；传 null 时用 {@link #handlers()} 里先放好的
     */
    public void registerTool(String tool, Map<String, String> methods,
                             Binding[] bindings, ToolHandler handler) {
        if (tool == null || tool.isEmpty() || methods == null || methods.isEmpty()) {
            throw new IllegalArgumentException("工具名与方法表都不能为空");
        }
        if (handler != null) {
            handlers.put(tool, handler);
        }
        if (!handlers.containsKey(tool)) {
            throw new IllegalStateException("工具 " + tool
                    + " 没有处理函数（用 handler 参数或 handlers()）");
        }

        StringBuilder methodJson = new StringBuilder("[");
        boolean first = true;
        for (Map.Entry<String, String> entry : methods.entrySet()) {
            if (!first) {
                methodJson.append(',');
            }
            first = false;
            methodJson.append("{\"name\":").append(JsonValues.quoted(entry.getKey()));
            if (entry.getValue() != null) {
                methodJson.append(",\"schema\":").append(entry.getValue());
            }
            methodJson.append('}');
        }
        methodJson.append(']');

        StringBuilder bindingJson = new StringBuilder("[");
        if (bindings != null) {
            for (int i = 0; i < bindings.length; i++) {
                Binding binding = bindings[i];
                if (i > 0) {
                    bindingJson.append(',');
                }
                bindingJson.append("{\"path\":").append(JsonValues.quoted(binding.path))
                        .append(",\"operation\":").append(binding.operation.code())
                        .append(",\"method\":").append(JsonValues.quoted(binding.method))
                        .append('}');
            }
        }
        bindingJson.append(']');

        long[] host = new long[1];
        int rc = Native.serverRegisterTool(requireOpen(), tool, methodJson.toString(),
                                           bindingJson.toString(), this, host);
        if (rc != 0) {
            handlers.remove(tool);
            throw new NclinkException(rc, "registerTool", tool);
        }
        toolHosts.add(Long.valueOf(host[0]));
    }

    /** `{工具名: 处理函数}`：注册时可以只给方法表，处理函数放这里。 */
    public Map<String, ToolHandler> handlers() {
        return handlers;
    }

    public void registerBuiltinTool() {
        NclinkException.check(Native.serverRegisterBuiltinTool(requireOpen()),
                              "registerBuiltinTool");
    }

    public void registerFileTool() {
        NclinkException.check(Native.serverRegisterFileTool(requireOpen()),
                              "registerFileTool");
    }

    public void startFtp() {
        NclinkException.check(Native.serverStartFtp(requireOpen()), "startFtp");
    }

    /** 订阅本 SN 的 6 个请求主题（接 MQTT 时用）。 */
    public void subscribe() {
        NclinkException.check(Native.serverSubscribe(requireOpen()), "subscribe");
    }

    // ------------------------------------------------------------ 离线 -- //

    /** 离线驱动一条请求，返回应答报文（不经过 MQTT、也不发布应答）。 */
    public Object dispatch(String topic, byte[] payload) {
        String[] out = new String[1];
        NclinkException.check(Native.serverDispatch(requireOpen(), topic, payload, out),
                              "dispatch");
        return parseResponse(responseTopic(topic), out[0]);
    }

    /** 离线调用一个工具方法（不经过 MQTT）；`paramsJson` 可空。 */
    public Object invokeMethodCall(String method) {
        return invokeMethodCall(method, null);
    }

    public Object invokeMethodCall(String method, String paramsJson) {
        String[] out = new String[1];
        NclinkException.check(Native.serverInvokeMethodCall(requireOpen(), method,
                                                           paramsJson, out),
                              "invokeMethodCall");
        return parseResponse("Method/Call/Response/" + sn(), out[0]);
    }

    /** 只按 schema 校验参数、不执行工具。 */
    public Object checkMethodCall(String method) {
        return checkMethodCall(method, null);
    }

    public Object checkMethodCall(String method, String paramsJson) {
        String[] out = new String[1];
        NclinkException.check(Native.serverCheckMethodCall(requireOpen(), method,
                                                          paramsJson, out),
                              "checkMethodCall");
        return parseResponse("Method/Call/Response/" + sn(), out[0]);
    }

    // ------------------------------------------------------------ 采样 -- //

    /** 启动模型里声明的采样通道。 */
    public void initSamples() {
        NclinkException.check(Native.serverInitSamples(requireOpen()), "initSamples");
    }

    /** 运行时加一个采样通道（一个 SAMPLE_CHANNEL 配置节点）。 */
    public void addSample(String configJson) {
        NclinkException.check(Native.serverAddSample(requireOpen(), configJson),
                              "addSample");
    }

    public void removeSample(String channelId) {
        NclinkException.check(Native.serverRemoveSample(requireOpen(), channelId),
                              "removeSample");
    }

    public void stopAllSamples() {
        Native.serverStopAllSamples(requireOpen());
    }

    // ------------------------------------------------------------ HTTP -- //

    /**
     * 起 HTTP 端点，返回 {@link HttpEndpoint}（用完 close；{@code server.close()}
     * 也会收）。
     *
     * <p>端点内容（全在库里）：
     *
     * <ul>
     *   <li>REST：{@code GET /api/schema}（OpenAPI 3.0 文档）、{@code GET /swagger-ui}、
     *       {@code POST /api/<工具>/<方法>}（等价于 methodCall）；
     *   <li>{@code withConfig=true} 再挂配置端点：{@code GET /api/cfg/getSn}、
     *       {@code POST /api/cfg/init}、{@code getModel|setModel}、{@code getDriver|setDriver}、
     *       {@code GET /api/getMqttUrl} + {@code POST /api/setMqttUrl}、
     *       {@code getServerList|setServer}。
     * </ul>
     *
     * @param port 0 表示用系统分配的随机端口（从 {@link HttpEndpoint#port()} 读）
     */
    public HttpEndpoint startHttp(int port, boolean withConfig) {
        long created = Native.httpStart(requireOpen(), port, withConfig);
        if (created == 0) {
            throw new NclinkException(-5, "startHttp", "HTTP 端口 " + port + " 起不来（被占用？）");
        }
        HttpEndpoint endpoint = new HttpEndpoint(this, created,
                                                Native.httpPort(created));
        httpEndpoints.add(endpoint);
        return endpoint;
    }

    public HttpEndpoint startHttp(int port) {
        return startHttp(port, true);
    }

    public HttpEndpoint startHttp() {
        return startHttp(9008, true);
    }

    /** 自定义路由的处理函数表：`{method + " " + path: 处理函数}`。 */
    public Map<String, HttpEndpoint.RouteHandler> routeHandlers() {
        return routeHandlers;
    }

    /* 由 HttpEndpoint 的回调桥转过来：返回 String[]{status, content_type, body}。 */
    String[] dispatchRoute(String method, String path, String query, String body) {
        HttpEndpoint.RouteHandler handler = routeHandlers.get(method + " " + path);
        if (handler == null) {
            for (Map.Entry<String, HttpEndpoint.RouteHandler> entry
                    : routeHandlers.entrySet()) {
                String key = entry.getKey();
                int space = key.indexOf(' ');
                if (space > 0 && "*".equals(key.substring(0, space))
                        && path != null && path.startsWith(key.substring(space + 1))) {
                    handler = entry.getValue();
                    break;
                }
            }
        }
        try {
            HttpEndpoint.Reply reply = handler == null ? null
                    : handler.handle(method, path, query, body);
            if (reply == null) {
                return new String[] {"404", null, null};
            }
            return new String[] {String.valueOf(reply.status), reply.contentType,
                                 reply.body};
        } catch (Throwable error) {
            lastCallbackError = error;
            return new String[] {"500", "text/plain; charset=utf-8",
                                 "handler failed: " + error};
        }
    }

    // ------------------------------------------------------------ 事件 -- //

    /** 推一条事件到 Event/&lt;sn&gt;；`eventJson` 形如 {"key":...,"value":...}。 */
    public void pushEvent(String eventId, String eventJson) {
        NclinkException.check(Native.serverPushEvent(requireOpen(), eventId, eventJson,
                                                     -1, null),
                              "pushEvent");
    }

    public void pushEvent(String eventId, String eventJson, long timeMs,
                          String messageId) {
        NclinkException.check(Native.serverPushEvent(requireOpen(), eventId, eventJson,
                                                     timeMs, messageId),
                              "pushEvent");
    }

    // ------------------------------------------------------------ 收尾 -- //

    /** 停采样、停 FTP、断开 MQTT、放掉回调（可重复调用）。 */
    @Override
    public void close() {
        if (handle == 0) {
            return;
        }
        for (HttpEndpoint endpoint : httpEndpoints) {   // 先收 HTTP：路由回调还挂在服务器上
            endpoint.close();
        }
        httpEndpoints.clear();
        routeHandlers.clear();
        Native.serverFree(handle);          // 先停服务：之后不会再回调进来
        handle = 0;
        for (Long host : toolHosts) {
            Native.hostFree(host.longValue());
        }
        toolHosts.clear();
        if (publishHost != 0) {
            Native.hostFree(publishHost);
            publishHost = 0;
        }
        publishSink = null;
        handlers.clear();
    }

    // ---------------------------------------------------------- 回调桥 -- //

    /* 由 JNI 从库的线程池回调：返回 JSON 文本（null = 没有值）；抛异常 = 该次调用按
       错误应答（JNI 会把异常文本放进 reason）。 */
    String onToolNative(String tool, String method, long paramsHandle) {
        ToolHandler handler = handlers.get(tool);
        if (handler == null) {
            throw new NclinkException(-6, "tool", "没有注册工具 " + tool);
        }
        Json params = paramsHandle == 0 ? null : Json.borrowed(paramsHandle);
        try {
            Object result = handler.handle(method, params);
            return result == null ? null : JsonValues.toJson(result);
        } catch (RuntimeException error) {
            lastCallbackError = error;
            throw error;
        } catch (Exception error) {
            lastCallbackError = error;
            throw new NclinkException(-1, "tool", String.valueOf(error.getMessage()));
        }
    }

    /* 自研传输：JNI 把出站报文交过来。 */
    void onPublishNative(String topic, byte[] payload) {
        PublishSink sink = publishSink;
        if (sink == null) {
            return;
        }
        try {
            sink.publish(topic, payload);
        } catch (Throwable error) {
            lastCallbackError = error;
        }
    }

    private static Object parseResponse(String topic, String text) {
        if (text == null) {
            throw new NclinkException(-1, "Server", "没有应答");
        }
        return Nclink.parse(topic, text.getBytes(java.nio.charset.StandardCharsets.UTF_8));
    }

    private static String responseTopic(String requestTopic) {
        if (requestTopic == null) {
            return null;
        }
        int index = requestTopic.indexOf("/Request/");
        return index < 0 ? requestTopic
                         : requestTopic.substring(0, index) + "/Response/"
                               + requestTopic.substring(index + "/Request/".length());
    }

    @Override
    public String toString() {
        return "Server(" + (handle != 0 ? sn() : "closed") + ")";
    }
}
