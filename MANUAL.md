# NC-Link 协议栈 · 使用手册

> 面向集成者：怎么在自己的设备/上位机工程里用这套库跑通 NC-Link 协议。
> 目录结构、构建、测试清单见 [README.md](README.md)。

## 目录

1. [这是什么](#1-这是什么)
2. [构建与集成](#2-构建与集成)
3. [十分钟上手](#3-十分钟上手)
4. [核心概念](#4-核心概念)
5. [模块手册](#5-模块手册)
6. [典型任务速查](#6-典型任务速查)
7. [常见问题与排错](#7-常见问题与排错)
8. [许可](#8-许可)
9. 附录：[A · API 索引](#附录-a--api-索引) ·
   [B · 错误码](#附录-b--错误码全表) ·
   [C · 主题](#附录-c--主题前缀一览) ·
   [D · 目录布局](#附录-d--安装根目录布局)

---

## 1. 这是什么

`nclink-core-c` 是 NC-Link 中间件（GB/T 41970-2022）的 C11 实现，覆盖协议
3.0.0。它让设备端和上位机能够：

| 能力 | 说明 |
|------|------|
| 数据模型 | 设备/组件/数据项/配置项的树，路径与节点 id 双向可查 |
| 读写数据 | `get_value` / `set_value` / `get_length` / `get_attributes`，支持索引与区间 |
| 方法调用 | `methodCall` 工具分发，支持参数 JSON Schema 校验（`check`） |
| 采样上报 | 按 `sampleInterval` 采集、按 `uploadInterval` 聚合成 `Sample` 报文 |
| 事件推送 | 设备主动发布 `Event/<sn>`，对端订阅并回调 |
| 文件传输 | MQTT 只传 `/temp/<名字>` 令牌，字节走 FTP；含自研 FTP 服务端/客户端 |
| 对外接口 | 内置 HTTP 服务、OpenAPI 3.0 文档、`/swagger-ui`、12 个配置接口 |
| 配置管理 | SN、模型、驱动、服务器列表、`conf/mqtt.cfg` 的读写 |
| 传输 | 自研 MQTT 5.0（QoS 0/1/2、保活、自动重连、订阅恢复）、TCP 套接字 |

**零第三方依赖**：JSON、线程池、TTL 缓存、MQTT、HTTP、FTP、SHA-256、JSON Schema
校验全部自带，只有启用压缩编解码时才需要 zlib。

两个角色，对应两个入口头文件：

| 角色 | 入口 | 典型场景 |
|------|------|----------|
| 设备端 | `nclink/ncl_server.h` | 机床/PLC/机器人等被访问的一方 |
| 上位机 | `nclink/ncl_client.h` | 网关、MES、调试工具等主动访问的一方 |

---

## 2. 构建与集成

### 2.1 目录结构

```
include/nclink/     公共头文件（-I 只需要指向 include）
lib/<平台>/         预编译静态库（发布包：windows-x64-msvc / linux-x86_64-gcc，另有 -staticmem 静态内存版）
examples/           两个可运行示例：设备端 / 客户端
adapters/           厂商协议驱动 + 设备程序 ncl_adapter（宿主：一台 NC-Link 设备）+ 可装载的适配器模块（plugins/ncl_driver_<协议>.*）
MANUAL.md/.docx     本手册；README/RELEASE/CHANGELOG 见同名文件

src/<模块>/         实现，共 13 个模块目录        ← 以下仅源码仓库有
tests/              26 个核心测试套件（含 mem 分配器不变量、mem_mc 蒙特卡洛、mem_mt 并发压测，
                    以及可选的 broker 互操作与 TLS 套件）+ 协议黄金样本；
adapters/tests/     16 个适配器测试套件（驱动接口、配置分派、宿主/模块装载、各协议黄金报文与靶机）
tools/              许可头检查、broker 互操作、文档生成与发布打包脚本
build.ps1           Windows 一键：配置 + 编译 + ctest
build-linux.sh      Linux 免 cmake 构建
```

> **发布包含头文件、两个平台的静态库、文档与示例程序**；实现源码与测试套件在
> 工程仓库里（需要自行重编时获取，见 2.5）。

### 2.2 Windows（已验证环境）

```powershell
.\build.ps1                 # 配置 + 编译 + 跑全部测试
.\build.ps1 -Clean          # 先清空 build 目录再全量编译
.\build.ps1 -NoTest         # 只编译
.\build.ps1 -Arch x86 -BuildDir build-x86   # 32 位（Win32）：库 + 示例 + 测试
```

除了 MSVC，**mingw-w64（GCC）在 Windows 目标上也整套验证过**（16.2.0 / UCRT /
posix-threads）：`CC=<mingw>/gcc AR=<mingw>/ar sh build-linux.sh build-mingw` 编出的库、
示例与测试全部通过（Go 绑定的 cgo 走的就是这条链，见 2.4.2）。

脚本会自动定位 Visual Studio 2022 Build Tools 自带的 CMake/Ninja，并按架构调用对应的
`vcvars64.bat` / `vcvars32.bat`，不用先开 VS 命令行；`-Arch x86` 出的就是 32 位

```bat
call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### 2.3 Linux（已验证：gcc 13.4）

补充验证（gcc 13，容器内）：3.4.0 时 `./build-linux.sh` 全量 **39/39 通过**（25 个核心
套件 + 14 个适配器套件），`mem_mc` 的统计与 Windows/MSVC 逐位一致（确定性序列）。
适配器进来后全量是 **42 个套件**（26 核心 + 16 适配器，多的是 `library`：
`ncl_library_*` 的装载接口），3.4.0 的 `ncl_fanuc_collector` 已改为
`ncl_adapter`（宿主）+ `plugins/ncl_driver_focas.dll`（模块）+ `conf/fanuc.json`
（连接参数与采样）这一组合方式（19 个点位在 `adapters/plugins/focas.c` 里声明）：
**默认堆版**已在 Windows/MSVC 与 MinGW/gcc 16.2 上复测 **42/42**，Linux 容器里跑同一条
命令即可；静态池版的尺寸边界见 4.9（那组数字是 39 套口径，未随这套重跑）：64 KiB 池
**38/39**（只剩 `file` 一套，它读回比较时要 1 MiB 连续块），1.5 MiB 池 **39/39**。
真 broker 互操作（`tests/test_broker`）对 **Mosquitto 2.1.2** 与 **EMQX 5.8.9** 各
**44 项检查全过**。

内存门禁：`./tools/asan-linux.sh`（`--docker` 可在 Windows/macOS 上一键跑）用
AddressSanitizer + LeakSanitizer 覆盖分配器、协议层与传输层；MSVC 的 ASan 不带泄漏
检测，所以 Linux 这一遍是唯一能报泄漏的。3.3.0 期间它抓到并修掉的两处（MQTT 重连漏
join 已结束的收包线程、`ncl_rest_attach()` 的上下文无人释放，后者由新增的
`ncl_http_server_own_context()` 按上下文登记一次随服务器释放）复跑已归零：
`asan-linux: 0 suite(s) with sanitizer findings`（见 CHANGELOG 3.3.0）。

源码是 C11，套接字层有 Winsock / BSD 两套实现，两个平台都已在真机编过并跑通
全部测试（Windows 见 2.2，Linux 见下）。

```sh
./build-linux.sh                 # 不需要 cmake：产出 build-linux/libnclink_core.a 并跑全部测试
CC=clang ./build-linux.sh out    # 换编译器 / 换输出目录
```

用 CMake 也行：

```sh
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j
ctest --test-dir build-linux --output-on-failure
```

手工编译（不走上面两个脚本）时注意：

| 要点 | 原因 |
|------|------|
| `-Iinclude -Isrc` | 公共头在 `include/`，极少数内部头在 `src/` 下（如 `file/file_internal.h`） |
| `-D_POSIX_C_SOURCE=200809L` | `-std=c11` 会隐藏 `strdup`、`getaddrinfo`、`localtime_r`、`pthread_*` 等 POSIX 接口 |
| `-lpthread` | 线程、互斥量、条件变量 |
| 可加 `-Wno-format-truncation` | 库内用定长路径缓冲（4096），GCC 对此的保守告警没有意义 |

**与真实 broker 的互操作验证**（可选，`tools/interop.sh` 用 Docker 起 broker，
默认监听 18830 Mosquitto / 18831 EMQX，不碰你本机 1883 上的 broker）：

```bash
./tools/interop.sh              # Mosquitto + EMQX 各跑一遍
./tools/interop.sh emqx         # 只跑其中一个
```

也可以直接对任意 broker 跑：`NCL_TEST_MQTT_BROKER=tcp://host:1883 <test_broker>`。
该套件覆盖 QoS 0/1/2、通配订阅、40 KB 报文、退订、空闲保活、会话被顶替（0x8E）
与显式重连后的订阅恢复；不设该环境变量时自动跳过，普通构建不需要 broker。

**TLS**（可选，默认构建零依赖、不含 TLS）：需要 `ssl://` 时按 2.4 打开
`NCLINK_WITH_TLS`，链接时加 `-lssl -lcrypto`（发布包里
`lib/linux-x86_64-gcc-tls/` 就是这份）。客户端侧配置见 5.5 的
`tls_ca_file` / `tls_verify_peer` / `tls_server_name` / `tls_client_cert`；
自签证书把证书本身当 `tls_ca_file` 传即可，主机名要与证书 SAN 一致。
`tools/interop.sh` 会对 Mosquitto 的 TLS 监听再跑一遍互操作套件。

### 2.4.1 C++ 封装（C++17，header-only）

`include/nclink/ncl.hpp` 在 C 库之上提供 RAII + 异常的薄封装，协议核心仍是 C：

```cpp
#include "nclink/ncl.hpp"

ncl::Client::init("tcp://broker:1883");     // ssl:// 需要启用 TLS 的构建
ncl::Client client("V203243111F");
ncl::Model  model = client.probe();          // 拉取设备模型
ncl::Json   v     = client.value("/STATUS"); // 读值
client.set("/STATUS", ncl::Json::parse("42"));
// 出错抛 ncl::Error（继承 std::runtime_error，e.code() 是 ncl_err）
ncl::Client::shutdown();
```

`ncl::Json` / `ncl::Message` / `ncl::Model` 是独占所有权的包装（禁拷贝、可移动、
可 `release()`），析构自动释放；`ncl::Server` 接管已有 `ncl_server*`。
构建开关：CMake `-DNCLINK_BUILD_CPP=ON`（默认开，加 `cpp` 测试套件）；
`build-linux.sh` 在检测到 `g++` 时会一并编译 `tests/test_*.cpp` 与 C++ 示例。

### 2.4.2 Go 绑定（cgo）

`bindings/go/` 是 cgo 绑定（模块 `github.com/huienming/nclink-c/bindings/go`），
用法与 C/C++ 示例一一对应：

```go
if err := nclink.Open("tcp://broker:1883", "", ""); err != nil { log.Fatal(err) }
defer nclink.Shutdown()

client, _ := nclink.Get("V203243111F")
model, _ := client.Probe(5000)          // 拉设备模型
v, _ := client.Value("/STATUS", 5000)   // 读值
defer v.Close()

client.SubscribeSamples(2, func(topic string, msg *nclink.Message) {
    // msg 只在回调期间有效（与 C 侧一致）
})
```

链接：`tools/stage-go-libs.sh` 把 `build-linux/libnclink_core.a`（Linux）与
`build-mingw/libnclink_core.a`（Windows）暂存到 `bindings/go/lib/<goos>-<goarch>/`
（不入库）。**Windows 上 cgo 只认 mingw 工具链，且必须链 mingw 编的库**——
MSVC 的 `nclink_core.lib` 链不上；装 mingw 后用
`CC=<mingw>/gcc AR=<mingw>/ar sh build-linux.sh build-mingw` 编一份即可（`build-linux.sh`
在 mingw 目标下会自动补上 `-lws2_32 -liphlpapi -lwinmm`）；发布包的
`lib/windows-amd64-mingw/` 里已经带了一份 mingw 编的 x64 库，可以直接暂存或链过去。
TLS 用 `-tags nclink_tls`（那份库也在同一个目录里，`libnclink_core_tls.a`）；Windows 上
它和 Linux 的 TLS 版一样**动态依赖 OpenSSL**——链接要 OpenSSL 3 的导入库（`-lssl -lcrypto`，
例如 Strawberry Perl 的 `c/lib`，`CGO_LDFLAGS=-L<dir>` 指过去），运行时要有
`libssl-3-x64*.dll` / `libcrypto-3-x64*.dll`。

### 2.4.3 Java 绑定（JNI）

`bindings/java/` 是 JNI 绑定，**零第三方依赖**（不用 Maven / Gradle），字节码是
Java 8：

```java
Nclink.logInit();
Nclink.init("tcp://127.0.0.1:1883");
try (DeviceClient device = Nclink.getDevice("V2023A7B762")) {
    try (Model model = device.probe()) { /* 遍历模型树 */ }
    try (Json value = device.getValue("/STATUS")) { /* 读值 */ }
    device.setValue("/STATUS", "42");
    device.subscribeSamples(2, (topic, sample) -> show(sample));  // 快照，出了回调也能用
}
Nclink.shutdown();
```

构建：`.\bindings\java\build.ps1`（native + javac + 自检）/ Linux
`./bindings/java/build.sh`（需要 `JAVA_HOME` 找 `jni.h`）。

设备端同样包了：`new Server(sn, modelJson, broker)` + `registerTool()` +
`new Server.Binding(路径, Operation.GET_VALUE, 方法名)` + `subscribe()` +
`initSamples()` + `pushEvent()`，即"这个 Java 进程就是一台机床"；不接 broker 也能用
`dispatch()` / `invokeMethodCall()` 离线驱动。示例见
`bindings/java/demo/com/nclink/demo/DeviceDemo.java`。

HTTP / REST 端点：`device.startHttp(9008, true)`（`0` = 随机端口）挂上库自带的
`GET /api/schema`（OpenAPI 3.0）、`GET /swagger-ui`、`POST /api/<工具>/<方法>`
（等价于 `methodCall`）与配置端点（SN / 模型 / 驱动 / mqtt.cfg / 服务器列表）；
`http.route(method, path, handler)` 还能挂自己的路由（返回 `HttpEndpoint.Reply`）。
关端点要在关 `device` 之前——`device.close()` 已经先收 HTTP 再停服务。

文件通道（`/CONTROLLER/FILE`，MQTT 只传 `/temp/<名字>` 令牌、字节走 FTP）：
`device.registerFileTool()` 让设备端能收文件；客户端侧
`client.uploadLocalFile(本地文件, "/data/x.bin")` / `client.downloadTo("/data/x.bin",
本地文件)` / `client.listFiles("/data")`（`FileInfo`）/ `client.makeDirectory` /
`client.deleteFile` / `client.methodCallFile(...)`（带文件参数的方法调用）。方向是
**设备当 FTP 客户端**、上位机当 FTP 服务端。传字节之前先握手：
`client.openFileChannel()` 把本机端点交给设备（设备随即往那儿拨 FTP），
`client.closeFileChannel()` 收回租约并撤销临时账号；上面的便利方法会自己确保通道
开着。本机不跟 broker 同机、或端口/账号不同时用
`client.openFileChannel(host, port, user, pass)`（`Nclink.startFileServer(port,
root, user, pass)` 用来换进程级端点的端口 / 根目录 / 账号）。
设备端也可以不握手，直接钉静态对端：`device.setFilePeer(host, port, user, pass)`。

`ssl://` 的连接选项也在绑定里：`Nclink.init(uri, user, pass, TlsOptions)`（CA、双向
证书、SNI、`verifyPeer(false)`）与设备端 `new Server(..., TlsOptions)`；用之前先问
`Nclink.tlsAvailable()`，库与 JNI 库都要带 TLS 编（见绑定 README 的"TLS 构建"）。

### 2.4.4 Python 绑定（ctypes）

`bindings/python/` 是 ctypes 绑定，只用标准库：

```python
import nclink

nclink.init("tcp://127.0.0.1:1883")
with nclink.get_device("V2023A7B762") as device:
    with device.probe() as model: ...                     # 拉模型（顺带装进客户端）
    with device.get_value("/STATUS") as value: ...         # 读值
    device.subscribe_samples(2, lambda topic, sample: print(sample.rows))
nclink.shutdown()
```

构建：`bindings\native\build-shim.ps1`（原生垫片）→
`python -m unittest discover -s bindings/python/tests`（自检，不需要 broker）。

设备端同样包了：`nclink.Server(sn=..., model=..., broker=...)` + `register_tool()` +
`subscribe()` + `init_samples()` + `push_event()`；示例
`bindings/python/examples/device_demo.py`（可以拿仓库里任意客户端去读它）。

HTTP / REST 端点用 `device.start_http(port, with_config=True)`：库自带
`GET /api/schema`（OpenAPI 3.0）、`GET /swagger-ui`、`POST /api/<工具>/<方法>` 与
配置端点；`http.route("GET", "/api/hello", handler)` 挂自己的路由（处理函数返回
`None` / `str` / 可 JSON 对象 / `(status, content_type, body)`）。

文件通道同一套形状：设备端 `device.register_file_tool()`，客户端
`client.upload_local_file(...)` / `download_to(...)` / `list_files(...)`（`FileInfo`）/
`make_directory(...)` / `delete_file(...)` / `method_call_file(...)`；传字节之前先握手
`client.open_file_channel()`（设备随即往本机的 FTP 端点拨），传完
`client.close_file_channel()` 收回租约并撤销临时账号——上面的便利方法会自己确保通道
开着。本机不跟 broker 同机时用
`client.open_file_channel(host=..., port=..., username=..., password=...)`；
`nclink.start_file_server(port, root=..., username=..., password=...)` 换的是进程级端点
自己的端口 / 根目录 / 账号。设备端不握手就钉静态对端：
`device.set_file_peer(host, port, user, pass)`。

`ssl://` 的连接选项：`nclink.init(uri, tls=nclink.TlsOptions(ca_file=..., ...))` 与
设备端 `nclink.Server(..., tls=...)`；先问 `nclink.tls_available()`。

### 2.4.5 C# 绑定（P/Invoke）

`bindings/csharp/` 是 P/Invoke 绑定，**同一份代码三个目标**：`netstandard2.0`
（.NET Framework 4.6.1+ / .NET Core 2.0+ / .NET 5+）与 `net472` / `net8.0`；零第三方
依赖（JSON 走库自己的解析器，不用 Newtonsoft.Json 也不用 System.Text.Json）。

```csharp
Nclink.LogInit();
Nclink.Init("tcp://127.0.0.1:1883");
using (NclDeviceClient device = Nclink.GetDevice("V2023A7B762"))
{
    using (NclModel model = device.Probe()) { /* 遍历模型树 */ }
    long status = device.GetLong("/STATUS");
    device.SetValue("/STATUS", "42");
    device.SampleReceived += (s, e) => Console.WriteLine(e.Sample);
    device.SubscribeSamples();
}
Nclink.Shutdown();
```

构建：`powershell -ExecutionPolicy Bypass -File .\bindings\csharp\build.ps1`
（垫片 + 三个工程 + 自检 106 项）/ Linux 用
`dotnet run --project bindings/csharp/tests/Nclink.SelfTest -c Release`（都不需要
broker）；想看"报文真的过 MQTT"的那一段，设 `NCLINK_TEST_BROKER=tcp://host:port`
再跑一遍自检（设备端 + 客户端同进程，132 项）。

客户端 + 设备端都包：`NclServer`（`RegisterTool` / `NclToolBinding` / `Subscribe` /
`InitSamples` / `PushEvent` / 离线 `Dispatch` / 自研传输 `NclPublishSink`）、
`NclHttpEndpoint`（`StartHttp` + `Route`，与 Java / Python 同一套 REST 端点）、
`Nclink.Parse(topic, payload)` → `NclMessage`（`AsSample()` / `AsEvent()` 拿快照），
以及文件通道：设备端 `RegisterFileTool`、客户端
`UploadLocalFile` / `DownloadTo` / `ListFiles`（`NclFileInfo`）/ `MakeDirectory` /
`DeleteRemoteFile` / `MethodCallFile`，本地小工具 `Nclink.FileChecksum` /
`FileAttribute` / `NeedCompression` / `TotalChunks`。
传字节之前先握手：`client.OpenFileChannel()`（本机不跟 broker 同机时给
`host` / `port` / `username` / `password`）把端点交给设备，
`client.CloseFileChannel()` 收回租约并撤销临时账号（`client.FileChannelIsOpen` 查状态）；
上面的便利方法会自己确保通道开着。设备端也可以不握手，钉静态对端用
`server.SetFilePeer(host, port, user, pass)`。
`ssl://` 的连接选项：`Nclink.Init(uri, user, pass, new NclTlsOptions { CaFile = ... })`
与设备端 `new NclServer(..., new NclTlsOptions { ... })`（先用 `Nclink.TlsAvailable`
问一下）；`Nclink.StartFileServer(port, root, user, pass)` 换进程级端点自己的
端口 / 根目录 / 账号；客户端侧还能用 `Nclink.LoadModel()`
装上自己那份模型（`ClearModel()` 卸下），路径 ↔ id 与采样补齐都靠它。
示例：`Nclink.Demo.Cli`（客户端）与 `Nclink.Demo.Device`（设备端，`broker` 传 `-`
即离线，出站报文走自研传输打到控制台）。

**三种托管绑定共用同一份原生垫片** `bindings/native/nclink_shim.c`：它把 C API
摊平成"不透明句柄 + 标量 + UTF-8 文本"，托管侧不依赖 C 结构体的内存布局。C# 走
P/Invoke、Java 走 JNI（`nclink_jni` 把垫片一起编进去）、Python 走 ctypes；C# 绑定
见 `bindings/csharp/README.md`。
3.2.0 起垫片还多了连接选项那组入口：`nclshim_open_ex` / `nclshim_tls_available`
（客户端 TLS）、`nclshim_server_create_ex`（设备端连 `ssl://` broker）、
`nclshim_server_set_file_peer` 与 `nclshim_file_start_ftp_ex`（文件通道对端可配）。
3.4.0 又加了文件通道握手那组：`nclshim_client_file_channel_open` /
`..._open_ex` / `..._close` / `..._is_open` 与便利入口
`nclshim_client_ensure_file_channel`（托管绑定的上传/下载等便利方法用它按需握手）。

### 2.4 CMake 选项

| 选项 | 默认 | 作用 |
|------|------|------|
| `NCLINK_BUILD_TESTS` | ON | 编译并注册测试套件 |
| `NCLINK_BUILD_EXAMPLES` | ON | 编译 `examples/` 下的两个示例 |
| `NCLINK_WITH_MQTT` | ON | 编译 MQTT 传输层、客户端、服务端、文件与 FTP |
| `NCLINK_WITH_ZLIB` | OFF | 启用 zlib 压缩编解码 |
| `NCLINK_WITH_TLS` | OFF | 启用 OpenSSL，支持 MQTT over `ssl://`（需要的现场才打开） |
| `NCLINK_STATIC_MEM` | OFF | 库内所有分配改由一个静态池供给，不调用 `malloc`（见 4.9） |
| `NCLINK_MEM_POOL_BYTES` | 20971520 | 静态池大小，单位字节（默认 20 MiB） |
| `NCLINK_MEM_CLASS_BYTES` | -1 | 小对象尺寸类区域大小；`-1` = 池的 1/4，`0` = 关掉尺寸类 |
| `NCLINK_MEM_SINGLE_THREAD` | OFF | 静态池不加锁，仅限单上下文/裸机 |
| `NCLINK_MEM_REPORT` | OFF | 退出时打印池的峰值用量，用来定池大小 |
| `NCLINK_MEM_FIRST_FIT` | OFF | 静态池改用首适配（只用于 A/B 对照测量，见 4.9） |

Windows 打开 TLS：`.\build.ps1 -Tls`（自动在 `OPENSSL_ROOT_DIR`、常见安装目录与
vcpkg 里找 OpenSSL；用 `-OpenSslRoot <目录>` 指定），运行时需要
`libssl-3-x64.dll` / `libcrypto-3-x64.dll`；Linux：`NCL_WITH_TLS=1 ./build-linux.sh`，
链接 `-lssl -lcrypto`。

### 2.5 集成到自己的工程

最小做法：把 `include/` 与 `src/` 纳入你的构建，或先编出静态库再链接。

```cmake
add_subdirectory(nclink-c)                       # 或自行 add_library(... STATIC)
target_link_libraries(your_app PRIVATE nclink::core)
```

有七件事必须留意：前三条是"能编出来、能跑起来"，后四条是"嵌进别人的进程之后"才会
遇到的（更细的一版在 4.9.5）。

1. **字符集**：源码与字符串字面量都是 UTF-8（日志与设备描述含中文），
   MSVC 必须加 `/utf-8`。否则在非 UTF-8 代码页上会出现 C4819，甚至中文字符串
   把结尾引号“吞掉”导致 C2001。CMake 工程已对全部目标统一设置。
2. **平台库**：Windows 需要 `ws2_32`（套接字）、`iphlpapi`（供 `ncl_net_ip_map_json()`
   枚举网卡）与 `winmm`（1 ms 级等待的 `timeBeginPeriod`，见 4.5）；MSVC 下源码自带
   `#pragma comment(lib, ...)`，MinGW/其它工具链需显式 `-lws2_32 -liphlpapi -lwinmm`；
   POSIX 需要 `-pthread`。
3. **收尾**：进程退出前可调用一次 `ncl_socket_system_release()` 显式释放网络栈。
   不调用也可以——库默认让网络栈存活到进程结束，以免误伤同进程内其它套接字。
4. **谁释放谁的指针（两个堆）**：库交出来的 `char *` 与对象只能用 `ncl_free_safe()` /
   `ncl_*_free()` 释放，**不要**用 libc 或宿主自己的 `free()`；反过来，宿主的指针也
   别喂进库。静态内存版（4.9）里这两处是两个堆：喂错指针默认只是**静默丢弃**（计进
   `foreign_frees`，表现是泄漏），编 `NCL_MEM_STRICT` 才会当场 `abort()`。
5. **一个进程一份库**：客户端 holder、环境根/路径表、logger、线程池、套接字表都是
   进程级全局，所以一个进程只有一个客户端、一个设备端。宿主里出现两份库（宿主链一份、
   某个 DLL 又链一份）就会得到两套全局状态、两个池，指针互不认。
6. **安装根别靠当前目录**：`ncl_env_set_root()` 默认是进程的 cwd，库会在它下面建/读
   `bin/sn.txt`、`conf/mqtt.cfg`、`log/out.txt`（10 MB 轮转）、`uploadFile/`、`temp/`。
   GUI 程序与 Windows 服务的 cwd 常常是 `system32` 或只读目录，启动时显式指定一次。
7. **池按量到的峰值开**：静态池版的容量是编译期常量，耗尽就是 `NCL_ERR_NOMEM`
   （不回退堆）。用真实流量 + `-MemReport` 量 `peak_footprint_bytes`，留 2~3 倍余量；
   量法与验收清单见 4.9.5。

Windows 用 MSVC 时还要注意 ABI 一致：发布包里的 `nclink_core.lib` 有 **x64 与 x86（32 位）**
两份，都是 **Release + /MD（动态 CRT）**，你的工程要用同样的架构与运行库设置；包内示例
可执行文件同样是 `/MD`，运行需要 VC++ 2015-2022 运行库。不一致时请用仓库里的源码重新
编译（见 2.3）。

---

## 3. 十分钟上手

`examples/` 下有两个**可直接编译运行**的完整程序，本节片段都摘自它们：

```powershell
.\build.ps1
build\examples\ncl_device_demo.exe                     # 设备端：一直运行到 Ctrl+C
build\examples\ncl_device_demo.exe - - 60              # 只想跑一会儿（离线、60 秒）
# 参数顺序 [broker] [sn] [seconds] [http-port]，五个语言的示例一致；
# 安装根目录走环境变量 NCL_DEVICE_ROOT（省略 = 当前目录）
build\examples\ncl_client_demo.exe <broker> <设备SN> <秒数>  # 客户端
```

设备端示例默认**一直运行**（真实设备就是这个跑法），只有 Ctrl+C（或 POSIX 的
SIGTERM）才停，停下时走正常清理：停采样、停 FTP/HTTP、断开 MQTT。给它第二个参数
（秒数）就只跑那么久，方便脚本里跑出确定长度的输出。

### 3.1 设备端最小程序

```c
#include "nclink/ncl_env.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_message.h"
#include "nclink/ncl_server.h"

/* 工具方法的签名固定：入参是请求的 params 对象，出参是 JSON 值。
 * 返回 NCL_OK 但 *result 为 NULL，会让应答 code=NG。 */
static ncl_err on_get_status(void *instance, const ncl_json *params,
                             ncl_json **result, char **reason) {
    (void)instance; (void)params; (void)reason;
    *result = ncl_json_new_int(42);
    return NCL_OK;
}

static ncl_err on_set_status(void *instance, const ncl_json *params,
                             ncl_json **result, char **reason) {
    long long value = 0;
    if (!ncl_json_as_int(ncl_json_obj_get(params, "value"), &value)) {
        *reason = ncl_strdup("value 必须是整数");
        return NCL_ERR_INVALID_VALUE;
    }
    /* ... 真正写设备 ... */
    *result = ncl_json_new_bool(true);
    return NCL_OK;
}

/* 第三个字段是参数 JSON Schema，可为 NULL；只在 check=true 时使用 */
static const ncl_tool_method methods[] = {
    {"getValue", on_get_status, NULL},
    {"setValue", on_set_status,
     "{\"type\":\"object\",\"properties\":{"
     "\"value\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":65535}},"
     "\"required\":[\"value\"]}"},
};

/* 绑定键是 "<operation>#<path>"，path 取自模型（见 4.3） */
static const ncl_tool_binding bindings[] = {
    {"/STATUS", NCL_OP_GET_VALUE, "getValue", "plc"},
    {"/STATUS", NCL_OP_SET_VALUE, "setValue", "plc"},
};

static ncl_server *g_server;

static void on_mqtt(void *user, const ncl_mqtt_publish *publish) {
    ncl_message *request = ncl_message_parse(publish->topic,
                                             (const char *)publish->payload,
                                             publish->payload_len);
    if (request != NULL) {
        ncl_server_on_message(g_server, publish->topic, request);
        /* 内部会转线程池处理：不要在 MQTT 收包线程上同步发布应答，
         * 否则会等自己还没处理的 PUBREC 而自我死锁。 */
    }
}

int main(void) {
    ncl_mqtt_config config;
    ncl_mqtt_client_options options;
    ncl_mqtt_client *mqtt;
    ncl_server_options server_options;
    char *sn;

    ncl_env_set_root(".");            /* 安装根目录：conf/bin/log 都在它下面 */
    ncl_log_init(NULL);               /* 写 <root>/log/out.txt */
    sn = ncl_sn_read();               /* bin/sn.txt 存在就沿用，没有才生成 */
    ncl_mqtt_config_read(&config);    /* url/username/password */

    ncl_mqtt_client_options_default(&options);
    options.url = config.url;
    options.client_id = sn;           /* 设备端用 SN 做 clientId */
    options.username = config.username;
    options.password = config.password;
    options.automatic_reconnect = true;
    options.on_message = on_mqtt;
    mqtt = ncl_mqtt_client_create(&options);
    ncl_mqtt_client_connect(mqtt);

    memset(&server_options, 0, sizeof(server_options));
    server_options.sn = sn;
    server_options.mqtt = mqtt;             /* 借用，不接管所有权 */
    server_options.model_json = model_json; /* conf/model/nclink.json 的内容 */
    g_server = ncl_server_create(&server_options);
    ncl_server_register_tool(g_server, "plc", device_state, methods, 2,
                             bindings, 2);
    ncl_server_register_builtin_tool(g_server);  /* addSample/removeSample */
    ncl_server_subscribe(g_server);              /* 订 6 个请求主题 */

    /* ... 一直运行 ... */

    ncl_server_free(g_server);
    ncl_mqtt_client_disconnect(mqtt);
    ncl_mqtt_client_destroy(mqtt);
    ncl_mqtt_config_free(&config);
    free(sn);
    return 0;
}
```

### 3.2 客户端最小程序

```c
#include "nclink/ncl_client.h"
#include "nclink/ncl_message.h"

int main(void) {
    ncl_client *client;
    ncl_json *value = NULL;
    ncl_message *probe = NULL;
    long long number = 0;

    /* 一个进程只需初始化一次：内部建立 MQTT 连接。本机的 FTP 端点不再顺手起，
     * 要传文件时由 ncl_client_open_file_channel() 按需拉起（见 5.10） */
    ncl_client_holder_init("tcp://192.168.1.10:1883", "admin", "123456");

    client = ncl_client_holder_get("V200583BC87");  /* 按 SN 取设备视图 */
    if (client == NULL) {
        return 1;
    }

    /* probe：拿设备模型。模型所有权要显式接管（见 4.7） */
    if (ncl_client_probe(client, 5000, &probe) == NCL_OK && probe != NULL) {
        ncl_client_set_root_node(client, ncl_message_take_model(probe));
        ncl_message_free(probe);
    }

    if (ncl_client_get_value(client, "/STATUS", 5000, &value) == NCL_OK) {
        ncl_json_as_int(value, &number);
        ncl_json_free(value);
    }
    ncl_client_set_value(client, "/STATUS", ncl_json_new_int(7), 5000);

    ncl_client_holder_shutdown();
    return 0;
}
```

### 3.3 示例实测输出

设备端（`ncl_device_demo.exe - - 20`，离线跑 20 秒；安装根目录走 `NCL_DEVICE_ROOT`）与客户端（`ncl_client_demo.exe
tcp://127.0.0.1:1883 166587125 12`）对跑，客户端侧输出（下面是客户端那 12 s 窗口里
的内容，头两条与结尾；SN 每次首启都会换一个，这里只是当次跑出来的那个）。
`166587125` 是那次实测里设备端首次启动生成的 SN（当时示例自己发 9 位纯数字；现在
默认由 `ncl_sn_read()` 生成 `V2` + 9 位十六进制，见 3.4 / 4.1），broker 是本机
MQTT 5.0 broker（mochi-mqtt v2.7.9，匿名 1883；更早几次实测用的是 EMQX 5.8.9，
行为一致）：

```
设备模型已装载: /NC_LINK_ROOT，/STATUS 的节点 id = 010302
模型里的采集通道 sample_channel0: 8 个采样项
    [0] /PART_COUNT
    [1] /FEED_OVERRIDE
    [2] /CONTROLLER/PROGRAM
    [3] /CONTROLLER/TOOL_NUMBER
    [4] /AXIS@S/SPEED
    [5] /STATUS
    [6] /MACHINING_MODE
    [7] /CONTROLLER/WARNING
模型里的采集通道 EdgeSersors: 12 个采样项
    [0] /AXIS@X/POWER@1
    [1] /AXIS@X/ACCELERATION@X
    ...（X/Y/Z/C 各一路 + 主轴两路，共 12 项）
GET /STATUS = 1
SET /STATUS = 42 成功
check 结果: code=NG reason=[#/value: expected maximum: 65535, found 99999]
文件回传路径: D:\...\166587125\demo.txt
  远端文件 demo.txt (14 字节)
收到采样 [Sample/166587125/EdgeSersors] 通道=EdgeSersors 采样周期=1ms 上报周期=100ms 采样项=12
    表头 paths(20 项) = ["/AXIS@X/POWER@1","/AXIS@X/ACCELERATION@X",...,"/AXIS@S/ACCELERATION@Y"]
    原始报文: {"paths":[...同上 12 项...],"id":"EdgeSersors","beginTime":"1789573512114",
              "data":[{"data":[800.0,812.5,...(中间省略)...,1100.0]},        ← 功率：100 个值
                      {"data":[[-1.0,-0.875,-0.75,-0.625],...]},          ← 振动：每槽一批
                      ...],"interval":1,"uploadInterval":100}
    /AXIS@X/POWER@1  编码=raw 本轮 100 个值: [800.0, 812.5, 825.0, 837.5, ...]
    ...
    /AXIS@S/POWER@2  编码=raw 本轮 100 个值: [2175.0, 2187.5, 2200.0, ...]   ← 主轴第二路
    /AXIS@S/ACCELERATION@Y 批量采样: 100 个槽位 × 每槽约 4 点 = 400 点，首个=0.875
    按行消费: 400 行（数据最多的那一列的点数）
      行[0] /AXIS@X/POWER@1=800.0  /AXIS@X/ACCELERATION@X=-1.0  /AXIS@Y/POWER@1=1137.5  ...
      行[1] /AXIS@X/POWER@1=800.0  /AXIS@X/ACCELERATION@X=-0.875  /AXIS@Y/POWER@1=1137.5  ...
      ...（共 400 行，这里只打前 8 行）
收到采样 [Sample/166587125/sample_channel0] 通道=sample_channel0 采样周期=1000ms 上报周期=1000ms 采样项=8
    表头 paths(8 项) = ["/PART_COUNT","/FEED_OVERRIDE","/CONTROLLER/PROGRAM","/CONTROLLER/TOOL_NUMBER","/AXIS@S/SPEED","/STATUS","/MACHINING_MODE","/CONTROLLER/WARNING"]
    /PART_COUNT      编码=raw 本轮 1 个值: [30]
    /FEED_OVERRIDE   编码=raw 本轮 1 个值: [70]
    /CONTROLLER/PROGRAM 编码=raw 本轮 1 个值: [1002]
    /CONTROLLER/TOOL_NUMBER 编码=raw 本轮 1 个值: [3]
    /AXIS@S/SPEED    编码=raw 本轮 1 个值: [4200]
    /STATUS          编码=raw 本轮 1 个值: [1]
    /MACHINING_MODE  编码=raw 本轮 1 个值: [1]
    /CONTROLLER/WARNING 编码=raw 本轮 1 个值: [0]
    按行消费: 1 行（数据最多的那一列的点数）
      行[0] /PART_COUNT=30  /FEED_OVERRIDE=70  /CONTROLLER/PROGRAM=1002  /CONTROLLER/TOOL_NUMBER=3  /AXIS@S/SPEED=4200  /STATUS=1  /MACHINING_MODE=1  /CONTROLLER/WARNING=0
收到事件 [Event/166587125] id=010307 key=PART_COUNT value=60
...
共收到 12 条事件、65 条采样上报
```

这六步分别验证了：模型交换、读、写、参数校验、采样上报、文件通道、事件推送。
默认模型把采样分成两个通道，正好是两种典型节奏：

| 通道 | 采样 / 上报 | 内容 | 一条报文里每列多少点 |
|------|-------------|------|----------------------|
| `sample_channel0` | 1 s / 1 s | 机床运行状态（上面八项，含报警号） | 1 点（也是"1 行"） |
| `EdgeSersors` | 1 ms / 100 ms | 5 轴的功率与振动（主轴两路，共 12 列） | 功率 100 点、振动 400 点 |

所以"一条报文多大"由通道自己决定：秒级的量走一个通道，毫秒级的波形走另一个通道，
慢的不会被快的撑大；12 s 的实测里 `sample_channel0` 出了 11 条、`EdgeSersors` 出了
54 条（≈ 222 ms 一条）。`uploadInterval` 是"采样次数"，而 1 ms 的槽位要真跑到 1 ms
得让等待不落在系统时钟粒度上 —— 库里对短等待走**高精度计时器**（Win10 1803+ 的
`CreateWaitableTimerEx`，不支持才回退到 `timeBeginPeriod(1)`），细节见 4.5；同一份
模型在 Linux 上实测 ≈ 118 ms 一条。两条通道的列采样率各自一致，所以"按行"与"按列"
读到的是同一条时间轴；同一通道内采样率不同时的行为见 4.5 的"按行消费"
（`EdgeSersors` 里功率 1 ms 一列 + 振动 0.25 ms 一列，行数 400）。


### 3.4 设备端示例的首次启动（自举）

设备端示例可以指着**一个还不存在的空目录**启动：目录会建好，根目录里缺的东西
按"出厂默认值"补齐，已经存在的一律不动。

```powershell
$env:NCL_DEVICE_ROOT = "D:\sim4"                   # 安装根目录（第一次会自己建好）
build\examples\ncl_device_demo.exe                # 第一次：准备 D:\sim4，然后一直运行
build\examples\ncl_device_demo.exe                # 第二次：沿用上一次的 SN 与配置
build\examples\ncl_device_demo.exe - - 60         # 也可以给秒数：跑 60 秒就自己退出
```

秒数省略（或写 0）就一直运行到 Ctrl+C —— 现场就是这么跑的；给正数则跑完自动退出，
步骤 1~8 完全一样，只是主循环多了一个"跑满就走"的出口。

首次启动补上这三样（`bin/sn.txt` 由 `ncl_sn_read()` 生成，另两个文件由示例写入），
它们也是设备身份与配置的来源：

| 文件 | 首次启动写什么 |
|------|----------------|
| `bin/sn.txt` | 设备 SN：`V2` + 9 位**十六进制**（大写，且保证含 A~F 字母，不会是一串纯数字），由 `ncl_sn_read()` 在文件缺失时生成并落盘（见 4.1）。示例不自己造 SN，跟着库走 |
| `conf/model/nclink.json` | 设备模型：一台数控机床（X/Y/Z/C 四轴 + 主轴 S + 数控系统），带两个采样通道：`sample_channel0`（机床运行状态，1 s 采样 / 1 s 上报，八项）、`EdgeSersors`（5 轴的功率与振动，**二十项** = 每轴 1 个功率 + 3 个方向的加速度；路径形如 `/AXIS@S/POWER@1`、`/AXIS@S/ACCELERATION@X`）。首次启动时由示例自举（模型编译在代码里：`examples/device_model.c`，五个语言的示例共用这一份），之后以这个文件为准 |
| `conf/mqtt.cfg` | 本机 broker：`url=tcp://127.0.0.1:1883`，`username=`/`password=` 留空 = 匿名连接（空值不会写进 MQTT 连接报文） |

之后以文件为准，示例不再覆盖：换模型改 `conf/model/nclink.json`（或走 REST 的
`/api/setModel`），换 broker 改 `conf/mqtt.cfg`（或 `/api/setMqttUrl`），下次启动
生效；把根目录删掉重跑，等于换一台新设备。

`sample_channel0`（通道 0）的八个采样项是 `010307 / 010305 / 01035409 / 01035413 /
01035506 / 010302 / 010309 / 01035412`，全是 1 s 采样 / 1 s 上报（机床那些"按秒看就
够了"的量）。设备端按 id 找到节点、取节点路径当表头，再按路径找工具取值，所以示例
的 `kBindings` 里注册的是这八条路径：

| 采样项 id | 路径（表头里的名字） | 示例工具 | 含义 |
|-----------|----------------------|----------|------|
| `010307` | `/PART_COUNT` | `plc/getCount` | 加工计件（件） |
| `010305` | `/FEED_OVERRIDE` | `plc/getFeedOverride` | 进给倍率（%） |
| `01035409` | `/CONTROLLER/PROGRAM` | `plc/getProgram` | 当前加工程序名 |
| `01035413` | `/CONTROLLER/TOOL_NUMBER` | `plc/getToolNumber` | 当前刀号 |
| `01035506` | `/AXIS@S/SPEED` | `plc/getSpeedS` | 主轴转速（r/min；SPEED 挂在主轴 S 轴上） |
| `010302` | `/STATUS` | `plc/getValue`（可写：`plc/setValue`） | 设备状态 |
| `010309` | `/MACHINING_MODE` | `plc/getMachiningMode` | 加工模式（0 手动 / 1 录入 / 2 自动） |
| `01035412` | `/CONTROLLER/WARNING` | `plc/getWarning` | 报警号（`0` = 无报警） |

模型里还有进给速度（`010303`）、主轴倍率（`010306`）等其他数据项，没进这个通道；
要采就把 id 加进 `ids`（见 4.5 的 `addSample`）。

功率与振动（= 加速度）挂在 `AXIS` 组件下，路径形如
`/AXIS@<轴号>/<类型>@<方向或传感器号>`，**二十项**都在通道 1 `EdgeSersors` 里
（`sampleInterval` 1 ms 槽位 / `uploadInterval` 100 ms 上报，即 100 个槽位一条报文）：

- 每个轴一个功率：`/AXIS@<轴>/POWER@1`（X/Y/Z/C/S 各一路，每槽 1 点）；
- 每个轴**三个加速度**：`/AXIS@<轴>/ACCELERATION@X|Y|Z` —— 振动信号在 X/Y/Z 三个
  方向上的分量，方向写在数据项的 `number` 里（同 `type`、不同 `number`、不同 id，
  见下面的"路径的组成"）。振动每槽 4 点（0.25 ms 一个 = 4 kHz），就是 4.5 里的
  亚毫秒采样。

| 采样项 id | 路径（表头里的名字） | 示例工具 | 每槽点数 |
|-----------|----------------------|----------|----------|
| `01035004` | `/AXIS@X/POWER@1` | `plc/getPowerX` | 1 |
| `01035104` | `/AXIS@Y/POWER@1` | `plc/getPowerY` | 1 |
| `01035204` | `/AXIS@Z/POWER@1` | `plc/getPowerZ` | 1 |
| `01035304` | `/AXIS@C/POWER@1` | `plc/getPowerC` | 1 |
| `01035504` | `/AXIS@S/POWER@1` | `plc/getPowerS` | 1 |
| `01035005` | `/AXIS@X/ACCELERATION@X` | `plc/getAccelerationXX` | 4 |
| `01035006` | `/AXIS@X/ACCELERATION@Y` | `plc/getAccelerationXY` | 4 |
| `01035007` | `/AXIS@X/ACCELERATION@Z` | `plc/getAccelerationXZ` | 4 |
| `01035105` | `/AXIS@Y/ACCELERATION@X` | `plc/getAccelerationYX` | 4 |
| `01035106` | `/AXIS@Y/ACCELERATION@Y` | `plc/getAccelerationYY` | 4 |
| `01035107` | `/AXIS@Y/ACCELERATION@Z` | `plc/getAccelerationYZ` | 4 |
| `01035205` | `/AXIS@Z/ACCELERATION@X` | `plc/getAccelerationZX` | 4 |
| `01035206` | `/AXIS@Z/ACCELERATION@Y` | `plc/getAccelerationZY` | 4 |
| `01035207` | `/AXIS@Z/ACCELERATION@Z` | `plc/getAccelerationZZ` | 4 |
| `01035305` | `/AXIS@C/ACCELERATION@X` | `plc/getAccelerationCX` | 4 |
| `01035306` | `/AXIS@C/ACCELERATION@Y` | `plc/getAccelerationCY` | 4 |
| `01035307` | `/AXIS@C/ACCELERATION@Z` | `plc/getAccelerationCZ` | 4 |
| `01035505` | `/AXIS@S/ACCELERATION@X` | `plc/getAccelerationSX` | 4 |
| `01035506` | `/AXIS@S/ACCELERATION@Y` | `plc/getAccelerationSY` | 4 |
| `01035507` | `/AXIS@S/ACCELERATION@Z` | `plc/getAccelerationSZ` | 4 |

消费端按行读（见 4.5 的"按行消费"）：行数 400（振动列的点数），功率
列在同一个槽位的 4 行里读到同一个点。上报周期写 100 ms：Linux 上实测 ≈118 ms 一条，
Windows 上（库里对短等待走高精度计时器，见 4.5）实测 ≈222 ms 一条 —— 多出来的那部分
主要是取值本身的开销（每槽 12 次完整 Query）。C 与 C++ 两个客户端示例的回调都这么消费。

轴上的量都按"轴 + 物理量"写：主轴转速就是主轴 S 轴的 `SPEED` 数据项（`01035506` →
`/AXIS@S/SPEED`，工具 `plc/getSpeedS`），它是 S 轴上的单路量，走的是通道 0（见上）。
设备端与两个客户端示例会把轴上的这几项按"路径 含义"打出来，同一个部件多路传感器
时含义后面跟 `#<number>`，一眼能看出是哪一路：

```
/AXIS@S/POWER@1          主轴功率 #1
/AXIS@S/SPEED            主轴转速
/AXIS@S/ACCELERATION@Y   主轴加速度 #2
```

注意路径的组成：挂在设备（`MACHINE`）下的数据项是 `/<TYPE>`，挂在组件
（`CONTROLLER`）下的数据项才带组件名，挂在轴（`AXIS`）下的则是
`/AXIS@<轴号>/<类型>`（主轴是 `/AXIS@S/...`）。

**数据项自己也可以带 `number`**：一个部件上挂多路同类传感器时，就是"同 `type`、不同
`number`、不同 id"的几个数据项，路径变成 `/<父路径>/<type>@<number>`
（`/AXIS@S/POWER@1`、`/AXIS@S/POWER@2`）。没有 `number` 的项就是单路，路径不带后缀
（`/AXIS@S/SPEED`）。`number` 是字符串、内容自定，示例里用 `"1"`、`"2"`；模型文本里它
写在 `type` 之后、`dataType` 之前。采样表头、工具绑定、按路径查询都用带 `number` 的
完整路径，所以多路传感器在采样报文里天然是各自一列。

路径对不上（比如把报警注册成 `/WARNING`，或漏了 `@2`）时采样照样发，但那一列只能是
`null`。

---

## 4. 核心概念

### 4.1 SN 与主题

SN 是设备身份，也是所有主题的地址。两种来源必须分清：

| 接口 | 行为 | 用途 |
|------|------|------|
| `ncl_sn_read()` | `bin/sn.txt` 存在即沿用，不存在才生成 `V2` + 9 位十六进制（大写，保证含 A~F 字母，不会是纯数字） | **设备启动时用这个** |
| `ncl_config_init(sn, …)` | **无条件覆盖** `bin/sn.txt`；`sn` 为 NULL 时写入 32 位 hex | 对应 REST `/api/cfg/init`，出厂初始化用 |

 主题由 `ncl_topic_*` 构造，常用如下（完整列表见附录 C）：

| 主题 | 方向 | 说明 |
|------|------|------|
| `Query/Request/<sn>` / `Query/Response/<sn>` | 客户端 → 设备 | 读值 |
| `Set/Request/<sn>` / `Set/Response/<sn>` | 客户端 → 设备 | 写值 |
| `Probe/Query/Request/<sn>` / `.../Response/<sn>` | 客户端 → 设备 | 探测（回传模型/版本） |
| `Method/Call/Request/<sn>` / `.../Response/<sn>` | 客户端 → 设备 | 方法调用（`async: true` 时异步） |
| `Method/Status/Request/<sn>` / `.../Response/<sn>` | 客户端 → 设备 | 异步调用的进度（按 `handler`） |
| `Method/Result/Request/<sn>` / `.../Response/<sn>` | 客户端 → 设备 | 异步调用的结果（按 `handler`） |
| `Sample/<sn>/<通道id>` | 设备 → 订阅方 | 采样上报 |
| `Event/<sn>` | 设备 → 订阅方 | 事件推送 |
| `Ping/<sn>` / `Pong/<sn>` | 双向 | 心跳 |

设备端 `ncl_server_subscribe()` 会订阅 8 个请求主题（含 Status/Result 两对）；客户端
`ncl_client_subscribe()` 订阅 8 个响应主题，事件主题需要单独
`ncl_client_subscribe_events()`。

**异步方法调用**：方法执行时间可能很长，所以请求可以带 `async`（报文键就叫 `async`；
历史版本里这个键拼成 `aysnc`，解析时两个都认）：

```c
/* 客户端：发一个异步调用，立刻拿句柄 */
ncl_message *request = ncl_message_new(NCL_MSG_METHOD_CALL_REQUEST);
ncl_message_set_method(request, "/plc/grind");
ncl_client_method_call_async(client, request, 5000, &ack);   /* ack: code=OK + handler */
char handler[64];
snprintf(handler, sizeof(handler), "%s", ncl_message_handler(ack));   /* 借用指针 */

/* 进度：跑着 status=executing、完成 stopped，process 由宿主可选上报 */
ncl_client_method_status(client, sn, handler, 5000, &status);

/* 结果：未完成回 code=PENDING（不带 result），完成后回 code + return + result
 * （finished / error），并且这个句柄同时被释放 */
ncl_client_method_result(client, sn, handler, 5000, &result);
```

设备端不用做任何异步的事：工具方法就是普通函数，`ncl_server` 把它丢进共享线程池、
生成句柄、登记状态与结果，并负责回答上面两条查询；想报进度再调
`ncl_server_report_method_progress(server, handler, process, status)`（可选）。
句柄表在 `ncl_server_free()` 时安全回收（正在跑的交给 worker 自己释放）。

### 4.2 消息与消息项

一个 `ncl_message` 对应一种 NC-Link 报文（22 种），字段用 `ncl_message_set_*`
写入、`ncl_message_write_string()` 序列化，字段顺序按规范固定。

```c
ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
ncl_query_request_item *item = ncl_query_request_item_new("/STATUS");
ncl_message_add_query_request_item(request, item);   /* 所有权转移 */
ncl_message_finalise(request);                       /* 补 @id（UUID） */
char *json = ncl_message_write_string(request);
```

解析走主题推断：

```c
ncl_message *msg = ncl_message_parse(topic, payload, payload_len);
```

### 4.3 数据模型与路径

模型就是 `conf/model/nclink.json` 那份文档：

```json
{"name":"nclink","id":"01","type":"NC_LINK_ROOT","devices":[
  {"id":"02","type":"PLC",
   "configs":[{"id":"ch1","type":"SAMPLE_CHANNEL",
               "sampleInterval":1000,"uploadInterval":5000,
               "ids":[{"id":"/STATUS"},{"id":"030002"}]}],
   "dataItems":[{"id":"030001","type":"STATUS"},
                {"id":"030002","type":"PART_COUNT"}],
   "version":"2.0"}]}
```

路径规则：

| 节点位置 | 路径 |
|----------|------|
| 根节点 | `/` + type，即 `/NC_LINK_ROOT` |
| 设备节点 | 根路径 + `/` + type，即 `/NC_LINK_ROOT/PLC` |
| 数据项/配置项，父是设备 | `/<type>[@<number>]`，即 `/STATUS` |
| 数据项/配置项，父是组件 | 父路径 + `/<type>[@<number>]` |
| 节点带 `source` 字段 | `/<source>/<type>`，**覆盖**父路径 |

客户端拿到模型后可以路径 ↔ id 互查：

```c
char *id = ncl_client_get_id(client, "/STATUS");    /* "030001" */
char *path = ncl_client_get_path(client, "030001"); /* "/STATUS" */
```

读写接口的 `path` 参数：以 `/` 开头按路径解释；否则按节点 id 查模型再换算成路径
（服务端 `ncl_server_resolve_path()` 有同样的规则）。

### 4.4 工具注册与 `<operation>#<path>` 绑定

把「模型路径 + 操作」映射到 C 函数用显式注册：

```c
ncl_server_register_tool(server, "plc", instance, methods, method_count,
                         bindings, binding_count);
```

绑定键形如 `<operation>#<path>`，例如
`get_value#/STATUS`、`set_value#/CONTROLLER/FILE`。

| operation（枚举 → 字符串） | 含义 |
|---------------------------|------|
| `NCL_OP_GET_VALUE` → `get_value` | 读值 |
| `NCL_OP_GET_LENGTH` → `get_length` | 读长度 |
| `NCL_OP_GET_KEYS` → `get_keys` | 读键列表 |
| `NCL_OP_GET_ATTRIBUTES` → `get_attributes` | 读属性 |
| `NCL_OP_SET_VALUE` → `set_value` | 写值 |
| `NCL_OP_ADD` → `add` | 新建 |
| `NCL_OP_DELETE` → `delete` | 删除 |
| `NCL_OP_FUNC_CALL` / `FUNC_STATUS` / `FUNC_RESULT` / `FUNC_CANCEL` → `call` / `status` / `result` / `cancel` | 功能调用族（本工程未使用） |

工具方法的参数名要与请求里的 `params` 键对上（如 `key`、`value`、`index`、
`offset`、`length`、`keys`、`localname`），见附录 A 里各 item 的访问器说明。

方法调用（`methodCall`）不走绑定键，而是按 `/工具名/方法名` 查找方法表：

```c
ncl_message_set_method(request, "/plc/setValue");   /* 也接受 "plc/setValue" */
```

### 4.5 采样与上报

采样通道写在模型的 `configs` 里（`type` 为 `SAMPLE_CHANNEL`）：

| 字段 | 含义 |
|------|------|
| `sampleInterval` | 采样周期（毫秒） |
| `uploadInterval` | 上报周期（毫秒），内部按「采样次数」向上取整 |
| `ids` | 采样项，可写路径（`/STATUS`）或节点 id（`030002`） |

```c
ncl_server_init_samples(server);              /* 按模型启动全部通道 */
ncl_server_add_sample(server, config_node);   /* 运行时单加一条（深拷贝） */
ncl_server_remove_sample(server, "ch1");
ncl_server_stop_all_samples(server);
```

上报报文发在 `Sample/<sn>/<通道id>`，结构见 `ncl_message` 的 `sample` 分支。

#### 采样周期是通道级的，而且只有整数毫秒

`sampleInterval` / `uploadInterval` 都是**通道级**字段（`ncl_node.sample_interval` /
`upload_interval`，类型 `long long`）：`sampleInterval` 是通道的**槽位节奏**，通道里
所有采样项每一轮各取一次。采样率不同不用拆通道 —— 各列的采样率由"每槽装几个点"
体现（见上面的亚毫秒采样）：1 ms 的列每槽 1 点、0.25 ms 的列每槽 4 点，同处一个
通道，按行读即可（见"按行消费"）。只有比槽位还慢的列（例如 10 ms 一列配 1 ms
一列）才需要另想办法：拆通道，或者让该列在多个槽位里重复同一个值。

单位是毫秒，且必须是整数，最小 1 ms：

- `"sampleInterval": 0.25` 这类小数会被判成非法值（解析走整数通道），整个通道起不来。
- 1 ms 的槽位要真跑到 1 ms，得让等待别落在系统时钟粒度上（Windows 默认 ~15.6 ms）。
  库里的做法是"**短等待走高精度计时器**"，按优先级两条路：

  1. **高精度可等待计时器**（Win10 1803+）：`CreateWaitableTimerEx` 带
     `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`，精度 ~0.5 ms，不动系统时钟；
     `ncl_cond_wait_timeout()` 用 `WaitForMultipleObjects` 同时等"被唤醒"与"计时器到期"，
     采样循环因此不再被时钟粒度拖住（计时器按线程持有，避免共用时互相踩到期时间）。
  2. **回退**（老系统上该计时器建不出来时）：`timeBeginPeriod(1)` 把系统时钟粒度提到
     1 ms，再走普通等待，精度 ~1.7 ms。

  只有 ≤ 100 ms 的等待走这条路，更长的等待仍交给系统 wait（省电）。本机实测
  （500 次平均）：

  | 等待 | 调优前 | 现在 |
  |------|--------|------|
  | `ncl_sleep_millis(1)` | 14.1 ms | **1.56 ms** |
  | `ncl_cond_wait_timeout(_, _, 1)`（采样循环走的这条） | 15.5 ms | **1.56 ms** |
  | `ncl_sleep_millis(100)` | ~100 ms | ~100 ms（长等待不变） |

  效果：示例的 `EdgeSersors`（1 ms 槽位 / 100 ms 上报）在本机从 ~1.4 s 一条变成
  ~222 ms 一条 —— 剩下的 ~1.2 ms/槽是取值本身的开销（每槽 12 次完整 Query，见下面
  最后一条）。Linux 上走 `nanosleep` / `pthread_cond_timedwait`，本来就是微秒级。
  Windows 侧的依赖：MSVC 由源码里的 `#pragma comment(lib, "winmm.lib")` 自动带上，
  MinGW/GCC 手工链接时加 `-lwinmm`。
- 真要做 0.25 ms（4 kHz 振动）得先扩库，至少要动四处：`ncl_node` 加亚毫秒字段（或让
  `sampleInterval` 支持小数）、模型编解码两侧跟改、`ncl_sample_collect()` 的调度从
  毫秒换成微秒、`ncl_platform` 补一个微秒级等待（Windows 上高精度计时器也只到
  ~0.5 ms，250 µs 还得配自旋补尾或独立的采集线程）。
  注意区分两件事：要的是"**0.25 ms 一个点**"的数据，上面 4.5 的亚毫秒采样就够
  （示例的 EdgeSersors 通道就是这么做的：1 ms 槽位，功率每槽 1 点、振动每槽 4 点）；
  要的是"**每 0.25 ms 真的采一次**"（精度、抖动有要求），才需要上面这一圈扩库。
- 更硬的瓶颈在采样环路的形状：`ncl_sample_collect()` 是**每个采样项每一轮**走一次
  完整的 Query 调用（建消息、生成 UUID、JSON 编解码、绑定查表）。1 ms × 10 项已经是
  每秒上万次调用；真要做 4 kHz 波形，应该改成「一次回调给一批值」的批量取值接口，
  而不是把周期继续往下压。

#### 采样通道的两种形态与异常处理

`ncl_server_add_sample()`（以及 `addSample` 方法调用）只接受两种形态，**其余一律返回
具体错误码且不启动任务**，不做静默降级：

| 形态 | 数据从哪来 | 表头来源 |
|------|-----------|----------|
| **1. 模型文件里有定义** | 模型里的 `SAMPLE_CHANNEL`，`ids` 写节点 id（或路径） | 由节点 id 在模型里解析出路径 |
| **2. 模型里没有定义，但给了表头** | `ids` 直接写路径 | 路径原样作为表头，走该路径上的工具绑定取值 |

```c
/* 情况 2：模型里完全没有这个通道，也无所谓 —— 表头直接给路径 */
const char *json =
    "{\"id\":\"chExt\",\"type\":\"SAMPLE_CHANNEL\","
    "\"sampleInterval\":40,\"uploadInterval\":80,"
    "\"ids\":[{\"id\":\"/EXT/A@0\"},{\"id\":\"/STATUS\"}]}";
ncl_node *config = /* ncl_node_from_json(...) */;
ncl_err rc = ncl_server_add_sample(server, config);   /* 前提：/EXT/A@0 上有工具绑定 */
```

异常（返回码 → 现象）：

| 情况 | 返回码 |
|------|--------|
| 通道 `id` 为空 | `NCL_ERR_INVALID_ARG` |
| `ids` 为空（没有采样项） | `NCL_ERR_INVALID_MODEL` |
| `sampleInterval` / `uploadInterval` 缺失或 ≤ 0 | `NCL_ERR_INVALID_ARG` |
| 采样项既不是路径，也无法在模型里按 id 找到 | `NCL_ERR_NOT_FOUND` |

经 `addSample` 方法调用时，同一个错误会变成应答 `code=NG`，原因在 `reason` 里
（`ncl_err_name()` 的文本），客户端 `ncl_client_add_sample()` 相应返回错误码。

#### 亚毫秒采样：值本身可以是数组（数组套数组）

采样周期最小仍是 1 ms（**外层槽位**，通道级）；但**一次采样可以返回一批值**——工具
返回数组时，这一列的每个槽位就是那个数组，报文自然变成"槽位数组套批次数组"。
库不配置、报文不加字段，每槽装几个点完全由数据决定，而且**各列可以不一样**：

```
sampleInterval = 1ms（槽位）, uploadInterval = 200ms
功率列：工具每次返回 1 个值  → 200 个槽位 × 每槽 1 点  = 200 点
振动列：工具每次返回 10 个值 → 200 个槽位 × 每槽 10 点 = 2000 点（槽内 0.1ms）
   "data":[ {"data":[[10 个值],[10 个值],...]}, {"data":[1,2,3,...]} ]
              ↑ 批量列（每槽一批）              ↑ 标量列（每槽 1 点）
```

**采样率就是"每槽装几个点"**，所以采样率不同的数据项可以放进同一个通道：1 ms 的
列每槽 1 点、0.25 ms 的列每槽 4 点即可（`sampleInterval` 只能写整数毫秒，比槽位还
细的周期一律靠"每槽多装点"实现）。外层槽位一致就够，内层各列自便。

列内不用自己判断两层结构，用助手即可（标量列、批量列、混杂都适用）：

```c
const ncl_sample_item *item = ncl_message_item_at(msg, 0);
if (ncl_sample_item_is_nested(item)) {                 /* 该列含数组元素 */
    size_t slots  = ncl_json_arr_len(item->data);      /* 槽位数 */
    size_t points = ncl_sample_item_value_count(item); /* 该列总点数 */
    const ncl_json *first = ncl_sample_item_value_at(item, 0);   /* 列内扁平取值 */
}
```

#### 按行消费：行轴是最细的那一列

各列采样率不同时，**按行**读最省事：行数取"数据最多的那一列"的点数（最细的那根
时间轴），其余列取**覆盖该行的第一个点**，于是不同采样率的列能在同一张表里逐行对齐：

```c
size_t rows = ncl_message_sample_point_count(msg);   /* 行数 = 最多那列的点数 */
for (size_t row = 0; row < rows; row++) {
    for (size_t col = 0; col < ncl_message_item_count(msg); col++) {
        const ncl_json *v = ncl_message_sample_value_at(msg, row, col);
        ...
    }
}
```

规则：最多那列逐点展开（第 `row` 行就是它的第 `row` 个点）；更粗的列落在同一段里
就反复取该段的第一个点。示例（一个通道：功率 1 ms + 振动 0.25 ms）：

```
按行消费: 400 行（数据最多的那一列的点数；100 个槽位 × 每槽 4 点）
  行[0] /AXIS@X/POWER@1=800.0  /AXIS@X/ACCELERATION@X=-1.0      ← 功率 4 行共用同一个点
  行[1] /AXIS@X/POWER@1=800.0  /AXIS@X/ACCELERATION@X=-0.875
  行[2] /AXIS@X/POWER@1=800.0  /AXIS@X/ACCELERATION@X=-0.75
  行[3] /AXIS@X/POWER@1=800.0  /AXIS@X/ACCELERATION@X=-0.625
  行[4] /AXIS@X/POWER@1=812.5  /AXIS@X/ACCELERATION@X=-0.5
```

C 与 C++ 两个客户端示例的回调都是这么消费的（各打前 8 行）。

#### 消费端拿到的报文一定是完整的（外层槽位对齐即可）

设备端发布前会校验 `ncl_message_sample_is_complete()`：

| 层次 | 要求 |
|------|------|
| 外层 | 表头（`paths`）非空；表头项数 == 数据块列数；**每列槽位数相同**且 ≥ 1 |
| 内层 | 不设统一形状：标量、数组、每槽点数不等都可以（各列按自己的采样率走） |

唯一例外是**整列 `[]`**：那是"本周期该项没有数据"的占位，判为还没填完，交给
`ncl_message_sample_normalise()` 换成 `null` 之后再发。外层对不上的窗口会被
**丢弃**并记 `采样报文不完整，已丢弃: 通道 xxx`。消费端可以再兜一层：

```c
if (!ncl_message_sample_is_complete(msg)) {
    ncl_log_warn("采样报文不完整，已忽略: %s", topic);
    return;
}
```

（某一列某次取值失败时，该值是 `null`：列还在、表头对得上，属于"完整但含空值"，
不是残缺报文。）

设备端（尤其是别家实现）也可能**省掉表头** `paths`，或用 `[]` 给"本周期这一项
没有数据"占位。这类报文客户端会先用设备模型兜一次：**在把报文交给采样回调之前**
自动调用 `ncl_message_sample_normalise()`，能补就补，补不了原样交过去。

- **表头**：`paths` 缺失或项数与 `data` 列数不符时，拿通道 id 到模型里找同名
  `SAMPLE_CHANNEL`，按它声明的 `ids` 顺序补出表头；模型项数与列数对不上就不补。
- **空列**：整列都是 `[]` 时换成等长的 `null`（按列处理，不看别的列是什么形状）。

补不了就绝不猜：通道查不到、项数对不上，报文原样交给回调，
`ncl_message_sample_is_complete()` 继续如实返回 false。自己直接吃 MQTT 时（见
6.6）也可以照样调它：

```c
ncl_message_sample_normalise(msg, ncl_client_root_node(client));
if (!ncl_message_sample_is_complete(msg)) {
    return;                            /* 还是补不出来，按残缺处理 */
}
```

注意：`ncl_message_is_valid()` 只检查外层（各列槽位数一致）；
`ncl_message_sample_is_complete()` 在它之上再多排掉"整列 `[]`"的占位列，两者都不
管内层形状。判断采样报文能否消费请一律用后者。

报文内容（也是客户端回调里拿到的字段）：

| 字段 | 含义 |
|------|------|
| 主题末段 / `sample->as.sample.id` | 采样通道 id |
| `sample->as.sample.paths` | **表头**：本次采集的数据项路径**数组**，与 `data` 按下标一一对应 |
| `sample->as.sample.data[i]` | 第 i 项的数据：`{encoding, data:[本轮各次采样值]}`，未编码时 `encoding` 为空 |
| `sample->as.sample.interval` | **采样周期**，取模型里该通道的 `sampleInterval`；注意报文键名是 `interval`，不是 `sampleInterval` |
| `sample->as.sample.upload_interval` | 上报周期，取模型里的 `uploadInterval` |
| `sample->as.sample.begin_time` | 本窗口起始时刻（epoch 毫秒，字符串形式） |

一条真实报文（字段顺序按规范固定）：

```json
{"paths":["/STATUS","/PART_COUNT"],"id":"ch1","beginTime":"1789450135470",
 "data":[{"data":[0,0]},{"data":[129,139]}],"interval":1000,"uploadInterval":2000}
```

即：`paths` 是表头（采集了哪些数据项），`data[i].data` 是第 i 项在本窗口内按时间
先后采到的值；`interval` = 模型 `sampleInterval`，`uploadInterval` = 模型
`uploadInterval`。若上层要一行字符串形式的表头（日志/CSV），用
`ncl_message_sample_header(msg, ";")` 把同一个数组拼起来即可。

两点容易误解的地方：

| 事项 | 说明 |
|------|------|
| 路径分隔符 | 组件/数据项的编号用 `@`：`/AXIS@0/POSITION` |
| `params` 后缀 | 采样项若在 `ids` 里带了 `params`（LIST 的 `indexes` / HASH 的 `keys`），模型层 `ncl_sample_ref_path()` 会给出带后缀的形式（`/AXIS@0/TRACE$LIST-0`、`/AXIS@0/PARAM$HASH-speed`，多个索引时是 `$LIST-[0, 1]`）。但**设备端上报的 `paths` 用的是数据项本身的路径**，不带后缀 |

对端怎么收这些报文见 5.5 的「采样」与 6.6。

### 4.6 错误码

所有会失败的接口返回 `ncl_err`（`int`）。`NCL_OK == 0`，其余为负值：

| 区间 | 含义 |
|------|------|
| `0` | 成功 |
| `-1 … -13` | 基础设施错误（`NCL_ERR_NOMEM` `-2`、`NCL_ERR_PARSE` `-3`、`NCL_ERR_TIMEOUT` `-4`、`NCL_ERR_IO` `-5`、`NCL_ERR_NOT_FOUND` `-6`、`NCL_ERR_INVALID_ARG` `-9`、`NCL_ERR_STATE` `-10`、`NCL_ERR_CONNECT` `-12`…） |
| `-100 … -117` | 协议域校验错误，19 条校验规则各一个（如 `NCL_ERR_INVALID_VALUE` `-116`） |

`ncl_err_name(err)` 返回稳定的文本名称（如 `"InvalidValueException"`），
便于日志与对端比对。完整表格见附录 B。

**注意**：返回 `ncl_err` 的函数在出错时**不一定**会写满出参，调用前把出参置空是
好习惯（库内部在失败路径上也会尽量置空）。

### 4.7 所有权与内存约定

C 没有 GC，规则统一为「谁申请谁负责，转移要显式」：

| 场景 | 规则 |
|------|------|
| `ncl_*_new()` / `cln_strdup()` / `ncl_message_write_string()` … | 返回堆内存，**调用方 `free()`** |
| 集合元素 | `ncl_ptrvec` / `ncl_strvec` 带析构回调，`*_free()` 时自动释放元素 |
| `ncl_message_add_*_item(msg, item)` | 成功后**所有权转移**给消息 |
| `ncl_client_query/set/method_call(client, request, …)` | 接管 `request`，响应放 `*out`，**调用方** `ncl_message_free()` |
| `ncl_server_invoke_*()` | 返回新消息，调用方负责释放 |
| `ncl_client_on_message(client, topic, msg)` | **任何情况下**接管 `msg` |
| 事件回调里的 `ncl_message *` | **借用**，回调返回后立即释放，不能保存指针 |
| `ncl_ptrvec_at()` / `ncl_client_root_node()` / `ncl_message_get_data()` | 借用，不要释放 |
| JSON 上的 `ncl_json_obj_get()` / `ncl_json_arr_get()` | 借用；`ncl_json_arr_take()` 才是脱开所有权 |
| `ncl_message_take_model(probe)` | 从消息里摘出模型，调用方接管（配合 `ncl_client_set_root_node()`） |
| `ncl_cache_take()` / `ncl_ptrvec_take()` | 摘除且不释放，所有权交给调用方 |
| `ncl_http_server_own_context(server, ctx, free_fn)` | 把上下文交给 HTTP 服务器：随 `ncl_http_server_free()` 释放**一次**（路由可共用同一个 ctx；同一指针登记两次返回 `NCL_ERR_EXISTS`）。登记后调用方不要再自己释放；传栈上/静态对象的旧写法不变（不登记即可） |

上表里的「调用方 `free()`」在默认构建里就是 C 运行库的 `free()`。**静态池构建
（4.9）下要改用 `ncl_free_safe()`**：库的内存来自池，和运行库的堆是两处，用 libc
的 `free()` 去放池指针会破坏堆。库自己的 `ncl_*_free()` 系列本身是对的用法。

### 4.8 线程模型与线程安全

| 组件 | 线程 | 说明 |
|------|------|------|
| `ncl_mqtt_client` | 1 个收包线程 | 回调 `on_message` **在收包线程上**执行，不要阻塞 |
| 服务端 | 共享线程池（5/10/100 + CallerRuns） | `ncl_server_on_message()` 提交后立即返回；工具方法在工作线程执行，可放心阻塞 |
| 采样 | 每通道 1 个线程 | 与请求处理并发 |
| HTTP 服务 | 1 个连接线程 + 监听线程 | 请求处理在监听线程上串行 |
| FTP 服务 | 1 个接受线程 + 每会话 1 个线程 | |
| 客户端 | 无自有线程 | `ncl_client_*` 请求会阻塞等待响应，可在任意线程调用；响应由收包线程写入缓存 |

| 对象 | 并发使用 |
|------|----------|
| `ncl_message` / `ncl_json` / `ncl_node` | **不**线程安全，按线程私有使用 |
| `ncl_client` | 请求/响应缓存有锁，可从多线程调用；同一时刻的并发请求各自按 `@id` 关联 |
| `ncl_client_holder` | 线程安全（内部互斥），`get()` 可随时调用 |
| `ncl_server` | 内部状态有锁；注册工具请在 `subscribe()` 之前完成 |
| `ncl_logger` | 线程安全 |
| `ncl_ftp_server` / `ncl_http_server` | 启停与运行线程安全，`stop()` 会 join 全部线程 |

日志函数（`ncl_log_info` 等）可在任意线程调用。

### 4.9 静态内存（无堆）构建

库内每一次分配都走 `ncl_mem_alloc()` / `ncl_mem_calloc()` / `ncl_mem_realloc()` /
`ncl_mem_free()`（`src/core/ncl_mem.c` 是唯一知道内存从哪来的文件，471 处分配点
已经全部改道）。默认实现直接转发给 C 运行库；打开 `NCLINK_STATIC_MEM` 后换成
**静态数组里的一个固定池**：库不再调用 `malloc`，池耗尽就返回 `NULL`（上层统一
翻成 `NCL_ERR_NOMEM`），绝不会偷偷回退到堆。

```powershell
.\build.ps1 -StaticMem                          # Windows：默认 20 MiB 池
.\build.ps1 -StaticMem -MemPoolBytes 65536      # 小设备：显式给 64 KiB
.\build.ps1 -StaticMem -MemReport               # 顺手量一下峰值
```

```sh
NCL_STATIC_MEM=1 ./build-linux.sh build-linux-static          # 默认 20 MiB
NCL_STATIC_MEM=1 NCL_MEM_POOL_BYTES=65536 ./build-linux.sh build-linux-static-64k
```

> 发布包里的 `lib/*-staticmem/` 就是按**默认 20 MiB** 编好的静态内存版（同一个包里默认堆版与它并存，目录名区分），细节见包内 RELEASE.md。

**默认池是 20 MiB**（`NCLINK_MEM_POOL_BYTES` / `NCL_MEM_POOL_BYTES`），对全量测试
与文件搬运都留了余量，先跑通再按下面的实测数据往下压。池放在 `.bss` 里，不占可执行
文件体积，也不占栈；换的小只是省 RAM。

**池开多大是量出来的**：加上 `-MemReport`（或 `-DNCLINK_MEM_REPORT=ON`）跑一遍真实
流量，退出时会打一行：

```
ncl_mem: static-pool peak 25632 of 65536 bytes, live 269 blocks, 3236 allocations, 0 failures, 0 foreign frees; free 45728 bytes in 13 blocks (largest 43808), largest request 1024
```

被拒的时候还会**当场点名被拒的是多大的请求**（最多打 8 行，之后只计数），这一行
才是定池大小最直接的依据——汇总行里 `largest request` 只统计被记进直方图的请求，
"请求比整个池还大"这条路径不进直方图：

```
ncl_mem: refused 81920 bytes with 59296 free bytes (largest contiguous 44000)
```

Linux 上再加 `-DNCL_MEM_TRACE=1`（`src/core/ncl_mem.c`，glibc `backtrace`）会把调用栈
一起打出来，纯粹用于定位"到底谁在要这块内存"，不随发行包提供。`build-linux.sh`
侧对应 `NCL_MEM_REPORT=1`（与 `build.ps1 -MemReport` 同一份统计）：

```sh
NCL_STATIC_MEM=1 NCL_MEM_POOL_BYTES=65536 NCL_MEM_REPORT=1 ./build-linux.sh build-linux-rep
```

运行时也可以读 `ncl_mem_get_stats()`：池大小、当前用量、峰值、活块数、**总空闲字节与
空闲块数**、最大连续空闲块、**历史最大单次请求**、分配次数、拒绝次数、外来释放次数、
**最后一次拒绝时的空闲快照**、坏链计数、**实际占用峰值（`peak_footprint_bytes`，含块头）
与每个尺寸类的用量**（见 4.9.2）。

本仓库全量测试（39 套）的实测峰值（Windows x64 / MSVC，20 MiB 池，尺寸类开启；
单位字节，**载荷口径**的峰值与最大单次请求，测试日志原值。含块头的"实际占用"口径
见 4.9.2）：

| 用例 | 峰值 | 最大请求 | 用例 | 峰值 | 最大请求 |
|------|------|------|------|------|------|
| file | 1092288 | 1060922 | s7 | 8000 | 4176 |
| ftp | 38608 | 16624 | mtconnect | 8400 | 2048 |
| event | 19952 | 1536 | meldas | 4480 | 664 |
| rest | 17520 | 2760 | lsv2 | 8448 | 4312 |
| message | 14672 | 1024 | syntec_driver | 12416 | 8328 |
| client | 13008 | 2600 | audit | 6304 | 1536 |
| server | 23792 | 2600 | knd | 3888 | 512 |
| config | 6800 | 2760 | adapter | 15664 | 1536 |
| http | 5904 | 2760 | driver_manager | 10640 | 1536 |
| model | 10368 | 1024 | driver | 3472 | 1536 |
| schema | 4032 | 1536 | modbus | 4656 | 736 |
| mqtt_client | 4016 | 2600 | mc | 8048 | 4280 |
| thread | 2208 | 1600 | fins | 8928 | 4280 |
| mqtt | 544 | 64 | cpp | 2736 | 336 |
| json | 1568 | 64 | topic / codec / common | 48 / 32 / 64 | 35 / 12 / 64 |

`mem` / `mem_mc` / `mem_mt` 三套是分配器自己的压测，故意把池吃满（峰值 12~16 MiB、
拒绝数千次），它们的数字不代表业务流量，故不入表。

- 实测边界（Linux / gcc 13，**39 套口径**——适配器插件化之前的测量，本轮未重跑）：
  **32 KiB → 37/39**（`file`、`ftp` 被拒）、**64 KiB → 38/39**（只剩 `file`）、
  **1.5 MiB → 39/39**。设备端常见组合
  （model + message + client/server + mqtt + 各协议驱动）在 **32~64 KiB** 就够——
  上表里除 `file` 外最大的 `ftp` 也只到 38 KiB。
- `file` 这一套为什么是大户：它用 `ncl_file_read_all()` 把 1 MiB 的文件**整块读进池里**
  与上传前的内容比对，12 万字节级的大块只能来自通用区，而默认尺寸类区占池的 1/4，
  于是池要 ≥ 4/3 × 1.06 MiB。两个办法：设备上改用流式读写（`ncl_file_write_chunk()` /
  `ncl_file_read_chunk()`，16 KiB 一块，`ftp` 那套跑的就是它），或把尺寸类区压小
  （`-MemClassBytes` / `NCLINK_MEM_CLASS_BYTES`）——实测尺寸类区为 0 时 **1.125 MiB
  池 39/39**。默认的 20 MiB 是"先跑通"的余量口径。
- 分配策略：**最佳适配**（能装下的最小空闲块） + 释放时**双向合并**；块头 32 字节、
  载荷 16 字节对齐（`NCL_MEM_ALIGNMENT`）；剩余空间小于"块头 + 对齐"时不再切分，免得
  池里堆满永远用不上的碎屑。同尺寸请求会命中完全匹配而提前结束查找。

**碎片是怎么处理的**（这部分给的是实测，不是设计承诺）：

1. **不留永久空洞**：同一套流量反复跑，工作负载回到空闲态时，它用过的空间必须重新
   变成一整块。`tests/test_mem.c` 的 `test_pool_fragmentation` 就是这条：长期块
   （模拟模型/服务端表）留在池里，周围做 6 轮 × 300 次混合尺寸的分配/重分配/释放，
   每轮结束断言**空闲块数 == 1**、最大连续空闲块**逐字节等于**轮次开始前的值。
2. **拒绝要能归因**：非紧凑分配器有一种固有失败——**单次请求大于最大连续空闲块**，
   哪怕总空闲够。为此池会记录最后一次拒绝时的 `failure_free_bytes` 与
   `failure_largest_free_bytes`：两个数接近 = 池真的小了；前者远大于后者 = 形状被
   打散了（该换更大的池，或把长期数据与短命缓冲分开用两个池）。
   实测抓到过一次真实的形状拒绝：64 KiB 池 + 首适配跑 file 用例，
   **总空闲 21152 / 最大连续 10592 / 请求 16384** → 拒绝；换成最佳适配后同一场景
   **0 拒绝**（file 峰值 47776，仍在 64 KiB 内）。
3. **不能消除的部分**：没有压缩（compaction），交出去的指针永远有效，所以上面第 2 条
   的"超大单次请求"只能靠**把池开够**（经验：峰值 × 1.5～2，或直接看
   `largest_request_bytes` 加上长期数据集的大小）。长期数据与短命缓冲混合、且尺寸差异
   极大时，最稳的做法是两个池（本项目目前是一个池，够用但不宣称免疫）。

**块的固定开销与请求尺寸分布**（决定"该不该用尺寸类"，也是小池的主要成本）：

- 每个块 32 字节块头 + 载荷按 16 字节对齐。所以一次 64 字节的请求实际占用 96 字节
  （32％ 是开销），一次 1 KiB 的请求占用 1056 字节（3％）。池里同时活着的块越多，
  这块开销越大：实测 message 用例在 64 KiB 池里同时有 282 个块，
  **9024 字节（14％）花在块头上**。`ncl_mem_stats.meta_bytes` 直接给这个数。
- 实测请求尺寸分布（`size_hist`，各用例真实流量）：**98％ 以上的请求 ≤ 128 字节**
  （server 16760 次里 16503 次、message 3273 次里 3238 次、ftp 786 次里 772 次），
  只有文件/FTP 通道会出现 8 KiB、16 KiB 的大请求。也就是说：库的分配画像天然适合
  "小对象尺寸类 + 大块走通用区"，见下面"下一步"。
- **增长型缓冲会放大单次请求**：`ncl_strbuf` 从 64 字节起翻倍，`ncl_ptrvec` 从 8 起
  翻倍，模型 map 从 16 起翻倍，JSON 对象数组同理。翻到 N 字节的过程中，旧的 N/2 与
  新的 N 可能同时存在（无法原地扩展时）→ **单次最大块需求可到最终大小的 1.5 倍以上**。
  这也是"池至少要能装下 2 × 最大期望缓冲"的原因（例如 REST 的 body 上限是 4 MiB，
  真收满时池里需要 6 MB 级别的连续空间）。

**排错用的自检**：`size_t ncl_mem_check(void)` 会走一遍整个池，校验"块都在池内、
前驱链接对得上、没有两个相邻的空闲块、所有块恰好覆盖整个池"，返回问题条数（默认构建
恒返回 0）。它已经抓出过两个真实缺陷：`realloc` 原地扩张留下的陈旧前驱链接，以及
切分产生的空闲尾块没有与后面空闲块合并（两个空闲块永久并排 = 空间是空的却是碎的）。
建议：现场怀疑"谁写越界了"时，在可疑阶段后调用它。

### 4.9.1 蒙特卡洛压测（`mem_mc` 套件）

`tests/test_mem_mc.c` 用随机流量把池往死里逼，比库自身的访问模式狠得多：

- **随机尺寸**跨三个数量级（对数均匀 8 B～pool/64，另有 5% 概率落在 pool/64～pool/8 的
  大块档），**随机操作**按权重分配/释放/重分配，长期把池维持在约半满，4 个固定种子 ×
  20000 次操作。
- 每个块都带**按自身种子生成的图案**，释放前与重分配前后都抽样校验（首尾各 32 B + 每
  97 B 采样），所以"两块重叠"或"头部写进载荷"会表现为内容不符，而不是几天后的神秘崩溃。
- **每次操作后**跑一次 `ncl_mem_check()` 与账目恒等式
  `in_use + free_bytes + meta_bytes == pool_bytes`；第一次不一致会打印**出问题的那次
  操作**（操作类型、尺寸）与前 6 步的操作轨迹。
- **拒绝必须正当**：只有当"最大连续空闲块 < 请求的对齐后大小"时才允许失败；失败时总
  空闲却够的，单独记为 `shape refusals`——这就是碎片造成的失败，是量出来的，不是估的。
- **同一种子跑两遍**，两遍的成功分配数、拒绝数、形状拒绝数、峰值必须逐项相等。池若漏
  了一块、或留下一个永久空洞，第二遍就会不一致。

同一套随机序列下，**最佳适配 vs 首适配**（64 KiB 池，4 种子 × 20000 次操作）：

| 指标 | 最佳适配（默认） | 首适配（`-MemFirstFit` A/B 用） |
|------|------------------|----------------------------------|
| 拒绝总数 | 20 | 22 |
| 其中**形状拒绝**（总空闲够） | **7（35%）** | **13（59%）** |
| 最坏空闲块数 | 20 | 22 |
| 最坏"最大连续空洞" | 5504 B | 3920 B |
| 平均峰值 | 52976 B | 52640 B |

不同池大小的同一套流量（最佳适配，全部零不一致）：

| 池 | 平均峰值 | 最大单次请求 | 拒绝（形状占） | 最坏形状 |
|----|----------|--------------|----------------|----------|
| 32 KiB | 26680 B | 4095 B | 18（50%） | 19 块 / 最大连续 1280 B |
| 64 KiB | 52976 B | 8190 B | 20（35%） | 20 块 / 最大连续 5504 B |
| 20 MiB | 16697140 B | 2621163 B | 38（58%） | 39 块 / 最大连续 1281488 B |

这张表最值得记的一条：**碎片压力取决于"最大单次请求 / 池大小"这个比值，而不只是池够不
够大**。20 MiB 池在随机流量里仍出现 22 次形状拒绝，因为随机流量会一次性申请到 2.6 MB
（池的 1/8）。库自身的常规最大请求是 16 KB（文件流式的块，见 4.9 的表），所以在
64 KiB 池里它占 1/4——"够用但很紧"说的就是这种比值：块一大就要从通用区拿，而通用区
默认只有池的 3/4（4.9.1 的 4/3 规则就是这么来的）。

跑法：`build.ps1 -StaticMem -MemPoolBytes 65536` 之后 `ctest -R mem_mc`；想复现上面的
首适配对照，加 `-MemFirstFit`（Linux：`NCL_MEM_FIRST_FIT=1`）。堆构建下这个套件同样会
跑（尺寸分布与内容/一致性检查与分配器无关），只是没有池统计可比。

### 4.9.2 小对象尺寸类

实测的请求尺寸分布是**极度偏小**的：全量测试里 **98% 以上的请求 ≤128 字节**，而其中
33～64 字节那一档就占了大约一半（结构体、key、短字符串）。所以池在首次使用时把自己
切成两块，小对象走**定长尺寸类**，其余走通用区：

```
+----------+----------+---- ... ----+------------------------------------+
| 类 16 B  | 类 32 B  |     ...     | 通用区（块头 + 变长块）             |
+----------+----------+---- ... ----+------------------------------------+
 <---------- NCL_MEM_CLASS_BYTES ---->   默认 = 池的 1/4（最多占一半）
```

尺寸类为 {16, 32, 48, 64, 96, 128, 192, 256} 字节。**为什么不是 2 的幂**：一个尺寸为 C
的类只在请求大于 C−32 时才划算（通用区的成本是"对齐后的请求 + 32 字节块头"），C=128
时 65～96 字节的请求在通用区反而更便宜；插入 48 与 96 两档把这个亏损窗口压到很小。

- **类内没有块头**：定长块不需要 size 字段，释放时靠"指针落在哪个区域"判定类别
  （一次范围检查），因此分配/释放都是 **O(1)**，而且**类内结构上不可能产生外部碎片**。
- **代价只有内部碎片**：40 字节的请求占一个 48 字节块（对比通用区的 32+48=80）。
- **类用完了自动回落通用区**：所以尺寸类配小了只损失速度，**不会产生新的失败模式**。
- **区域份额按实测分布加权**（`g_class_share`），可用宏覆盖；`-MemReport` 会打印每一类的
  `live/free/region` 用量，现场按真实流量调。

实测收益（Windows x64 / MSVC，64 KiB 池，39 套全部重跑，**实际占用＝载荷＋块头**的
峰值，开/关尺寸类各编一份）：

| 用例 | 关（字节） | 开（字节） | 省 | 用例 | 关 | 开 | 省 |
|------|-----------|-----------|----|------|----|----|----|
| server | 36384 | 25744 | **29.2%** | model | 16304 | 10720 | 34.2% |
| message | 24064 | 16256 | **32.4%** | event | 34864 | 23776 | 31.8% |
| client | 19520 | 13584 | **30.4%** | json | 2800 | 1568 | 44.0% |
| rest | 25904 | 18000 | **30.5%** | schema | 7456 | 4032 | 45.9% |
| config | 8896 | 6720 | 24.5% | mqtt | 1120 | 544 | 51.4% |
| adapter | 25936 | 17120 | **34.0%** | knd | 6976 | 3984 | 42.9% |
| driver_manager | 16752 | 10768 | **35.7%** | meldas | 7712 | 4544 | 41.1% |
| modbus | 7792 | 4784 | **38.6%** | s7 | 11152 | 8128 | 27.1% |
| mem | 65376 | 49504 | 24.3% | ftp | 39584 | 38768 | 2.1% |

设备端那几条主力路径都在 **29%～38%**，与设计预期一致；`ftp` 收益小是因为它几乎全是
16 KiB 的大块（本来就不进尺寸类）。**池大小的边界也跟着变了**：不开尺寸类时 32 KiB
池会多丢一套 `message`（开尺寸类后它能过），64 KiB 起 38/39（39 套口径，含 `file`
之前的所有套件；`file` 在 64 KiB 池里连跑都跑不完，所以不进这张表——见 4.9）。

度量口径提醒：`in_use_bytes` 只算载荷，**不含块头**，因此它天然偏向通用区；要比较
"池到底省没省"，看 `footprint_bytes` / `peak_footprint_bytes`（载荷 + 每块 32 字节
块头，尺寸类块整块计入）。这两个字段与每类的 `class_size/class_bytes/class_live/
class_free` 都在 `ncl_mem_stats` 里。

开关：`build.ps1 -StaticMem -MemClassBytes 0` 关掉尺寸类做对照（Linux：
`NCL_MEM_CLASS_BYTES=0`）；想给小池多留通用区，就把它调小（例如 `-MemClassBytes 8192`）。
- 线程安全：池自带锁，Windows 上是静态初始化的 `CRITICAL_SECTION`，POSIX 上是
  `PTHREAD_MUTEX_INITIALIZER`，都不需要先分配对象（否则会自己咬自己）。单上下文/
  裸机可以 `NCLINK_MEM_SINGLE_THREAD=ON` 去掉锁。
- 排错：`-DNCLINK_MEM_STRICT=ON` 时，一旦释放了池没发出去的指针就 `abort()` 并打印。
- **接口约定**：静态池构建下，库交出来的指针只能用 `ncl_free_safe()` 或
  `ncl_*_free()` 释放，**不能用 libc 的 `free()`**。仓库里的测试、示例与 `ncl.hpp`
  都按这条改过；把 `free` 当回调传的地方也要跟着改，例如
  `ncl_ptrvec_init(&v, ncl_mem_free)`、`ncl_cache_create(ttl, false, ncl_mem_free)`。
- **池管不到的部分**（要一整块堆都不用，还需这些）：C 运行库自己的内部缓冲（`fopen`
  的文件缓冲）、`getaddrinfo()`、线程栈（`pthread_create` / `_beginthreadex` 由 OS
  分配）、以及打开 TLS 后的 OpenSSL。也就是说「**库的分配全静态**」这一步已经完成；
  「**整个进程零堆**」还要把传输层接到 RTOS 的 socket/线程上——`ncl_platform.h`
  （互斥、条件变量、线程、时钟）与 `ncl_socket.h`（TCP/TLS）就是这两个接缝。

### 4.9.3 长跑（soak）：多个池尺寸并行

不同池尺寸是**编译期常量**，所以"多尺寸同时压"要编多份程序：
`tools/soak-linux.sh`（Windows/macOS 上加 `--docker`）会为每个尺寸编一份库 + 压测程序，
然后**并行**跑满指定时长（默认 3600 s）：

```sh
./tools/soak-linux.sh --docker                                   # 1 小时，7 个默认尺寸
NCL_SOAK_SECONDS=600 NCL_SOAK_SIZES="65536 1048576" ./tools/soak-linux.sh --docker
# 并发版（每个尺寸内再起多个线程，线程间传递块所有权）：
NCL_SOAK_MT=1 NCL_SOAK_SECONDS=600 NCL_MEM_MT_THREADS=8 ./tools/soak-linux.sh --docker
```

每一份都是蒙特卡洛流量（随机尺寸跨三个数量级、随机分配/释放/重分配），**每次操作后**校验
池不变量与账目恒等式，**每一轮**把池彻底排空并要求它回到"一整块空闲"，任何一条不满足就
判失败并非零退出。日志在 `build-soak/<模式>/<尺寸>.log`（每 30 s 一行心跳，末行是累计值）。

实测（2026-09-17，单机 18 逻辑核，7 个进程并行，3600 s，容器 exit=0）：

| 池 | 轮数 | 操作数 | 分配数 | 拒绝 | 其中形状拒绝 | 最坏空闲块数 | 最坏最大连续空洞 |
|----|------|--------|--------|------|--------------|--------------|------------------|
| 16 KiB | 102834 | 20.57 亿 | 8.70 亿 | 673550 | 412535 | 20 | 128 B |
| 32 KiB | 71678 | 14.34 亿 | 6.06 亿 | 212260 | 127278 | 22 | 368 B |
| 64 KiB | 46053 | 9.21 亿 | 3.90 亿 | 91505 | 54472 | 23 | 864 B |
| 128 KiB | 27734 | 5.55 亿 | 2.35 亿 | 53184 | 31154 | 26 | 2080 B |
| 512 KiB | 8294 | 1.66 亿 | 0.70 亿 | 19748 | 11638 | 29 | 9552 B |
| 4 MiB | 3080 | 0.62 亿 | 0.26 亿 | 13030 | 7840 | 36 | 62864 B |
| 20 MiB | 716 | 0.14 亿 | 0.06 亿 | 4869 | 2964 | 41 | 332992 B |
| **合计** | **260389** | **约 52.1 亿** | **约 22.0 亿** | 1068146 | 647881（60.7%） | — | — |

- 每个进程都是 `checks, 0 failures`，收尾 `soak: 0 process(es) failed`，容器 `exit=0`；
  结束后各池一律 `in_use 0` + **一整块空闲** → 26 万轮里没有一轮留下永久空洞。
- 形状拒绝在所有尺寸下都稳定占拒绝的约 **60%**：这是**合成**流量的特征（存活集稳在约半个
  通用区、单次请求可占池的 1/8），不是库的真实画像；它衡量的是"池被随机流量逼到极限时
  还剩多少形状余量"。
- 最坏"最大连续空洞"≈ 池的 1/128～1/50，可作为"最大单次请求 / 池大小"这条经验的量化对照。

### 4.9.4 多线程

池是**一个全局对象**（与 C 运行库的堆一样），库里所有线程都打到同一个池上：MQTT 收包线程、
服务端线程池（5/10/100）、采样线程、HTTP/FTP 受理线程。因此：

- 池自带一把锁，覆盖**整个**分配/释放/重分配临界区（含尺寸类的空闲链操作）。Windows 上是
  静态初始化的 `CRITICAL_SECTION`，POSIX 上是 `PTHREAD_MUTEX_INITIALIZER`——都不需要
  "先分配一把锁"，否则池会自己咬自己。
- **跨线程所有权是允许的，也是库依赖的行为**：一个线程分配的块可以由另一个线程释放
  （响应报文由收包线程分配、调用方线程释放）。锁保护的是池的结构，不是"谁分配谁释放"。
- 单上下文/裸机可用 `NCLINK_MEM_SINGLE_THREAD=ON` 去掉锁；**去掉之后就不能再有第二个
  线程碰池**（`mem_mt` 套件在这种构建下会自己跳过）。

**怎么验证的**（`tests/test_mem_mt.c`，套件名 `mem_mt`，已进常规 ctest）：

- 多个线程共享同一个池做随机流量，每块带图案、释放前校验——**同一块被同时交给两个所有者**
  会直接表现为内容不符；
- 线程间通过环形队列**传递所有权**：A 分配并写图案，B 收到后校验再释放；
- 主线程在它们跑的同时不停调用 `ncl_mem_check()`：该自检在同一把锁下取快照（所以"看到的
  状态本来就必须一致"），并且会**从块本身重算**账目恒等式 `已用 + 空闲 + 块头×块数 == 池大小`
  再与计数器比对——这正是它能当并发不变量用的原因；
- 收尾要求池回到一整块空闲、`in_use 0`，且拒绝路径正确（申请"最大空洞"大小必须成功，
  比它大一个对齐单位必须被拒）。

```powershell
.\build.ps1 -StaticMem -MemPoolBytes 65536      # 常规：4 线程 × 40000 次操作
cd build-static-64k\tests; .\ncl_test_mem_mt.exe
```

工具侧：`./tools/asan-linux.sh --docker` 已把 `test_mem_mt` 纳入（ASan + 泄漏检测）；
`./tools/asan-linux.sh --tsan --docker` 用 **ThreadSanitizer** 专压这个并发用例
（TSan 与容器 ASLR 冲突，脚本会自动把 `vm.mmap_rnd_bits` 降到 28，因此需要 `--privileged`）。

单轮实测（64 KiB 池，4 线程 × 40000 次操作）：**160000 次操作、22838 次跨线程交接、
0 处内容不符、0 处不变量违规**；TSan 下 6 线程 × 20000 次操作 **0 数据竞争**；
并发长跑（8 线程/进程 × 3 个池尺寸，600 s，容器 exit=0）：

| 池 | 操作数 | 分配数 | 拒绝 | 跨线程交接 | 每 worker 峰值存活 | 轮数 | 失败 |
|----|--------|--------|------|------------|--------------------|------|------|
| 64 KiB | 3.82 亿 | 0.97 亿 | 8219928 | 54550267（54521324 收到） | 16167 B | 4778 | 0 |
| 512 KiB | 1.64 亿 | 0.42 亿 | 3344825 | 23470775（23458865 收到） | 138306 B | 2054 | 0 |
| 20 MiB | 0.16 亿 | 0.04 亿 | 364793 | 2316368（2314926 收到） | 5850785 B | 202 | 0 |
| **合计** | **约 5.63 亿** | **约 1.44 亿** | 11929546 | **约 8030 万**（差额是收尾时排空的环形队列） | — | 7034 | **0** |

7034 轮并发轮次、每轮都以"池回到一整块空闲 + 账目恒等式成立"收尾，全部通过；期间主线程
持续调用 `ncl_mem_check()`，**没有报出任何一次不一致**。

### 4.9.5 集成约束：把静态池版的库嵌进别人的进程

库自己"不再调用 `malloc`"只是第一步。宿主是别人的程序——它有自己的内存策略、自己的
全局状态、自己的日志——下面这些才是真正的边界，每一条都对应一个可检查的信号。

**① 两个堆，两条释放路径。** 库交出的指针只能由 `ncl_free_safe()` / `ncl_*_free()`
释放，宿主的指针也别喂进库。静态池版里这两处完全不同：

- 池外的指针进 `ncl_mem_free()`：计一次 `foreign_frees`，然后**静默丢弃**。这是默认
  行为，表现是"内存慢慢少了"，不是崩溃——所以混用不会被自动发现；编 `NCL_MEM_STRICT`
  时才会打印地址并 `abort()`（集成测试期应该一直开着）。
- 认不出的指针**不会**被转交给 libc 的 `free()`，所以混用不会立刻踩坏堆，代价就是
  上一条说的"安静"：运行期只能靠 `foreign_frees` / `bad_links` 两个计数器盯着。
- `ncl_mem_realloc()` 拿到外来指针时返回 `NULL` 并计一次 `foreign_frees`——宿主别把
  这一种 `NULL` 误读成"池满了"。
- 对齐：静态池保证 16 字节（`NCL_MEM_ALIGNMENT`；池在数组里运行时对齐，32 位同样是
  16）；堆构建给的是运行库的保证（x64 16、Win32 小块 8）。
- `ncl_mem_realloc()` 跨区/跨尺寸类时会**搬家**（类内放得下就不搬），别在库对象内部
  留借用指针跨 realloc。

**② 库用不了宿主的池。** 分配接缝是编译期的两种实现（转发运行库 / 静态池），没有
"注册自定义分配器"这类接口。宿主自己有池时，进程里就是**两个独立的池**：RAM 叠加、
互不共享、空闲也不归还谁。要做"整进程一个池"，唯一的路是把 `src/core/ncl_mem.c` 换成
自己的实现——它是唯一知道内存从哪来的文件，对外只有 4 个分配函数加
`ncl_mem_get_stats()` / `ncl_mem_check()` / `ncl_mem_mode()`。

**③ 静态版覆盖的是库的分配，不是整个进程。** 仍然走系统堆/系统资源的路径：线程栈
（`CreateThread` / `pthread_create`）、`getaddrinfo()` 与 DNS 解析（libc 内部会 malloc）、
**OpenSSL（`ssl://` 整条路）**、C 运行库与 stdio 内部。目标若是"整进程零堆"，这几条得
逐条有结论；TLS 通常是第一个破功的。

**④ 容量：客户端比设备端难估。** 客户端的池峰值是异步的——MQTT 收包线程、线程池、
采样通道、文件通道同时活着，比设备端那条"模型 + 采样 + 应答"的路径离散得多。

- 量法：真实流量跑一遍 `-MemReport`，读 `peak_footprint_bytes`（**载荷 + 块头**；
  `in_use_bytes` 不含块头，偏乐观）与 `largest_request_bytes`，池取峰值的 2~3 倍。
  默认 20 MiB 只是"先跑通"的口径，不是必须。
- 形状：池是非搬迁分配器，**单个请求不能跨空洞**——`file` / `ftp` 要一次 16 KiB 连续
  块，是最大的硬需求；`largest_request_bytes` 是池的绝对下限。长跑实测最坏空洞约为池的
  1/128~1/50（见 4.9.3），所以别只按"总空闲够"估。
- 拒绝：`failure_free_bytes` 明显大于 `failure_largest_free_bytes` 说明是被打散了，
  两个都小才是池真的小。
- 失败路径：用一个**故意很小**的池（4 KiB / 64 KiB）跑宿主的真实调用序列，把
  `NCL_ERR_NOMEM` 压出来，确认宿主有用户可见的失败语义（重试 / 降级）而不是崩。

**⑤ 锁与自检的代价。** 池用一把全局锁（Windows `CRITICAL_SECTION`、POSIX 静态
`pthread_mutex`）串行化所有分配释放；临界区很短，但多线程高频收发值得量一次争用。
`NCL_MEM_SINGLE_THREAD` 只在"库被单上下文驱动"时能开——客户端有多线程，**不能**开。
运行期自检用 `ncl_mem_get_stats()`（拷贝一份计数器快照，很轻，任意线程可调）；
`ncl_mem_check()` 要遍历整池、是 O(池大小)，只当现场诊断开关，别放进周期任务或回调。

**⑥ 进程级单例、路径与退出顺序。** 见 2.5 的第 5、6 条：一个进程一份库、安装根显式
指定。另外：日志默认还往控制台打（宿主有自己的日志系统时接管或关掉）；退出前显式收尾
（停采样 / 停 FTP、HTTP / 断开 MQTT，可选 `ncl_socket_system_release()`），别依赖宿主
的 `ExitProcess` 或 DLL 卸载顺序——池在 `.bss` 里没有析构，残留对象不会有人报出来。

**验收清单**（把下面这些变成宿主 CI 里的断言）：

| 项 | 怎么做 | 通过标准 |
|----|--------|----------|
| 池容量 | 真实流量 + `-MemReport`，读 `peak_footprint_bytes` / `largest_request_bytes` | 池 ≥ 峰值 × 2~3，且大于最大单次请求 + 常驻集 |
| 指针混用 | 集成测试期编 `NCL_MEM_STRICT` | 不再 abort，`foreign_frees == 0` |
| 健康计数器 | 运行期周期性 `ncl_mem_get_stats()`（水位看门狗） | `foreign_frees` / `bad_links` / `failures` 恒 0，水位超阈值告警 |
| 失败路径 | 故意很小的池跑真实调用序列 | NOMEM 有用户可见语义，不崩、不卡死 |
| 长跑 | 连断 + 文件 + 采样跑够时长（4.9.3 的量级） | 结束后 `in_use` 归零、`free_blocks` 回到 1 |
| 链接 | 只链一份库；`/MD`、`/utf-8`、架构一致；显式 `ncl_env_set_root()` | 零警告，运行期路径落在预期目录 |
| 零堆（若为目标） | 列清线程栈 / DNS / OpenSSL / CRT 四条 | 每条有结论：替换、接受或不用 |

---

## 5. 模块手册

### 5.1 基础层

#### ncl_common.h —— 错误码、字符串、容器

```c
char *s = ncl_strdup("abc");            /* 堆字符串，调用方 free */
char *t = ncl_str_trim_dup(" a ");      /* 去首尾空白的新副本 */
bool  b = ncl_str_starts_with(s, "ab");
char  uuid[37];  ncl_uuid4(uuid, sizeof(uuid));   /* 小写 UUID v4 */

/* 字符串缓冲：拼 JSON、拼路径、攒报文都靠它 */
ncl_strbuf sb;
ncl_strbuf_init(&sb);
ncl_strbuf_puts(&sb, "id=");
ncl_strbuf_printf(&sb, "%d", 7);
char *text = ncl_strbuf_detach(&sb);    /* 取走内容，sb 复位 */
free(text);
ncl_strbuf_free(&sb);

/* 指针容器：带析构回调，free 时自动释放元素 */
ncl_ptrvec v;
ncl_ptrvec_init(&v, ncl_file_attribute_release);
ncl_ptrvec_push(&v, attribute);          /* 所有权转移 */
ncl_ptrvec_free(&v);
```

#### ncl_platform.h —— 时间、随机数、线程、互斥、条件变量

```c
int64_t ms  = ncl_time_millis();             /* 墙上时钟 */
int64_t now = ncl_time_monotonic_millis();   /* 单调时钟，算间隔用这个 */
ncl_sleep_millis(100);
ncl_random_bytes(buf, sizeof(buf));

ncl_mutex *m = ncl_mutex_create();
ncl_cond  *c = ncl_cond_create();
ncl_mutex_lock(m);
ncl_cond_wait_timeout(c, m, 1000);           /* 唤醒或超时都返回 */
ncl_mutex_unlock(m);

ncl_thread *th = ncl_thread_start(worker, arg);
ncl_thread_join(th);                          /* join 会释放句柄 */
```

#### ncl_logger.h —— 日志

```c
ncl_log_init(NULL);              /* 写 <root>/log/out.txt，10 MB 轮转；同时输出控制台 */
ncl_log_set_level(NCL_LOG_DEBUG);/* DEBUG/INFO/WARN/ERROR/NONE，默认 INFO */
ncl_log_set_console(false);      /* 关掉控制台镜像 */
ncl_log_info("采样任务已启动: %s", id);   /* 打印中文没问题（UTF-8） */
ncl_log_error("连接失败: %s", reason);
ncl_log_shutdown();
```

控制台镜像按"真控制台"还是"管道"分别解码（Windows 上）：真控制台（cmd /
PowerShell / VS Code 终端）转宽字符走 `WriteConsoleW`，跟当前代码页无关；管道
（VS Code 调试控制台、`> file` 重定向）先转成本地 ANSI 代码页再写。两种都不对
时可以用环境变量兜底：`NCL_CONSOLE_ENCODING=utf8` 强制按 UTF-8 原样写
（默认 `auto`）。日志文件始终是 UTF-8。

#### ncl_env.h —— 运行环境、路径、SN、mqtt.cfg

安装根目录决定所有路径，启动时设定一次：

```c
ncl_env_set_root("/opt/nclink");        /* 传 NULL 恢复为当前工作目录 */
ncl_env_root();        /* /opt/nclink */
ncl_env_conf_path();   /* /opt/nclink/conf */
ncl_env_run_path();    /* /opt/nclink/bin */
ncl_env_log_path();    /* /opt/nclink/log */
ncl_env_log_file();    /* /opt/nclink/log/out.txt */
ncl_env_mqtt_cfg_file();
ncl_env_model_file();
ncl_env_sn_file();     /* /opt/nclink/bin/sn.txt */
```

文件小工具。注意 `ncl_file_write_all()` / `ncl_file_append()` **不会**创建父目录
（直接用 `fopen`），路径上层的目录请先 `ncl_mkdir_p()`；而 `ncl_file_copy()`
会为副本建好父目录。

```c
ncl_mkdir_p(ncl_env_conf_path());
char *text; size_t len;
ncl_file_read_all("in.json", &text, &len);      /* text 需 free */
ncl_file_write_all("out.json", text, len);
ncl_file_append("out.log", "more\n", 5);
ncl_file_copy("a.txt", "b.txt");                /* 会自动建 b 的父目录 */
ncl_path_exists(p); ncl_path_is_dir(p);
ncl_file_size(p); ncl_file_mtime_ms(p);
ncl_path_remove(p);                             /* 目录则递归删除 */
```

#### ncl_thread.h —— 线程池与 TTL 缓存

```c
/* 全局单例线程池（默认：核心 5 / 最大 10 / 队列 100，队满时在调用线程执行） */
ncl_thread_pool_submit(ncl_thread_service(), work_fn, arg);
ncl_thread_service_shutdown();

/* 自带线程池 */
ncl_thread_pool_options opt;
ncl_thread_pool_options_default(&opt);
opt.core_threads = 2;
ncl_thread_pool *pool = ncl_thread_pool_create(&opt);

/* TTL 缓存：客户端响应缓存、设备客户端表都用它 */
ncl_cache *cache = ncl_cache_create(5 * 60 * 1000, false, free_fn);
ncl_cache_put(cache, key, value);
void *v  = ncl_cache_get(cache, key);     /* 借用 */
void *t  = ncl_cache_take(cache, key);    /* 摘除且不释放，所有权归你 */
ncl_cache_free(cache);
```

### 5.2 ncl_json.h —— JSON DOM

紧凑输出、忽略未知字段、空值省略由调用方显式控制、数字保留原始字面量。

```c
/* 构造 */
ncl_json *obj = ncl_json_new_object();
ncl_json_obj_set_string(obj, "name", "plc");
ncl_json_obj_set_int(obj, "size", 3);
ncl_json_obj_set_bool(obj, "ok", true);
ncl_json *arr = ncl_json_new_array();
ncl_json_arr_push(arr, ncl_json_new_int(1));      /* 所有权转移 */
ncl_json_obj_set(obj, "items", arr);              /* 所有权转移 */

/* 访问（全部是借用指针） */
const char *name = ncl_json_obj_get_string(obj, "name");
long long size   = ncl_json_obj_get_int(obj, "size", 0);
ncl_json *items  = ncl_json_obj_get(obj, "items");
size_t n         = ncl_json_arr_len(items);
ncl_json *first  = ncl_json_arr_get(items, 0);
long long v;  ncl_json_as_int(first, &v);         /* 类型不符返回 false */

/* 解析与序列化 */
ncl_json *doc = ncl_json_parse(text, len, NULL);  /* NULL 处可传 ncl_strbuf 收错误 */
char *out = ncl_json_write_string(doc);           /* 调用方 free */
ncl_json_free(doc);
```

### 5.3 协议层

#### ncl_general.h —— 常量、枚举、校验

```c
ncl_operation_to_string(NCL_OP_GET_VALUE);        /* "get_value" */
ncl_operation_parse("set_value", &op);
ncl_code_to_string(NCL_CODE_NG);                  /* "NG" */
ncl_check_is_code_ok(code); ncl_check_is_code_ng(code);
NCL_PATH_SEPARATOR  NCL_OPERATION_SEPARATOR  NCL_DATA_TYPE_HASH
```

#### ncl_topic.h —— 主题构造

```c
char *t = ncl_topic_query_request(sn, NULL);      /* Query/Request/<sn>；需 free */
char *e = ncl_topic_event(sn, NULL);              /* Event/<sn> */
char *s = ncl_topic_sample(sn, "ch1");            /* Sample/<sn>/ch1 */
char *x = ncl_topic_extract_sn("Query/Request/V203243111F");  /* 反解 SN */
```

第二个参数是「客户端 id」：传 NULL 表示不带，某些部署会拼成 `<主题>/<clientId>`。

#### ncl_message.h —— 报文

见 4.2。常用补充：

```c
ncl_msg_type type = ncl_msg_type_from_topic(topic);
bool valid = ncl_message_is_valid(msg);           /* 合法性校验 */
bool match = ncl_message_matches(response, request);
ncl_json *data = ncl_message_get_data(msg);       /* 仅 QUERY_RESPONSE 用这个 */
```

#### ncl_model.h —— 数据模型

多数字段直接读结构体（`ncl_node` 是压平后的联合体，带 `type` 判别字段）：

```c
ncl_node *root = ncl_root_node_parse(json_text);   /* 解析 + 后构造（算路径） */
ncl_node *device = ncl_node_device_at(root, 0);
ncl_node *item = ncl_node_data_item_at(device, 0);
const char *path = ncl_node_path(item);            /* "/STATUS" */
ncl_node *found = ncl_node_find_by_id(root, "030001");
ncl_node *copy = ncl_node_clone(item);             /* 深拷贝 */
ncl_node_free(root);
```

#### ncl_codec.h —— 十六进制与压缩

```c
ncl_buffer out;
ncl_codec_encode_hex(src, len, &out);      /* out.data / out.len，用完 ncl_buffer_free */
ncl_codec_decode_hex(src, len, &out);
if (ncl_codec_compress_available()) {      /* 需要 NCLINK_WITH_ZLIB=ON */
    ncl_codec_encode_compress(src, len, &out);
}
```

### 5.4 传输层

#### ncl_socket.h —— TCP

```c
ncl_socket *s = ncl_socket_connect("127.0.0.1", 1883, 5000, err, sizeof(err));
ncl_socket_send(s, data, len);
int n = ncl_socket_recv(s, buf, sizeof(buf), 1000);  /* NCL_SOCKET_TIMEOUT(-2) 表示超时 */
ncl_socket_recv_exact(s, buf, len, 1000);

ncl_socket *l = ncl_socket_listen(9008, err, sizeof(err));   /* 端口 0 = 随机 */
unsigned port = ncl_socket_local_port(l);
ncl_socket *c = ncl_socket_accept(l, 500);

ncl_socket_shutdown(s);   /* 只关句柄、幂等：用来唤醒阻塞中的读 */
ncl_socket_close(s);      /* 释放对象 */

char host[64];
ncl_socket_local_ip(s, host, sizeof(host));
ncl_socket_peer_ip(s, host, sizeof(host));
char *map = ncl_net_ip_map_json();   /* {"eth0":"192.168.1.5"}，需 free */
```

#### ncl_mqtt.h —— MQTT 5.0

报文编解码层（`ncl_mqtt_packet_*`）一般不需要直接使用；常用的是客户端：

```c
ncl_mqtt_client_options opt;
ncl_mqtt_client_options_default(&opt);
opt.url = "tcp://broker:1883";       /* ssl:// 需要带 NCLINK_WITH_TLS=ON 编的库 */
opt.client_id = sn;
opt.username = user;  opt.password = pass;
opt.keep_alive_seconds = 60;
opt.connect_timeout_ms = 10000;
opt.clean_start = true;
opt.automatic_reconnect = true;
opt.on_message = on_message;         /* 收包线程回调 */
opt.on_disconnect = on_disconnect;   /* 断线回调，可带 reason_code */
opt.user = your_context;

ncl_mqtt_client *c = ncl_mqtt_client_create(&opt);
ncl_mqtt_client_connect(c);
ncl_mqtt_client_subscribe(c, "Query/Response/V1", 2);
/* 发布时自动附带 version=2.0 用户属性 */
ncl_mqtt_client_publish(c, topic, payload, len, 1, NULL, 5000);

const char *err = ncl_mqtt_client_last_error(c);
ncl_mqtt_client_is_connected(c);
ncl_mqtt_client_disconnect(c);
ncl_mqtt_client_destroy(c);
```

自动重连成功后，客户端会自动恢复之前订阅的主题。

### 5.5 ncl_client.h —— 客户端

#### 三层结构

```
ncl_message_channel   传输接口（函数指针表）。客户端管理器是 MQTT 实现；
                      测试或嵌入场景可自己实现，把请求直接交给 ncl_server
ncl_client            单台设备的协议视图：请求/响应关联、5 分钟响应缓存
ncl_client_holder     进程级单例：MQTT 连接 + 按 SN 的客户端表（30 分钟空闲过期）
```

#### 请求接口

```c
ncl_client *client = ncl_client_holder_get("V203243111F");

/* 便捷读值 */
ncl_json *value = NULL;
ncl_client_get_value(client, "/STATUS", 5000, &value);
ncl_client_get_value_range(client, "/PART_COUNT", 0, 9, 5000, &value);
long long length;
ncl_client_get_length(client, "/STATUS", 5000, &length);

/* 写值：返回 NCL_OK 表示设备应答 OK；NG 返回 NCL_ERR */
ncl_client_set_value(client, "/STATUS", ncl_json_new_int(7), 5000);
ncl_client_set_value_index(client, "/LIST", ncl_json_new_string("x"), 2, 5000);

/* 探测 / 心跳 */
ncl_message *probe = NULL;
ncl_client_probe(client, 5000, &probe);          /* 回传设备模型（见 4.7 接管） */
ncl_client_ping(client, 5000, &pong);

/* 方法调用：request 的所有权交给库，response 由你释放 */
ncl_message *request = ncl_message_new(NCL_MSG_METHOD_CALL_REQUEST);
ncl_message_set_method(request, "/plc/setValue");
ncl_message_set_params(request, params_json);    /* 所有权转移 */
ncl_message_set_check(request, true);            /* 只校验不执行 */
ncl_message *response = NULL;
ncl_client_method_call(client, request, 5000, &response);
```

`ncl.client` 的每个请求都会阻塞到响应或超时；超时返回 `NCL_ERR_TIMEOUT`。

#### 独立使用（不用客户端管理器）

```c
/* 自己实现通道，把发布/订阅接到任意传输上 */
static ncl_err my_publish(ncl_message_channel *self, const char *topic,
                          const ncl_message *msg, int qos,
                          const ncl_mqtt_properties *props) { /* ... */ }

ncl_message_channel channel = { my_publish, my_subscribe, my_unsubscribe };
ncl_client *client = ncl_client_create("V203243111F", &channel);
ncl_client_subscribe(client);
/* 收到响应时把它交给客户端（库会按 @id 关联并唤醒等待者） */
ncl_client_on_message(client, topic, message);
```

#### 事件

```c
static void on_event(ncl_client *c, const char *topic,
                     const ncl_message *msg, void *user) {
    const char *key = ncl_json_obj_get_string(msg->as.event.event, "key");
    /* msg 是借用的，回调返回后即被释放 */
}
ncl_client_set_event_handler(client, on_event, NULL);
ncl_client_subscribe_events(client, 2);
ncl_client_unsubscribe_events(client);
size_t n = ncl_client_event_count(client);
```

#### 运行期更换 broker

```c
/* 改完 conf/mqtt.cfg（例如经 REST /api/setMqttUrl）后，让连接按新配置重来 */
ncl_client_holder_restart();
```

> 配置写接口只落盘、不动运行中的连接；要让新配置生效就调用这个函数。

#### 采样

设备按 `uploadInterval` 把窗口内的采样值聚合成一条 `Sample` 报文，发在
`Sample/<sn>/<通道id>`。客户端订阅一次通配主题即可覆盖该设备的所有通道：

```c
/* 回调：msg 是借用的，返回后立即释放，不要保存指针 */
static void on_sample(ncl_client *client, const char *topic,
                      const ncl_message *msg, void *user) {
    size_t i, items = ncl_message_item_count(msg);   /* = 采样项个数 */
    /* 表头：本次采集了哪些数据项。线上是数组 "paths":[...] */
    ncl_json *header = ncl_strvec_to_json(&msg->as.sample.paths);
    char *header_text = header != NULL ? ncl_json_write_string(header) : NULL;
    ncl_log_info("通道 %s，采样 %lldms（=模型 sampleInterval），上报 %lldms，表头 %s",
                 msg->as.sample.id, msg->as.sample.interval,
                 msg->as.sample.upload_interval,
                 header_text != NULL ? header_text : "[]");
    free(header_text);
    ncl_json_free(header);

    for (i = 0; i < items; i++) {
        const ncl_sample_item *item = ncl_message_item_at(msg, i);
        const char *path = ncl_strvec_at(&msg->as.sample.paths, i);  /* 表头第 i 项 */
        /* item->data 是本轮（uploadInterval 内）按时间先后采集到的值数组 */
        ncl_log_info("  %s: %u 个值，第一个=%s", path,
                     (unsigned)ncl_json_arr_len(item->data),
                     ncl_json_number_raw(ncl_json_arr_get(item->data, 0)));
    }
}

ncl_client_set_sample_handler(client, on_sample, NULL);
ncl_client_subscribe_samples(client, 0);       /* 订 "Sample/<sn>/#" */
ncl_log_info("已订阅 %s", ncl_client_sample_topic(client));

/* ... 运行期间回调会不断被触发 ... */

ncl_client_unsubscribe_samples(client);
ncl_client_set_sample_handler(client, NULL, NULL);
size_t got = ncl_client_sample_count(client);   /* 收到过多少条上报 */
```

几点说明：

| 事项 | 说明 |
|------|------|
| 订阅范围 | `ncl_client_subscribe_samples()` 订的是 `Sample/<sn>/#`，一台设备的所有通道一次覆盖；只想收某个通道就自己订 `Sample/<sn>/<通道id>`（见 6.6） |
| QoS | 示例用 0；需要断线补发就用 1（设备侧上报固定按 QoS 0 发，见 `Server` 采样段） |
| 触发时机 | 上报由设备侧驱动，`uploadInterval` 一到就发；客户端只是被动接收 |
| 采样项对应 | `paths[i]` 与 `data[i]` 一一对应，顺序与设备模型里 `ids` 的顺序一致 |
| 表头 | `paths` 是数组（不是拼接字符串）；要一行字符串用 `ncl_message_sample_header(msg, ";")` |
| `interval` | 就是模型里的 `sampleInterval`（报文键名是 `interval`） |
| 时间戳 | 报文里带 `beginTime`（窗口起点，epoch 毫秒字符串），需要严格时间对齐时用它 |
| 完整性 | 设备端只发完整报文（**外层**：表头与各列槽位对齐）；消费端可用 `ncl_message_sample_is_complete()` 复核 |
| 设备省掉表头 | 客户端会先用设备模型补回规范形状（`ncl_message_sample_normalise()`，见 4.5）；补不了才原样交给回调 |
| 亚毫秒采样 | 值本身可以是数组（一个槽位一批数据），各列每槽点数可以不同；读它用 `ncl_sample_item_is_nested()` / `_value_count()` / `_value_at()` |
| 按行消费 | 采样率不同的列放在一张表里读：`ncl_message_sample_point_count()` 取行数（最多那列的点数），`ncl_message_sample_value_at(msg, row, col)` 取每行每列的值（粗列取覆盖该行的第一个点） |

### 5.6 ncl_server.h —— 服务端

#### 创建与订阅

```c
ncl_server_options opt;
memset(&opt, 0, sizeof(opt));
opt.sn = sn;
opt.mqtt = mqtt;                        /* 借用；也可换成下面的 publish 钩子 */
opt.model_json = model_text;            /* 可为 NULL，之后再 load_model */
opt.publish = my_publish;               /* 可选：自定义出站通道（无 broker 场景） */
opt.publish_user = my_ctx;

ncl_server *server = ncl_server_create(&opt);
ncl_server_load_model(server, text);    /* 或 ncl_server_set_model(server, root) */
ncl_server_save_model(server);          /* 写回 conf/model/nclink.json */
ncl_server_subscribe(server);           /* 订 6 个请求主题 */
```

#### 工具注册

```c
ncl_server_register_tool(server, "file", instance, methods, n_methods,
                         bindings, n_bindings);
ncl_server_register_builtin_tool(server);   /* /nclinkServer/addSample、removeSample */
ncl_server_register_file_tool(server);      /* /CONTROLLER/FILE 的 5 个方法 */
```

#### 离线调用（不起 MQTT 也能测）

```c
ncl_message *response = ncl_server_invoke_query(server, request);
ncl_message *response = ncl_server_invoke_set(server, request);
ncl_message *response = ncl_server_invoke_method_call(server, request);
ncl_message *response = ncl_server_check_method_call(server, request);  /* 只校验 */
ncl_message *response = ncl_server_dispatch(server, topic, request);    /* 按类型分发 */
```

#### 出站钩子

没有 MQTT 也要跑（嵌入式、单元测试、自建传输）时，注册一个 publish 回调即可，
服务端会把「主题 + 已序列化的报文体」交给你：

```c
static ncl_err my_publish(void *user, const char *topic,
                          const char *payload, size_t len) {
    return ncl_socket_send((ncl_socket *)user, payload, len) == NCL_OK
               ? NCL_OK : NCL_ERR_IO;
}
ncl_server_set_publish_sink(server, my_publish, my_socket);
```

#### 事件与统计

```c
ncl_json *event = ncl_json_new_object();
ncl_json_obj_set_string(event, "key", "PART_COUNT");
ncl_json_obj_set_int(event, "value", 12);
ncl_server_push_event(server, "030002", event);       /* 发到 Event/<sn> */
ncl_server_push_event_ex(server, "030002", event, 1700000000000LL, "evt-1");
ncl_json_free(event);

size_t uploads = ncl_server_sample_upload_count(server);
size_t events  = ncl_server_event_count(server);
size_t binds   = ncl_server_binding_count(server);
```

#### 其它

```c
ncl_json *schema = ncl_server_openapi_schema(server, "http://host:9008/api");
char *schema_json = ncl_server_openapi_schema_json(server, base_url);
ncl_node *model = ncl_server_model(server);
const char *sn = ncl_server_sn(server);
ncl_server_set_user_data(server, my_state, my_cleanup);   /* 挂载自有数据 */
```

### 5.7 ncl_http.h / ncl_rest.h —— HTTP 与 REST

```c
ncl_http_server *http = ncl_http_server_create(9008);

/* 自定路由：路径是**精确匹配**（含前导 '/'），method 传 "GET"/"POST"/...，
 * 传 NULL 或 "*" 表示任意方法；handler 跑在受理连接的那个线程上。 */
ncl_http_server_route(http, "GET", "/api/hello", hello_handler, user);
ncl_http_server_route(http, "*", "/api/health", health_handler, user);

/* 多个路由共用一个堆上下文时，把它登记成"服务器拥有"：只释放一次，
 * 时机是 ncl_http_server_free()（路由先走，上下文后走）。 */
ncl_http_server_own_context(http, heap_ctx, ncl_mem_free);
ncl_http_server_route(http, "GET", "/api/a", a_handler, heap_ctx);
ncl_http_server_route(http, "GET", "/api/b", b_handler, heap_ctx);

/* 内置挂载：/api/schema + /swagger-ui，以及 12 个配置接口 */
ncl_rest_attach(http, server);
ncl_rest_attach_config(http);

ncl_http_server_set_cors(http, true);
ncl_http_server_start(http);
/* ... */
ncl_http_server_stop(http);
ncl_http_server_free(http);
```

handler 里读请求、写应答：

```c
static void hello_handler(ncl_http_request *req, ncl_http_response *res,
                          void *user) {
    const char *name = ncl_http_query(req, "name");     /* 查询参数 */
    const char *body = ncl_http_body(req);              /* 正文（NUL 结尾） */
    size_t len       = ncl_http_body_len(req);
    ncl_json *json   = ncl_http_json_body(req);         /* 解析 JSON 正文，需释放 */
    const char *auth = ncl_http_header(req, "Authorization");

    /* Result 封装：{"status":true,"data":...} / {"status":false,"data":"原因"} */
    ncl_rest_reply(res, 200, ncl_result_success_string("ok"));
    /* 直接写 JSON 值（内部负责序列化） */
    ncl_json *doc = ncl_json_new_object();
    ncl_json_obj_set_string(doc, "hello", "world");
    ncl_http_reply_json(res, 200, doc);          /* 所有权转移 */
    /* 或者返回纯文本 */
    ncl_http_reply_text(res, 404, "not found");
}
```

`ncl_http_url_encode/decode()` 处理百分号编码；`ncl_http_form_field()` 解析
`application/x-www-form-urlencoded` 正文。

#### 通过 HTTP 调用工具方法

`ncl_rest_attach()` 除了挂 `/api/schema` 与 `/swagger-ui`，还会挂一条**兜底路由**：

```
POST /api/<工具名>/<方法名>
Content-Type: application/json
{"…":"请求参数"}                 ← 直接作为 methodCall 的 params
```

应答统一是 `Result` 封装，**工具返回 NG 时原因会原样回给调用方**：

```json
{"status":true,"data":{…}}                      // code=OK
{"status":false,"data":"NotFoundException"}     // code=NG，data 就是 reason
```

| 请求 | 结果 |
|------|------|
| `POST /api/nclinkServer/addSample` | 启动采样通道；失败时 `data` 是错误名（如 `NotFoundException`） |
| `POST /api/nclinkServer/removeSample` | 停掉通道 |
| `POST /api/<你的工具>/<你的方法>` | 调用注册表里的任意方法 |
| 非 POST，或路径不是 `/api/<工具>/<方法>`（段数不对） | `404` |
| 工具/方法不存在 | `{"status":false,"data":"未找到方法"}` |

要点：

| 事项 | 说明 |
|------|------|
| 参数校验 | 想让设备只校验不执行，正文里加 `"check": true`（该字段会随 params 传给方法调用） |
| 路由优先级 | 精确路由优先于 `/api` 兜底路由，所以 `/api/schema`、12 个配置接口不会被兜底吃掉；**先挂哪个都行** |
| 工具返回文件 | 服务端把 `{"@file":…}` 换成 `/temp/<名字>` 令牌并在 `data.fileKeys` 里列键名，HTTP 调用方再按文件通道取字节（同 MQTT 流程） |
| multipart | **不支持**；带 `Content-Type: multipart/form-data` 会被拒绝并提示改用 JSON，文件传输请走文件通道 |

```bash
curl -X POST http://127.0.0.1:9008/api/nclinkServer/addSample \
     -H 'Content-Type: application/json' \
     -d '{"request":{"id":"ch1","type":"SAMPLE_CHANNEL",
                    "sampleInterval":1000,"uploadInterval":2000,
                    "ids":[{"id":"/STATUS"},{"id":"030002"}]}}'
# → {"status":true,"data":true}
```

### 5.8 ncl_config.h —— 设备配置

每个函数都有等价的 REST 接口（见 5.7 的配置接口表）。

```c
/* 初始化：建 conf/bin/log 目录并写 SN（覆盖式，见 4.1） */
char *sn = NULL;
ncl_config_init(NULL, &sn);          /* 不传则生成 32 位 hex */
ncl_config_init("V2TEST00001", &sn); /* 指定 SN */

/* 读取 */
char     *sn2   = ncl_config_get_sn();      /* 需 free（内部走 ncl_sn_read） */
ncl_json *model = ncl_config_get_model();   /* 文件缺失返回 NULL */
ncl_json *drv   = ncl_config_get_driver();
ncl_json *list  = ncl_config_get_server_list();
ncl_json *mqtt  = ncl_config_get_mqtt();    /* {"url","username","password"} */
ncl_json *ip    = ncl_config_get_ip_conf();

/* 写入 */
ncl_config_set_model(json_text);
ncl_config_set_driver(json_text);
ncl_config_set_server_list(json_text);
ncl_config_set_mqtt(json);                  /* 借用 */
ncl_config_set_ip_conf(json_text);
```

| 文件 | 内容 |
|------|------|
| `bin/sn.txt` | 设备序列号 |
| `conf/model/nclink.json` | 数据模型 |
| `conf/driver/*.json` | 驱动配置 |
| `conf/mqtt.cfg` | `url=` / `username=` / `password=` 三行 |
| `conf/ipConf.json` | 网络配置 |
| `conf/server.json` | 服务器列表 |
| `bin/ftp.txt` | FTP 端口与账号（见 5.9） |

### 5.9 ncl_ftp.h —— FTP 服务端与客户端

FTP 两端都是本库自带实现（RFC 959/2389 子集），无第三方依赖。

#### 服务端

```c
ncl_ftp_server_options opt;
memset(&opt, 0, sizeof(opt));
opt.port = 2323;            /* 0 表示随机端口，用 ncl_ftp_server_port() 查 */
opt.root = "/srv/share";    /* 登录根：路径规范化后无法用 ".." 越出 */
opt.user = "admin";
opt.password = "123456";
opt.allow_write = true;     /* 允许 STOR/DELE/MKD/RMD */
opt.idle_timeout_ms = 300000;

ncl_ftp_server *ftp = ncl_ftp_server_create_ex(&opt);
ncl_ftp_server_start(ftp);
/* ... */
ncl_ftp_server_stop(ftp);
ncl_ftp_server_free(ftp);   /* 不删除 root 目录 */
```

支持：`USER PASS SYST FEAT OPTS PWD CWD CDUP TYPE MODE STRU PASV EPSV PORT EPRT
LIST NLST RETR STOR APPE DELE RMD MKD RNFR RNTO SIZE MDTM REST ABOR NOOP STAT
ALLO HELP QUIT`，主动与被动两种数据连接都支持。

一个端点可以挂多个登录：创建时的账号（`opt.user` / `opt.password` / `opt.root`）与
`anonymous` 以外，还能加：

```c
ncl_ftp_account account = { "chan-1", "s3cret", NULL, true };  /* 名字/口令/根=NULL 用服务端根/可写 */
ncl_ftp_server_add_account(ftp, &account);      /* 同名则原地更新 */
size_t n = ncl_ftp_server_account_count(ftp);
ncl_ftp_server_remove_account(ftp, "chan-1");   /* 同名会话随即被断开（凭据撤销） */
```

文件通道的临时账号就建在这上面：撤销一条通道的账号不会碰到同一个端点上别的对端。

#### 客户端

```c
ncl_ftp_client_options opt;
memset(&opt, 0, sizeof(opt));
opt.passive = false;        /* 默认主动模式；跨 NAT 时改 true 走被动 */
opt.connect_timeout_ms = 5000;
opt.io_timeout_ms = 30000;

ncl_ftp_client *c = ncl_ftp_client_create_ex("127.0.0.1", 2323, "admin",
                                             "123456", &opt);
if (ncl_ftp_client_detect(c)) {               /* 连接 + 登录 + TYPE I，带 NOOP 存活判定 */
    ncl_ftp_client_mkdir(c, "/data");
    ncl_ftp_client_chdir(c, "/data");
    ncl_ftp_client_store_file(c, "a.txt", "local/a.txt");   /* 上传 */
    ncl_ftp_client_retrieve_file(c, "a.txt", "local/b.txt");/* 下载 */

    ncl_ptrvec entries;
    ncl_ptrvec_init(&entries, ncl_ftp_entry_free);
    ncl_ftp_client_list(c, ".", &entries);    /* 每个元素 ncl_ftp_entry */
    ncl_ptrvec_free(&entries);

    ncl_ftp_client_delete(c, "a.txt");
    ncl_ftp_client_rmdir(c, ".");
}
ncl_ftp_client_disconnect(c);
ncl_ftp_client_free(c);
```

`ncl_ftp_client_noop()` 可做存活探测；`ncl_ftp_client_reply_code/text()` 拿到最近
一次应答，便于排错。

### 5.10 ncl_file.h —— 文件传输

#### 设备端（收文件）

```c
ncl_server_register_file_tool(server);   /* 注册 file 工具 + 5 条绑定 + 2 条握手方法 */
ncl_server_start_ftp(server);            /* 可选：设备自己也服务 FTP（读 bin/ftp.txt，默认 2121） */
```

注册后，`/CONTROLLER/FILE` 上就有 `write/read/ll/mkdir/delete` 五个方法，外加文件
通道的握手方法 `file/openFileChannel` / `file/closeFileChannel`。**握手之前设备没有
对端**：文件方法一律答 `NoFileChannelException`（`NCL_ERR_NO_CHANNEL`）。对端只有
两种来源：

| 来源 | 谁开 | 生命周期 |
|------|------|----------|
| `file/openFileChannel` | 上位机（客户端）握手 | 直到 `file/closeFileChannel`、被下一条通道顶替（`force`）或设备重启 |
| `ncl_server_set_file_peer(host, port, user, pass)` | 设备自己配置的静态对端 | 静态；被后来的握手顶替 |

握手的参数：`host` / `port` / `user` 必填，`password`、`channelId`（租约名）、`path`
（远端前缀，默认本机 SN）、`force`（顶替已有通道）可选。**租约名相同**的重复握手是
幂等的 no-op（应答 `reused=true`）——断线重连后重试不会打断正在传的传输；租约名不同
又没给 `force` 则被拒绝（`NG`，理由里带当前租约名），保护正在用这条通道的对端。
设备侧查状态：`ncl_server_file_channel_is_open()` / `ncl_server_file_channel_id()`。

字节怎么走：**流式 + 可续传**。上传按 256 KiB 从本地文件读着发（对端已有前缀就
`APPE` 续、没有就 `STOR`），下载按本地已有大小 `REST` 续、64 KiB 一块写盘；单次
调用内最多重试 3 次，每次从断点继续。所以内存不随文件大小增长（512 MiB 的文件也只要
256 KiB + 64 KiB 两个缓冲，20 MiB 静态池构建同样能传），网络断在半路也不会重传。
计数在 `ncl_ftp_client_bytes_*()` / `ncl_ftp_server_bytes_*()` /
`ncl_server_file_tool_bytes_*()`（工具级，跨重连不归零）。

协议路径约定：

| 侧 | 路径基准 |
|----|----------|
| 设备（ncl_server_file_tool） | `<root>/uploadFile/<相对路径>`，对端目录树为 `/<sn>/<相对路径>` |
| 客户端（ncl_file_client_tool） | `<cwd>/<sn>/<相对路径>` |

即：**同一个相对路径**（如 `/demo.txt`）在两边各自落到上面两个位置。

#### 客户端（传文件）

```c
#include "nclink/ncl_file.h"

/* 1. 开文件通道：把本进程的 FTP 端点交出去（设备是 FTP 客户端，往这儿拨）。地址
 *    默认取"到 broker 的本机地址"（回环时改取本机第一个非回环 IPv4）、端口默认取
 *    进程级端点（没起就按 2323 起）、账号是库临时生成的一对（只有设备知道）。
 *    参数传 NULL = 全默认。 */
ncl_client_open_file_channel(client, NULL);

/* 2. 上传：文件必须先放到 <cwd>/<sn>/<相对路径>，再用同样的相对路径调用 */
ncl_mkdir_p(sn);
ncl_file_write_all("V200583BC87/demo.txt", text, strlen(text));
ncl_client_write(client, "/demo.txt");        /* 设备侧落到 uploadFile/demo.txt */

/* 下载：返回本地绝对路径（堆字符串），失败返回 NULL */
char *local = ncl_client_read(client, "/demo.txt");
free(local);

/* 目录列举 */
ncl_ptrvec files;
ncl_ptrvec_init(&files, ncl_file_attribute_release);
ncl_client_ll(client, "/", &files);            /* 相对 uploadFile/ */
for (size_t i = 0; i < ncl_ptrvec_len(&files); i++) {
    const ncl_file_attribute *a = ncl_ptrvec_at(&files, i);
    printf("%s %lld %d\n", a->file_name, a->file_size, a->total_chunks);
}
ncl_ptrvec_free(&files);

ncl_file_client_tool_mkdir(ncl_client_file_tool(client), "/docs");
ncl_file_client_tool_delete(ncl_client_file_tool(client), "/demo.txt");

/* 3. 收回租约（幂等）：撤销临时账号，设备那条 FTP 会话随之断开；进程级 FTP 端点
 *    本身留到 ncl_client_holder_stop_ftp() / shutdown() 才收。 */
ncl_client_close_file_channel(client);
```

对端不在这台机器上、或者端口有映射时用 `ncl_file_channel_options`：

```c
ncl_file_channel_options options;
ncl_file_channel_options_default(&options);
options.host = "10.0.0.7";       /* 设备拨得到的地址；NULL = 路由表选到 broker 的本机地址
                                  *      （回环则改取本机第一个非回环 IPv4） */
options.port = 2323;             /* 0 = 进程级端点自己的端口 */
options.user = "chan";           /* 指向本进程端点时库会把账号加上、关闭时撤销 */
options.password = "secret";
options.force = true;            /* 顶替别人占着的通道（默认 false：被拒绝） */
ncl_client_open_file_channel(client, &options);
```

**C API 不会自己开通道**（`ncl_client_write/read/ll` 之前必须显式握手，否则设备答
`NoFileChannelException`）；托管语言绑定里的上传/下载等便利方法会在没通道时自动
握一次手。查状态：`ncl_client_file_channel_is_open()` / `ncl_client_file_channel_id()`。

#### 配置文件 `conf/ftp.txt`（可选）

不想把地址写在代码里（或者部署现场才定）就放在 `<root>/conf/ftp.txt`。键全是可选的，
缺文件 / 缺键 / 空值都退回默认，**文件写坏也不会让库交出去一个坏地址**（只记一条
WARNING）。每次 `ncl_client_open_file_channel()` 与 `ncl_client_holder_start_ftp*()`
都重新读它，改完不用重启；优先级是 **函数参数 > conf/ftp.txt > 推导默认**。

```json
{
  "host": "10.0.0.7",      // 交给设备的地址；缺省 = 到 broker 的本机地址（回环→LAN 地址）
  "port": 2323,            // 本进程 FTP 端点监听端口；缺省/0 = 2323
  "advertisePort": 4023,   // 交给设备的端口（有端口映射时用）；缺省/0 = "port"
  "root": "D:/share",      // 端点登录根；缺省 = 安装根
  "userName": "nclink",    // 端点账号，同时也是交给设备的登录；缺省 = 库按通道临时生成
  "password": "secret",    // 与 userName 成对；缺省 = 随机口令（关闭通道时撤销）
  "path": "V200583BC87",   // 设备侧远端前缀；缺省 = 设备自己的 SN（客户端镜像是
                           //   <root>/<sn>/，要改它得连着端点根目录布局一起改）
  "passive": true,         // 让设备用 PASV 传输（设备在 NAT/容器/防火墙后面时必需）
  "force": true            // 交出去时顶替已占用的通道；缺省 false（被拒绝）
}
```

读写它：`ncl_file_channel_config_read()` / `ncl_file_channel_config_write()` /
`ncl_file_channel_config_free()`（结构体 `ncl_file_channel_config`）。配了
`userName`/`password` 时这对账号就是"端点自己的账号"，关闭通道只断会话、不撤销账号；
不配则由库按通道临时生成一对，`ncl_client_close_file_channel()` 会撤销它。

跨网段 / 设备在 NAT 后面时加 `"passive": true`（或 C API 的
`ncl_file_channel_options.passive`）：设备改被动模式主动外连，否则主动模式要求
上位机的 FTP 服务端反向连回设备，通常不可达。吞吐实测与复现方式见
[TRANSFER_PERF.md](TRANSFER_PERF.md)。

#### 方法调用里的文件参数

协议报文里无法直接携带文件，方法参数与返回值用**标记对象**
`{"@file":"<本地路径>"}` 表示：

```c
/* 工具返回文件：返回标记对象，服务端会自动复制到 <root>/temp/<名字>，
 * 把结果替换成 "/temp/<名字>"，并把键名放进 "fileKeys" */
ncl_json *marker = ncl_json_new_object();
ncl_json_obj_set_string(marker, NCL_FILE_MARKER, "/path/to/local.bin");
*result = ncl_json_new_object();
ncl_json_obj_set(*result, "copy", marker);

/* 客户端发送带文件的调用：keys 与 paths 一一对应 */
const char *keys[]  = { "key" };
const char *paths[] = { "local.bin" };
ncl_client_method_call_file(client, request, keys, paths, 1, 5000, &response);
/* 返回后 data["copy"] 已被替换成本地路径 */
```

#### 其它工具

```c
bool  need = ncl_file_need_compression("a.txt");   /* 文本类为 true */
int   n    = ncl_file_total_chunks(size);          /* 256 KB 一片 */
char *hex  = NULL;
ncl_file_checksum("a.bin", &hex);                  /* SHA-256 十六进制，需 free */
ncl_file_attribute *attr = NULL;
ncl_file_attribute_of("a.bin", "/data", &attr);
ncl_json *json = ncl_file_attribute_to_json(attr); /* 字段顺序按规范固定 */
ncl_file_attribute_free(attr);
```

### 5.11 ncl_schema.h —— 参数校验

实现 JSON Schema **draft-07 子集**：`type`（含数组形式）、`enum`、`const`、
`required`、`properties`、`additionalProperties`、`items`（单 schema 与元组）、
`minItems`/`maxItems`/`uniqueItems`、`minLength`/`maxLength`/`pattern`、
`minimum`/`maximum`/`exclusiveMinimum`/`exclusiveMaximum`/`multipleOf`、
`allOf`/`anyOf`/`oneOf`/`not`/`if-then-else`、内部 `$ref`、常见 `format`。

```c
/* 一次性校验：文档与 schema 都是 JSON 文本 */
ncl_strvec errors;
ncl_strvec_init(&errors);
ncl_json_schema_validate(json_text, schema_text, &errors);
if (errors.len > 0) {
    char *joined = ncl_schema_join_errors(&errors);   /* "[\"#/x: ...\", ...]" */
    ncl_log_error("参数不合法: %s", joined);
    free(joined);
}
ncl_strvec_free(&errors);

/* 复用同一份 schema 多次校验（注册工具时内部就是这么做的） */
char *error = NULL;
ncl_schema *schema = ncl_schema_compile_text(schema_text, strlen(schema_text),
                                             &error);
ncl_schema_validate(schema, value_json, &errors);
ncl_schema_free(schema);

/* 单独使用正则引擎（ECMA 子集，回溯实现） */
ncl_regex *re = ncl_regex_compile("^[A-Za-z_][A-Za-z0-9_]*$", NULL);
bool ok = ncl_regex_search(re, "abc_1", 5);
ncl_regex_free(re);
```

**限制**（有意为之）：不支持 `patternProperties`、`dependencies`、
`propertyNames`、`unevaluatedProperties`、外部 `$ref`，正则不支持反向引用与
前后向断言，惰性量词按贪婪处理。错误文本的措辞不属于协议契约。

---

## 6. 典型任务速查

### 6.1 增加一个数据点

1. **模型**里加一个数据项（`conf/model/nclink.json`）：
   ```json
   {"id":"030003","type":"TEMPERATURE","number":1,"dataType":"FLOAT"}
   ```
   路径随之变为 `/TEMPERATURE@1`（父是设备时前缀清空）。
2. **工具**里实现读写函数，并在 `bindings[]` 里绑定：
   ```c
   {"/TEMPERATURE@1", NCL_OP_GET_VALUE, "readTemp", "plc"},
   {"/TEMPERATURE@1", NCL_OP_SET_VALUE, "writeTemp", "plc"},
   ```
3. 重新 `ncl_server_load_model()` + `ncl_server_register_tool()` + `subscribe()`。

### 6.2 增加一个工具方法（供上位机 methodCall）

```c
static ncl_err my_method(void *inst, const ncl_json *params,
                         ncl_json **result, char **reason) {
    long long speed = 0;
    if (!ncl_json_as_int(ncl_json_obj_get(params, "speed"), &speed)) {
        *reason = ncl_strdup("speed 必须存在且为整数");   /* 进应答 reason */
        return NCL_ERR_INVALID_VALUE;                    /* 应答 code=NG */
    }
    *result = ncl_json_new_string("accepted");
    return NCL_OK;
}

static const ncl_tool_method methods[] = {
    {"setSpeed", my_method,
     "{\"type\":\"object\",\"properties\":{"
     "\"speed\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100}},"
     "\"required\":[\"speed\"]}"},
};
ncl_server_register_tool(server, "motion", inst, methods, 1, NULL, 0);
```

上位机调用 `/motion/setSpeed`；带 `check: true` 时设备只校验参数。

### 6.3 让 HTTP 接口支持文件上传

```c
/* 1. 设备端注册文件工具，客户端上传的文件落在 <root>/uploadFile/<相对路径> */
ncl_server_register_file_tool(server);

/* 2. 自己的 HTTP 路由里转交给 file 工具，或用 /api/schema 里生成的接口 */
static void upload_handler(ncl_http_request *req, ncl_http_response *res,
                           void *user) {
    const char *name = ncl_http_form_field(req, "name");
    if (name == NULL) { ncl_rest_reply_error(res, 400, "缺少 name"); return; }
    ncl_rest_reply(res, 200, ncl_result_success_string("ok"));
}
```

### 6.4 设备主动上报（不用采样通道）

```c
ncl_json *event = ncl_json_new_object();
ncl_json_obj_set_string(event, "key", "ALARM");
ncl_json_obj_set_string(event, "value", "tool_wear");
ncl_server_push_event(server, "/ALARM@1", event);
ncl_json_free(event);
```

### 6.5 让日志更像现场

```c
ncl_log_set_console(true);
ncl_log_set_level(NCL_LOG_DEBUG);   /* 排查时打开 */
```

设备现场建议把 `<root>/log/out.txt` 纳入日志轮转/上报。

### 6.6 接收采样数据

**常规做法**（用库封装，见 5.5「采样」）：订阅通配主题 + 注册回调。

```c
ncl_client_set_sample_handler(client, on_sample, NULL);
ncl_client_subscribe_samples(client, 0);     /* Sample/<sn>/# */
```

**读表头**：报文里的 `paths` 是**数组**，列出这次采集了哪些数据项（与 `data` 按下标对应）：

```c
const ncl_strvec *paths = &msg->as.sample.paths;
for (size_t i = 0; i < ncl_strvec_len(paths); i++) {
    ncl_log_info("第 %u 列: %s", (unsigned)i, ncl_strvec_at(paths, i));
}
/* 需要一行字符串（日志/CSV 表头）时： */
char *line = ncl_message_sample_header(msg, ";");   /* "/STATUS;/AXIS@0/POSITION" */
free(line);
```

**只要某一个通道**：自己订更精确的主题，回调仍是同一个（按主题里的通道 id 分发）。

```c
char *topic = ncl_topic_sample(sn, "ch1");       /* Sample/<sn>/ch1 */
ncl_channel_subscribe(client 对应的 channel, topic, 0);
```

**不用库封装、直接吃 MQTT**：适合一个连接管多台设备、或要把采样转发给别的系统。
此时自己收包、用 `ncl_message_parse()` 解析，主题以 `Sample/` 开头的就是上报：

```c
static void on_mqtt(void *user, const ncl_mqtt_publish *publish) {
    ncl_message *msg;
    if (strncmp(publish->topic, "Sample/", 7) != 0) {
        return;                                  /* 其它主题交给客户端处理 */
    }
    msg = ncl_message_parse(publish->topic, (const char *)publish->payload,
                            publish->payload_len);
    if (msg != NULL && msg->type == NCL_MSG_SAMPLE) {
        forward_to_my_database(publish->topic, &msg->as.sample);
    }
    ncl_message_free(msg);
}

ncl_mqtt_client_subscribe(mqtt, "Sample/+/ch1", 0);   /* 所有设备的 ch1 通道 */
```

**补采/回溯**：采样上报是"推"的模式，历史数据不在协议里。要历史值就自己去
`getValue(path, start, end)` 读区间（见 5.5 请求接口）。

**排错**：

| 现象 | 检查 |
|------|------|
| 一条都收不到 | 设备端 `ncl_server_init_samples()` 是否执行；`uploadInterval` 是否比你的等待时间长（示例设备 2 s）；是否真的订上了（`ncl_client_sample_topic()`） |
| 只收到一部分通道 | 订阅主题是否被写死成单通道（应该用 `Sample/<sn>/#`） |
| 值是 null | 采样项的路径或节点 id 在设备模型里能否解析（设备侧取值失败会填 null） |
| 收不到但事件能收到 | 说明 MQTT 通、采样没起来：看设备日志有没有"采样任务已启动: ch1 -> Sample/..." |

### 6.7 用 HTTP 管理采样通道

设备端的 HTTP 服务（见 5.7）可以直接增删采样通道，返回体就是 `Result`：

```bash
# 起一个通道：模型里没有定义也行，ids 直接给路径（表头）
curl -X POST http://<设备IP>:9008/api/nclinkServer/addSample \
     -H 'Content-Type: application/json' \
     -d '{"request":{"id":"ch1","type":"SAMPLE_CHANNEL",
                    "sampleInterval":1000,"uploadInterval":2000,
                    "ids":[{"id":"/STATUS"},{"id":"/AXIS@0/POSITION"}]}}'

# 停掉
curl -X POST http://<设备IP>:9008/api/nclinkServer/removeSample \
     -H 'Content-Type: application/json' -d '{"id":"ch1"}'

# 只想校验参数、不想真的起通道
curl -X POST http://<设备IP>:9008/api/nclinkServer/addSample \
     -H 'Content-Type: application/json' \
     -d '{"check":true,"request":{…}}'
```

参数不合法时返回 `{"status":false,"data":"NotFoundException"}` 之类的错误名
（错误码表见附录 B），不会留下半启动的通道。

---

## 7. 常见问题与排错

| 现象 | 原因与处理 |
|------|-----------|
| MQTT 连不上，`ncl_mqtt_client_last_error()` 提示等待 CONNACK 超时 | broker 地址/端口/账号不对；用 `ssl://` 但库没带 `NCLINK_WITH_TLS=ON` 编（返回 `NCL_ERR_NOT_SUPPORTED`）；防火墙 |
| 设备端示例刚启动就退出，日志里有「MQTT 连接失败」 | 它的默认 broker 是本机 `tcp://127.0.0.1:1883`（见 3.4）：先把 broker 起来，或改 `conf/mqtt.cfg` 指向你的 broker |
| 客户端请求全部超时 | 设备端是否 `ncl_server_subscribe()`；SN 是否一致（主题里带 SN）；设备端工具是否已注册（未注册应答 `NG 未找到`） |
| 设备端收不到请求 | 收包回调里是否调用了 `ncl_server_on_message()`；不要自己在收包线程上同步发布应答（会自我死锁，库内部已转线程池） |
| `SET` 返回失败 | 工具方法返回 NCL_OK 但 `*result` 为 NULL 时，应答 code=NG |
| 客户端 `setValue` 返回错误 | 说明设备应答里有 `code=NG` 的项（写失败被拒绝），报文 `reason` 里有原因 |
| 采样报文某一列全是 `null` | 该采样项的路径在设备端没有绑定工具（见 3.4 的路径表）：`kBindings` 里的路径要和模型里数据项的路径一致 |
| 文件上传报 `Error` / `NoFileChannelException` | 检查四件事：① 是否先握了手（`ncl_client_open_file_channel`，托管绑定里便利方法会自己握）；② 本地文件是否放在 `<cwd>/<sn>/<相对路径>`；③ 设备拨回的地址是否可达（跨网段用 `options.host`，设备端静态对端用 `ncl_server_set_file_peer`）；④ 客户端本机 2323 端口是否被占用 |
| FTP 主动模式连不上 | 默认走主动模式：服务端要能反向连到客户端的监听端口。跨 NAT 时改用被动：`ncl_ftp_client_set_passive(c, true)` |
| `ncl_sn_read()` 每次启动都变 | 根目录是否可写、`bin/sn.txt` 是否被 `/api/cfg/init` 覆盖过（该接口无条件重写，见 4.1） |
| 中文字符串编译报 C4819/C2001 | MSVC 没加 `/utf-8` |
| 端口被占用导致启动失败 | HTTP 9008 / 设备 FTP 2121 / 客户端 FTP 2323；同一进程内不能起两个同端口服务 |
| 日志出现「MQTT 会话被顶替 (0x8E)」 | 有另一个连接用了同一个 clientId（设备端就是 SN）。此时客户端**不会**自动重连（否则两边会互相顶替、死循环），需要检查是否有重复的 SN，确认后调用 `ncl_mqtt_client_connect()` 显式抢回身份 |
| 设备重启后旧进程还在跑 | 新进程用同一 SN 连接会顶掉旧进程，旧进程收到 0x8E 后停止重连并记警告，不会与新进程反复抢占 |
| ASan 报 `use-after-free` | 检查所有权表（4.7）：最常见是保存了事件回调里的 `ncl_message*`，或用了 `ncl_ptrvec_at()` 拿到的借用指针却去 free |
| 程序退出时偶发崩溃 | 释放顺序：先 `ncl_server_free()`（停采样/FTP/线程池任务）→ 再 `ncl_mqtt_client_destroy()` → 最后 `ncl_env_shutdown()`；不要在两个线程同时销毁同一对象 |
| 想看线上报文 | `ncl_log_set_level(NCL_LOG_DEBUG)`；解析失败时 `ncl_message_parse()` 返回 NULL 并记 WARN |
| 想验证参数不合法时的应答 | 发 `check: true` 的 MethodCall，看 `reason` 里的消息列表 |

---

## 8. 许可

本库以 **MIT License** 授权，全文见包内 `LICENSE`：

```
Copyright (c) 2026 huienming
```

- 可以自由使用、修改、分发、商用与再授权，只需在副本或实质部分中保留上述
  版权声明与许可声明。
- 所有源文件都带 `SPDX-License-Identifier: MIT` 头；静态库与文档同样适用。
- 软件按"现状"提供，不附带任何明示或默示担保（详见 `LICENSE` 全文）。

新增源文件请照抄这两行头（脚本类文件放在 shebang 之后）。构建末尾的
`license` 测试套件会逐个检查，缺头直接判失败；也可以手工跑
`tools/check_license.ps1` 检查、`-Fix` 批量补齐。

---

## 附录 A · API 索引

按头文件分组，由 `tools/gen_api_index.py` 从 `include/nclink/*.h` 自动生成（重新生成：`python tools/gen_api_index.py`）。

### `nclink/ncl_client.h`

- `ncl_err (*publish)(ncl_message_channel *self, const char *topic, const ncl_message *message, int qos, const ncl_mqtt_properties *properties);`
- `ncl_err (*subscribe)(ncl_message_channel *self, const char *topic, int qos);`
- `ncl_err (*unsubscribe)(ncl_message_channel *self, const char *topic);`
- `static inline ncl_err ncl_channel_publish(ncl_message_channel *channel, const char *topic, const ncl_message *message, int qos, const ncl_mqtt_properties *properties)`
- `static inline ncl_err ncl_channel_subscribe(ncl_message_channel *channel, const char *topic, int qos)`
- `static inline ncl_err ncl_channel_unsubscribe(ncl_message_channel *channel, const char *topic)`
- `ncl_client *ncl_client_create(const char *sn, ncl_message_channel *channel);` — Create a client for @p sn bound to @p channel (not owned).
- `void ncl_client_free(ncl_client *client);`
- `const char *ncl_client_sn(const ncl_client *client);`
- `ncl_err ncl_client_subscribe(ncl_client *client);` — Subscribe to every response topic for this serial number (QoS 2).
- `ncl_err ncl_client_unsubscribe(ncl_client *client);`
- `void ncl_client_on_message(ncl_client *client, const char *topic, ncl_message *message);` — Deliver an inbound message.
- `ncl_node *ncl_client_root_node(const ncl_client *client);` — Device model accessors (the model is borrowed, not owned).
- `void ncl_client_set_root_node(ncl_client *client, ncl_node *root_node);`
- `char *ncl_client_get_id(ncl_client *client, const char *path);` — Path -> id and id -> path lookups through the device model.
- `char *ncl_client_get_path(ncl_client *client, const char *id);`
- `ncl_err ncl_client_subscribe_events(ncl_client *client, int qos);` — Subscribe to the device's event topic ("Event/<sn>").
- `ncl_err ncl_client_unsubscribe_events(ncl_client *client);`
- `void ncl_client_set_event_handler(ncl_client *client, ncl_client_event_fn fn, void *user);` — Install (or clear, with @p fn == NULL) the event callback.
- `size_t ncl_client_event_count(const ncl_client *client);` — Number of event messages delivered to the callback so far.
- `ncl_err ncl_client_subscribe_samples(ncl_client *client, int qos);` — Subscribe to every sample channel of this device ("Sample/<sn>/#").
- `ncl_err ncl_client_unsubscribe_samples(ncl_client *client);`
- `void ncl_client_set_sample_handler(ncl_client *client, ncl_client_sample_fn fn, void *user);` — Install (or clear, with @p fn == NULL) the sample callback.
- `size_t ncl_client_sample_count(const ncl_client *client);` — Number of sample messages delivered to the callback so far.
- `const char *ncl_client_sample_topic(const ncl_client *client);` — Topic filter used by ncl_client_subscribe_samples(), e.g.
- `ncl_err ncl_client_ping(ncl_client *client, unsigned timeout_ms, ncl_message **out);`
- `ncl_err ncl_client_query(ncl_client *client, ncl_message *request, unsigned timeout_ms, ncl_message **out);`
- `ncl_err ncl_client_set(ncl_client *client, ncl_message *request, unsigned timeout_ms, ncl_message **out);`
- `ncl_err ncl_client_probe(ncl_client *client, unsigned timeout_ms, ncl_message **out);` — Probe query: the response is matched by its response topic.
- `ncl_err ncl_client_probe_set(ncl_client *client, ncl_message *request, unsigned timeout_ms, ncl_message **out);`
- `ncl_err ncl_client_method_call(ncl_client *client, ncl_message *request, unsigned timeout_ms, ncl_message **out);`
- `ncl_err ncl_client_method_call_async(ncl_client *client, ncl_message *request, unsigned timeout_ms, ncl_message **out);` — Set "async" on @p request and issue it (takes ownership of the request).
- `ncl_err ncl_client_method_status(ncl_client *client, const char *object_id, const char *handler, unsigned timeout_ms, ncl_message **out);` — Method/Status query: @p object_id is the "id" field (the device id) and
- `ncl_err ncl_client_method_result(ncl_client *client, const char *object_id, const char *handler, unsigned timeout_ms, ncl_message **out);` — Method/Result query: while the call runs the response is code=PENDING with no
- `ncl_err ncl_client_get_value(ncl_client *client, const char *path, unsigned timeout_ms, ncl_json **out);` — Read a single value: *out receives a clone of values[0].
- `ncl_err ncl_client_get_value_range(ncl_client *client, const char *path, int start, int end, unsigned timeout_ms, ncl_json **out);` — Read the values in the index range [start, end].
- `ncl_err ncl_client_get_length(ncl_client *client, const char *path, unsigned timeout_ms, long long *out_length);` — getLength(path, timeout).
- `ncl_err ncl_client_set_value(ncl_client *client, const char *path, ncl_json *value, unsigned timeout_ms);` — setValue(path, value, timeout).
- `ncl_err ncl_client_set_value_index(ncl_client *client, const char *path, ncl_json *value, int index, unsigned timeout_ms);` — setValue(path, value, index, timeout).
- `ncl_err ncl_client_add_sample(ncl_client *client, const ncl_node *config, unsigned timeout_ms);` — addSample(config) / removeSample(id) method calls.
- `ncl_err ncl_client_remove_sample(ncl_client *client, const char *id, unsigned timeout_ms);`
- `bool ncl_client_is_ready(ncl_client *client);` — True when the last operation completed within @p timeout_ms.
- `void ncl_client_holder_options_default(ncl_client_holder_options *options);` — Convenience initialiser: uri + credentials only, every other field default.
- `ncl_err ncl_client_holder_init_ex(const ncl_client_holder_options *options);` — Initialise the process wide MQTT client with explicit options (TLS included).
- `ncl_err ncl_client_holder_init(const char *server_uri, const char *username, const char *password);` — Initialise the process wide MQTT client.
- `ncl_client *ncl_client_holder_get(const char *sn);` — Fetch (creating on demand) the client for @p sn.
- `bool ncl_client_holder_is_initialised(void);` — True once ncl_client_holder_init() succeeded.
- `ncl_mqtt_client *ncl_client_holder_mqtt(void);` — The underlying MQTT client, for diagnostics.
- `void ncl_client_holder_shutdown(void);` — Disconnect MQTT, drop every client and stop the FTP-less file hook.
- `size_t ncl_client_holder_client_count(void);` — Number of live per-device clients (idle expiry runs on every access).

### `nclink/ncl_codec.h`

- `void ncl_buffer_free(ncl_buffer *buf);`
- `ncl_err ncl_codec_encode_hex(const unsigned char *src, size_t src_len, ncl_buffer *out);` — Hex encode: two upper case characters per input byte.
- `ncl_err ncl_codec_decode_hex(const unsigned char *src, size_t src_len, ncl_buffer *out);` — Hex decode.
- `ncl_err ncl_codec_encode_compress(const unsigned char *src, size_t src_len, ncl_buffer *out);` — Deflate (zlib container) @p src into @p out.
- `ncl_err ncl_codec_decode_compress(const unsigned char *src, size_t src_len, ncl_buffer *out);` — Inflate (zlib container) @p src into @p out.
- `bool ncl_codec_compress_available(void);` — True when the compress codec was built in (-DNCLINK_WITH_ZLIB=ON).
- `ncl_err ncl_codec_encode(ncl_codec_kind kind, const unsigned char *src, size_t src_len, ncl_buffer *out);` — Encode with the codec named by @p kind.
- `ncl_err ncl_codec_decode(ncl_codec_kind kind, const unsigned char *src, size_t src_len, ncl_buffer *out);` — Decode with the codec named by @p kind.

### `nclink/ncl_common.h`

- `const char *ncl_err_name(ncl_err err);` — Stable, human readable name of an error code.
- `void *ncl_mem_alloc(size_t size);` — Allocate @p size bytes, or NULL when the allocator is exhausted.
- `void *ncl_mem_calloc(size_t count, size_t size);` — Allocate @p count * @p size zeroed bytes, or NULL (with overflow check).
- `void *ncl_mem_realloc(void *ptr, size_t size);` — Resize @p ptr (NULL behaves as ncl_mem_alloc(), 0 as ncl_mem_free()).
- `void ncl_mem_free(void *ptr);` — Release @p ptr; NULL is a no-op.
- `const char *ncl_mem_mode(void);` — Which allocator is compiled in: "heap" or "static-pool".
- `void ncl_mem_get_stats(ncl_mem_stats *out);` — Snapshot the allocator counters (safe to call from any thread).
- `void ncl_mem_reset_stats(void);` — Clear peak/allocations/failures counters, keeping in_use and pool_bytes.
- `size_t ncl_mem_check(void);` — Walk the whole pool and verify its invariants: every block lies inside the
- `char *ncl_strdup(const char *s);` — Heap copy of @p s (NULL safe).
- `char *ncl_strndup(const char *s, size_t len);` — Heap copy of the first @p len bytes of @p s (NUL terminated).
- `ncl_err ncl_asprintf(char **out, const char *fmt, ...);` — printf into a freshly allocated buffer.
- `ncl_err ncl_vasprintf(char **out, const char *fmt, va_list ap);`
- `bool ncl_streq_ignore_case(const char *a, const char *b);` — Case-insensitive ASCII equality.
- `bool ncl_str_starts_with(const char *s, const char *prefix);`
- `bool ncl_str_ends_with(const char *s, const char *suffix);`
- `bool ncl_str_is_empty(const char *s);` — True when @p s is NULL or empty.
- `bool ncl_str_is_blank(const char *s);` — True when @p s is NULL, empty, or contains only whitespace.
- `char *ncl_str_trim_dup(const char *s);` — Duplicate @p s with leading and trailing ASCII whitespace removed.
- `void ncl_free_safe(void *ptr);` — Free a heap pointer and set the variable to NULL.
- `ncl_err ncl_uuid4(char *out, size_t out_len);` — Format a random version 4 UUID into @p out in lower case (8-4-4-4-12).
- `void ncl_strbuf_init(ncl_strbuf *sb);`
- `void ncl_strbuf_free(ncl_strbuf *sb);`
- `void ncl_strbuf_reset(ncl_strbuf *sb);`
- `ncl_err ncl_strbuf_reserve(ncl_strbuf *sb, size_t additional);`
- `ncl_err ncl_strbuf_append(ncl_strbuf *sb, const char *data, size_t len);`
- `ncl_err ncl_strbuf_puts(ncl_strbuf *sb, const char *s);`
- `ncl_err ncl_strbuf_putc(ncl_strbuf *sb, char c);`
- `ncl_err ncl_strbuf_printf(ncl_strbuf *sb, const char *fmt, ...);`
- `char *ncl_strbuf_detach(ncl_strbuf *sb);` — Detach the buffer contents; caller frees.
- `const char *ncl_strbuf_cstr(ncl_strbuf *sb);` — NUL terminated view of the buffer (never NULL for an initialised buffer).
- `void ncl_ptrvec_init(ncl_ptrvec *v, ncl_free_fn free_fn);`
- `void ncl_ptrvec_free(ncl_ptrvec *v);`
- `void ncl_ptrvec_clear(ncl_ptrvec *v);`
- `ncl_err ncl_ptrvec_push(ncl_ptrvec *v, void *item);`
- `ncl_err ncl_ptrvec_push_owned(ncl_ptrvec *v, void *item);`
- `void *ncl_ptrvec_at(const ncl_ptrvec *v, size_t index);`
- `size_t ncl_ptrvec_len(const ncl_ptrvec *v);`
- `void *ncl_ptrvec_take(ncl_ptrvec *v, size_t index);` — Detach ownership of element @p index; slot is removed.
- `void ncl_strvec_init(ncl_strvec *v);`
- `void ncl_strvec_free(ncl_strvec *v);`
- `void ncl_strvec_clear(ncl_strvec *v);`
- `ncl_err ncl_strvec_push(ncl_strvec *v, const char *s);` — Copies @p s; returns NCL_OK / NCL_ERR_NOMEM.
- `const char *ncl_strvec_at(const ncl_strvec *v, size_t index);`
- `size_t ncl_strvec_len(const ncl_strvec *v);`
- `bool ncl_strvec_contains(const ncl_strvec *v, const char *s);`

### `nclink/ncl_config.h`

- `ncl_err ncl_config_init(const char *sn, char **out_sn);` — Create bin/, conf/ and log/ under the install root and write bin/sn.txt,
- `char *ncl_config_get_sn(void);` — The serial number, or NULL when bin/sn.txt is missing.
- `ncl_json *ncl_config_get_model(void);` — The parsed model document.
- `ncl_err ncl_config_set_model(const char *json);` — Write @p json to conf/model/nclink.json.
- `ncl_json *ncl_config_get_driver(void);` — The parsed driver configuration.
- `ncl_err ncl_config_set_driver(const char *json);` — Write @p json to conf/driver/driver.json.
- `ncl_json *ncl_config_get_server_list(void);` — The known servers.
- `ncl_err ncl_config_set_server_list(const char *json);` — Store the server list (JSON array or object).
- `ncl_json *ncl_config_get_mqtt(void);` — {"url":..,"username":..,"password":..} read from conf/mqtt.cfg, or NULL when
- `ncl_err ncl_config_set_mqtt(const ncl_json *config);` — Write the three fields back to conf/mqtt.cfg, one per line.
- `ncl_json *ncl_config_get_ip_conf(void);` — The parsed conf/ipConf.json (same rules as ncl_config_get_model()).
- `ncl_err ncl_config_set_ip_conf(const char *json);` — Write @p json to conf/ipConf.json.

### `nclink/ncl_env.h`

- `void ncl_env_set_root(const char *path);` — Override the installation root.
- `const char *ncl_env_root(void);` — Installation root; defaults to the current working directory.
- `const char *ncl_env_conf_path(void);`
- `const char *ncl_env_run_path(void);`
- `const char *ncl_env_driver_path(void);`
- `const char *ncl_env_log_path(void);`
- `const char *ncl_env_log_file(void);`
- `const char *ncl_env_mqtt_cfg_file(void);`
- `const char *ncl_env_model_file(void);`
- `const char *ncl_env_driver_cfg_file(void);`
- `const char *ncl_env_sn_file(void);`
- `void ncl_env_set_server_list(const char *const *servers, size_t count);` — Known NC-Link servers.
- `size_t ncl_env_server_count(void);`
- `const char *ncl_env_server_at(size_t index);`
- `void ncl_env_shutdown(void);` — Release memory held by the module (call once at shutdown).
- `ncl_err ncl_mqtt_config_read(ncl_mqtt_config *out);` — Read <conf>/mqtt.cfg.
- `void ncl_mqtt_config_free(ncl_mqtt_config *cfg);`
- `char *ncl_sn_read(void);` — Read <root>/bin/sn.txt, generating and persisting a serial number with
- `char *ncl_sn_generate(void);` — Generate a serial number: "V2" followed by nine upper-case hex digits.
- `bool ncl_path_exists(const char *path);` — True when a file or directory exists.
- `ncl_err ncl_mkdir_p(const char *path);` — Create @p path and any missing parents.
- `ncl_err ncl_file_read_all(const char *path, char **out, size_t *out_len);` — Read a whole file into memory (NUL terminated).
- `ncl_err ncl_file_write_all(const char *path, const void *data, size_t len);` — Write @p data to @p path, creating parents as needed.
- `ncl_err ncl_file_append(const char *path, const void *data, size_t len);` — Append @p data to @p path, creating it (and its parents) when missing.
- `ncl_err ncl_file_copy(const char *src, const char *dst);` — Copy @p src to @p dst, creating the parent directory of @p dst.
- `long long ncl_file_size(const char *path);` — Size of @p path in bytes, or -1 when it cannot be read.
- `bool ncl_path_same_file(const char *a, const char *b);` — True when @p a and @p b name the same file on disk (absolute paths compared,
- `ncl_err ncl_file_open_read(const char *path, long long offset, ncl_file_stream **out);`
- `size_t ncl_file_read_chunk(ncl_file_stream *stream, void *buf, size_t len);` — Next piece (<= @p len bytes); 0 at end of file.
- `void ncl_file_close_read(ncl_file_stream *stream);`
- `ncl_err ncl_file_open_write(const char *path, long long offset, bool append, ncl_file_stream **out);`
- `ncl_err ncl_file_write_chunk(ncl_file_stream *stream, const void *buf, size_t len);`
- `ncl_err ncl_file_close_write(ncl_file_stream *stream);`
- `int64_t ncl_file_mtime_ms(const char *path);` — Last modification time of @p path in epoch milliseconds, 0 when unknown.
- `bool ncl_path_is_dir(const char *path);` — True when @p path exists and names a directory.
- `ncl_err ncl_path_remove(const char *path);` — Remove @p path; directories are removed recursively.

### `nclink/ncl_file.h`

- `ncl_file_attribute *ncl_file_attribute_new(void);`
- `void ncl_file_attribute_free(ncl_file_attribute *attribute);`
- `void ncl_file_attribute_release(void *attribute);` — ncl_free_fn compatible destructor for ncl_ptrvec.
- `bool ncl_file_attribute_is_dir(const ncl_file_attribute *attribute);` — True when the attribute describes a directory.
- `ncl_json *ncl_file_attribute_to_json(const ncl_file_attribute *attribute);` — Property order: fileName, fileType, fileSize, totalChunks, compressed,
- `ncl_file_attribute *ncl_file_attribute_from_json(const ncl_json *json);`
- `ncl_json *ncl_file_attributes_to_json(const ncl_ptrvec *attributes);` — Serialise a list of attributes into a JSON array.
- `ncl_err ncl_sha256_hex(const void *data, size_t len, char **out_hex);` — SHA-256 of @p len bytes at @p data, lower case hex into a heap string.
- `ncl_sha256 *ncl_sha256_new(void);`
- `ncl_err ncl_sha256_update(ncl_sha256 *ctx, const void *data, size_t len);`
- `char *ncl_sha256_finish(ncl_sha256 *ctx);` — Hex digest (heap, ncl_free_safe()); NULL when the context is unusable.
- `void ncl_sha256_free(ncl_sha256 *ctx);`
- `bool ncl_file_need_compression(const char *file_name);` — True when the extension of @p file_name is compressed on transfer.
- `int ncl_file_total_chunks(long long size);` — ceil(size / NCL_FILE_CHUNK_SIZE), 0 for a size of 0.
- `ncl_err ncl_file_checksum(const char *path, char **out_hex);` — SHA-256 over the contents of @p path.
- `ncl_err ncl_file_attribute_of(const char *path, const char *parent, ncl_file_attribute **out);` — Attribute of the local file or directory @p path: a directory yields type 1,
- `ncl_err ncl_file_attribute_from_entry(const ncl_ftp_entry *entry, const char *parent, ncl_file_attribute **out);` — Attribute of the remote @p entry reported by the peer's FTP server.
- `ncl_err ncl_ftp_info_read(ncl_ftp_response *out);` — Populate from bin/ftp.txt, creating it with the built in defaults (admin /
- `ncl_err ncl_ftp_info_write(const ncl_ftp_response *info);` — Persist to bin/ftp.txt.
- `void ncl_ftp_info_free(ncl_ftp_response *info);`
- `ncl_server_file_tool *ncl_server_file_tool_create(const char *ip, unsigned port, const char *user, const char *password, const char *sn);`
- `void ncl_server_file_tool_free(ncl_server_file_tool *tool);`
- `void ncl_server_file_tool_set_passive(ncl_server_file_tool *tool, bool passive);` — Use passive mode (PASV) instead of the default active mode (PORT) for the FTP
- `bool ncl_server_file_tool_is_passive(const ncl_server_file_tool *tool);` — True when the tool transfers in passive mode.
- `long long ncl_server_file_tool_bytes_sent(const ncl_server_file_tool *tool);` — Bytes this tool has put on / taken off the wire so far (data connections
- `long long ncl_server_file_tool_bytes_received(const ncl_server_file_tool *tool);`
- `bool ncl_server_file_tool_detect(ncl_server_file_tool *tool);` — Validate the link with NOOP first, reconnecting when it is gone.
- `void ncl_server_file_tool_disconnect(ncl_server_file_tool *tool);` — Log out and drop the control connection.
- `bool ncl_server_file_tool_write(ncl_server_file_tool *tool, const char *local_path, const char *remote_dir);` — Upload @p local_path into "/<sn><remoteDir>/".
- `char *ncl_server_file_tool_read(ncl_server_file_tool *tool, const char *remote_file_path);` — Download "/<sn>/<remoteFilePath>" to <root>/uploadFile/<remoteFilePath> and
- `ncl_err ncl_server_file_tool_ll(ncl_server_file_tool *tool, const char *remote_dir, ncl_ptrvec *out);` — List @p remote_dir into @p out (initialize with
- `bool ncl_server_file_tool_mkdir(ncl_server_file_tool *tool, const char *remote_dir);` — Create @p remote_dir on the peer.
- `bool ncl_server_file_tool_delete(ncl_server_file_tool *tool, const char *remote_file_path);` — Delete @p remote_file_path, recursive for directories.
- `ncl_file_client_tool *ncl_file_client_tool_create(ncl_client *client);`
- `void ncl_file_client_tool_free(ncl_file_client_tool *tool);`
- `bool ncl_file_client_tool_detect(ncl_file_client_tool *tool);` — Create <cwd>/<sn>.
- `bool ncl_file_client_tool_write(ncl_file_client_tool *tool, const char *local_file_path);` — Upload @p local_file_path with "set" on /CONTROLLER/FILE.
- `char *ncl_file_client_tool_read(ncl_file_client_tool *tool, const char *remote_file_path);` — Download @p remote_file_path with "get_value"; returns the heap local path
- `ncl_err ncl_file_client_tool_ll(ncl_file_client_tool *tool, const char *remote_dir, ncl_ptrvec *out);` — List @p remote_dir with "get_attributes".
- `bool ncl_file_client_tool_mkdir(ncl_file_client_tool *tool, const char *remote_dir);` — Create @p remote_dir with "add".
- `bool ncl_file_client_tool_delete(ncl_file_client_tool *tool, const char *remote_file_path);` — Delete @p remote_file_path with "delete".
- `ncl_err ncl_client_method_call_file(ncl_client *client, ncl_message *request, const char *const *file_keys, const char *const *file_paths, size_t file_count, unsigned timeout_ms, ncl_message **out);` — Method call with file parameters: every entry of @p file_paths is copied to
- `ncl_err ncl_client_write(ncl_client *client, const char *local_file_path);` — Upload @p local_file_path through the file channel installed by
- `char *ncl_client_read(ncl_client *client, const char *remote_file_path);` — Download @p remote_file_path; returns a heap local path or NULL.
- `ncl_err ncl_client_ll(ncl_client *client, const char *remote_dir, ncl_ptrvec *out);` — List @p remote_dir on the peer.
- `void ncl_client_set_file_tool(ncl_client *client, ncl_file_client_tool *tool);` — Install the file channel of @p client, or remove it when @p tool is NULL.
- `ncl_file_client_tool *ncl_client_file_tool(ncl_client *client);`
- `ncl_err ncl_client_holder_start_ftp_ex(unsigned port, const char *root, const char *user, const char *password);` — Start the process wide FTP server that receives the files a device pushes.
- `ncl_err ncl_client_holder_start_ftp(void);`
- `void ncl_client_holder_stop_ftp(void);` — Stop the process wide FTP server.
- `ncl_ftp_server *ncl_client_holder_ftp_endpoint(void);` — The process wide FTP endpoint (borrowed, NULL when it is not running).
- `ncl_err ncl_client_holder_restart(void);` — Rebuild the process wide client from conf/mqtt.cfg: the running connection is
- `const char *ncl_client_holder_server_uri(void);` — Broker URL the process wide client was initialised with; NULL before
- `ncl_err ncl_file_channel_config_read(ncl_file_channel_config *out);` — Read `conf/ftp.txt` into @p out (memset first, then the file's values).
- `ncl_err ncl_file_channel_config_write(const ncl_file_channel_config *config);` — Write @p config to `conf/ftp.txt` (keys with a value only).
- `void ncl_file_channel_config_free(ncl_file_channel_config *config);` — Release the strings of @p config and zero it.
- `void ncl_file_channel_options_default(ncl_file_channel_options *options);` — Zero @p options and install the documented defaults.
- `ncl_err ncl_client_open_file_channel(ncl_client *client, const ncl_file_channel_options *options);` — Open (or refresh) the file channel of @p client: make sure the process wide
- `ncl_err ncl_client_close_file_channel(ncl_client *client);` — Drop the channel of @p client: file/closeFileChannel, then revoke the login
- `bool ncl_client_file_channel_is_open(ncl_client *client);` — True when @p client holds a file channel.
- `bool ncl_client_file_channel_id(ncl_client *client, char *out, size_t out_len);` — Copy the lease name of the channel into @p out; false when there is none.
- `ncl_err ncl_server_register_file_tool(ncl_server *server);` — Register the built in "file" tool on @p server.
- `ncl_err ncl_server_set_file_peer(ncl_server *server, const char *host, unsigned port, const char *user, const char *password);` — Point the device at a fixed FTP endpoint (the "static peer"), for hosts that
- `bool ncl_server_file_channel_is_open(ncl_server *server);` — True when a channel is open (handshake) or a static peer is configured.
- `bool ncl_server_file_channel_id(ncl_server *server, char *out, size_t out_len);` — Copy the open channel's id into @p out; false when no channel is open.
- `ncl_err ncl_server_start_ftp(ncl_server *server);` — Start the server side FTP endpoint: read bin/ftp.txt for the port and
- `void ncl_server_stop_ftp(ncl_server *server);` — Stop the server side FTP endpoint.

### `nclink/ncl_ftp.h`

- `void ncl_ftp_entry_free(void *entry);` — Release an entry allocated by ncl_ftp_client_list().
- `bool ncl_ftp_entry_is_dir(const ncl_ftp_entry *entry);` — True when @p entry is a directory.
- `ncl_ftp_client *ncl_ftp_client_create(const char *host, unsigned port, const char *user, const char *password);` — Create a client with the defaults (active mode, 5 s / 30 s timeouts).
- `ncl_ftp_client *ncl_ftp_client_create_ex(const char *host, unsigned port, const char *user, const char *password, const ncl_ftp_client_options *options);`
- `void ncl_ftp_client_free(ncl_ftp_client *client);`
- `const char *ncl_ftp_client_host(const ncl_ftp_client *client);` — Host the client was created for.
- `unsigned ncl_ftp_client_port(const ncl_ftp_client *client);`
- `bool ncl_ftp_client_detect(ncl_ftp_client *client);` — Validate the current connection with NOOP, or (re)connect, log in, switch to
- `void ncl_ftp_client_disconnect(ncl_ftp_client *client);` — Log out and drop the control connection.
- `bool ncl_ftp_client_is_connected(const ncl_ftp_client *client);` — True when a control connection is open (no NOOP is sent).
- `bool ncl_ftp_client_noop(ncl_ftp_client *client);` — Send NOOP over the control connection.
- `int ncl_ftp_client_reply_code(const ncl_ftp_client *client);` — Last reply code seen on the control connection (0 before the first reply).
- `const char *ncl_ftp_client_reply_text(const ncl_ftp_client *client);` — Last reply text, or "" when there is none.
- `void ncl_ftp_client_set_passive(ncl_ftp_client *client, bool passive);` — Switch between active (PORT/EPRT) and passive (PASV/EPSV) transfers.
- `bool ncl_ftp_client_is_passive(const ncl_ftp_client *client);`
- `ncl_err ncl_ftp_client_pwd(ncl_ftp_client *client, char *buf, size_t buf_len);`
- `ncl_err ncl_ftp_client_chdir(ncl_ftp_client *client, const char *path);`
- `ncl_err ncl_ftp_client_mkdir(ncl_ftp_client *client, const char *path);`
- `ncl_err ncl_ftp_client_rmdir(ncl_ftp_client *client, const char *path);`
- `ncl_err ncl_ftp_client_delete(ncl_ftp_client *client, const char *path);`
- `ncl_err ncl_ftp_client_rename(ncl_ftp_client *client, const char *from, const char *to);`
- `ncl_err ncl_ftp_client_size(ncl_ftp_client *client, const char *path, long long *out_size);` — SIZE.
- `ncl_err ncl_ftp_client_mdtm(ncl_ftp_client *client, const char *path, int64_t *out_time);` — MDTM.
- `bool ncl_ftp_client_is_dir(ncl_ftp_client *client, const char *path);` — True when @p path names a directory on the server (CWD probe).
- `ncl_err ncl_ftp_client_store(ncl_ftp_client *client, const char *remote, const void *data, size_t len);` — STOR: upload @p len bytes to @p remote.
- `ncl_err ncl_ftp_client_store_file(ncl_ftp_client *client, const char *remote, const char *local_path);` — Upload @p local_path with STOR as @p remote_name.
- `ncl_err ncl_ftp_client_upload(ncl_ftp_client *client, const char *remote, const char *local_path);` — Upload @p local_path as @p remote, **resuming**: whatever the peer already
- `ncl_err ncl_ftp_client_download(ncl_ftp_client *client, const char *remote, const char *local_path);` — Download @p remote into @p local_path, **resuming**: the local file's current
- `long long ncl_ftp_client_bytes_sent(const ncl_ftp_client *client);` — Bytes this client has put on / taken off its data connections so far.
- `long long ncl_ftp_client_bytes_received(const ncl_ftp_client *client);`
- `ncl_err ncl_ftp_client_retrieve(ncl_ftp_client *client, const char *remote, ncl_strbuf *out);` — RETR into @p out.
- `ncl_err ncl_ftp_client_retrieve_file(ncl_ftp_client *client, const char *remote, const char *local_path);` — Download @p remote_path with RETR into @p local_path.
- `ncl_err ncl_ftp_client_list(ncl_ftp_client *client, const char *path, ncl_ptrvec *out);` — LIST @p path into @p out, an ncl_ptrvec of ncl_ftp_entry*.
- `ncl_err ncl_ftp_client_nlst(ncl_ftp_client *client, const char *path, ncl_strvec *out);` — NLST @p path: the entry names only, appended to @p out.
- `ncl_ftp_server *ncl_ftp_server_create(void);` — Create a server with the defaults.
- `ncl_ftp_server *ncl_ftp_server_create_ex(const ncl_ftp_server_options *options);`
- `void ncl_ftp_server_free(ncl_ftp_server *server);` — Stop the server if it is running and release it.
- `ncl_err ncl_ftp_server_start(ncl_ftp_server *server);` — Bind and start accepting sessions.
- `ncl_err ncl_ftp_server_add_account(ncl_ftp_server *server, const ncl_ftp_account *account);` — Register @p account (or update it in place when the user name is already
- `ncl_err ncl_ftp_server_remove_account(ncl_ftp_server *server, const char *user);` — Drop @p user and close its live sessions.
- `size_t ncl_ftp_server_account_count(ncl_ftp_server *server);` — Number of accounts added with ncl_ftp_server_add_account().
- `long long ncl_ftp_server_bytes_sent(ncl_ftp_server *server);` — Data bytes the endpoint has sent / received since it started.
- `long long ncl_ftp_server_bytes_received(ncl_ftp_server *server);`
- `void ncl_ftp_server_stop(ncl_ftp_server *server);` — Stop accepting, close every session and join all threads.
- `bool ncl_ftp_server_is_running(ncl_ftp_server *server);`
- `unsigned ncl_ftp_server_port(const ncl_ftp_server *server);`
- `size_t ncl_ftp_server_session_count(ncl_ftp_server *server);`
- `long long ncl_ftp_server_command_count(ncl_ftp_server *server);`
- `const char *ncl_ftp_server_root(const ncl_ftp_server *server);` — Login root, as an absolute path.

### `nclink/ncl_general.h`

- `const char *ncl_code_to_string(ncl_code code);` — Keyword of @p code: "OK", "NG" or "PENDING".
- `bool ncl_code_parse(const char *text, ncl_code *out);` — Parse "OK" / "NG" / "PENDING" (case sensitive).
- `const char *ncl_operation_to_string(ncl_operation op);` — Keyword of @p op, e.g.
- `bool ncl_operation_parse(const char *text, ncl_operation *out);` — Parse an operation name; false when unknown.
- `bool ncl_check_is_code_valid(const char *code);` — True when @p code is "OK" or "NG" (case insensitive); NULL is invalid.
- `bool ncl_check_is_code_ok(const char *code);` — True when @p code is "OK".
- `bool ncl_check_is_code_ng(const char *code);` — True when @p code is "NG".
- `bool ncl_check_is_pending(const char *code);` — True when @p code is "PENDING".
- `bool ncl_check_is_data_type_valid(const char *data_type);` — True when @p data_type is NULL (no suffix), "LIST" or "HASH".

### `nclink/ncl_http.h`

- `ncl_http_server *ncl_http_server_create(unsigned port);` — Create a server bound to @p port once started (0 = ephemeral).
- `void ncl_http_server_free(ncl_http_server *server);`
- `ncl_err ncl_http_server_route(ncl_http_server *server, const char *method, const char *path, ncl_http_handler handler, void *user);` — Register a handler for an exact path.
- `ncl_err ncl_http_server_own_context(ncl_http_server *server, void *context, ncl_free_fn free_fn);` — Hand @p context to @p server: it is released through @p free_fn when the
- `ncl_err ncl_http_server_start(ncl_http_server *server);` — Bind and start serving.
- `void ncl_http_server_stop(ncl_http_server *server);` — Shut the listener down and join the accept thread.
- `unsigned ncl_http_server_port(const ncl_http_server *server);` — Port actually bound (useful when creating with port 0).
- `size_t ncl_http_server_request_count(const ncl_http_server *server);` — Number of requests handled so far.
- `void ncl_http_server_set_cors(ncl_http_server *server, bool enabled);` — Enable or disable the Access-Control-Allow-Origin: * header (default on).
- `const char *ncl_http_method(const ncl_http_request *request);`
- `const char *ncl_http_path(const ncl_http_request *request);` — Path without the query string.
- `const char *ncl_http_query_string(const ncl_http_request *request);` — Raw query string (without '?'), or "".
- `const char *ncl_http_query(const ncl_http_request *request, const char *name);` — Value of a query parameter, or NULL.
- `const char *ncl_http_header(const ncl_http_request *request, const char *name);` — Header lookup, case insensitive.
- `const char *ncl_http_body(const ncl_http_request *request);`
- `size_t ncl_http_body_len(const ncl_http_request *request);`
- `ncl_json *ncl_http_json_body(const ncl_http_request *request);` — Parse the body as JSON.
- `const char *ncl_http_form_field(const ncl_http_request *request, const char *name);` — Value of a form field in an application/x-www-form-urlencoded body.
- `void ncl_http_set_status(ncl_http_response *response, int status);`
- `void ncl_http_set_header(ncl_http_response *response, const char *name, const char *value);`
- `void ncl_http_reply(ncl_http_response *response, int status, const char *content_type, const char *body, size_t body_len);` — Set the body.
- `void ncl_http_reply_text(ncl_http_response *response, int status, const char *text);`
- `void ncl_http_reply_json(ncl_http_response *response, int status, const ncl_json *json);`
- `const char *ncl_http_status_text(int status);` — Reason phrase for a status code.
- `char *ncl_http_url_decode(const char *value);` — URL-decode @p value into a heap string ('+' becomes a space).
- `char *ncl_http_url_encode(const char *value);` — Percent-encode @p value for use in a query string.

### `nclink/ncl_json.h`

- `ncl_json *ncl_json_new_null(void);`
- `ncl_json *ncl_json_new_bool(bool value);`
- `ncl_json *ncl_json_new_int(long long value);`
- `ncl_json *ncl_json_new_double(double value);`
- `ncl_json *ncl_json_new_string(const char *value);`
- `ncl_json *ncl_json_new_string_len(const char *value, size_t len);`
- `ncl_json *ncl_json_new_array(void);`
- `ncl_json *ncl_json_new_object(void);`
- `ncl_json *ncl_json_clone(const ncl_json *j);`
- `void ncl_json_free(ncl_json *j);`
- `ncl_json_type ncl_json_type_of(const ncl_json *j);`
- `bool ncl_json_is_null(const ncl_json *j);`
- `bool ncl_json_is_number(const ncl_json *j);`
- `ncl_err ncl_json_obj_set(ncl_json *obj, const char *key, ncl_json *value);` — Insert or replace @p key.
- `ncl_err ncl_json_obj_set_string(ncl_json *obj, const char *key, const char *value);`
- `ncl_err ncl_json_obj_set_int(ncl_json *obj, const char *key, long long value);`
- `ncl_err ncl_json_obj_set_double(ncl_json *obj, const char *key, double value);`
- `ncl_err ncl_json_obj_set_bool(ncl_json *obj, const char *key, bool value);`
- `ncl_err ncl_json_obj_set_null(ncl_json *obj, const char *key);`
- `ncl_json *ncl_json_obj_get(const ncl_json *obj, const char *key);`
- `bool ncl_json_obj_has(const ncl_json *obj, const char *key);`
- `size_t ncl_json_obj_len(const ncl_json *obj);`
- `const char *ncl_json_obj_key_at(const ncl_json *obj, size_t index);`
- `ncl_json *ncl_json_obj_val_at(const ncl_json *obj, size_t index);`
- `ncl_err ncl_json_obj_remove(ncl_json *obj, const char *key);`
- `const char *ncl_json_obj_get_string(const ncl_json *obj, const char *key);` — Convenience getters.
- `long long ncl_json_obj_get_int(const ncl_json *obj, const char *key, long long def);`
- `double ncl_json_obj_get_double(const ncl_json *obj, const char *key, double def);`
- `bool ncl_json_obj_get_bool(const ncl_json *obj, const char *key, bool def);`
- `ncl_err ncl_json_arr_push(ncl_json *arr, ncl_json *value);` — Append @p value; ownership transfers to @p arr in every case.
- `ncl_json *ncl_json_arr_get(const ncl_json *arr, size_t index);`
- `size_t ncl_json_arr_len(const ncl_json *arr);`
- `ncl_json *ncl_json_arr_take(ncl_json *arr, size_t index);`
- `const char *ncl_json_as_string(const ncl_json *j);` — String payload, or NULL when @p j is not a JSON string.
- `const char *ncl_json_number_raw(const ncl_json *j);` — Raw number literal as written in the source document.
- `bool ncl_json_as_int(const ncl_json *j, long long *out);` — True when the value is a number, or a string holding a decimal integer.
- `bool ncl_json_as_double(const ncl_json *j, double *out);` — True when the value is a number, or a string holding a finite number.
- `bool ncl_json_as_bool(const ncl_json *j, bool *out);`
- `char *ncl_json_as_text(const ncl_json *j);` — Textual representation of a scalar value, used by the request parsers
- `ncl_json *ncl_json_parse(const char *text, size_t len, ncl_strbuf *err);` — Parse a JSON document.
- `ncl_json *ncl_json_parse_cstr(const char *text, ncl_strbuf *err);`
- `ncl_err ncl_json_write(const ncl_json *j, ncl_strbuf *out);` — Serialise compactly into @p out.
- `char *ncl_json_write_string(const ncl_json *j);` — Serialise into a freshly allocated string.
- `bool ncl_json_equals(const ncl_json *a, const ncl_json *b);` — Deep structural equality (object key order is not significant).
- `ncl_json *ncl_strvec_to_json(const ncl_strvec *v);` — Serialise a string vector (ncl_common.h) as a JSON array of strings.

### `nclink/ncl_logger.h`

- `bool ncl_log_init(const char *log_dir);` — Start writing to <log_dir>/out.txt.
- `void ncl_log_shutdown(void);` — Stop writing to the file and flush.
- `void ncl_log_set_console(bool enabled);` — Enable or disable the stderr mirror (default: enabled).
- `void ncl_log_set_level(ncl_log_level level);` — Minimum level written to the file and console (default: NCL_LOG_INFO).
- `void ncl_log_write(ncl_log_level level, const char *fmt, ...);`
- `void ncl_log_info(const char *fmt, ...);` — Informational line.
- `void ncl_log_error(const char *fmt, ...);` — Error line.
- `void ncl_log_warn(const char *fmt, ...);` — Warning and debug lines; useful for MQTT tracing.
- `void ncl_log_debug(const char *fmt, ...);`

### `nclink/ncl_message.h`

- `const char *ncl_msg_type_name(ncl_msg_type type);`
- `ncl_msg_type ncl_msg_type_from_topic(const char *topic);` — The message kind implied by a topic prefix (NCL_MSG_UNKNOWN when none).
- `ncl_query_request_item *ncl_query_request_item_new(const char *id);`
- `void ncl_query_request_item_free(ncl_query_request_item *item);`
- `bool ncl_query_request_item_is_valid(const ncl_query_request_item *item);`
- `const char *ncl_query_request_item_operation(const ncl_query_request_item *item);`
- `ncl_err ncl_query_request_item_indexes(const ncl_query_request_item *item, long long **out, size_t *count);` — Expands an "indexes" string ("3" or "1-4") into a flat id list.
- `ncl_query_response_item *ncl_query_response_item_new(const char *id);`
- `void ncl_query_response_item_free(ncl_query_response_item *item);`
- `bool ncl_query_response_item_is_valid(const ncl_query_response_item *item);`
- `const char *ncl_query_response_item_operation(const ncl_query_response_item *item);`
- `bool ncl_query_response_item_has_data(const ncl_query_response_item *item);`
- `ncl_json *ncl_query_response_item_data(const ncl_query_response_item *item);`
- `ncl_err ncl_query_response_item_add_value(ncl_query_response_item *item, ncl_json *value);`
- `bool ncl_query_response_item_matches(const ncl_query_response_item *item, const ncl_query_request_item *request);` — True when the response item answers this request item.
- `ncl_set_request_item *ncl_set_request_item_new(const char *id);`
- `void ncl_set_request_item_free(ncl_set_request_item *item);`
- `bool ncl_set_request_item_is_valid(const ncl_set_request_item *item);`
- `const char *ncl_set_request_item_operation(const ncl_set_request_item *item);`
- `ncl_set_response_item *ncl_set_response_item_new(const char *id);`
- `void ncl_set_response_item_free(ncl_set_response_item *item);`
- `bool ncl_set_response_item_is_valid(const ncl_set_response_item *item);`
- `bool ncl_set_response_item_matches(const ncl_set_response_item *item, const ncl_set_request_item *request);`
- `bool ncl_set_response_item_has_error(const ncl_set_response_item *item);` — True when the item reports a failure, i.e.
- `ncl_sample_item *ncl_sample_item_new(void);`
- `ncl_sample_item *ncl_sample_item_clone(const ncl_sample_item *item);`
- `void ncl_sample_item_free(ncl_sample_item *item);`
- `bool ncl_sample_item_is_valid(const ncl_sample_item *item);`
- `ncl_err ncl_sample_item_add_value(ncl_sample_item *item, ncl_json *value);`
- `bool ncl_sample_item_is_same(const ncl_sample_item *a, const ncl_sample_item *b);`
- `const char *ncl_params_operation(const ncl_json *params, const char *fallback);` — Helpers for the derived members shared by the request and response items,
- `bool ncl_params_has(const ncl_json *params, const char *key);`
- `const char *ncl_params_string(const ncl_json *params, const char *key);`
- `bool ncl_params_int(const ncl_json *params, const char *key, long long *out);`
- `ncl_json *ncl_params_get(const ncl_json *params, const char *key);`
- `ncl_err ncl_params_set_string(ncl_json **params, const char *key, const char *value);` — Sets params[key] = string, creating the params object when needed.
- `ncl_err ncl_params_set_int(ncl_json **params, const char *key, long long value);`
- `ncl_err ncl_params_set(ncl_json **params, const char *key, ncl_json *value);` — Sets params[key] = value (ownership transfers).
- `ncl_err ncl_params_append_string(ncl_json **params, const char *key, const char *value);` — Appends a string to the array stored at params[key].
- `ncl_err ncl_params_indexes(const ncl_json *params, long long **out, size_t *count);` — Expands ["3","1-4"] into [3,1,4].
- `ncl_message *ncl_message_new(ncl_msg_type type);`
- `void ncl_message_free(ncl_message *msg);`
- `ncl_err ncl_message_finalise(ncl_message *msg);` — Returns NCL_ERR_INVALID_MESSAGE when the object is not valid, otherwise
- `ncl_err ncl_message_set_message_id(ncl_message *msg, const char *id);` — Sets "@id".
- `ncl_err ncl_message_set_code(ncl_message *msg, const char *code);`
- `ncl_err ncl_message_set_reason(ncl_message *msg, const char *reason);`
- `ncl_err ncl_message_set_open_api_schema(ncl_message *msg, const char *schema);`
- `ncl_err ncl_message_set_version(ncl_message *msg, const char *version);`
- `ncl_err ncl_message_set_device_id(ncl_message *msg, const char *device_id);`
- `ncl_err ncl_message_set_model(ncl_message *msg, ncl_node *model);`
- `ncl_node *ncl_message_take_model(ncl_message *msg);` — Detach the device model carried by a probe message.
- `ncl_err ncl_message_set_method(ncl_message *msg, const char *method);`
- `ncl_err ncl_message_set_params(ncl_message *msg, ncl_json *params);`
- `ncl_err ncl_message_set_token(ncl_message *msg, const char *token);`
- `ncl_err ncl_message_set_check(ncl_message *msg, bool check);`
- `ncl_err ncl_message_set_data(ncl_message *msg, ncl_json *data);`
- `ncl_err ncl_message_set_event(ncl_message *msg, ncl_json *event);`
- `ncl_err ncl_message_set_sample_id(ncl_message *msg, const char *id);`
- `ncl_err ncl_message_set_handler(ncl_message *msg, const char *handler);`
- `ncl_err ncl_message_set_request_id(ncl_message *msg, const char *id);`
- `ncl_err ncl_message_set_async(ncl_message *msg, bool async);`
- `ncl_err ncl_message_set_status(ncl_message *msg, const char *status);`
- `ncl_err ncl_message_set_process(ncl_message *msg, long long process);`
- `ncl_err ncl_message_set_result(ncl_message *msg, const char *result);`
- `ncl_err ncl_message_set_return(ncl_message *msg, ncl_json *value);` — Method/Result/Response "return"; takes ownership of @p value.
- `const char *ncl_message_handler(const ncl_message *msg);`
- `const char *ncl_message_request_id(const ncl_message *msg);`
- `bool ncl_message_async(const ncl_message *msg);`
- `bool ncl_message_has_async(const ncl_message *msg);`
- `const char *ncl_message_status(const ncl_message *msg);`
- `bool ncl_message_process(const ncl_message *msg, long long *out);` — True when "process" was present; copies it into @p out.
- `const char *ncl_message_result(const ncl_message *msg);`
- `ncl_json *ncl_message_get_return(const ncl_message *msg);` — Borrowed "return" of a Method/Result/Response.
- `ncl_err ncl_message_set_event_time_ms(ncl_message *msg, int64_t millis);` — Event.time in milliseconds since the epoch.
- `ncl_err ncl_message_set_begin_time(ncl_message *msg, const char *begin_time);`
- `ncl_err ncl_message_set_sample_interval(ncl_message *msg, long long interval);`
- `ncl_err ncl_message_set_upload_interval(ncl_message *msg, long long interval);`
- `ncl_err ncl_message_add_sample_path(ncl_message *msg, const char *path);`
- `ncl_err ncl_message_add_sample_item(ncl_message *msg, ncl_sample_item *item);`
- `ncl_err ncl_message_add_query_request_item(ncl_message *msg, ncl_query_request_item *item);`
- `ncl_err ncl_message_add_query_response_item(ncl_message *msg, ncl_query_response_item *item);`
- `ncl_err ncl_message_add_set_request_item(ncl_message *msg, ncl_set_request_item *item);`
- `ncl_err ncl_message_add_set_response_item(ncl_message *msg, ncl_set_response_item *item);`
- `size_t ncl_message_item_count(const ncl_message *msg);`
- `void *ncl_message_item_at(const ncl_message *msg, size_t index);` — Item accessors; the concrete type depends on the message kind.
- `char *ncl_message_sample_header(const ncl_message *msg, const char *separator);` — "表头" of a Sample message - the list of data items this report collected.
- `bool ncl_message_sample_is_complete(const ncl_message *msg);` — True when a Sample message is ready to hand to a consumer: it carries a
- `ncl_err ncl_message_sample_normalise(ncl_message *msg, ncl_node *root);` — 用设备模型把设备端"省掉/占位"的采样信息补回规范形状，好让消费端继续按行
- `bool ncl_sample_item_is_nested(const ncl_sample_item *item);` — 该列是否含数组元素（即是否是亚毫秒批量采样）。
- `size_t ncl_sample_item_value_count(const ncl_sample_item *item);` — 该列的总点数：标量（含 null）算 1，数组算其长度。
- `const ncl_json *ncl_sample_item_value_at(const ncl_sample_item *item, size_t index);` — 按"扁平下标"取第 @p index 个点：标量列等同于下标取值，数组列按槽位顺序展开。
- `size_t ncl_message_sample_point_count(const ncl_message *msg);` — 行数（= 整个报文的点数）：**数据最多的那一列**的点数。
- `const ncl_json *ncl_message_sample_value_at(const ncl_message *msg, size_t row, size_t column);` — 按行读某一列的值：@p row 取 [0, ncl_message_sample_point_count())，@p column 取
- `bool ncl_message_is_valid(const ncl_message *msg);` — True when the message satisfies the rules of its kind.
- `bool ncl_message_matches(const ncl_message *response, const ncl_message *request);` — True when @p response answers every item of @p request.
- `bool ncl_message_has_data(const ncl_message *msg);` — Whether a query response carries a "data" member, and that member.
- `ncl_json *ncl_message_get_data(const ncl_message *msg);`
- `ncl_json *ncl_message_to_json(const ncl_message *msg);` — Serialise using the property order of the specification (null entries are
- `char *ncl_message_write_string(const ncl_message *msg);`
- `ncl_message *ncl_message_from_json(ncl_msg_type type, const ncl_json *json);` — Build a message of @p type from its JSON form.
- `ncl_message *ncl_message_parse(const char *topic, const char *payload, size_t payload_len);` — Convenience: infer the type from @p topic then parse @p payload.

### `nclink/ncl_model.h`

- `const char *ncl_node_type_name(ncl_node_type type);`
- `const char *ncl_upload_type_name(ncl_upload_type type);`
- `bool ncl_upload_type_parse(const char *text, ncl_upload_type *out);`
- `ncl_sample_params *ncl_sample_params_new(void);`
- `ncl_sample_params *ncl_sample_params_clone(const ncl_sample_params *p);`
- `void ncl_sample_params_free(ncl_sample_params *p);`
- `bool ncl_sample_params_is_valid(const ncl_sample_params *p);`
- `ncl_json *ncl_sample_params_to_json(const ncl_sample_params *p);`
- `ncl_sample_params *ncl_sample_params_from_json(const ncl_json *j);`
- `ncl_sample_ref *ncl_sample_ref_new(const char *id);`
- `void ncl_sample_ref_free(ncl_sample_ref *ref);`
- `bool ncl_sample_ref_is_valid(const ncl_sample_ref *ref);`
- `char *ncl_sample_ref_path(ncl_sample_ref *ref);` — Path of the referenced data item, including the LIST/HASH suffixes.
- `ncl_node *ncl_node_new(ncl_node_type type);` — Create an empty node of the given kind (fields NULL / vectors empty).
- `void ncl_node_free(ncl_node *node);`
- `ncl_node *ncl_node_clone(const ncl_node *node, bool shallow_children);` — Deep copy.
- `ncl_err ncl_node_set_name(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_id(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_type_name(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_description(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_number(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_data_type(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_mapping(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_value_type(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_source(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_version(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_guid(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_unique_id(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_settable(ncl_node *node, bool value);`
- `ncl_err ncl_node_set_value(ncl_node *node, ncl_json *value);` — Takes ownership of @p value.
- `ncl_err ncl_node_add_config(ncl_node *parent, ncl_node *config);`
- `ncl_err ncl_node_add_data_item(ncl_node *parent, ncl_node *item);`
- `ncl_err ncl_node_add_component(ncl_node *parent, ncl_node *component);`
- `ncl_err ncl_node_add_device(ncl_node *root, ncl_node *device);`
- `ncl_err ncl_node_add_sample_item(ncl_node *config, ncl_sample_ref *ref);`
- `ncl_err ncl_node_add_child(ncl_node *parent, ncl_node *child);` — Generic child insertion: the parent kind decides which slot @p child goes
- `size_t ncl_node_child_count(const ncl_node *node);`
- `ncl_node *ncl_node_config_at(const ncl_node *node, size_t index);`
- `ncl_node *ncl_node_data_item_at(const ncl_node *node, size_t index);`
- `ncl_node *ncl_node_component_at(const ncl_node *node, size_t index);`
- `ncl_node *ncl_node_device_at(const ncl_node *node, size_t index);`
- `size_t ncl_node_sample_count(const ncl_node *node);`
- `ncl_sample_ref *ncl_node_sample_at(const ncl_node *node, size_t index);`
- `ncl_node *ncl_node_find_by_id(const ncl_node *node, const char *id);` — Depth first lookup by id below @p node.
- `bool ncl_node_is_sample_node(const ncl_node *node);` — True when the node type string equals NCL_NODE_TYPE_SAMPLE_CHANNEL.
- `ncl_err ncl_node_set_path(ncl_node *node, const char *parent_path);` — Recompute the path of @p node and of its subtree, applying the "parent of a
- `const char *ncl_node_path(const ncl_node *node);` — Effective path: the root derives it from its type, others return the stored
- `void ncl_node_build_relations(ncl_node *node);` — Wire the parent pointers through the subtree rooted at @p node.
- `bool ncl_node_is_valid(const ncl_node *node);` — True when the node and its subtree satisfy the model rules.
- `ncl_node *ncl_root_node_parse(const char *text);` — Parse @p text into a root node and run ncl_root_node_post_construct().
- `ncl_node *ncl_root_node_from_json(const ncl_json *json);`
- `ncl_node *ncl_node_from_json(const ncl_json *json, ncl_node_type type);` — Build a node of the requested kind from its JSON representation.
- `ncl_node *ncl_root_node_post_construct(ncl_node *root);` — Fill in parents, paths, sample channel defaults and path/id maps.
- `ncl_json *ncl_node_to_json(const ncl_node *node);` — Serialise a node (and its subtree) using the property order of the
- `char *ncl_node_write_string(const ncl_node *node);`
- `void ncl_node_map_init(ncl_node_map *map);`
- `void ncl_node_map_free(ncl_node_map *map);`
- `ncl_err ncl_node_map_put(ncl_node_map *map, const char *key, ncl_node *node);`
- `ncl_node *ncl_node_map_get(const ncl_node_map *map, const char *key);`
- `size_t ncl_node_map_len(const ncl_node_map *map);`
- `const char *ncl_node_map_key_at(const ncl_node_map *map, size_t index);`
- `ncl_node *ncl_node_map_val_at(const ncl_node_map *map, size_t index);`
- `ncl_err ncl_root_node_path_map(const ncl_node *root, ncl_node_map *out);` — Map every path in the subtree to its node.
- `ncl_err ncl_root_node_id_map(const ncl_node *root, ncl_node_map *out);` — Map every id in the subtree to its node.

### `nclink/ncl_mqtt.h`

- `void ncl_mqtt_properties_init(ncl_mqtt_properties *props);`
- `void ncl_mqtt_properties_free(ncl_mqtt_properties *props);`
- `ncl_err ncl_mqtt_properties_add_user(ncl_mqtt_properties *props, const char *key, const char *value);` — Append a user property, preserving order.
- `const char *ncl_mqtt_properties_get_user(const ncl_mqtt_properties *props, const char *key);` — First value of @p key, or NULL.
- `bool ncl_mqtt_properties_has_user(const ncl_mqtt_properties *props, const char *key);`
- `ncl_err ncl_mqtt_properties_write(const ncl_mqtt_properties *props, ncl_strbuf *out);` — Encode the property set as it appears inside a packet: a variable byte
- `bool ncl_mqtt_properties_block_is_empty(const ncl_strbuf *block);` — True when the encoded block produced by ncl_mqtt_properties_write holds no
- `ncl_err ncl_mqtt_properties_read(const unsigned char *data, size_t len, ncl_mqtt_properties *props, size_t *consumed);` — Decode @p len bytes of properties.
- `ncl_err ncl_mqtt_encode_connect(const ncl_mqtt_connect_options *options, ncl_buffer *out);` — Encode a complete CONNECT packet.
- `void ncl_mqtt_connack_free(ncl_mqtt_connack *connack);`
- `ncl_err ncl_mqtt_decode_connack(const unsigned char *body, size_t len, ncl_mqtt_connack *out);` — Decode a CONNACK body (the bytes after the fixed header).
- `ncl_err ncl_mqtt_encode_publish(const char *topic, const unsigned char *payload, size_t payload_len, int qos, bool retain, bool duplicate, uint16_t packet_id, const ncl_mqtt_properties *properties, ncl_buffer *out);` — Encode a PUBLISH packet.
- `void ncl_mqtt_publish_free(ncl_mqtt_publish *publish);`
- `ncl_err ncl_mqtt_decode_publish(uint8_t header_flags, const unsigned char *body, size_t len, ncl_mqtt_publish *out);`
- `ncl_err ncl_mqtt_encode_ack(ncl_mqtt_packet_type type, uint16_t packet_id, uint8_t reason_code, ncl_buffer *out);` — Encode an acknowledgement packet (PUBACK/PUBREC/PUBREL/PUBCOMP).
- `ncl_err ncl_mqtt_decode_ack(const unsigned char *body, size_t len, uint16_t *packet_id, uint8_t *reason_code);` — Decode an acknowledgement body: packet id plus optional reason/properties.
- `ncl_err ncl_mqtt_encode_subscribe(uint16_t packet_id, const char *topic_filter, int qos, const ncl_mqtt_properties *properties, ncl_buffer *out);` — Encode a SUBSCRIBE packet for a single topic filter.
- `ncl_err ncl_mqtt_encode_unsubscribe(uint16_t packet_id, const char *topic_filter, const ncl_mqtt_properties *properties, ncl_buffer *out);` — Encode an UNSUBSCRIBE packet for a single topic filter.
- `void ncl_mqtt_suback_free(ncl_mqtt_suback *suback);`
- `ncl_err ncl_mqtt_decode_suback(const unsigned char *body, size_t len, ncl_mqtt_suback *out);`
- `ncl_err ncl_mqtt_encode_ping(bool response, ncl_buffer *out);` — Encode PINGREQ (and, with @p response, PINGRESP).
- `ncl_err ncl_mqtt_encode_disconnect(uint8_t reason_code, const ncl_mqtt_properties *properties, ncl_buffer *out);` — Encode DISCONNECT with an optional reason code and properties.
- `void ncl_mqtt_disconnect_free(ncl_mqtt_disconnect *disconnect);`
- `ncl_err ncl_mqtt_decode_disconnect(const unsigned char *body, size_t len, ncl_mqtt_disconnect *out);`
- `size_t ncl_mqtt_varint_encode(uint32_t value, unsigned char out[4]);` — Encode a variable byte integer.
- `size_t ncl_mqtt_varint_decode(const unsigned char *data, size_t len, uint32_t *value);` — Decode a variable byte integer.
- `bool ncl_mqtt_peek_header(const unsigned char *data, size_t len, ncl_mqtt_packet_type *type, uint8_t *flags, uint32_t *remaining_length, size_t *header_len);` — Peek at the fixed header of a buffer.
- `const char *ncl_mqtt_packet_type_name(ncl_mqtt_packet_type type);` — Human readable name of a packet type, used by the logger.
- `void ncl_mqtt_client_options_default(ncl_mqtt_client_options *options);` — Fill @p options with the defaults (clean start, 60 s keep alive, 10 s
- `ncl_mqtt_client *ncl_mqtt_client_create(const ncl_mqtt_client_options *options);`
- `void ncl_mqtt_client_destroy(ncl_mqtt_client *client);` — Stop the reader thread and release every resource.
- `ncl_err ncl_mqtt_client_connect(ncl_mqtt_client *client);` — Open the TCP connection, send CONNECT and wait for CONNACK.
- `ncl_err ncl_mqtt_client_disconnect(ncl_mqtt_client *client);` — Send DISCONNECT and close the socket (no reconnect afterwards).
- `bool ncl_mqtt_client_is_connected(ncl_mqtt_client *client);`
- `ncl_err ncl_mqtt_client_publish(ncl_mqtt_client *client, const char *topic, const void *payload, size_t payload_len, int qos, const ncl_mqtt_properties *properties, unsigned timeout_ms);` — Publish a message.
- `ncl_err ncl_mqtt_client_subscribe(ncl_mqtt_client *client, const char *topic_filter, int qos, unsigned timeout_ms, int *granted_qos);` — Subscribe and wait for the SUBACK.
- `ncl_err ncl_mqtt_client_unsubscribe(ncl_mqtt_client *client, const char *topic_filter, unsigned timeout_ms);` — Unsubscribe and wait for the UNSUBACK.
- `const char *ncl_mqtt_client_last_error(ncl_mqtt_client *client);` — Last transport level error message (never NULL).
- `size_t ncl_mqtt_client_subscription_count(ncl_mqtt_client *client);` — Number of topics the client is subscribed to (including while offline).
- `bool ncl_mqtt_client_wait_connected(ncl_mqtt_client *client, unsigned timeout_ms);` — Wait until the client is connected, up to @p timeout_ms (0 = forever).

### `nclink/ncl_platform.h`

- `int64_t ncl_time_millis(void);` — Milliseconds since the Unix epoch (wall clock).
- `int64_t ncl_time_monotonic_millis(void);` — Monotonic milliseconds, suitable for measuring intervals.
- `void ncl_sleep_millis(unsigned ms);` — Sleep for the given number of milliseconds.
- `bool ncl_random_bytes(void *buf, size_t len);` — Fill @p buf with @p len cryptographically-seeded random bytes.
- `void ncl_console_write(const char *text);` — 把一段 UTF-8 文本写到 stderr（日志的控制台镜像走这里）。
- `ncl_mutex *ncl_mutex_create(void);`
- `void ncl_mutex_destroy(ncl_mutex *m);`
- `void ncl_mutex_lock(ncl_mutex *m);`
- `void ncl_mutex_unlock(ncl_mutex *m);`
- `ncl_cond *ncl_cond_create(void);`
- `void ncl_cond_destroy(ncl_cond *c);`
- `void ncl_cond_wait(ncl_cond *c, ncl_mutex *m);` — Wait until signalled; @p m must be held and is re-acquired on return.
- `bool ncl_cond_wait_timeout(ncl_cond *c, ncl_mutex *m, unsigned timeout_ms);` — Wait at most @p timeout_ms (0 means "no limit").
- `void ncl_cond_signal(ncl_cond *c);`
- `void ncl_cond_broadcast(ncl_cond *c);`
- `ncl_thread *ncl_thread_start(ncl_thread_fn fn, void *arg);` — Start a thread running @p fn.
- `ncl_err ncl_thread_join(ncl_thread *t);` — Block until the thread finishes, then release the handle.
- `void ncl_thread_detach(ncl_thread *t);` — Release the handle without waiting (the thread keeps running).

### `nclink/ncl_rest.h`

- `ncl_json *ncl_result_success(ncl_json *data);` — Success answer: takes ownership of @p data (may be NULL).
- `ncl_json *ncl_result_failed(const char *message);` — Failure answer carrying @p message as "data".
- `ncl_json *ncl_result_success_bool(bool value);` — Success answer carrying a boolean.
- `ncl_json *ncl_result_success_string(const char *value);` — Success answer carrying a string.
- `void ncl_rest_reply(ncl_http_response *response, int status, ncl_json *data);` — Reply with a Result envelope in one call.
- `void ncl_rest_reply_error(ncl_http_response *response, int status, const char *message);`
- `ncl_err ncl_rest_attach(ncl_http_server *http, ncl_server *server);` — Register the endpoints that are derived from the NC-Link server state:
- `ncl_err ncl_rest_attach_config(ncl_http_server *http);` — Register the device configuration endpoints:

### `nclink/ncl_schema.h`

- `ncl_schema *ncl_schema_compile(const ncl_json *schema, char **error);` — Parse and prepare @p schema.
- `ncl_schema *ncl_schema_compile_text(const char *text, size_t len, char **error);`
- `void ncl_schema_free(ncl_schema *schema);`
- `const ncl_json *ncl_schema_root(const ncl_schema *schema);` — The schema document the object was compiled from (borrowed).
- `ncl_err ncl_schema_validate(const ncl_schema *schema, const ncl_json *value, ncl_strvec *errors);` — Validate @p value against @p schema, appending one message per violation to
- `ncl_err ncl_schema_validate_text(const ncl_schema *schema, const char *text, size_t len, ncl_strvec *errors);` — Parse @p text then validate it.
- `ncl_err ncl_json_schema_validate(const char *json_text, const char *schema_text, ncl_strvec *errors);` — Validate a JSON document given as text against a schema given as text.
- `char *ncl_schema_join_errors(const ncl_strvec *errors);` — Render a message list as "[msg1, msg2]".
- `ncl_regex *ncl_regex_compile(const char *pattern, char **error);` — Compile an ECMA-style pattern; NULL with *error set when unsupported.
- `void ncl_regex_free(ncl_regex *regex);`
- `bool ncl_regex_search(const ncl_regex *regex, const char *text, size_t len);` — True when @p regex matches anywhere inside @p text (unanchored search).

### `nclink/ncl_server.h`

- `ncl_server *ncl_server_create(const ncl_server_options *options);`
- `void ncl_server_free(ncl_server *server);`
- `ncl_err ncl_server_set_user_data(ncl_server *server, void *data, ncl_server_cleanup_fn cleanup);` — Attach caller-owned data to the server, released (through @p cleanup when it
- `void *ncl_server_user_data(const ncl_server *server);`
- `const char *ncl_server_sn(const ncl_server *server);` — Serial number this server answers for.
- `ncl_err ncl_server_load_model(ncl_server *server, const char *model_json);` — Parse a model document, run post-construction and take ownership.
- `ncl_err ncl_server_set_model(ncl_server *server, ncl_node *root);` — Install a model constructed by the caller (ownership transfers).
- `ncl_node *ncl_server_model(ncl_server *server);`
- `ncl_err ncl_server_save_model(ncl_server *server);` — Persist the model to the file ncl_env_model_file() names.
- `ncl_err ncl_server_register_tool(ncl_server *server, const char *tool_name, void *instance, const ncl_tool_method *methods, size_t method_count, const ncl_tool_binding *bindings, size_t binding_count);`
- `size_t ncl_server_binding_count(const ncl_server *server);` — Number of "<operation>#<path>" bindings currently registered.
- `size_t ncl_server_operation_count(const ncl_server *server);` — Number of distinct (tool, method) pairs, i.e.
- `const char *ncl_server_operation_tool(const ncl_server *server, size_t index);` — Name of the tool owning operation @p index.
- `const char *ncl_server_operation_method(const ncl_server *server, size_t index);` — Method name of operation @p index.
- `ncl_json *ncl_server_openapi_schema(ncl_server *server, const char *base_url);` — Build the OpenAPI 3.0 document describing the server's operations: one POST
- `char *ncl_server_openapi_schema_json(ncl_server *server, const char *base_url);` — ncl_server_openapi_schema() serialised to a heap JSON string.
- `ncl_message *ncl_server_invoke_query(ncl_server *server, const ncl_message *request);` — Takes ownership of nothing; returns a new message the caller frees.
- `ncl_message *ncl_server_invoke_set(ncl_server *server, const ncl_message *request);`
- `ncl_message *ncl_server_invoke_method_call(ncl_server *server, const ncl_message *request);`
- `ncl_message *ncl_server_check_method_call(ncl_server *server, const ncl_message *request);` — A *dry run* of a method call: the parameters are validated against the
- `ncl_message *ncl_server_dispatch(ncl_server *server, const char *topic, const ncl_message *request);` — Dispatch a parsed request to the matching invoke_* function.
- `ncl_err ncl_server_subscribe(ncl_server *server);` — Subscribe to the six request topics of this serial number.
- `void ncl_server_on_message(ncl_server *server, const char *topic, ncl_message *request);` — Handle one inbound message: process it and publish the response.
- `ncl_err ncl_server_report_method_progress(ncl_server *server, const char *handler, long long process, const char *status);` — Report progress of the running call @p handler (process 0..100, status one of
- `size_t ncl_server_pending_method_count(ncl_server *server);` — Calls with a handler that have not been collected through the result pair.
- `ncl_err ncl_server_publish(ncl_server *server, const char *topic, const ncl_message *response);` — Publish a response message on @p topic.
- `void ncl_server_set_publish_sink(ncl_server *server, ncl_server_publish_fn fn, void *user);` — Install (or clear) the outbound transport hook after creation.
- `ncl_err ncl_server_add_sample(ncl_server *server, const ncl_node *config);` — Register a sample channel and start its sampling/upload task.
- `ncl_err ncl_server_remove_sample(ncl_server *server, const char *id);` — Stop and remove a sample channel by id.
- `void ncl_server_stop_all_samples(ncl_server *server);` — Stop every sample channel.
- `ncl_err ncl_server_start_sample(ncl_server *server, const ncl_node *config);` — Start a sampling task without checking the configuration first.
- `ncl_err ncl_server_init_samples(ncl_server *server);` — Start a task for every SAMPLE_CHANNEL config of the first device in the
- `size_t ncl_server_sample_count(ncl_server *server);`
- `size_t ncl_server_sample_upload_count(ncl_server *server);` — Number of uploads published so far on the sample topics (diagnostics).
- `ncl_err ncl_server_register_builtin_tool(ncl_server *server);` — Register the built in "nclinkServer" tool (addSample / removeSample).
- `ncl_err ncl_server_push_event(ncl_server *server, const char *event_id, const ncl_json *event);` — Publish an Event message on "Event/<sn>".
- `ncl_err ncl_server_push_event_ex(ncl_server *server, const char *event_id, const ncl_json *event, int64_t time_ms, const char *message_id);` — Push an event, overriding the message time and/or "@id".
- `size_t ncl_server_event_count(ncl_server *server);` — Number of events published so far (diagnostics).

### `nclink/ncl_socket.h`

- `ncl_err ncl_socket_system_init(void);` — Initialise the platform networking stack (idempotent).
- `void ncl_socket_system_shutdown(void);`
- `void ncl_socket_system_release(void);` — Ask for the networking stack to be released once every socket is closed.
- `ncl_socket *ncl_socket_connect(const char *host, unsigned port, unsigned timeout_ms, char *err, size_t err_len);` — Connect to @p host:@p port.
- `bool ncl_socket_tls_available(void);` — True when this build can speak TLS.
- `ncl_socket *ncl_socket_connect_tls(const char *host, unsigned port, unsigned timeout_ms, const ncl_socket_tls_options *options, char *err, size_t err_len);` — Connect to @p host:@p port like ncl_socket_connect() and then run the TLS
- `ncl_socket *ncl_socket_listen(unsigned port, char *err, size_t err_len);` — Create a listening socket bound to @p port (0 picks an ephemeral port).
- `ncl_socket *ncl_socket_accept(ncl_socket *listener, unsigned timeout_ms);` — Accept one connection; returns NULL on timeout or error.
- `unsigned ncl_socket_local_port(const ncl_socket *s);` — Local port of a bound socket, or 0 when unknown.
- `ncl_err ncl_socket_local_ip(const ncl_socket *s, char *buf, size_t buf_len);` — Local address of @p s as text ("192.168.1.7" or "fe80::1%12"), which is the
- `ncl_err ncl_socket_peer_ip(const ncl_socket *s, char *buf, size_t buf_len);` — Peer address of @p s as text.
- `ncl_err ncl_socket_local_ip_toward(const char *host, unsigned port, char *buf, size_t buf_len);` — Local IPv4 address the routing table would use to reach @p host:@p port -
- `const char *ncl_net_local_ipv4(void);` — First non-loopback IPv4 address of an up interface: the address a peer on the
- `char *ncl_net_ip_map_json(void);` — Build `{"<interface>":"<ipv4>", ...}` in interface enumeration order.
- `ncl_err ncl_socket_send(ncl_socket *s, const void *data, size_t len);` — Send exactly @p len bytes.
- `int ncl_socket_recv(ncl_socket *s, void *buf, size_t len, unsigned timeout_ms);` — Receive up to @p len bytes.
- `ncl_err ncl_socket_recv_exact(ncl_socket *s, void *buf, size_t len, unsigned timeout_ms);` — Receive exactly @p len bytes, looping over partial reads.
- `void ncl_socket_set_nodelay(ncl_socket *s, bool enable);` — Disable Nagle's algorithm (used by the MQTT client).
- `void ncl_socket_set_keepalive(ncl_socket *s, bool enable);` — Enable TCP keep-alive probes.
- `void ncl_socket_close(ncl_socket *s);`
- `void ncl_socket_shutdown(ncl_socket *s);` — Close the underlying handle without freeing the ncl_socket.
- `ncl_err ncl_socket_parse_url(const char *url, char **host, unsigned *port, bool *tls);` — Parse "tcp://host:port", "mqtt://host:port" or a bare "host:port".

### `nclink/ncl_thread.h`

- `void ncl_thread_pool_options_default(ncl_thread_pool_options *options);` — Fill @p options with the defaults listed above.
- `ncl_thread_pool *ncl_thread_pool_create(const ncl_thread_pool_options *options);`
- `ncl_err ncl_thread_pool_submit(ncl_thread_pool *pool, ncl_thread_fn fn, void *arg);` — Queue @p fn for execution.
- `void ncl_thread_pool_shutdown(ncl_thread_pool *pool, bool wait);` — Stop accepting work; when @p wait is true, join every worker.
- `size_t ncl_thread_pool_pending(const ncl_thread_pool *pool);`
- `int ncl_thread_pool_worker_count(const ncl_thread_pool *pool);`
- `ncl_thread_pool *ncl_thread_service(void);` — Process wide pool, created on first use.
- `void ncl_thread_service_shutdown(void);` — Shut the shared pool down (call once during process teardown).
- `ncl_cache *ncl_cache_create(unsigned ttl_ms, bool expire_after_access, ncl_cache_free_fn free_fn);` — Create a cache.
- `void ncl_cache_free(ncl_cache *cache);`
- `void ncl_cache_clear(ncl_cache *cache);`
- `ncl_err ncl_cache_put(ncl_cache *cache, const char *key, void *value);` — Insert or replace @p key.
- `void *ncl_cache_get(ncl_cache *cache, const char *key);` — Look up @p key; returns NULL when absent or expired.
- `bool ncl_cache_remove(ncl_cache *cache, const char *key);` — Remove @p key; returns true when an entry was removed.
- `void *ncl_cache_take(ncl_cache *cache, const char *key);` — Remove @p key and hand the value to the caller without invoking the cache's
- `size_t ncl_cache_size(ncl_cache *cache);`
- `void ncl_cache_purge_expired(ncl_cache *cache);` — Drop expired entries (also done lazily by get/put).

### `nclink/ncl_topic.h`

- `char *ncl_topic_build(const char *prefix, const char *device_id, const char *client_id);` — Build "<prefix><deviceId>[/<clientId>]".
- `char *ncl_topic_ping(const char *sn);`
- `char *ncl_topic_pong(const char *sn);`
- `char *ncl_topic_probe_query_request(const char *device_id, const char *client_id);`
- `char *ncl_topic_probe_query_response(const char *device_id, const char *client_id);`
- `char *ncl_topic_probe_set_request(const char *device_id, const char *client_id);`
- `char *ncl_topic_probe_set_response(const char *device_id, const char *client_id);`
- `char *ncl_topic_query_request(const char *device_id, const char *client_id);`
- `char *ncl_topic_query_response(const char *device_id, const char *client_id);`
- `char *ncl_topic_set_request(const char *device_id, const char *client_id);`
- `char *ncl_topic_set_response(const char *device_id, const char *client_id);`
- `char *ncl_topic_sample(const char *device_id, const char *client_id);`
- `char *ncl_topic_probe_version(const char *device_id, const char *client_id);`
- `char *ncl_topic_method_call_request(const char *device_id, const char *client_id);`
- `char *ncl_topic_method_call_response(const char *device_id, const char *client_id);`
- `char *ncl_topic_method_status_request(const char *device_id, const char *client_id);`
- `char *ncl_topic_method_status_response(const char *device_id, const char *client_id);`
- `char *ncl_topic_method_result_request(const char *device_id, const char *client_id);`
- `char *ncl_topic_method_result_response(const char *device_id, const char *client_id);`
- `char *ncl_topic_event(const char *device_id, const char *client_id);`
- `char *ncl_topic_register_request(void);`
- `char *ncl_topic_extract_sn(const char *topic);` — Extract the device serial number from an inbound topic: split on '/', and for

## 附录 B · 错误码全表

来自 `nclink/ncl_common.h`。第三列是 `ncl_err_name()` 返回的稳定名称，
便于跨语言比日志。

| 名称 | 值 | `ncl_err_name()` |
|------|----|------------------|
| `NCL_ERR` | (-1) | （通用失败） |
| `NCL_ERR_NOMEM` | (-2) | 内存不足 |
| `NCL_ERR_PARSE` | (-3) | JSON/报文解析失败 |
| `NCL_ERR_TIMEOUT` | (-4) | 请求超时 |
| `NCL_ERR_IO` | (-5) | IoException / 文件失败 |
| `NCL_ERR_NOT_FOUND` | (-6) | 查找失败 |
| `NCL_ERR_EXISTS` | (-7) | 重复条目 |
| `NCL_ERR_NOT_SUPPORTED` | (-8) | 功能未编译 / 未实现 |
| `NCL_ERR_INVALID_ARG` | (-9) | IllegalArgumentException / 调用方传参错误 |
| `NCL_ERR_STATE` | (-10) | 对象不可用 |
| `NCL_ERR_RANGE` | (-11) | 越界 |
| `NCL_ERR_CONNECT` | (-12) | MqttException / 连接失败 |
| `NCL_ERR_CLOSED` | (-13) | 对象已关闭 |
| `NCL_ERR_NO_CHANNEL` | (-14) | NoFileChannelException / 设备还没有文件通道 |
| `NCL_ERR_INVALID_CODE` | (-100) | InvalidCodeException |
| `NCL_ERR_INVALID_DATA_NAME` | (-101) | InvalidDataNameException |
| `NCL_ERR_INVALID_DATA_TYPE` | (-102) | InvalidDataTypException |
| `NCL_ERR_INVALID_DEVICE_ID` | (-103) | InvalidDeviceIdException |
| `NCL_ERR_INVALID_ENCODING` | (-104) | InvalidEncodingException |
| `NCL_ERR_INVALID_ID` | (-105) | InvalidIdException |
| `NCL_ERR_INVALID_INDEX_RANGE` | (-106) | InvalidIndexRangeException |
| `NCL_ERR_INVALID_ITEM` | (-107) | InvalidItemException |
| `NCL_ERR_INVALID_KEY` | (-108) | InvalidKeyException |
| `NCL_ERR_INVALID_MESSAGE` | (-109) | InvalidMessageException |
| `NCL_ERR_INVALID_MESSAGE_ID` | (-110) | InvalidMessageIdException |
| `NCL_ERR_INVALID_MODEL` | (-111) | InvalidModelException |
| `NCL_ERR_INVALID_NODE` | (-112) | InvalidNodeException |
| `NCL_ERR_INVALID_NUMBER` | (-113) | InvalidNumberException |
| `NCL_ERR_INVALID_REQUEST` | (-114) | InvalidRequestException |
| `NCL_ERR_INVALID_TYPE` | (-115) | InvalidTypeException |
| `NCL_ERR_INVALID_VALUE` | (-116) | InvalidValueException |
| `NCL_ERR_INVALID_VERSION` | (-117) | InvalidVersionException |

## 附录 C · 主题前缀一览

来自 `nclink/ncl_topic.h`；除 `Register` 外都带设备 SN。

| 常量 | 值 |
|------|----|
| `NCL_TOPIC_PING_PREFIX` | "Ping/" |
| `NCL_TOPIC_PONG_PREFIX` | "Pong/" |
| `NCL_TOPIC_PROBE_QUERY_REQUEST_PREFIX` | "Probe/Query/Request/" |
| `NCL_TOPIC_PROBE_QUERY_RESPONSE_PREFIX` | "Probe/Query/Response/" |
| `NCL_TOPIC_PROBE_SET_REQUEST_PREFIX` | "Probe/Set/Request/" |
| `NCL_TOPIC_PROBE_SET_RESPONSE_PREFIX` | "Probe/Set/Response/" |
| `NCL_TOPIC_QUERY_REQUEST_PREFIX` | "Query/Request/" |
| `NCL_TOPIC_QUERY_RESPONSE_PREFIX` | "Query/Response/" |
| `NCL_TOPIC_SET_REQUEST_PREFIX` | "Set/Request/" |
| `NCL_TOPIC_SET_RESPONSE_PREFIX` | "Set/Response/" |
| `NCL_TOPIC_SAMPLE_PREFIX` | "Sample/" |
| `NCL_TOPIC_REGISTER_REQUEST` | "Register/Request" |
| `NCL_TOPIC_PROBE_VERSION_PREFIX` | "Probe/Version/" |
| `NCL_TOPIC_METHOD_CALL_REQUEST_PREFIX` | "Method/Call/Request/" |
| `NCL_TOPIC_METHOD_CALL_RESPONSE_PREFIX` | "Method/Call/Response/" |
| `NCL_TOPIC_METHOD_STATUS_REQUEST_PREFIX` | "Method/Status/Request/" |
| `NCL_TOPIC_METHOD_STATUS_RESPONSE_PREFIX` | "Method/Status/Response/" |
| `NCL_TOPIC_METHOD_RESULT_REQUEST_PREFIX` | "Method/Result/Request/" |
| `NCL_TOPIC_METHOD_RESULT_RESPONSE_PREFIX` | "Method/Result/Response/" |
| `NCL_TOPIC_EVENT_PREFIX` | "Event/" |
## 附录 D · 安装根目录布局

```
<root>/
  bin/sn.txt              设备序列号（首次启动生成 "V2" + 9 位十六进制，见 3.4）
  bin/ftp.txt             FTP 端口与账号（设备端 FTP 端点用）
  conf/mqtt.cfg           url/username/password（示例首次启动写本机 1883、匿名）
  conf/model/nclink.json  数据模型（示例首次启动写默认机床模型）
  conf/driver/*.json      驱动配置
  conf/ipConf.json        网络配置
  conf/server.json        服务器列表
  log/out.txt             日志（10 MB 轮转）
  uploadFile/             设备侧文件镜像（相对路径的基准）
  temp/                   文件通道的临时交换目录
  <sn>/                   客户端侧文件镜像（相对路径的基准）
```
