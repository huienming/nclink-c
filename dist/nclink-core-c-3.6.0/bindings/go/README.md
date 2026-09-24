# NC-Link Go 绑定（cgo）

`go get github.com/huienming/nclink-c/examples/sdk/go`（或直接把本目录拷进工程）。

## 链接

包内默认链接 `lib/<goos>-<goarch>/` 下的静态库：

```
examples/sdk/go/lib/windows-amd64/libnclink_core.a   # 用 mingw 编的（cgo 的 Windows 工具链是 mingw）
examples/sdk/go/lib/linux-amd64/libnclink_core.a
```

`tools/stage-go-libs.sh` 会把各平台的库拷到这里（库本身不入库）。

- **Windows**：cgo 需要 mingw-w64（不认 MSVC），且必须链 **mingw 编的库**；
  MSVC 的 `nclink_core.lib` 不能用于 Go。
  编库：`CC=<mingw>/gcc AR=<mingw>/ar sh build-linux.sh build-mingw`
  或直接：`for f in $(find src -name '*.c'); do gcc -c -O2 -Istack/include -Istack/src $f; done` + `ar rcs`。
- **Linux**：`sh build-linux.sh build-linux` 出的 `libnclink_core.a` 直接用。
- **TLS**：加 `-tags nclink_tls`，此时链的是 `libnclink_core_tls.a`（`NCL_WITH_TLS=1`
  编的那份）加 `-lssl -lcrypto`；Windows 上还需要静态 OpenSSL 的导入库。用
  `nclink.TLSAvailable()` 问一下当前构建带没带 TLS（不带时 `ssl://` 返回
  `NCL_ERR_NOT_SUPPORTED`，不是笼统的连接失败）。

包内除了 `nclink.go`（客户端）/ `server.go`（设备端），还有一个 `nclink_thunks.c`：
cgo 不能把 Go 函数指针交给 C，而 C API 又是**用函数指针认工具方法**的，所以每种
方法一个 C 跳板（一次进程最多注册 `maxToolMethods` = 32 个方法、32 条 HTTP 路由）；
回调的用户数据是一次单独的 `handleBox` 分配 —— cgo 只允许把"不含 Go 指针的 Go
内存"交给 C，`Client`/`Server` 结构体里本身有切片与函数值，句柄不能直接住在里面。

## 用法

```go
if err := nclink.Open("tcp://broker:1883", "", ""); err != nil { log.Fatal(err) }
defer nclink.Shutdown()

c, err := nclink.Get("V2023A7B762")
model, err := c.Probe(5000)              // 拉模型
v, err := c.Value("/MACHINE/STATUS", 5000)       // 读值
defer v.Close()
err = c.Set("/MACHINE/STATUS", mustJSON("42"), 5000)

err = c.SubscribeSamples(2, func(topic string, msg *nclink.Message) {
    // msg 只在回调期间有效（C 侧规则一致）
})
```

所有 C 内存都由 `Close()` 释放（`runtime.SetFinalizer` 兜底）。

## 设备端（Server）

同一个包也能让"这个进程就是一台机床"：注册工具方法、把 `<operation>#<path>` 绑到
方法上、按模型起采样通道、推事件、挂 HTTP/REST 端点，客户端可以是 C / C# / Java /
Python 里任意一个。

```go
device, err := nclink.NewServer(nclink.ServerOptions{
    SN:     "V2GODEV0001",
    Model:  modelJSON,                 // "" = 库内置模型
    Broker: "tcp://127.0.0.1:1883",    // "" = 离线：出站报文交给 Publish
    Publish: func(topic string, payload []byte) error {   // 自研传输（可选）
        log.Printf("out %s %s", topic, payload)
        return nil
    },
})
if err != nil { log.Fatal(err) }
defer device.Close()

err = device.RegisterTool("plc",
    []nclink.ToolMethod{
        {Name: "getStatus"},
        {Name: "setCount", Schema: `{"type":"object","properties":{"value":` +
            `{"type":"number"}},"required":["value"]}`},
    },
    []nclink.Binding{
        {Path: "/MACHINE/STATUS", Operation: nclink.OpGetValue, Method: "getStatus"},
        {Path: "/MACHINE/PART_COUNT", Operation: nclink.OpSetValue, Method: "setCount"},
    },
    func(method string, params any) (any, error) {
        // params 是请求参数的 JSON 解码结果（没有参数时是 nil）；
        // 返回任何能 json.Marshal 的值，或 nil（= "没有值"）；返回 error 就是
        // 应答 code=NG + 该错误文本。回调跑在服务端的工作线程上。
        return 42, nil
    })

device.RegisterBuiltinTool()   // /nclinkServer/addSample、removeSample
device.RegisterFileTool()      // /CONTROLLER/FILE（对端 FTP 用 SetFilePeer 指定）
device.Subscribe()             // 订请求主题（离线设备不需要）
device.InitSamples()           // 按模型里的 SAMPLE_CHANNEL 起采样
device.PushEvent("030001", map[string]any{"key": "PART_COUNT", "value": 21})

endpoint, err := device.StartHTTP(9008, true)   // 0 = 随机端口
endpoint.Route("GET", "/api/hello", func(r *nclink.HTTPRequest) *nclink.HTTPReply {
    return &nclink.HTTPReply{Status: 200, Body: `{"ok":true}`}   // nil = 404
})
```

离线（`Broker: ""`）时不给 `Publish` 也能跑：`Dispatch` / `InvokeQuery` /
`InvokeSet` / `InvokeMethodCall` / `CheckMethodCall` 直接把报文喂进去取应答，
离线自检就是这么测的。设备端示例在 `examples/device/go`
（`examples/client/go` 是客户端示例；两边各自是一个小模块，用 `replace` 指回本目录）：

```sh
cd ../../device/go && go run .                      # 离线，出站报文打到控制台
cd ../../device/go && go run . tcp://127.0.0.1:1883 # 过 broker
```

自检：`go test ./...`（不需要 broker；Linux 上 cgo 用系统 gcc，Windows 上要 mingw）。

## 内存与线程

| 对象 | 谁释放 |
|------|--------|
| `Json` / `Message` / `Model` | **自有**：`Close()`（`runtime.SetFinalizer` 兜底） |
| `Client` | 进程级：断开是 `Shutdown()`；采样回调里拿到的 `Message` 只在回调期间有效 |
| `Server` | **自有**：`Close()`（先收 HTTP，再停采样/FTP 与文件工具、断开 MQTT、放掉回调）；幂等 |
| `HTTPEndpoint` | **自有**：`Close()`（幂等；`Server.Close()` 也会先收它） |

工具方法回调跑在服务端的工作线程上，`params` 已经是解码好的 Go 值（出了回调照样
用），返回值会被序列化成应答；别在回调里做耗时操作。注册上限是 32 个方法 / 32 条
HTTP 路由（每个回调需要一个 C 跳板，见上）。
