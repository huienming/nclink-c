# NC-Link C# 绑定

`nclink-core-c` 的 .NET 封装：**客户端 + 设备端（Server）+ HTTP/REST 端点 + 文件通道**，
同一份代码同时支持 .NET Framework 与 .NET（Core）。

| 目标框架 | 说明 |
|----------|------|
| `netstandard2.0` | .NET Framework 4.6.1+ / .NET Core 2.0+ / .NET 5+ 都能引用（默认给 NuGet 用这个） |
| `net472` | .NET Framework 4.7.2 专用产物（老项目直接引） |
| `net8.0` | .NET 8 |

不含第三方依赖：JSON 直接用库自己的解析器/序列化器，所以 .NET Framework 上不需要
Newtonsoft.Json，.NET 8 上也不需要 System.Text.Json。

## 目录

```
bindings/csharp/
  src/Nclink.Core/            托管封装（多目标：netstandard2.0 / net472 / net8.0）
  samples/Nclink.Demo.Cli/    客户端示例（net472 + net8.0 两个产物）
  samples/Nclink.Demo.Device/ 设备端示例（"这个进程就是一台机床"）
  tests/Nclink.SelfTest/      自检（不需要 broker）：106 项检查
  build.ps1                   一键构建：垫片 + 三个工程 + 自检
```

## 为什么要垫片

C 库是静态库、没有导出宏，而且托管侧不该依赖 C 结构体的内存布局（库里改个字段就会
静默错位）。所以中间加一层 `nclink_shim`（源码在 `bindings/native/`，**C#/Java/Python
三份绑定共用同一份**）：

- 只暴露三样东西：不透明句柄（`void*`）、标量、UTF-8 文本；
- 报文/模型/JSON 都用**库自己的**编解码器，托管侧不做 JSON 解析（`NclJson` 只是
  这些句柄的薄封装）；
- 回调把 `const ncl_message*` 作为不透明句柄传给托管侧，托管侧立刻拷成托管对象
  （见"采样与事件"）。

## 构建

```powershell
# 一条命令全做完（先编核心静态库 .\build.ps1，再跑这个；需要 .NET SDK 8+）
powershell -ExecutionPolicy Bypass -File .\bindings\csharp\build.ps1
# 等价于：垫片 + 三个工程 + 自检；-SkipNative 只编托管、-NoTest 不跑自检
```

Linux：

```sh
./build-linux.sh build-linux               # 出 build-linux/libnclink_core.a
sh bindings/native/build-shim.sh           # 出 bindings/native/bin/libnclink_shim.so
dotnet build bindings/csharp/src/Nclink.Core/Nclink.Core.csproj -c Release
dotnet run --project bindings/csharp/tests/Nclink.SelfTest -c Release    # 自检
```

`nclink_shim.dll` / `libnclink_shim.so` 必须和你的程序在同一个目录（或者在 `PATH` /
`LD_LIBRARY_PATH` 上）；几个工程的 csproj 已经把 `bindings/native/bin/` 下的那个作为
`None ... CopyToOutputDirectory` 拷进输出目录了。

## 用法

先看客户端（`NclDeviceClient`，进程级连接一次、按 SN 拿设备），再看设备端
（`NclServer` + `NclHttpEndpoint`，"这个进程就是一台机床"）。

### 客户端

```csharp
using Nclink;

Nclink.LogInit();
Nclink.Init("tcp://127.0.0.1:1883");            // 进程级连接，一个进程一次

using (NclDeviceClient device = Nclink.GetDevice("V2023A7B762"))
{
    using (NclModel model = device.Probe())     // 拉模型
    {
        foreach (NclNode item in model.Root.Devices) { /* 遍历设备/组件/数据项 */ }
        string id = device.GetId("/MACHINE/AXIS@X/POWER");   // 路径 → 节点 id
    }

    long status = device.GetLong("/MACHINE/STATUS");         // getValue
    device.SetValue("/MACHINE/STATUS", "42");                // setValue（值用 JSON 文本）
    using (NclJson range = device.GetValueRange("/MACHINE/PART_COUNT", 0, 9)) { }
    using (NclJson reply = device.MethodCall("/plc/setValue",
                                             "{\"value\":99999}", check: true)) { }

    device.SampleReceived += (s, e) => Console.WriteLine(e.Sample);
    device.EventReceived += (s, e) => Console.WriteLine(e.Event);
    device.SubscribeSamples();                       // Sample/<sn>/#
    device.SubscribeEvents();                        // Event/<sn>

    System.Threading.Thread.Sleep(5000);
}                                                    // Dispose = 退订 + 清回调桥

Nclink.Shutdown();
```

