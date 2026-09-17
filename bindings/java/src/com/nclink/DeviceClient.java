// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

/**
 * 某个 SN 的设备视图：读值 / 写值 / 探测 / 方法调用 / 采样与事件订阅。
 *
 * <p>由 {@link Nclink#getDevice(String)} 创建；同一个 SN 在库里只有一份客户端
 * （进程级连接断开时统一回收），所以 {@link #close()} 只退订、清回调，不断开
 * MQTT —— 那是 {@link Nclink#shutdown()} 的事。
 */
public final class DeviceClient implements AutoCloseable {
    private final String sn;
    private long handle;
    private long sampleHost;
    private long eventHost;
    private SampleListener sampleListener;
    private EventListener eventListener;
    private volatile Throwable lastCallbackError;

    DeviceClient(String sn, long handle) {
        this.sn = sn;
        this.handle = handle;
    }

    public String sn() {
        return sn;
    }

    /** 回调里抛出的异常（没抛过就是 null）。 */
    public Throwable lastCallbackError() {
        return lastCallbackError;
    }

    private long requireOpen() {
        if (handle == 0) {
            throw new NclinkException(-13, "DeviceClient", "客户端已关闭");
        }
        return handle;
    }

    // ------------------------------------------------------------ 读值 -- //

    /** 探测设备，拿到模型（顺带装进客户端，见 {@link #loadModel(Model)}）。 */
    public Model probe() {
        return probe(5000);
    }

    public Model probe(int timeoutMs) {
        long[] out = new long[1];
        NclinkException.check(Native.clientProbe(requireOpen(), timeoutMs, out), "probe");
        Model model = new Model(out[0]);
        loadModel(model);
        return model;
    }

    /**
     * 把模型装进客户端（路径 ↔ id 互查、采样报文按模型补齐缺的 paths 都靠它）。
     *
     * <p>客户端拿的是**自己的拷贝**，所以 {@code model} 还是调用方的，照常
     * {@link Model#close()}；{@code null} 表示清掉当前装载的模型。
     */
    public void loadModel(Model model) {
        NclinkException.check(
                Native.clientSetRootNode(requireOpen(), model == null ? 0 : model.root().handle()),
                "loadModel");
    }

    /** 清掉客户端当前装载的模型。 */
    public void clearModel() {
        loadModel(null);
    }

    /** 读值（返回的 {@link Json} 用完要 close）。 */
    public Json getValue(String path) {
        return getValue(path, 5000);
    }

    public Json getValue(String path, int timeoutMs) {
        long[] out = new long[1];
        NclinkException.check(
                Native.clientGetValue(requireOpen(), path, timeoutMs, out), "getValue");
        return Json.owned(out[0]);
    }

    /** 读一个整数；值不是整数时（或读失败）给 defaultValue。 */
    public long getLong(String path, int timeoutMs, long defaultValue) {
        Json value = getValue(path, timeoutMs);
        try {
            return value.asLong(defaultValue);
        } finally {
            value.close();
        }
    }

    public long getLong(String path, int timeoutMs) {
        return getLong(path, timeoutMs, 0);
    }

    public long getLong(String path) {
        return getLong(path, 5000, 0);
    }

    /** 读一个浮点；读不到就给 defaultValue。 */
    public double getDouble(String path, int timeoutMs, double defaultValue) {
        Json value = getValue(path, timeoutMs);
        try {
            return value.asDouble(defaultValue);
        } finally {
            value.close();
        }
    }

    public double getDouble(String path) {
        return getDouble(path, 5000, 0);
    }

    /** 读一个区间（数组数据项）。 */
    public Json getValueRange(String path, int start, int end) {
        return getValueRange(path, start, end, 5000);
    }

    public Json getValueRange(String path, int start, int end, int timeoutMs) {
        long[] out = new long[1];
        NclinkException.check(Native.clientGetValueRange(requireOpen(), path, start,
                end, timeoutMs, out), "getValueRange");
        return Json.owned(out[0]);
    }

    /** 数组数据项的长度。 */
    public long getLength(String path) {
        return getLength(path, 5000);
    }

    public long getLength(String path, int timeoutMs) {
        long[] out = new long[1];
        NclinkException.check(
                Native.clientGetLength(requireOpen(), path, timeoutMs, out), "getLength");
        return out[0];
    }

    // ------------------------------------------------------------ 写值 -- //

    /** 写值；{@code valueJson} 是 JSON 文本（要写字符串记得带引号）。 */
    public void setValue(String path, String valueJson) {
        setValue(path, valueJson, 5000);
    }

    public void setValue(String path, String valueJson, int timeoutMs) {
        NclinkException.check(
                Native.clientSetValue(requireOpen(), path, valueJson, timeoutMs),
                "setValue");
    }

    public void setValue(String path, Json value) {
        setValue(path, value == null ? "null" : value.encode(), 5000);
    }

    /** 按索引写数组里的一个元素。 */
    public void setValueIndex(String path, String valueJson, int index) {
        setValueIndex(path, valueJson, index, 5000);
    }

    public void setValueIndex(String path, String valueJson, int index, int timeoutMs) {
        NclinkException.check(Native.clientSetValueIndex(requireOpen(), path, valueJson,
                index, timeoutMs), "setValueIndex");
    }

    // -------------------------------------------------------- 方法调用 -- //

    /** 方法调用（不校验参数）。 */
    public Json methodCall(String method) {
        return methodCall(method, null, false, 5000);
    }

