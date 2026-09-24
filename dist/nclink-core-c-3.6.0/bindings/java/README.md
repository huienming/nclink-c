# NC-Link Java 绑定（JNI）

`nclink-core-c` 的 Java 封装：**零第三方依赖**（不用 Maven / Gradle，也不引 JSON
库），JDK 8 以上都能跑（编译出的字节码是 Java 8）。

```
examples/sdk/java/
  native/nclink_jni.c           JNI 胶水：Java 字符串 <-> UTF-8、句柄、回调
  native/build-native.ps1/.sh   编出 nclink_jni.dll / libnclink_jni.so
  stack/src/com/nclink/               托管封装（Nclink / DeviceClient / Json / Model ...）
  demo/com/nclink/demo/          控制台示例 ClientDemo
  stack/test/com/nclink/SelfTest.java 自检（不需要 broker）
  build.ps1 / build.sh          一条命令：native + javac + 自检
```

## 为什么要垫片

C 库是静态库、没有导出符号，而且 Java 侧不该依赖 C 结构体的内存布局（库里改个字段
就静默错位）。所以三种托管绑定（C# / Java / Python）共用
`examples/sdk/native/nclink_shim.c` 这层扁平的 C ABI：

- 只暴露三样东西：不透明句柄（`long`）、标量、UTF-8 文本；
- 报文/模型/JSON 都由库自己编解码，Java 侧不做 JSON 解析；
- 回调把 `const ncl_message*` 作为不透明句柄抛回 `DeviceClient`，Java 侧在回调里
  立刻拷成快照（`Sample` / `Event`），出了回调照样能用。

`nclink_jni` 把垫片一起编进去了，所以只需要一个原生库文件。

## 构建

```powershell
# 1) 先编核心静态库（仓库根）
.\build.ps1

# 2) Java 绑定：native + javac + 自检（107 项，不需要 broker）
powershell -ExecutionPolicy Bypass -File .\bindings\java\build.ps1

# 3) 想连真 broker 再跑一遍端到端（设备端 + 客户端同进程，报文真的过 MQTT；12 项）
$env:NCLINK_TEST_BROKER = "tcp://127.0.0.1:1883"
powershell -ExecutionPolicy Bypass -File .\bindings\java\build.ps1

# 4) TLS 端到端（再 +1 项：设备端与客户端都走 ssl://；要带 TLS 的库 + JNI 库）
$env:NCLINK_TEST_TLS_BROKER = "ssl://127.0.0.1:18832"
$env:NCLINK_TEST_TLS_CA = "stack/test/data/tls_localhost_cert.pem"
java "-Djava.library.path=examples/sdk/java/native/bin-tls" `
     -cp examples/sdk/java/build/classes com.nclink.BrokerE2E
```

Linux：

```sh
./build-linux.sh build-linux        # 出 build-linux/libnclink_core.a
./examples/sdk/java/build.sh            # 需要 JAVA_HOME（找 jni.h）
```

原生库的位置（`Native` 的加载顺序）：

1. 环境变量 `NCLINK_JNI` 指向的文件；
2. `NCLINK_JNI_DIR` 目录下的 `nclink_jni.dll` / `libnclink_jni.so`；
3. 显式给的 `-Djava.library.path=...`（TLS 版就靠它指到 `bin-tls/`，见下文）；
4. 当前目录、`examples/sdk/java/native/bin/`、`native/bin/`、class 文件旁边；
5. 交给系统：`System.loadLibrary`。

没有 Maven/Gradle 也能用：把 `src`（或编译出的 classes/jar）和 `nclink_jni.dll`
放进你的工程即可。

## 用法

```java
import com.nclink.*;

Nclink.logInit();                                  // 日志走 <root>/log/out.txt
Nclink.init("tcp://127.0.0.1:1883");               // 进程级连接，一次