### 设备端（这个进程就是一台机床）

设备端**不需要** `Nclink.Init`：`NclServer` 自己建 MQTT 连接（clientId = SN、自动
重连），也可以完全不接 broker 离线用。

```csharp
using Nclink;

// broker 传 null = 不接 MQTT：离线用 Dispatch/InvokeMethodCall，或者给最后一个参数
// 一个"自研传输"回调，把每条出站报文（事件/采样）自己送走。
using (NclServer device = new NclServer("V2CS0000001", modelJson, "tcp://127.0.0.1:1883"))
{
    // 方法表 + 路径绑定 + 处理函数；处理函数收 (方法名, 参数) 返回要应答的值
    device.RegisterTool(
        "plc",
        new Dictionary<string, string> { { "getStatus", null },
                                         { "setCount", "{\"type\":\"object\",\"properties\":"
                                                     + "{\"value\":{\"type\":\"integer\"}}}" } },
        new NclToolBinding[]
        {
            new NclToolBinding("/MACHINE/STATUS", NclOperation.GetValue, "getStatus"),
            new NclToolBinding("/MACHINE/PART_COUNT", NclOperation.SetValue, "setCount")
        },
        delegate(string method, NclJson parameters)
        {
            return method == "getStatus" ? (object)1L
                 : parameters.Get("value").AsLong();
        });

    device.RegisterBuiltinTool();      // 内置工具：addSample / removeSample
    device.Subscribe();                // 订阅 6 个请求主题（要接 MQTT 才能收）
    device.InitSamples();              // 启动模型里声明的采样通道
    device.PushEvent("010307", "{\"key\":\"PART_COUNT\",\"value\":7}");

    Console.WriteLine(device.Model.Root.Id);        // 借用视图，不用 Dispose
    Console.WriteLine(device.OpenapiJson("http://localhost:9008/api"));
}                                                    // Dispose = 先收 HTTP，再停服务
```

- 处理函数的返回值：`NclJson`（原样）、`string`（**按 JSON 文本**处理）、数字 /
  `bool`（序列化成 JSON 字面量）、`null` = 成功但没有值（路径取值按 NG 应答，与
  C API 一致）。抛异常 → 该次调用按错误应答、异常文本进 reason，异常记在
  `device.LastCallbackError` 上，不穿回原生层。
- 不接 broker 时用 `device.Dispatch(topic, payload)` / `device.InvokeMethodCall(...)`
  / `device.CheckMethodCall(...)` 离线驱动（返回应答报文的 `NclJson`）。
- 设备端示例：`Nclink.Demo.Device.exe [broker] [SN] [秒数] [HTTP端口]`（broker 写
  `-` 就是离线：出站报文打到控制台）；仓库里任意客户端都能读它，例如
  `build\examples\ncl_client_demo.exe tcp://127.0.0.1:1883 V2CS0000001 8`。

### HTTP / REST 端点

端点的内容全在库里：`GET /api/schema`（OpenAPI 3.0 文档）、`GET /swagger-ui`
（浏览器里直接调工具方法）、`POST /api/<工具>/<方法>`（等价于 `methodCall`），
再加上配置端点（SN / 模型 / 驱动 / mqtt.cfg / 服务器列表）。

```csharp
using (NclServer device = new NclServer("V2CS0000001"))
{
    NclHttpEndpoint http = device.StartHttp(9008, withConfig: true);   // 0 = 随机端口
    Console.WriteLine("{0}/swagger-ui", http.Url);

    // 自己挂路由：返回 null = 404，抛异常 = 500（异常文本进报文）
    http.Route("GET", "/api/hello", request => NclHttpReply.Json(
        "{\"path\":\"" + request.Path + "\",\"query\":\"" + request.Query + "\"}"));

    Console.WriteLine(http.RequestCount);          // 诊断用
    http.SetCors(false);                           // 默认 Access-Control-Allow-Origin: *
    http.Dispose();                                // 幂等；device.Dispose() 也会替你收
}
```

