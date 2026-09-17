# NC-Link Python 绑定（ctypes）

`nclink-core-c` 的 Python 封装：**零第三方依赖**（只用标准库的 `ctypes`），
JSON 也走库自己的解析器/序列化器。

```
bindings/python/
  nclink/__init__.py     init / shutdown / get_device / 日志 / 安装根目录
  nclink/_ffi.py         ctypes 桥：找 nclink_shim、声明原型、错误码 -> 异常
  nclink/_json.py        Json（库自己的 JSON）
  nclink/_model.py       Model / Node（模型树）
  nclink/_message.py     Sample / Event / Message + parse(topic, payload)
  nclink/client.py       DeviceClient（读值/写值/探测/订阅）
  tests/test_nclink.py   自检（不需要 broker）
  examples/client_demo.py 客户端示例（对着设备端示例跑）
```

## 为什么要垫片

C 库是静态库、没有导出符号，而且 Python 侧不该依赖 C 结构体的内存布局（库里改个
字段就静默错位）。所以三种托管绑定（C# / Java / Python）共用
`bindings/native/nclink_shim.c` 这层扁平的 C ABI：

- 只暴露三样东西：不透明句柄（`void*`）、标量、UTF-8 文本；
- 报文/模型/JSON 都由库自己编解码，Python 侧不做 JSON 解析；
- 回调把 `const ncl_message*` 作为不透明句柄传过来，绑定立刻拷成 Python 对象。

## 构建

```powershell
# 1) 先编核心静态库（仓库根）
.\build.ps1

# 2) 编原生垫片 → bindings/native/bin/nclink_shim.dll
powershell -ExecutionPolicy Bypass -File .\bindings\native\build-shim.ps1

# 3) 自检（客户端 + 设备端，不需要 broker）
python -m unittest discover -s bindings/python/tests -v

# 4) 想连真 broker 再跑一遍端到端（设备端 + 客户端同进程，报文真的过 MQTT）
NCLINK_TEST_BROKER=tcp://127.0.0.1:1883 python -m unittest discover -s bindings/python/tests -v
```

Linux：

```sh
./build-linux.sh build-linux          # 出 build-linux/libnclink_core.a
sh bindings/native/build-shim.sh      # 出 bindings/native/bin/libnclink_shim.so
python -m unittest discover -s bindings/python/tests -v
```

`tests/test_broker_e2e.py` 默认跳过（没设 `NCLINK_TEST_BROKER` 时）；设了就跑
"设备端 + 客户端同进程"的端到端：probe、路径绑定取值/写值、methodCall、
采样上报、事件推送，以及整条文件通道（上传 / 列目录 / 下载 / 建目录 / 删文件 /
带文件参数的方法调用）——离线自检覆盖不到的"过 MQTT 那一段"靠它守住。

`nclink_shim.dll` / `libnclink_shim.so` 不用拷来拷去：绑定的加载顺序是

1. 环境变量 `NCLINK_SHIM` 指向的文件；
2. 包目录（把 DLL 放在 `nclink/` 旁边，pip/拷贝安装就用这种）；
3. 仓库布局 `bindings/native/bin/`、`build/`、`build/bin/`、`build-linux/`；
4. 交给系统（`find_library` / `PATH` / `LD_LIBRARY_PATH`）。

把本目录加进 `sys.path`（或拷进工程）即可 `import nclink`，不需要装包。

## 用法

```python
import time
import nclink

nclink.log_init()                                  # 日志走 <root>/log/out.txt
nclink.init("tcp://127.0.0.1:1883")                # 进程级连接，一次

with nclink.get_device("V2023A7B762") as device:
    with device.probe() as model:                  # 拉模型（顺带装进客户端）
        print(model.root.id, model.root.name)
        for item in model.root.devices[0].data_items:
            print(item.path, "->", item.id)
        print(device.get_id("/STATUS"), device.get_path("010302"))

    with device.get_value("/STATUS") as value:     # 读值
        print("STATUS =", value.to_python())
    device.set_value("/STATUS", 42)                # 写值
    with device.get_value_range("/PART_COUNT", 0, 9) as window:
        print(window.to_python())
    with device.method_call("/plc/getCount", check=True) as reply:
        print(reply.to_python())
    device.ping()

    device.subscribe_samples(2, lambda topic, sample: print(sample.rows))
    device.subscribe_events(2, lambda topic, event: print(event.key, event.value))
    time.sleep(5)
    print(device.sample_count, device.event_count)

nclink.shutdown()
```