try (DeviceClient device = Nclink.getDevice("V2023A7B762")) {
    try (Model model = device.probe()) {           // 拉模型（顺带装进客户端）
        System.out.println(model.root().id() + " " + model.root().name());
        for (Node item : model.root().devices().get(0).dataItems()) {
            System.out.println(item.path() + " -> " + item.id());
        }
        System.out.println(device.getId("/MACHINE/STATUS") + " " + device.getPath("010302"));
    }

    try (Json value = device.getValue("/MACHINE/STATUS")) {  // 读值
        System.out.println("STATUS = " + value.toJavaObject());
    }
    device.setValue("/MACHINE/STATUS", "42");                // 写值（JSON 文本）
    try (Json window = device.getValueRange("/MACHINE/PART_COUNT", 0, 9)) { /* ... */ }
    try (Json reply = device.methodCall("/plc/getCount", null, true, 5000)) {
        System.out.println(reply.encode());          // {"code":"OK", ...}
    }
    device.ping();

    device.subscribeSamples(2, (topic, sample) -> System.out.println(sample.rows()));
    device.subscribeEvents(2, (topic, event) -> System.out.println(event.key()));
    Thread.sleep(5000);
    System.out.println(device.sampleCount() + " / " + device.eventCount());
}
Nclink.shutdown();
```

跑法（对着设备端示例，另开一个窗口）：

```powershell
build\examples\ncl_device_demo.exe D:\sim-java 30
java -Djava.library.path=bindings\java\native\bin -cp bindings\java\build\classes `
     com.nclink.demo.ClientDemo tcp://127.0.0.1:1883 <设备SN> 6
```

> **中文输出**：JDK 17 及更早版本在非 UTF-8 区域（Windows 控制台、Linux 容器里的
> POSIX locale）默认按 ASCII 编码打印，中文会变成 `?`。加上
> `-Dfile.encoding=UTF-8`（Java 18+ 已经默认 UTF-8，不用加）即可；
> `build.ps1` / `build.sh` 跑自检时已经带上了这个参数。

### 采样怎么读

外层是**槽位**（通道的 `sampleInterval` 一槽），内层是"每槽装几个点"——采样率不同的
数据项可以放在**同一个通道**里（1 ms 的列每槽 1 点、0.25 ms 的列每槽 4 点）。按行
读最省事：

```java
device.subscribeSamples(2, (topic, sample) -> {
    System.out.println(sample.id() + " " + sample.intervalMs() + "ms");
    for (SampleColumn column : sample.columns()) {     // path / slots / points / isNested
        System.out.println(column.path() + " " + column.slots() + " " + column.points());
    }
    for (int row = 0; row < sample.rows(); row++) {    // 行数 = 数据最多的那一列
        for (int col = 0; col < sample.columns().size(); col++) {
            Object value = sample.valueAt(row, col);   // 粗列取"覆盖该行的第一个点"
            double number = sample.getDouble(row, col);
        }
    }
});
```

### 解析报文

自己从别处拿到的报文（离线回放、日志、文件）也能按库的规则解码：

```java
Object message = Nclink.parse("Sample/V203243111F/s1", payloadBytes);
if (message instanceof Sample) {
    System.out.println(((Sample) message).header(" "));
} else if (message instanceof Event) {
    Event event = (Event) message;
    System.out.println(event.key() + "=" + event.value());
}
```

## 设备端（Java 当一台机床）

`com.nclink.Server` 把设备端（`ncl_server`）也包了：注册工具方法、把模型路径绑到
方法上、启动采样通道、推事件。MQTT 由库负责（也可以完全不接 broker）。

```java
try (Server device = new Server("V2JAVA00001", modelJson, "tcp://127.0.0.1:1883")) {
    device.registerTool(
        "plc",
        new String[] {"getStatus", "setCount"},
        new Server.Binding[] {
            new Server.Binding("/MACHINE/STATUS", Operation.GET_VALUE, "getStatus"),
            new Server.Binding("/MACHINE/PART_COUNT", Operation.SET_VALUE, "setCount")},
        (method, params) -> "getStatus".equals(method) ? 1
                               : ((Map<?, ?>) params.toJavaObject()).get("value"));
    device.registerBuiltinTool();       // addSample / removeSample
    device.subscribe();                 // 订阅 6 个请求主题
    device.initSamples();               // 启动模型里声明的采样通道
    device.pushEvent("010307", "{\"key\":\"PART_COUNT\",\"value\":7}");
    System.out.println(device.operationCount() + " 个操作，采样通道 " + device.sampleCount());
}
```