`method` 支持 `"*"`；`path` 以 `/api/` 开头时是前缀匹配，否则要求完全相等（库的
匹配规则）。**关端点要在关服务器之前**（路由回调还挂在服务器上），`device.Dispose()`
已经按这个顺序做了。

### 异步方法调用（长方法）

设备端方法可能跑很久，所以请求可以带 `async`：设备立刻回 `code=OK` + `handler`
（方法句柄，代表那个线程/任务），方法在设备端线程池里跑，随后按句柄查进度与结果。

```csharp
using (NclDeviceClient client = Nclink.GetDevice("V2CS0000001"))
{
    using (NclJson ack = client.MethodCallAsync("/plc/grind", "{\"depth\":3}"))
    {
        string handler = ack.Get("handler").AsString();          // code=OK + handler
        using (NclJson status = client.MethodStatus(client.Sn, handler))
        {
            Console.WriteLine(status.Get("status").AsString());  // executing / stopped / ...
        }
        NclJson result;
        while ((result = client.MethodResult(client.Sn, handler)) != null &&
               result.Get("code").AsString() == "PENDING")        // 还在跑
        {
            result.Dispose();
            Thread.Sleep(50);
        }
        Console.WriteLine(result.Get("result").AsString());      // finished / error
        result.Dispose();
    }
}
```

- 结果取走后句柄就释放了（再查同句柄是 `NG`）。
- 设备侧不用做异步的事：工具方法就是普通函数；想报进度用
  `server.ReportMethodProgress(handler, process, status)`（可选），离线自检用
  `InvokeMethodCallAsync` / `InvokeMethodStatus` / `InvokeMethodResult`。

### 文件通道（上传 / 下载）

MQTT 报文里只传 `/temp/<名字>` 这样的**令牌**，字节走 FTP。方向要记住：**设备是
FTP 客户端**，托管侧是 FTP 服务端（进程级端点 127.0.0.1:2323、admin / 123456、根
= 安装根）。传字节之前先**握手**：`client.OpenFileChannel()` 把端点交给设备
（file/openFileChannel），设备随即往那儿拨 FTP；`client.CloseFileChannel()` 收回
租约并撤销库给这条通道加的临时账号。下面的上传/下载等便利方法会自己确保通道开着。

```csharp
// 设备端（收文件的那一边）
using (NclServer device = new NclServer("V2CS0000001", null, "tcp://127.0.0.1:1883"))
{
    device.RegisterFileTool();          // /CONTROLLER/FILE：write/read/ll/mkdir/delete
    device.Subscribe();
}

// 客户端（上位机那一侧）
using (NclDeviceClient client = Nclink.GetDevice("V2CS0000001"))
{
    client.OpenFileChannel();       // 握手：设备往这台机器的 FTP 端点拨
    client.UploadLocalFile(@"D:\work\report.txt", "/data/report.txt");  // 传上去
    foreach (NclFileInfo item in client.ListFiles("/data"))
    {
        Console.WriteLine("{0} {1} 字节 {2} 片", item.FileName, item.FileSize,
                          item.TotalChunks);
    }
    string local = client.DownloadTo("/data/report.txt", @"D:\work\back.txt");
    client.MakeDirectory("/docs");
    client.DeleteRemoteFile("/data/report.txt");
    client.CloseFileChannel();      // 收回租约（幂等；不调也会随 Nclink.Shutdown 收掉）

    // 带文件参数的方法调用：keys 与 paths 一一对应，应答里的 fileKeys 会被换成本地路径
    using (NclJson reply = client.MethodCallFile("plc/convert",
                                                "{\"input\":\"\"}",
                                                new[] { "input" }, new[] { @"D:\a.bin" }))
    { }
}

// 本地文件小工具（不需要 broker）
bool binary = Nclink.NeedCompression("model.json");
int chunks = Nclink.TotalChunks(300 * 1024);
string sha256 = Nclink.FileChecksum(@"D:\work\report.txt");
NclFileInfo info = Nclink.FileAttribute(@"D:\work\report.txt");
```

- `UploadFile(relative)` 只是"把已经在 `<当前目录>/<sn><relative>` 上的文件传上去"
  （与 C API 一致）；`UploadLocalFile` 会先替你摆到那个位置。