跑法（对着设备端示例，另开一个窗口）：

```powershell
build\examples\ncl_device_demo.exe D:\sim-py 30
python bindings\python\examples\client_demo.py tcp://127.0.0.1:1883 <设备SN> 6
```

> **中文输出**：控制台不是 UTF-8 时（Windows 的默认代码页、Linux 容器里的 POSIX
> locale），示例打印的中文会乱码或抛 `UnicodeEncodeError`。Windows 上先
> `chcp 65001`、Linux 上加 `PYTHONIOENCODING=utf-8` 即可。库自身的日志始终写
> UTF-8。

### 采样怎么读

外层是**槽位**（通道的 `sampleInterval` 一槽），内层是"每槽装几个点"——采样率不同的
数据项可以放在**同一个通道**里（1 ms 的列每槽 1 点、0.25 ms 的列每槽 4 点）。按行
读最省事：

```python
def on_sample(topic, sample):
    print(sample.id, sample.interval_ms, sample.upload_interval_ms)
    for column in sample.columns:                  # path / slots / points / is_nested
        print(column.path, column.slots, column.points)
    for row in range(sample.rows):                 # 行数 = 数据最多的那一列的点数
        for col, column in enumerate(sample.columns):
            value = sample.value_at(row, col)      # 粗列取"覆盖该行的第一个点"
            number = sample.get_double(row, col)
```

回调里拿到的 `Sample` / `Event` 已经是**快照**（纯 Python 对象），出了回调照样能用；
原始 JSON 文本在 `sample.raw_json`。语义与 C 的 `ncl_message_sample_value_at()` 一致。

### 解析报文

自己从别处拿到的报文（离线回放、日志、文件）也能按库的规则解码：

```python
message = nclink.parse("Sample/V203243111F/s1", payload_bytes)
if isinstance(message, nclink.Sample):
    print(message.header())
elif isinstance(message, nclink.Event):
    print(message.key, message.value)
```

## 设备端（Python 当一台机床）

`nclink.Server` 把设备端（`ncl_server`）也包了：注册工具方法、把模型里的路径绑到
方法上、启动采样通道、推事件。MQTT 由库负责（也可以完全不接 broker）。

```python
import nclink

device = nclink.Server(sn="V2PY0000001", model=model_json, broker="tcp://127.0.0.1:1883")
device.register_tool(
    "plc",
    methods={"getStatus": None, "setCount": {"type": "object",
                                             "properties": {"value": {"type": "integer"}}}},
    handlers={"getStatus": lambda params: 1,
              "setCount": lambda params: params["value"]},
    bindings=[("/STATUS", nclink.Operation.GET_VALUE, "getStatus"),
              ("/PART_COUNT", nclink.Operation.SET_VALUE, "setCount")])
device.register_builtin_tool()      # addSample / removeSample
device.subscribe()                  # 订阅 6 个请求主题
device.init_samples()               # 启动模型里声明的采样通道
device.push_event("010307", {"key": "PART_COUNT", "value": 7})

print(device.model.root.id, device.operation_count, device.sample_count)
print(device.sample_upload_count, device.event_count, device.openapi_json("http://x/api"))
device.close()
```

- 处理函数收 `params`（Python 对象或 None），返回要应答的值：**返回 None = 没有值**
  （库按 NG 应答，与 C API 一致）；抛异常 → 该次调用按错误应答、异常文本进 reason，
  异常本身记在 `device.last_callback_error` 上，不会穿回原生层。
- 不接 broker 时（`broker=None`）用 `device.dispatch(topic, payload)` 或
  `device.invoke_method_call(method, params)` 离线驱动；给 `publish=` 一个回调就能
  自己当传输（每条出站报文交给你）。
- 设备端示例：`python examples/device_demo.py tcp://127.0.0.1:1883 V2PY0000001 30`
  （它注册 4 个方法 + 一个采样通道 + 每秒一条事件，仓库里任意客户端都能读它，例如
  `build\examples\ncl_client_demo.exe tcp://127.0.0.1:1883 V2PY0000001 8`）。

### HTTP / REST 端点