    public Json methodCall(String method, String paramsJson) {
        return methodCall(method, paramsJson, false, 5000);
    }

    /**
     * 方法调用；返回应答报文的 {@link Json}（code / params / data / reason）。
     *
     * <p>{@code method} 形如 {@code /plc/setValue}（{@code plc/setValue} 也认），
     * {@code check} 为 true 时只校验参数、不执行。
     */
    public Json methodCall(String method, String paramsJson, boolean check,
                           int timeoutMs) {
        String[] out = new String[1];
        NclinkException.check(Native.clientMethodCall(requireOpen(), method, paramsJson,
                check, timeoutMs, out), "methodCall");
        return Json.parse(out[0]);
    }

    /** 心跳：Ping/&lt;sn&gt; -&gt; Pong/&lt;sn&gt;。 */
    public void ping() {
        ping(5000);
    }

    public void ping(int timeoutMs) {
        NclinkException.check(Native.clientPing(requireOpen(), timeoutMs), "ping");
    }

    // -------------------------------------------------------- 路径 / id -- //

    /** 数据项路径 -> 节点 id（没有就是 null）。 */
    public String getId(String path) {
        return Native.clientGetId(requireOpen(), path);
    }

    /** 节点 id -> 数据项路径（没有就是 null）。 */
    public String getPath(String id) {
        return Native.clientGetPath(requireOpen(), id);
    }

    // -------------------------------------------------------------- 订阅 -- //

    /** 订阅采样（{@code Sample/<sn>/#}），QoS 0。 */
    public void subscribeSamples(SampleListener listener) {
        subscribeSamples(0, listener);
    }

    public void subscribeSamples(int qos, SampleListener listener) {
        if (listener != null) {
            this.sampleListener = listener;
        }
        long[] host = new long[1];
        int rc = Native.clientSubscribeSamples(requireOpen(), qos, this, host);
        if (rc != 0) {
            this.sampleListener = null;
            throw new NclinkException(rc, "subscribeSamples");
        }
        this.sampleHost = host[0];
    }

    /** 退订采样（回调也一并清掉）。 */
    public void unsubscribeSamples() {
        if (sampleHost == 0) {
            return;
        }
        int rc = Native.clientUnsubscribeSamples(requireOpen(), sampleHost);
        sampleHost = 0;
        sampleListener = null;
        NclinkException.check(rc, "unsubscribeSamples");
    }

    /** 订阅事件（{@code Event/<sn>}），QoS 2。 */
    public void subscribeEvents(EventListener listener) {
        subscribeEvents(2, listener);
    }

    public void subscribeEvents(int qos, EventListener listener) {
        if (listener != null) {
            this.eventListener = listener;
        }
        long[] host = new long[1];
        int rc = Native.clientSubscribeEvents(requireOpen(), qos, this, host);
        if (rc != 0) {
            this.eventListener = null;
            throw new NclinkException(rc, "subscribeEvents");
        }
        this.eventHost = host[0];
    }

    /** 退订事件（回调也一并清掉）。 */
    public void unsubscribeEvents() {
        if (eventHost == 0) {
            return;
        }
        int rc = Native.clientUnsubscribeEvents(requireOpen(), eventHost);
        eventHost = 0;
        eventListener = null;
        NclinkException.check(rc, "unsubscribeEvents");
    }

    /** 收到过多少条采样。 */
    public int sampleCount() {
        return Native.clientSampleCount(requireOpen());
    }

    /** 收到过多少条事件。 */
    public int eventCount() {
        return Native.clientEventCount(requireOpen());
    }

    // ------------------------------------------------------ 运行时通道 -- //

    /** 运行时加一个采样通道；{@code configJson} 是一个 SAMPLE_CHANNEL 配置节点。 */
    public void addSample(String configJson) {
        addSample(configJson, 5000);
    }

    public void addSample(String configJson, int timeoutMs) {
        NclinkException.check(
                Native.clientAddSample(requireOpen(), configJson, timeoutMs), "addSample");
    }

    /** 运行时删掉一个采样通道。 */
    public void removeSample(String channelId) {
        removeSample(channelId, 5000);
    }

    public void removeSample(String channelId, int timeoutMs) {
        NclinkException.check(
                Native.clientRemoveSample(requireOpen(), channelId, timeoutMs),
                "removeSample");
    }

    // -------------------------------------------------------------- 收尾 -- //

    /** 退订采样/事件、清回调（可重复调用）。 */
    @Override
    public void close() {
        if (handle == 0) {
            return;
        }
        try {
            unsubscribeSamples();
            unsubscribeEvents();
        } catch (NclinkException ignored) {
            // 收尾时出错不往外抛
        }
        handle = 0;
    }

    // ---------------------------------------------------------- 回调桥 -- //

    /* 由 JNI 从原生读取线程回调；快照必须在这里做完（句柄只在回调期间有效）。 */
    void onSampleNative(String topic, long msg) {
        SampleListener listener = sampleListener;
        if (listener == null) {
            return;
        }
        try {
            listener.onSample(topic, Sample.capture(topic, msg));
        } catch (Throwable error) {
            lastCallbackError = error;
        }
    }

    void onEventNative(String topic, long msg) {
        EventListener listener = eventListener;
        if (listener == null) {
            return;
        }
        try {
            listener.onEvent(topic, Event.capture(topic, msg));
        } catch (Throwable error) {
            lastCallbackError = error;
        }
    }

    @Override
    public String toString() {
        return "DeviceClient(" + sn + ")";
    }
}