- 下载回来的文件先落在 `<当前目录>/<sn>/` 下，`DownloadFile` 返回绝对路径，
  `DownloadTo` 再替你复制到目标。
- 通道里广播的地址默认是"到 broker 的本机地址" + 进程级端点端口（拿不到配置就
  127.0.0.1）——托管侧与 broker 同一台机器时开箱即用。
- 设备侧的 `StartFtp()` 是"设备自己也开个 FTP 端点"（读 `bin/ftp.txt`），客户端传文件
  用不到它；缺文件时它抛异常，示例里是容忍着来的。
- 托管侧不跟 broker 同机（或者端口/账号不一样）时改用带参数的握手：
  ```csharp
  client.OpenFileChannel("10.0.0.7", 2323);                        // 设备拨到这台机器的 2323
  ```
  地址不想写死在代码里就放 `<root>/conf/ftp.txt`（可省文件，键全可选：
  `host` / `port` / `advertisePort` / `root` / `userName` / `password` / `path` /
  `force`）；函数参数优先于它，它优先于推导默认。
- 设备端也可以不握手，直接钉一个静态 FTP 对端（要求对端自己跑 FTP 服务端、目录布局
  是 `/<sn>/...`）：
  ```csharp
  device.SetFilePeer("10.0.0.7", 2323);                          // 设备端指到上位机的 FTP
  Nclink.StartFileServer(2323, @"D:\files", "admin", "123456");   // 本机端点换端口/根/账号
  ```

### TLS（ssl://）

```csharp
if (!Nclink.TlsAvailable) { /* 这个垫片没带 TLS：见下面"TLS 构建" */ }

Nclink.Init("ssl://broker.example.com:8883", null, null,
            new NclTlsOptions
            {
                CaFile = @"C:\certs\ca.pem",                  // 内网 CA；留空 = 平台信任库
                ClientCertificate = @"C:\certs\client.pem",   // 双向 TLS 才要
                ClientKey = @"C:\certs\client.key",
                ServerName = "broker.example.com",            // 留空 = URL 里的主机名
                VerifyPeer = true                             // 默认就是 true
            });

// 设备端直连 ssl:// broker 也支持：
new NclServer(sn, modelJson, "ssl://broker.example.com:8883", null, null, null,
              new NclTlsOptions { CaFile = @"C:\certs\ca.pem" });
```

**TLS 构建**（库与垫片都得带 TLS）：

```powershell
.\build.ps1 -Tls                                   # 出 build-tls\nclink_core.lib
powershell -ExecutionPolicy Bypass -File .\bindings\native\build-shim.ps1 -Tls
# → bindings\native\bin-tls\nclink_shim.dll（连 libssl-3-x64.dll / libcrypto-3-x64.dll 一起拷好了）
# 把那个 DLL 放到你的程序旁边（P/Invoke 按 DLL 名解析，所以要么同目录、
# 要么把它的目录加进 PATH）就能用 ssl:// 了
```

库没带 TLS 时用 `ssl://` 会拿到明确的 `NOT_SUPPORTED`（-8），不用在"连接失败"里猜。

### 报文解析

自己拿到的报文（离线回放、日志里存的样本）也能按库的规则解码：

```csharp
using (NclMessage message = Nclink.Parse("Sample/V203243111F/s1", payload))
{
    if (message.Type == NclMessageType.Sample)
    {
        NclSample sample = message.AsSample();     // 托管快照，出了 using 也能用
    }
    else if (message.Type == NclMessageType.Event)
    {
        NclEvent item = message.AsEvent();
    }
    Console.WriteLine(message.Json);               // 整条报文的 JSON 文本
}
```

### 采样怎么读

外层是**槽位**（通道的 `sampleInterval` 一槽），内层是"每槽装几个点"——采样率不同
的数据项可以放在**同一个通道**里（1 ms 的列每槽 1 点、0.25 ms 的列每槽 4 点）。按行
读最省事：

