// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

import java.io.File;
import java.net.URL;
import java.util.ArrayList;
import java.util.List;

/**
 * JNI 声明与加载。
 *
 * <p>库调用全部走 {@code nclink_jni}（它把共用的 {@code nclink_shim} 一起编了进
 * 去，见 {@code bindings/native/nclink_shim.c}）：不透明句柄用 {@code long} 传，
 * 文本按 UTF-8 走，出参用 {@code long[]} / {@code String[]} 回填。Java 侧不碰
 * C 结构体布局。
 */
final class Native {
    private Native() {
    }

    private static final String LIB_NAME = "nclink_jni";

    static {
        load();
    }

    // ------------------------------------------------------------ 加载 -- //

    private static String fileName() {
        String os = System.getProperty("os.name", "").toLowerCase();
        if (os.contains("win")) {
            return "nclink_jni.dll";
        }
        if (os.contains("mac")) {
            return "libnclink_jni.dylib";
        }
        return "libnclink_jni.so";
    }

    private static List<File> candidates() {
        List<File> dirs = new ArrayList<File>();
        String explicit = System.getenv("NCLINK_JNI");
        if (explicit != null && !explicit.isEmpty()) {
            dirs.add(new File(explicit));
        }
        String envDir = System.getenv("NCLINK_JNI_DIR");
        if (envDir != null && !envDir.isEmpty()) {
            dirs.add(new File(envDir, fileName()));
        }
        // 显式给的 -Djava.library.path 优先于仓库里的默认位置：TLS 版在 bin-tls/，
        // 两条路径同时存在时，光靠下面写死的 native/bin 永远选不到带 TLS 的那份。
        String libPath = System.getProperty("java.library.path", "");
        for (String entry : libPath.split(File.pathSeparator)) {
            if (!entry.isEmpty()) {
                dirs.add(new File(entry, fileName()));
            }
        }
        String cwd = System.getProperty("user.dir", ".");
        dirs.add(new File(cwd, fileName()));
        dirs.add(new File(new File(cwd, "bindings/java/native/bin"), fileName()));
        dirs.add(new File(new File(cwd, "native/bin"), fileName()));
        // 与 class 放在一起（拷贝部署）
        try {
            URL self = Native.class.getProtectionDomain().getCodeSource().getLocation();
            File location = new File(self.toURI());
            File dir = location.isDirectory() ? location : location.getParentFile();
            if (dir != null) {
                dirs.add(new File(dir, fileName()));
            }
        } catch (Exception ignored) {
            // 拿不到就算了，后面还有 java.library.path
        }
        return dirs;
    }

    private static void load() {
        for (File candidate : candidates()) {
            if (candidate.isFile()) {
                System.load(candidate.getAbsolutePath());
                return;
            }
        }
        // 交给系统：-Djava.library.path=bindings/java/native/bin
        System.loadLibrary(LIB_NAME);
    }

    // -------------------------------------------------------------- misc -- //

    static native String version();

    static native String errName(int code);

    static native void free(long ptr);

    static native void envSetRoot(String root);

    /** 五个语言设备端示例共用的设备模型（JSON 文本；编译在垫片里）。 */
    static native String deviceModel();

    static native String envRoot();

    static native int logInit(String dir);

    static native void logShutdown();

    static native void logSetLevel(int level);

    static native void logSetConsole(int enabled);

    // -------------------------------------------------------------- json -- //

    static native long jsonParse(String text);

    static native long jsonClone(long json);

    static native void jsonFree(long json);

    static native String jsonWrite(long json);

    static native int jsonType(long json);

    static native int jsonIsNull(long json);

    static native int jsonAsLong(long json, long[] out);

    static native int jsonAsDouble(long json, double[] out);

    static native int jsonAsBool(long json, int[] out);

    static native String jsonString(long json);

    static native String jsonText(long json);

    static native int jsonCount(long json);

    static native long jsonArrayGet(long json, int index);

    static native long jsonObjectValueAt(long json, int index);

    static native String jsonObjectKeyAt(long json, int index);

    static native long jsonObjectGet(long json, String key);

    // ------------------------------------------------------------- model -- //

    static native long modelParse(String text);