端点的内容全在库里：`GET /api/schema`（OpenAPI 3.0 文档）、`GET /swagger-ui`
（浏览器里直接调工具方法）、`POST /api/<工具>/<方法>`（等价于 `methodCall`），
`with_config=True` 再挂配置端点（SN / 模型 / 驱动 / mqtt.cfg / 服务器列表）。

```python
http = device.start_http(9008, with_config=True)     # 0 = 随机端口
# 处理函数返回 None = 200 空报文；str = text/plain；dict/list = JSON；
# (status, content_type, body) = 完全自己决定
http.route("GET", "/api/hello",
           lambda method, path, query, body: {"query": query})
print(http.url, http.request_count)
http.set_cors(False)                                 # 默认 Access-Control-Allow-Origin: *
http.close()                                         # 幂等；device.close() 也会替你收
```

- `method` 支持 `"*"`；`path` 以 `/api/` 开头时是前缀匹配，否则要求完全相等。
- **关端点要在关 `device` 之前**（路由回调还挂在服务器上），`device.close()` 已经按
  这个顺序做了。
- 设备端示例会把它挂起来：`python examples/device_demo.py ... 30 9008`，然后
  `curl -X POST http://127.0.0.1:9008/api/plc/getCount -d '{}'` 就能调工具方法。

### 文件通道（上传 / 下载）

MQTT 报文里只传 `/temp/<名字>` 这样的**令牌**，字节走 FTP。方向要记住：**设备是
FTP 客户端**，本机是 FTP 服务端（进程级端点 127.0.0.1:2323、admin / 123456、根 =
安装根；`nclink.init()` 时已经起好，`nclink.start_file_server()` 是显式的幂等版本）。

```python
# 设备端（收文件的那一边）
device = nclink.Server(sn="V2PY0000001", model=model_json,
                       broker="tcp://127.0.0.1:1883")
device.register_file_tool()          # /CONTROLLER/FILE：write/read/ll/mkdir/delete
device.subscribe()

# 客户端（上位机那一侧）
with nclink.get_device("V2PY0000001") as client:
    client.upload_local_file("report.txt", "/data/report.txt")   # 传上去
    for item in client.list_files("/data"):                       # nclink.FileInfo
        print(item.file_name, item.file_size, item.total_chunks, item.checksum)
    local = client.download_to("/data/report.txt", "back.txt")    # 取回来
    client.make_directory("/docs")
    client.delete_file("/data/report.txt")

    # 带文件参数的方法调用：keys 与 paths 一一对应，应答里的 fileKeys 会被换成本地路径
    with client.method_call_file("plc/convert", params={"input": ""},
                                 keys=["input"], paths=["a.bin"]) as reply:
        print(reply.to_python()["data"])

# 本地文件小工具（不需要 broker）
nclink.file_need_compression("model.json")     # True
nclink.file_total_chunks(300 * 1024)           # 2
nclink.file_checksum("report.txt")             # SHA-256 十六进制
nclink.file_attribute("report.txt")            # nclink.FileInfo
```

- `upload_file(relative)` 只是"把已经在 `<当前目录>/<sn><relative>` 上的文件传上去"
  （与 C API 一致）；`upload_local_file()` 会先替你摆到那个位置。
- 下载回来的文件先落在 `<当前目录>/<sn>/` 下，`download_file()` 返回绝对路径，
  `download_to()` 再替你复制到目标。
- 设备按 `conf/mqtt.cfg` 里 broker 的主机名 + 端口 2323 找本机的 FTP 端点（拿不到配置
  就 127.0.0.1）——所以本机与 broker 是同一台机器时开箱即用。
- 设备侧的 `start_ftp()` 是"设备自己也开个 FTP 端点"（读 `bin/ftp.txt`），客户端传文件
  用不到它；缺文件时它抛 `NclinkError`，示例里是容忍着来的。
- 对端不跟 broker 同机（或者端口/账号不一样）时显式指定：

```python
device.set_file_peer("10.0.0.7", 2323)                    # 设备端指到本机的 FTP
nclink.start_file_server(2323, root="D:/files", username="admin", password="123456")
```

### TLS（ssl://）