```csharp
device.SampleReceived += (s, e) =>
{
    NclSample sample = e.Sample;                  // 已是托管快照
    for (int row = 0; row < sample.Rows; row++)   // Rows = 数据最多的那一列的点数
    {
        for (int col = 0; col < sample.Columns.Count; col++)
        {
            object value = sample.ValueAt(row, col);   // 粗列取"覆盖该行的第一个点"
            double d = sample.GetDouble(row, col);
        }
    }
    foreach (NclSampleColumn column in sample.Columns)
    {
        // column.Path / Slots / Points / IsNested / Encoding
    }
};
```

`NclSample` 在回调里就把报文拷成了托管对象（`long` / `double` / `string` / `bool` /
`null`），所以出了回调还能用；原始 JSON 文本在 `RawJson`。语义与 C 的
`ncl_message_sample_value_at()` 完全一致。

## 内存与线程

无第三方依赖，GC 也不是替代品：带原生内存的对象请显式 `Dispose`（或 `using`）。

| 对象 | 谁释放 |
|------|--------|
| `NclJson`（`Parse`/`Clone`/`GetValue*`/`Dispatch` 的返回值） | **自有**：`Dispose` / `using` |
| `NclModel`（`Probe`/`Parse` 的返回值） | **自有**，同上 |
| `NclMessage`（`Nclink.Parse` 的返回值） | **自有**，同上 |
| `NclDeviceClient` | `Dispose`（退订 + 清回调桥）；断开 MQTT 是 `Nclink.Shutdown()` |
| `NclServer` | `Dispose`（先收 HTTP 端点，再停采样/FTP、断 MQTT、放回调） |
| `NclHttpEndpoint` | `Dispose`（幂等；`server.Dispose()` 也会收） |
| `NclNode`、`NclJson` 的下标/成员视图、`NclServer.Model` | **借用**：宿主活着就有效；`Dispose` 是空操作（借用的东西不归你管） |
| `NclSample` / `NclEvent` | 纯托管快照，没有句柄 |
| `NclFileInfo` | 纯托管快照，没有句柄 |

- 采样/事件回调在客户端自己的读取线程上触发，**别在回调里做耗时操作**；工具方法回调
  跑在库自己的线程池上（不是 MQTT 读取线程）。回调里抛出的异常不会跨到原生层，会记在
  `NclDeviceClient.LastCallbackError` / `NclServer.LastCallbackError` 上。
- `Init`/`Shutdown`/`GetDevice` 是线程安全的（底层有锁）。
- `Subscribe`/`Unsubscribe` 请在同一个线程里成对调用。

## 自检

```powershell
dotnet run --project .\bindings\csharp\tests\Nclink.SelfTest -c Release

# 连真 broker 再跑一遍端到端（设备端 + 客户端同进程，报文真的过 MQTT）
$env:NCLINK_TEST_BROKER = "tcp://127.0.0.1:1883"
dotnet run --project .\bindings\csharp\tests\Nclink.SelfTest -c Release

# TLS 端到端（再 +2 项）：垫片要带 TLS 编（build-shim.ps1 -Tls），
# 并把 bin-tls 里的 nclink_shim.dll 与两个 OpenSSL DLL 放到程序旁边
$env:NCLINK_TEST_TLS_BROKER = "ssl://127.0.0.1:18832"
$env:NCLINK_TEST_TLS_CA = "tests/data/tls_localhost_cert.pem"
dotnet run --project .\bindings\csharp\tests\Nclink.SelfTest -c Release
```

不需要 broker（跑本机回环）：106 项检查覆盖 JSON / 模型 / 报文解析 / 设备端（离线
dispatch、工具注册、采样通道、事件、自研传输、关闭语义）/ HTTP 端点（REST、配置
端点、swagger-ui、自定义路由、错误路径、幂等关闭）/ 文件小工具（压缩判断、分片数、
SHA-256、属性）。

设了 `NCLINK_TEST_BROKER` 再多跑 26 项"过 MQTT"的端到端（设备端与客户端同进程）：
probe、路径绑定取值/写值、`MethodCall`（应答文本要解析成 `NclJson`）、采样上报、
事件推送，以及整条文件通道（上传 / 列目录 / 下载 / 建目录 / 删文件 / 带文件参数的
方法调用，含"工具返回文件"的反向）—— 一共 132 项。再设 `NCLINK_TEST_TLS_BROKER`
与 `NCLINK_TEST_TLS_CA` 多跑 2 项 TLS 端到端（设备端与客户端都过 `ssl://`；不给 CA
必须被拒），一共 134 项。

