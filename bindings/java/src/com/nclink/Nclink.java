// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

/**
 * 绑定入口：进程级初始化 / 关闭、日志、安装根目录、按 SN 取设备客户端。
 *
 * <pre>{@code
 * Nclink.init("tcp://127.0.0.1:1883");
 * try (DeviceClient device = Nclink.getDevice("V2023A7B762")) {
 *     try (Model model = device.probe()) { ... }
 *     try (Json value = device.getValue("/STATUS")) { ... }
 *     device.subscribeSamples(0, (topic, sample) -> System.out.println(sample.rows()));
 * }
 * Nclink.shutdown();
 * }</pre>
 */
public final class Nclink {
    private Nclink() {
    }

    /** C 库版本号（如 3.1.0）。 */
    public static String version() {
        return Native.version();
    }

    /** 进程级连接是否已经建好。 */
    public static boolean isOpen() {
        return Native.isOpen() != 0;
    }

    /**
     * 建进程级连接（内部连 broker 并起客户端管理器）。一个进程调一次，配
     * {@link #shutdown()}。
     *
     * @param uri 例如 tcp://127.0.0.1:1883 或 ssl://host:8883
     */
    public static void init(String uri) {
        init(uri, null, null);
    }

    public static void init(String uri, String username, String password) {
        if (uri == null || uri.isEmpty()) {
            throw new IllegalArgumentException("uri 不能为空，例如 tcp://127.0.0.1:1883");
        }
        NclinkException.check(Native.open(uri, username, password), "init");
    }

    /** 断开连接、释放所有客户端（没连过就是空操作）。 */
    public static void shutdown() {
        if (isOpen()) {
            Native.close();
        }
    }

    /** 取（必要时创建）某个 SN 的设备客户端；同一个 SN 只有一份。 */
    public static DeviceClient getDevice(String sn) {
        if (sn == null || sn.isEmpty()) {
            throw new IllegalArgumentException("sn 不能为空");
        }
        long client = Native.clientGet(sn);
        if (client == 0) {
            throw new NclinkException(-6, "getDevice", "取设备客户端失败: " + sn
                    + "（先调 init？）");
        }
        return new DeviceClient(sn, client);
    }

    /** 安装根目录（conf/、bin/、log/ 都在它下面）。 */
    public static void setRootDirectory(String path) {
        Native.envSetRoot(path);
    }

    /** 当前的安装根目录。 */
    public static String rootDirectory() {
        return Native.envRoot();
    }

    /** 初始化日志；目录为空时用 &lt;root&gt;/log。 */
    public static boolean logInit(String directory) {
        return Native.logInit(directory) != 0;
    }

    public static boolean logInit() {
        return logInit(null);
    }

    /** 关掉日志（写盘线程回收）。 */
    public static void logShutdown() {
        Native.logShutdown();
    }

    /** 日志级别。 */
    public static void setLogLevel(LogLevel level) {
        Native.logSetLevel(level.code());
    }

    /** 是否同时往控制台打（默认开）。 */
    public static void setConsoleLog(boolean enabled) {
        Native.logSetConsole(enabled ? 1 : 0);
    }

    /**
     * 把一条 MQTT 报文按库的规则解码：采样 -&gt; {@link Sample}，事件 -&gt;
     * {@link Event}，其它 -&gt; {@link Message}。
     */
    public static Object parse(String topic, byte[] payload) {
        if (topic == null || payload == null) {
            throw new IllegalArgumentException("topic / payload 都不能为空");
        }
        long message = Native.messageParse(topic, payload);
        if (message == 0) {
            throw new NclinkException(-3, "parse", "报文解析失败: " + topic);
        }
        try {
            MessageType type = MessageType.of(Native.messageType(message));
            if (type == MessageType.SAMPLE) {
                return Sample.capture(topic, message);
            }
            if (type == MessageType.EVENT) {
                return Event.capture(topic, message);
            }
            return new Message(topic, type, Native.messageWrite(message));
        } finally {
            Native.messageFree(message);
        }
    }

    // ---------------------------------------------------------- 文件通道 -- //

    /**
     * 起进程级 FTP 端点（127.0.0.1:2323，admin / 123456，根 = 安装根）。
     *
     * <p>文件通道里**设备是 FTP 客户端**，本机得有 FTP 服务端等着它来取/送；
     * {@link #init} 时已经起过了，这里是给"先要文件后连 broker"的场合用的（幂等）。
     */
    public static void startFileServer() {
        NclinkException.check(Native.fileStartFtp(), "startFileServer");
    }

    /** 停掉进程级 FTP 端点。 */
    public static void stopFileServer() {
        Native.fileStopFtp();
    }

    /** 这个扩展名的文件传输时要不要压缩（文本类为 true）。 */
    public static boolean fileNeedCompression(String fileName) {
        return Native.fileNeedCompression(fileName) != 0;
    }

    /** 按 256 KB 一片算，这个字节数要几片。 */
    public static int fileTotalChunks(long size) {
        return Native.fileTotalChunks(size);
    }

    /** 本地文件内容的 SHA-256（小写十六进制）；读不了返回 null。 */
    public static String fileChecksum(String path) {
        return Native.fileChecksum(path);
    }

    /** 本地文件/目录的属性（目录的 fileType 为 1）；拿不到返回 null。 */
    public static FileInfo fileAttribute(String path, String parent) {
        String json = Native.fileAttributeJson(path, parent);
        if (json == null || json.isEmpty()) {
            return null;
        }
        Json value = Json.parse(json);
        try {
            return FileInfo.fromJavaObject(value.toJavaObject());
        } finally {
            value.close();
        }
    }

    public static FileInfo fileAttribute(String path) {
        return fileAttribute(path, null);
    }
}