- 处理函数返回要应答的值：`Json` 原样、`Map`/`List`/数字/布尔会序列化成 JSON、
  **`String` 按 JSON 文本处理**（要回字符串就返回 `Map` 或 `Json.parse("\"...\"")`）、
  **返回 null = 没有值**（库按 NG 应答，与 C API 一致）；抛异常 → 该次调用按错误应答、
  异常文本进 reason，异常本身记在 `device.lastCallbackError()` 上。
- 方法带 schema（`Map<String,String>` 的 `{方法名: schema JSON}`）时，`check` 会按
  schema 校验；没有 schema 的方法按库的规则只接受空参数。
- 不接 broker 时（`broker=null`）用 `device.dispatch(topic, payload)` 或
  `device.invokeMethodCall(method, paramsJson)` 离线驱动；给一个 `PublishSink` 就能
  自己当传输。
- 示例：`com.nclink.demo.DeviceDemo tcp://127.0.0.1:1883 V2JAVA00001 30`（4 个方法 +
  一个采样通道 + 每秒一条事件；仓库里任意客户端都能读它，例如
  `build\examples\ncl_client_demo.exe tcp://127.0.0.1:1883 V2JAVA00001 8`）。

### HTTP / REST 端点

端点的内容全在库里：`GET /api/schema`（OpenAPI 3.0 文档）、`GET /swagger-ui`
（浏览器里直接调工具方法）、`POST /api/<工具>/<方法>`（等价于 `methodCall`），
配置端点（SN / 模型 / 驱动 / mqtt.cfg / 服务器列表）由 `withConfig=true` 挂上。

```java
HttpEndpoint http = device.startHttp(9008, true);          // 0 = 随机端口
http.route("GET", "/api/hello", (method, path, query, body) ->
        HttpEndpoint.Reply.json("{\"query\":\"" + query + "\"}"));
System.out.println(http.url() + "/swagger-ui");
System.out.println(http.requestCount());
http.close();                    // 幂等；device.close() 也会替你收
```

- `Reply.text/json/of(status, type, body)/status(code)`；处理函数返回 `null` = 404。
- `method` 支持 `"*"`；`path` 以 `/api/` 开头时是前缀匹配，否则要求完全相等。
- **关端点要在关服务器之前**（路由回调还挂在服务器上），`device.close()` 已经按这个
  顺序做了。
- 设备端示例会把它挂起来：`DeviceDemo ... [HTTP端口]`，然后
  `curl -X POST http://127.0.0.1:9008/api/plc/getCount -d '{}'` 就能调工具方法。

### 异步方法调用（长方法）

设备端方法可能跑很久，所以请求可以带 `async`：设备立刻回 `code=OK` + `handler`
（方法句柄，代表那个线程/任务），方法在设备端线程池里跑，随后按句柄查进度与结果。

```java
try (DeviceClient client = Nclink.getDevice("V2JAVA00001")) {
    try (Json ack = client.methodCallAsync("/plc/grind", "{\"depth\":3}", 5000)) {
        String handler = ack.get("handler").asString();              // code=OK + handler
        try (Json status = client.methodStatus(client.sn(), handler, 5000)) {
            System.out.println(status.get("status").asString());     // executing / stopped / ...
        }
        Json result;
        while ((result = client.methodResult(client.sn(), handler, 5000)) != null
                && "PENDING".equals(result.get("code").asString())) {
            result.close();
            Thread.sleep(50);
        }
        System.out.println(result.get("result").asString());         // finished / error
        result.close();
    }
}
```

- 结果取走后句柄就释放了（再查同句柄是 `NG`）。
- 设备侧不用做异步的事：工具方法就是普通函数；想报进度用
  `server.reportMethodProgress(handler, process, status)`（可选），离线自检用
  `invokeMethodCallAsync` / `invokeMethodStatus` / `invokeMethodResult`。

### 文件通道（上传 / 下载）

MQTT 报文里只传 `/temp/<名字>` 这样的**令牌**，字节走 FTP。方向要记住：**设备是
FTP 客户端**，本机是 FTP 服务端（进程级端点 127.0.0.1:2323、admin / 123456、根 =
安装根）。传字节之前先**握手**：`client.openFileChannel()` 把端点交给设备
（file/openFileChannel），设备随即往那儿拨 FTP；`client.closeFileChannel()` 收回
租约并撤销库给这条通道加的临时账号。下面的上传/下载等便利方法会自己确保通道开着。