## 示例输出

```
connected: tcp://127.0.0.1:1883 (nclink 3.1.0)
probe: model root id=01 name=机床模型文件
各轴的功率与主轴振动（路径 含义）:
/AXIS@X/POWER            X轴功率
/AXIS@S/ACCELERATION     主轴加速度
GET /STATUS = 1
SET /STATUS = 42 ok
subscribed: Sample/011533226/# and Event/011533226
event Event/011533226: key=PART_COUNT value=120
sample Sample/011533226/EdgeSersors: id=EdgeSersors interval=1ms upload=1000ms columns=10 rows=4000
  /AXIS@X/POWER: 1000 个槽位 × 每槽约 1 点 = 1000 点
  /AXIS@S/ACCELERATION: 100 个槽位 × 每槽约 4 点 = 400 点（批量）
  行[0] /AXIS@X/POWER=800  /AXIS@S/ACCELERATION=-1  /AXIS@Y/POWER=1112.5 ...
  行[1] /AXIS@X/POWER=800  /AXIS@S/ACCELERATION=-0.875  /AXIS@Y/POWER=1112.5 ...
  ...（共 4000 行，这里只打前 8 行）
received 2 samples, 4 events
```

跑法：

```powershell
build\examples\ncl_device_demo.exe D:\sim-cs 30         # 设备端（另开一个窗口）
bindings\csharp\samples\Nclink.Demo.Cli\bin\Release\net8.0\Nclink.Demo.Cli.dll `
    tcp://127.0.0.1:1883 <设备SN> 6
# .NET Framework 版：
bindings\csharp\samples\Nclink.Demo.Cli\bin\Release\net472\Nclink.Demo.Cli.exe `
    tcp://127.0.0.1:1883 <设备SN> 6
```

反过来（C# 当设备端）：

```powershell
# 有 broker：其它客户端都能读它
bindings\csharp\samples\Nclink.Demo.Device\bin\Release\net8.0\Nclink.Demo.Device.exe `
    tcp://127.0.0.1:1883 V2CS0000001 30
# 离线（不接 MQTT）：出站报文走"自研传输"打到控制台，REST 端点照样能用
bindings\csharp\samples\Nclink.Demo.Device\bin\Release\net8.0\Nclink.Demo.Device.exe `
    - V2CS0000001 30 9008
```

离线模式的输出与 REST 实测：

```
HTTP: http://localhost:9008/api/schema（Swagger UI: http://localhost:9008/swagger-ui）
设备端已就绪：SN=V2CS0000001 broker=(不接 MQTT)，工具 6 个操作，采样通道 1 个
publish Sample/V2CS0000001/cs_channel {"paths":["/STATUS","/PART_COUNT","/CONTROLLER/WARNNING"], ...
publish Event/V2CS0000001 {"@id":"d9fd5cb3-...","id":"010307","time":"1789609920540","event":{"key":"PART_COUNT","value":13}}
离线模式：没有 MQTT，客户端读不到；REST 端点照常用。

# 另开一个窗口（PowerShell 的 Invoke-RestMethod / curl 都行）：
GET  /api/schema        -> 200 openapi=3.0.0
POST /api/plc/getCount  -> {"status":true,"data":8}
POST /api/plc/setCount  -> {"status":true,"data":99}   （{"value":99}）
GET  /api/hello         -> {"sn":"V2CS0000001","partCount":99}
GET  /swagger-ui        -> 200
GET  /api/cfg/getSn     -> {"status":true,"data":"V24359A51AD"}
GET  /api/nope          -> 404 not found
```

## 还没做的

- 零拷贝读采样：现在 `NclSample` 在回调里整份拷贝（方便、安全）；需要极致吞吐可以
  加一个"只在回调期间有效"的借用视图 API。
- NuGet 包：`PackageId` 与元数据已经写好，但还没做 `dotnet pack` + 按 RID 把垫片放进
  `runtimes/`；现在按"源码 + 预编译垫片"一起用。
- 对着真 broker 的回归：自检里那一段（`NCLINK_TEST_BROKER=tcp://host:port`，132 项）
  是手工设环境变量跑的；`tools/interop.sh` 起 Mosquitto / EMQX 目前只驱动 C 套件，
  还没把三份托管绑定串进去。