    static native void modelFree(long root);

    static native String modelWrite(long root);

    static native long modelFindById(long root, String id);

    static native int nodeType(long node);

    static native String nodeTypeName(long node);

    static native String nodeName(long node);

    static native String nodeId(long node);

    static native String nodePath(long node);

    static native String nodeDescription(long node);

    static native String nodeNumber(long node);

    static native String nodeDataType(long node);

    static native String nodeValueType(long node);

    static native String nodeMapping(long node);

    static native String nodeSource(long node);

    static native String nodeVersion(long node);

    static native String nodeGuid(long node);

    static native int nodeSettable(long node);

    static native int nodeIsSampleChannel(long node);

    static native long nodeSampleInterval(long node);

    static native long nodeUploadInterval(long node);

    static native int nodeSampleItemCount(long node);

    static native String nodeSampleItemPath(long node, int index);

    static native long nodeValue(long node);

    static native int nodeCount(long node, int kind);

    static native long nodeAt(long node, int kind, int index);

    // --------------------------------------------------- message / sample -- //

    static native long messageParse(String topic, byte[] payload);

    static native void messageFree(long msg);

    static native String messageWrite(long msg);

    static native int messageType(long msg);

    static native int sampleRows(long msg);

    static native int sampleColumns(long msg);

    static native int sampleIsComplete(long msg);

    static native String sampleId(long msg);

    static native String sampleBeginTime(long msg);

    static native long sampleInterval(long msg);

    static native long sampleUploadInterval(long msg);

    static native int sampleColumnSlots(long msg, int col);

    static native int sampleColumnPoints(long msg, int col);

    static native int sampleColumnNested(long msg, int col);

    static native String samplePath(long msg, int col);

    static native String sampleColumnEncoding(long msg, int col);

    static native String sampleHeader(long msg, String separator);

    static native long sampleValueAt(long msg, int row, int col);

    static native long sampleColumnValueAt(long msg, int col, int index);

    static native String eventId(long msg);

    static native String eventTime(long msg);

    static native String eventKey(long msg);

    static native long eventValue(long msg);

    // ------------------------------------------------------------ client -- //

    static native int open(String uri, String user, String password);

    /** 同 open，但带上 TLS 选项（json 字段见 TlsOptions；只在编了 TLS 的库里有效）。 */
    static native int openEx(String uri, String user, String password, String caFile,
                             String clientCert, String clientKey, String serverName,
                             boolean verifyPeer);

    /** 当前的原生库有没有编 TLS。 */
    static native int tlsAvailable();

    static native void close();

    static native int isOpen();

    static native long clientGet(String sn);

    static native int clientProbe(long client, int timeoutMs, long[] outModel);

    static native int clientSetRootNode(long client, long root);

    static native int clientGetValue(long client, String path, int timeoutMs,
                                     long[] outJson);

    static native int clientGetValueRange(long client, String path, int start, int end,
                                          int timeoutMs, long[] outJson);

    static native int clientGetLength(long client, String path, int timeoutMs,
                                      long[] outLength);

    static native int clientSetValue(long client, String path, String valueJson,
                                     int timeoutMs);

    static native int clientSetValueIndex(long client, String path, String valueJson,
                                          int index, int timeoutMs);

    static native int clientMethodCall(long client, String method, String paramsJson,
                                       boolean check, int timeoutMs, String[] outJson);

    static native int clientPing(long client, int timeoutMs);

    static native String clientGetId(long client, String path);

    static native String clientGetPath(long client, String id);

    static native int clientSubscribeSamples(long client, int qos, DeviceClient target,
                                             long[] outHost);

    static native int clientUnsubscribeSamples(long client, long host);

    static native int clientSubscribeEvents(long client, int qos, DeviceClient target,
                                            long[] outHost);

    static native int clientUnsubscribeEvents(long client, long host);

    static native int clientSampleCount(long client);

    static native int clientEventCount(long client);

    static native int clientAddSample(long client, String configJson, int timeoutMs);

    static native int clientRemoveSample(long client, String id, int timeoutMs);

    // ------------------------------------------------------------ server -- //