```java
// 设备端（收文件的那一边）
try (Server device = new Server("V2JAVA00001", modelJson, broker)) {
    device.registerFileTool();       // /CONTROLLER/FILE：write/read/ll/mkdir/delete
    device.subscribe();
}

// 客户端（上位机那一侧）
try (DeviceClient client = Nclink.getDevice("V2JAVA00001")) {
    client.openFileChannel();        // 握手：设备往这台机器的 FTP 端点拨
    client.uploadLocalFile(new File("report.txt"), "/data/report.txt");
    for (FileInfo item : client.listFiles("/data")) {
        System.out.println(item.fileName() + " " + item.fileSize() + " 字节");
    }
    client.downloadTo("/data/report.txt", new File("back.txt"));
    client.closeFileChannel();       // 收回租约（幂等；不调也会随 Nclink.shutdown 收掉）
    client.makeDirectory("/docs");
    client.deleteFile("/data/report.txt");

    // 带文件参数的方法调用：keys 与 paths 一一对应，应答里的 fileKeys 会被换成本地路径
    try (Json reply = client.methodCallFile("plc/convert", "{\"input\":\"\"}",
                                            new String[] {"input"},
                                            new String[] {"a.bin"})) { }
}

// 本地文件小工具（不需要 broker）
Nclink.fileNeedCompression("model.json");  // true
Nclink.fileTotalChunks(300 * 1024);        // 2
Nclink.fileChecksum("report.txt");         // SHA-256 十六进制
Nclink.fileAttribute("report.txt");        // com.nclink.FileInfo
```

- `uploadFile(relative)` 只是"把已经在 `<当前目录>/<sn><relative>` 上的文件传上去"
  （与 C API 一致）；`uploadLocalFile()` 会先替你摆到那个位置。
- 下载回来的文件先落在 `<当前目录>/<sn>/` 下，`downloadFile()` 返回绝对路径，
  `downloadTo()` 再替你复制到目标。
- 通道里广播的地址默认是"到 broker 的本机地址" + 进程级端点端口（拿不到配置就
  127.0.0.1）——本机与 broker 是同一台机器时开箱即用。
- 设备侧的 `startFtp()` 是"设备自己也开个 FTP 端点"（读 `bin/ftp.txt`），客户端传文件
  用不到它；缺文件时它抛 `NclinkException`，示例里是容忍着来的。
- 本机不跟 broker 同机（或者端口/账号不一样）时改用带参数的握手：

```java
client.openFileChannel("10.0.0.7", 2323, null, null);      // 设备拨到这台机器的 2323
```

  地址不想写死在代码里就放 `<root>/conf/ftp.txt`（可省文件，键全可选：
  `host` / `port` / `advertisePort` / `root` / `userName` / `password` / `path` /
  `force`）；函数参数优先于它，它优先于推导默认。

  设备端也可以不握手，直接钉一个静态 FTP 对端（要求对端自己跑 FTP 服务端、目录布局
  是 `/<sn>/...`）：

```java
device.setFilePeer("10.0.0.7", 2323, null, null);          // 设备端指到上位机的 FTP
Nclink.startFileServer(2323, "D:/files", "admin", "123456");   // 本机端点换端口/根/账号
```

### TLS（ssl://）

```java
if (!Nclink.tlsAvailable()) { /* 这个 JNI 库没带 TLS：见下面"TLS 构建" */ }

Nclink.init("ssl://broker.example.com:8883", null, null,
        new TlsOptions()
                .caFile("C:/certs/ca.pem")                 // 内网 CA；不设 = 平台信任库
                .clientCert("C:/certs/client.pem", "C:/certs/client.key")  // 双向 TLS
                .serverName("broker.example.com")          // 不设 = URL 里的主机名
                .verifyPeer(true));                        // 默认就是 true

// 设备端直连 ssl:// broker 也支持：
new Server(sn, modelJson, "ssl://broker.example.com:8883", null, null, null,
           new TlsOptions().caFile("C:/certs/ca.pem"));
```