```python
nclink.tls_available()          # 这个垫片带 TLS 编译吗

nclink.init("ssl://broker.example.com:8883",
            tls=nclink.TlsOptions(ca_file="ca.pem",          # 内网 CA；不设 = 平台信任库
                                  client_cert="client.pem",  # 双向 TLS 才要
                                  client_key="client.key",
                                  server_name="broker.example.com",
                                  verify_peer=True))         # 默认就是 True

# 设备端直连 ssl:// broker 也支持：
nclink.Server(sn="V2PY0000001", broker="ssl://broker.example.com:8883",
              tls=nclink.TlsOptions(ca_file="ca.pem"))
```

**TLS 构建**：`sh bindings/native/build-shim.sh` 那套在 Linux 上要带 `NCL_WITH_TLS=1` 编库
（见手册 2.4），Windows 上：

```powershell
.\build.ps1 -Tls
powershell -ExecutionPolicy Bypass -File .\bindings\native\build-shim.ps1 -Tls
# → bindings\native\bin-tls\nclink_shim.dll；让 NCLINK_SHIM 指向它，或用它覆盖包目录那份
```

库没带 TLS 时用 `ssl://` 会拿到明确的 `NOT_SUPPORTED`（-8）。用例：设
`NCLINK_TEST_TLS_BROKER=ssl://127.0.0.1:18832` 与 `NCLINK_TEST_TLS_CA=tests/data/tls_localhost_cert.pem`
就多跑两个 TLS 端到端（设备端 + 客户端都过真 TLS broker；不给 CA 必须被拒）。

## 内存与所有权

| 对象 | 谁释放 |
|------|--------|
| `Json`（`parse` / `clone` / `get_value` / `method_call` / `get_value_range` 的返回值） | **自有**：`close()` 或 `with`；忘了也有 GC 兜底 |
| `Model`（`probe` / `parse` 的返回值） | **自有**，同上 |
| `DeviceClient` | `close()`（退订 + 清回调；断开 MQTT 是 `nclink.shutdown()`） |
| `Node`、`Json` 的下标/成员视图 | **借用**：持有宿主引用，不用关 |
| `Sample` / `Event` / `Message` | 纯 Python 快照，没有句柄 |
| `Server` | **自有**：`close()`（停采样/FTP、断开 MQTT、放掉所有回调） |
| `HttpEndpoint` | **自有**：`close()`（幂等；`server.close()` 也会收） |
| `server.model` | **借用**：服务器活着就有效，不用关 |
| `FileInfo` | 纯 Python 快照，没有句柄 |

采样/事件回调在客户端自己的**读取线程**上触发，回调里别做耗时操作；回调里抛出的异常
不会穿到原生层，会记在 `device.last_callback_error` 上。`init` / `shutdown` /
`get_device` 是线程安全的（底层有锁）；`subscribe_*` / `unsubscribe_*` 请在同一个
线程里成对调用。

## 示例输出

```
connected: tcp://127.0.0.1:1883 (nclink 3.1.0)
probe: model root id=01 name=机床模型文件
各轴的功率与振动（路径 含义）:
/AXIS@X/POWER@1          X轴功率
/AXIS@S/ACCELERATION@Z   主轴加速度（Z 向）
GET /STATUS = 1
SET /STATUS = 42 ok
id(/STATUS) = 010302, path(010302) = /STATUS
subscribed: Sample/V20BB7D849F/# and Event/V20BB7D849F
sample Sample/V20BB7D849F/EdgeSersors: id=EdgeSersors interval=1ms upload=100ms columns=12 rows=400
  /AXIS@X/POWER@1: 100 个槽位 × 每槽约 1 点 = 100 点
  /AXIS@X/ACCELERATION@X: 100 个槽位 × 每槽约 4 点 = 400 点（批量）
  行[0] /AXIS@X/POWER@1=800.0  /AXIS@X/ACCELERATION@X=-1.0  /AXIS@Y/POWER@1=1137.5 ...
event Event/V20BB7D849F: key=PART_COUNT value=50
received 32 samples, 6 events
```

## 还没做的

- 打包成 wheel / PyPI：现在按"C 库 + 绑定源码"一起用；要做 wheel 得把
  `nclink_shim.dll` 打进包（`package_data`）并带上平台标签。
- 零拷贝读采样：现在回调里给的是纯 Python 快照（方便、安全）；需要极致吞吐可以加一个
  "只在回调期间有效"的借用视图 API。