    /** 建服务器：返回 rc；句柄与回调 host 从两个出参里取。 */
    static native int serverCreate(String sn, String modelJson, String broker,
                                   String username, String password,
                                   Server publishSink, long[] outServer,
                                   long[] outHost);

    /** 同 serverCreate，但设备端可以带 TLS 选项连 {@code ssl://} / {@code tls://} broker。 */
    static native int serverCreateEx(String sn, String modelJson, String broker,
                                     String username, String password,
                                     String caFile, String clientCert, String clientKey,
                                     String serverName, boolean verifyPeer,
                                     Server publishSink, long[] outServer,
                                     long[] outHost);

    static native void serverFree(long server);

    /** 放掉一个回调 host（全局引用 + 结构体）。 */
    static native void hostFree(long host);

    static native String serverSn(long server);

    static native long serverModel(long server);

    static native String serverModelJson(long server);

    static native String serverOpenapiJson(long server, String baseUrl);

    static native int serverBindingCount(long server);

    static native int serverOperationCount(long server);

    static native int serverSampleCount(long server);

    static native int serverSampleUploadCount(long server);

    static native int serverEventCount(long server);

    static native int serverSubscribe(long server);

    static native int serverRegisterBuiltinTool(long server);

    static native int serverRegisterFileTool(long server);

    static native int serverStartFtp(long server);

    static native int serverInitSamples(long server);

    /** 注册工具：返回 rc；host 从出参取（Server 存着，close() 时 hostFree）。 */
    static native int serverRegisterTool(long server, String tool, String methodsJson,
                                         String bindingsJson, Server target,
                                         long[] outHost);

    static native int serverDispatch(long server, String topic, byte[] payload,
                                     String[] outJson);

    static native int serverInvokeMethodCall(long server, String method, String paramsJson,
                                             String[] outJson);

    static native int serverCheckMethodCall(long server, String method, String paramsJson,
                                            String[] outJson);

    static native int serverAddSample(long server, String configJson);

    static native int serverRemoveSample(long server, String id);

    static native void serverStopAllSamples(long server);

    static native int serverPushEvent(long server, String eventId, String eventJson,
                                      long timeMs, String messageId);

    // ------------------------------------------------------------- http -- //

    /** 起 HTTP：返回句柄（0 = 失败）。 */
    static native long httpStart(long server, int port, boolean withConfig);

    static native void httpFree(long http);

    static native int httpPort(long http);

    static native int httpRequestCount(long http);

    static native void httpSetCors(long http, boolean enabled);

    /** 挂自定义路由：返回 rc，host 从出参取（HttpEndpoint close 时 hostFree）。 */
    static native int httpRoute(long http, String method, String path,
                                HttpEndpoint target, long[] outHost);

    // ------------------------------------------------------------- file -- //
    /** 起进程级 FTP 端点（幂等；open 里已经起过）。 */
    static native int fileStartFtp();
    /** 同上，可换端口 / 根目录 / 账号（0 / null 用默认）。 */
    static native int fileStartFtpEx(int port, String root, String user, String password);
    /** 覆盖设备端文件通道对端的 FTP 端点（host 必填）。 */
    static native int serverSetFilePeer(long server, String host, int port, String user,
                                        String password);
    static native void fileStopFtp();
    static native int clientFileChannelOpen(long client);

    static native int clientFileChannelOpenEx(long client, String host, int port, String user,
                                              String password);

    static native int clientFileChannelClose(long client);

    static native int clientFileChannelIsOpen(long client);

    static native int clientFileWrite(long client, String localFilePath);
    static native String clientFileRead(long client, String remoteFilePath);
    static native String clientFileLlJson(long client, String remoteDir);
    static native int clientFileMkdir(long client, String remoteDir);
    static native int clientFileDelete(long client, String remoteFilePath);
    /** 带文件参数的方法调用；应答 JSON 文本从出参取。 */
    static native int clientMethodCallFile(long client, String method, String paramsJson,
                                           String keysJson, String pathsJson,
                                           int timeoutMs, String[] outJson);
    static native int fileNeedCompression(String fileName);
    static native int fileTotalChunks(long size);
    static native String fileChecksum(String path);
    static native String fileAttributeJson(String path, String parent);
}