**TLS 构建**（库与 JNI 库都得带 TLS）：

```powershell
.\build.ps1 -Tls                                     # 出 build-tls\nclink_core.lib
.\bindings\java\native\build-native.ps1 -Tls       # 出 bindings\java\native\bin-tls\nclink_jni.dll
```
跑的时候把原生库指到那个目录：`-Djava.library.path=examples/sdk/java/native/bin-tls`（或
`NCLINK_JNI_DIR=examples/sdk/java/native/bin-tls`）。OpenSSL 的两个 DLL 已经拷进
`bin-tls/`，不用再动 PATH。

库没带 TLS 时用 `ssl://` 会拿到明确的 `NOT_SUPPORTED`（-8）。

## 内存与线程

| 对象 | 谁释放 |
|------|--------|
| `Json`（`parse` / `clone` / `getValue` / `methodCall` / `getValueRange`） | **自有**：`close()` 或 try-with-resources |
| `Model`（`probe` / `parse`） | **自有**，同上 |
| `DeviceClient` | `close()`（退订 + 清回调；断开 MQTT 是 `Nclink.shutdown()`） |
| `Node`、`Json` 的下标/成员视图 | **借用**：持有宿主引用，不用关 |
| `Sample` / `SampleColumn` / `Event` / `Message` | 纯 Java 快照，没有句柄 |
| `Server` | **自有**：`close()`（停采样/FTP、断开 MQTT、放掉所有回调） |
| `HttpEndpoint` | **自有**：`close()`（幂等；`server.close()` 也会收） |
| `Server.model()` | **借用**：服务器活着就有效，不用关 |
| `FileInfo` | 纯 Java 快照，没有句柄 |

Java 没有可靠的析构钩子，所以**自有的对象必须显式 close**（忘了就漏到进程结束，不会
崩）。采样/事件回调在客户端自己的**读取线程**上触发：JNI 会把该线程挂到 JVM 上、
回调结束再摘下来，回调里别做耗时操作；回调抛出的异常记在
`device.lastCallbackError()` 上，不会穿回原生层。`init` / `shutdown` / `getDevice`
是线程安全的（底层有锁）；`subscribe*` / `unsubscribe*` 请在同一个线程里成对调用。

> 文本按 UTF-8 走（JNI 的"修改版 UTF-8"对 BMP 字符与标准 UTF-8 一致，中文没问题；
> 只有 emoji 这类**增补平面**字符会被编成 CESU-8，跨语言时建议避开）。

## 示例输出

```
connected: tcp://127.0.0.1:1883 (nclink 3.1.0)
probe: model root id=01 name=机床模型文件
各轴的功率与主轴振动（路径 含义）:
/AXIS@X/POWER@1          X轴功率
/AXIS@S/ACCELERATION@Z   主轴加速度（Z 向）
GET /STATUS = 1
SET /STATUS = 42 ok
id(/STATUS) = 010302, path(010302) = /STATUS
methodCall(check) = {"@id":"...","code":"OK","method":"/plc/getCount","check":true}
subscribed: Sample/V24A92C126D/# and Event/V24A92C126D
sample Sample/V24A92C126D/EdgeSersors: id=EdgeSersors interval=1ms upload=100ms columns=12 rows=400
  /AXIS@X/POWER@1: 100 个槽位 × 每槽约 1 点 = 100 点
  /AXIS@S/ACCELERATION@X: 100 个槽位 × 每槽约 4 点 = 400 点（批量）
  行[0] /AXIS@X/POWER@1=800.0  /AXIS@S/ACCELERATION@X=-1.0  /AXIS@Y/POWER@1=1137.5 ...
event Event/V24A92C126D: key=PART_COUNT value=50
received 35 samples, 6 events
```

## 还没做的

- Maven / Gradle 坐标与 jar 打包：现在是"源码 + 一个原生库"直接用；要发包得把
  `nclink_jni.dll` / `libnclink_jni.so` 按平台打进 jar 或另发。
- 零拷贝读采样：现在回调里给的是纯 Java 快照（方便、安全）；需要极致吞吐可以加一个
  "只在回调期间有效"的借用视图 API。
