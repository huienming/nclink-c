# NC-Link C# 绑定

`nclink-core-c` 的 .NET 封装：**同一份代码同时支持 .NET Framework 与 .NET（Core）**。

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
  samples/Nclink.Demo.Cli/    控制台示例（net472 + net8.0 两个产物）
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
# 1) 先编核心静态库（仓库根）
.\build.ps1

# 2) 编原生垫片 → bindings/native/bin/nclink_shim.dll
powershell -ExecutionPolicy Bypass -File .\bindings\native\build-shim.ps1

# 3) 编托管封装与示例（需要 .NET SDK）
dotnet build .\bindings\csharp\samples\Nclink.Demo.Cli\Nclink.Demo.Cli.csproj -c Release
```

Linux：

```sh
./build-linux.sh build-linux               # 出 build-linux/libnclink_core.a
sh bindings/native/build-shim.sh           # 出 bindings/native/bin/libnclink_shim.so
dotnet build bindings/csharp/src/Nclink.Core/Nclink.Core.csproj -c Release
```

`nclink_shim.dll` / `libnclink_shim.so` 必须和你的程序在同一个目录（或者在 `PATH` /
`LD_LIBRARY_PATH` 上）；两个工程的 csproj 已经把 `bindings/native/bin/` 下的那个作为
`None ... CopyToOutputDirectory` 拷进输出目录了。

## 用法

```csharp
using Nclink;

Nclink.LogInit();
Nclink.Init("tcp://127.0.0.1:1883");            // 进程级连接，一个进程一次

using (NclDeviceClient device = Nclink.GetDevice("V2023A7B762"))
{
    using (NclModel model = device.Probe())     // 拉模型
    {
        foreach (NclNode item in model.Root.Devices) { /* 遍历设备/组件/数据项 */ }
        string id = device.GetId("/AXIS@X/POWER");   // 路径 → 节点 id
    }

    long status = device.GetLong("/STATUS");         // getValue
    device.SetValue("/STATUS", "42");                // setValue（值用 JSON 文本）
    using (NclJson range = device.GetValueRange("/PART_COUNT", 0, 9)) { }
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

- **自有**对象要 Dispose：`NclJson`（`Parse`/`Clone`/`GetValue*` 的返回值）、
  `NclModel`（`Probe`/`Parse` 的返回值）、`NclDeviceClient`。
- **借用**对象不用管：`NclNode`、`NclJson` 的下标/成员视图——只要宿主对象还活着就
  有效（内部持有宿主引用）。
- 采样/事件回调在客户端自己的读取线程上触发，**别在回调里做耗时操作**；回调里抛出的
  异常不会跨到原生层，会记在 `NclDeviceClient.LastCallbackError` 上。
- `Init`/`Shutdown`/`GetDevice` 是线程安全的（底层有锁）。

## 示例输出

```
connected: tcp://127.0.0.1:1883 (nclink 3.1.0)
probe: model root id=01 name=机床模型文件
各轴的功率与振动（路径 含义）:
/AXIS@X/POWER            X轴功率
/AXIS@S/ACCELERATION     主轴加速度
GET /STATUS = 1
SET /STATUS = 42 ok
subscribed: Sample/011533226/# and Event/011533226
event Event/011533226: key=PART_COUNT value=120
sample Sample/011533226/EdgeSersors: id=EdgeSersors interval=1ms upload=1000ms columns=10 rows=4000
  /AXIS@X/POWER: 1000 个槽位 × 每槽约 1 点 = 1000 点
  /AXIS@X/ACCELERATION: 1000 个槽位 × 每槽约 4 点 = 4000 点（批量）
  行[0] /AXIS@X/POWER=800  /AXIS@X/ACCELERATION=-1  /AXIS@Y/POWER=1112.5 ...
  行[1] /AXIS@X/POWER=800  /AXIS@X/ACCELERATION=-0.875  /AXIS@Y/POWER=1112.5 ...
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

## 还没做的

- **设备端（Server）**：`ncl_server_*`、工具方法注册、采样任务、事件推送都还没包；
  垫片里加一组 `nclshim_server_*`（工具回调同样用"JSON 文本进、JSON 文本出"的桥）
  即可，托管侧再给 `NclServer` + `RegisterTool`。
- 文件通道（`/nclinkClient/*` 上传下载）与 REST/HTTP 接口。
- TLS（`ssl://`）：库要带 `NCLINK_WITH_TLS=ON` 编，垫片不用改。
- 零拷贝读采样：现在 `NclSample` 在回调里整份拷贝（方便、安全）；需要极致吞吐可以
  加一个"只在回调期间有效"的借用视图 API。
- 单元测试工程（现在靠 `Nclink.Demo.Cli` 对着设备端示例做冒烟验证）。
