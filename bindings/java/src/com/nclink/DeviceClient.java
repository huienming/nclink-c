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

    // -------------------------------------------------- 异步方法调用 -- //

    /**
     * 异步方法调用：立刻回一个应答（{@code code=OK} + {@code handler}），方法在设备端
     * 线程池里跑。用 {@link #methodStatus} / {@link #methodResult} 拿那个句柄查。
     */
    public Json methodCallAsync(String method, String paramsJson, int timeoutMs) {
        String[] out = new String[1];
        NclinkException.check(
                Native.clientMethodCallAsync(requireOpen(), method, paramsJson,
                                             timeoutMs, out),
                "methodCallAsync");
        return Json.parse(out[0]);
    }

    public Json methodCallAsync(String method) {
        return methodCallAsync(method, null, 5000);
    }

    /** 按句柄查异步调用的状态（process / status / code）。 */
    public Json methodStatus(String objectId, String handler, int timeoutMs) {
        String[] out = new String[1];
        NclinkException.check(
                Native.clientMethodStatus(requireOpen(), objectId, handler,
                                          timeoutMs, out),
                "methodStatus");
        return Json.parse(out[0]);
    }

    /**
     * 按句柄查异步调用的结果：未完成 {@code code=PENDING}（没有 result），完成后
     * {@code code} + {@code return} + {@code result}（finished / error）并释放句柄。
     */
    public Json methodResult(String objectId, String handler, int timeoutMs) {
        String[] out = new String[1];
        NclinkException.check(
                Native.clientMethodResult(requireOpen(), objectId, handler,
                                          timeoutMs, out),
                "methodResult");
        return Json.parse(out[0]);
    }

    // ------------------------------------------------------ 文件通道 -- //

    /**
     * 开文件通道（file/openFileChannel）：把托管侧的 FTP 端点交给设备，设备随即
     * 往那儿拨 FTP 传字节。
     *
     * <p>不传参就是默认：地址 = 到 broker 的本机地址（拿不到就 127.0.0.1）、端口 =
     * 进程级端点端口（没起就按 2323 起）、账号 = 库临时生成的一对（关闭时撤销）。
     * 对端不在这台机器上、或端口有映射时用 {@link #openFileChannel(String, int,
     * String, String)}。
     *
     * <p>幂等；通道是租约，{@link #closeFileChannel()} 之前一直是这条对端。上传/
     * 下载/列目录/建目录/删除这些便利方法也会在没通道时自动开一次。
     */
    public void openFileChannel() {
        NclinkException.check(Native.clientFileChannelOpen(requireOpen()),
                              "openFileChannel");
    }

    /** 同上，但显式给出设备要拨的 host / port 和账号（host、port 必填）。 */
    public void openFileChannel(String host, int port, String username, String password) {
        NclinkException.check(
                Native.clientFileChannelOpenEx(requireOpen(), host, port, username,
                                               password),
                "openFileChannel");
    }

    /** 收回文件通道：设备停用它的 FTP 连接，库给这条通道加的账号一并撤销（幂等）。 */
    public void closeFileChannel() {
        NclinkException.check(Native.clientFileChannelClose(requireOpen()),
                              "closeFileChannel");
    }

    /** 这条客户端手上有没有文件通道。 */
    public boolean fileChannelIsOpen() {
        return Native.clientFileChannelIsOpen(requireOpen()) != 0;
    }

    /**
     * 上传 {@code relativePath}（形如 {@code /demo.txt}）：文件必须已经在
     * {@code <当前目录>/<sn><relativePath>} 上——用 {@link #uploadLocalFile} 就不用
     * 操心这件事。
     */
    public void uploadFile(String relativePath) {
        NclinkException.check(Native.clientFileWrite(requireOpen(), relativePath),
                              "uploadFile");
    }

    /** 把本地文件传过去；{@code relativePath} 省略时用本地文件名。 */
    public void uploadLocalFile(java.io.File localPath, String relativePath)
            throws java.io.IOException {
        if (relativePath == null || relativePath.isEmpty()) {
            relativePath = "/" + localPath.getName();
        }
        java.io.File staged = new java.io.File(stagedPath(relativePath));
        java.io.File parent = staged.getParentFile();
        if (parent != null) {
            parent.mkdirs();
        }
        if (!staged.getAbsoluteFile().equals(localPath.getAbsoluteFile())) {
            java.nio.file.Files.copy(localPath.toPath(), staged.toPath(),
                                     java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        }
        uploadFile(relativePath);
    }

    public void uploadLocalFile(java.io.File localPath) throws java.io.IOException {
        uploadLocalFile(localPath, null);
    }

    /** 下载文件，返回落盘后的本地绝对路径（在 {@code <当前目录>/<sn>/} 下面）。 */
    public String downloadFile(String relativePath) {
        String local = Native.clientFileRead(requireOpen(), relativePath);
        if (local == null || local.isEmpty()) {
            throw new NclinkException(-1, "downloadFile", "下载失败: " + relativePath);
        }
        return local;
    }

    /** 下载并复制到 {@code localPath}；返回落点绝对路径。 */
    public String downloadTo(String relativePath, java.io.File localPath)
            throws java.io.IOException {
        java.io.File staged = new java.io.File(downloadFile(relativePath));
        java.io.File parent = localPath.getParentFile();
        if (parent != null) {
            parent.mkdirs();
        }
        java.nio.file.Files.copy(staged.toPath(), localPath.toPath(),
                                 java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        return localPath.getAbsolutePath();
    }

    /** 列设备上的某个目录（默认根 "/"）。 */
    public java.util.List<FileInfo> listFiles(String remoteDir) {
        return FileInfo.parseList(Native.clientFileLlJson(requireOpen(),
                                                          remoteDir == null ? "/"
                                                                            : remoteDir));
    }

    public java.util.List<FileInfo> listFiles() {
        return listFiles("/");
    }

    /** 在设备上建目录。 */
    public void makeDirectory(String remoteDir) {
        NclinkException.check(Native.clientFileMkdir(requireOpen(), remoteDir),
                              "makeDirectory");
    }

    /** 删设备上的文件或目录（目录递归删）。 */
    public void deleteFile(String remotePath) {
        NclinkException.check(Native.clientFileDelete(requireOpen(), remotePath),
                              "deleteFile");
    }

    /**
     * 带文件参数的方法调用：{@code keys} 与 {@code paths} 一一对应，参数里那些键会
     * 被换成 "/temp/&lt;名字&gt;"（文件经文件通道送过去），应答里 "fileKeys" 列出的
     * 值会被换成本地路径。
     */
    public Json methodCallFile(String method, String paramsJson, String[] keys,
                               String[] paths) {
        String[] out = new String[1];
        NclinkException.check(Native.clientMethodCallFile(requireOpen(), method,
                                                          paramsJson,
                                                          jsonArray(keys),
                                                          jsonArray(paths), 5000,
                                                          out),
                              "methodCallFile");
        return Json.parse(out[0]);
    }

    /** 文件通道的暂存位置：{@code <当前目录>/<sn><相对路径>}。 */
    public String stagedPath(String relativePath) {
        String root = new java.io.File(System.getProperty("user.dir"), sn())
                .getAbsolutePath();
        if (relativePath == null || relativePath.isEmpty()) {
            return root;
        }
        String tail = relativePath.replace('\\', '/');
        while (tail.startsWith("/")) {
            tail = tail.substring(1);
        }
        return root + java.io.File.separator
                + tail.replace('/', java.io.File.separatorChar);
    }

    private static String jsonArray(String[] values) {
        StringBuilder builder = new StringBuilder("[");
        if (values != null) {
            for (int i = 0; i < values.length; i++) {
                if (i > 0) {
                    builder.append(',');
                }
                builder.append(JsonValues.quoted(values[i]));
            }
        }
        return builder.append(']').toString();
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
