# 变更记录

本文件记录 NC-Link C 实现（`nclink-core-c`）的版本变更。版本号跟随
NC-Link 规范版本：**3.0.0** 对应 GB/T 41970-2022 协议 3.0.0。

## 3.1.0

采样链路的消费侧补齐、设备端示例改成现场跑法、C# 绑定可用，发布包直接带**编好的
示例可执行文件**（Windows x64 / x86 与 Linux）。

### 采样与报文

- `ncl_message_sample_value_at()`：**按行**取某列的值。行数 = 数据最多的那一列的
  点数（最细的那根时间轴），更粗的列在覆盖该行的段里反复取第一个点 —— 1 ms 的功率
  列与 0.25 ms 的振动列可以放在同一张表里逐行读，消费端不用自己判断谁粗谁细。
- `ncl_message_sample_point_count()` 的语义明确为"行数 = 最多那列的点数"（此前要求
  各列点数相同，现在不同也能读）。
- **完整性口径放宽到只看外层**：表头项数 == 列数、各列槽位数一致即可，内层（每槽
  装几个点）各列自便，每槽点数抖动也照常上报。整列 `[]` 仍按"本周期无数据"处理，
  由 `ncl_message_sample_normalise()` 按列换成 `null`（不再要求"换完能回到统一
  标量形状"）。
- 设备端/客户端示例按新口径消费：逐列打印编码与点数，再按行打前 8 行。

### 设备端示例（ncl_device_demo）

- **默认一直运行**：省略运行秒数（或写 0）就一直跑到 Ctrl+C / SIGTERM；给正数则跑完
  自动退出（脚本、冒烟）。Ctrl+C 走正常清理路径（停采样、停 FTP/HTTP、断开 MQTT），
  退出码 0；SIGINT/SIGTERM 的处理函数只置标志，主循环 100 ms 一跳，响应不迟。
- 默认模型的采样整理成**两个通道**：
  - 通道 0 `sample_channel0`（1 s / 1 s，机床运行状态八项）：加工计件、进给倍率、
    当前加工程序名、当前刀号、主轴转速、设备状态、加工模式、报警号；
  - 通道 1 `EdgeSersors`（`sampleInterval` 1 ms / `uploadInterval` 100 ms，十二项）：
    5 轴的功率与振动（振动每槽 4 点 = 0.25 ms）；主轴 S 上挂**两路**传感器，用数据项的
    `number` 区分。一条报文 100 个槽位（功率列 100 点、振动列 400 点）。
- **数据项支持 `number`**：一个部件挂多路同类传感器时就是"同 `type`、不同 `number`、
  不同 id"的几个数据项，路径变成 `/<父路径>/<type>@<number>`（`/AXIS@S/POWER@1`、
  `/AXIS@S/POWER@2`），工具绑定、采样表头、按路径查询都按这条完整路径走；没有
  `number` 的项就是单路，路径不带后缀。`tests/test_model.c` 增补了路径、按路径反查、
  同一个通道两路传感器各成一列，以及 `number` 的序列化字段顺序（在 `dataType` 之前）。
- 主轴转速按"轴 + 物理量"写在主轴 S 轴上：`/AXIS@S/SPEED`（`SPEED` 数据项），不再
  是设备级自成一类的 `SPINDLE_SPEED`；示例补上对应工具绑定与模拟值。
- 三个示例（设备端、C 客户端、C++ 客户端）都会打印轴上的量（`路径 含义`），
  `SPEED` 读作"转速"：`/AXIS@S/SPEED 主轴转速`。
- 模拟产件数与主循环计数改成 64 位，长时间运行（现场是"一直跑"）不会溢出。
- 日志同时写 `<root>/log/out.txt`（UTF-8，10 MB 轮转）与控制台（stderr，真控制台
  走 `WriteConsoleW`，中文在任何代码页下都对）。

### 修复

- **垫片 `nclshim_server_create()` 的"模型默认走内置模型"没实现**：文件头的注释一直
  这么写，但代码把 `model_json == NULL` 直接当"空模型"传给 `ncl_server_create()`，
  于是"不传模型"的设备端没有模型——采样通道、按路径应答都无从谈起（Java / Python
  绑定绕过了它：它们自己先把内置模型序列化出来再传）。现在垫片真的会把内置模型
  （`ncl_root_node_parse(NULL)`）序列化后传进去，C# / Java / Python 的行为一致。
- **`ncl_mqtt_client_disconnect()` 断开前先把套接字里在途的字节读干净**。
  之前是"发完 DISCONNECT 直接 shutdown(SD_BOTH) + closesocket"：Windows 上若关闭时
  还有没读走的接收数据（例如刚到的 SUBACK），close 会走 **RST** 而不是 FIN，对端收到
  RST 时会把它**还没读**的数据一起丢掉 —— 刚发过去的 DISCONNECT 就这样消失，broker
  只看到"连接被重置"（表现为测试里 `disconnect_count` 一直是 0，约 10~25% 偶发；
  真机上就是 broker 日志里的"客户端非正常断开"）。现在断开路径先把在途字节读掉
  （最多 8 KB、每次 recv 1 ms），再 FIN 收尾，broker 能正常读到干净的 DISCONNECT。
  实测：`mqtt_client` 套件由 40 次里 10 次失败 → **40/40 通过**，全套 22/22 连跑 5 轮通过。

### 平台与定时精度

- **短等待不再被 Windows 的时钟粒度拖住**（默认 ~15.6 ms）。`ncl_cond_wait_timeout()`
  与 `ncl_sleep_millis()` 对 ≤ 100 ms 的等待改走高精度计时器：优先 Win10 1803+ 的
  `CreateWaitableTimerEx`（`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`，精度 ~0.5 ms，
  计时器按线程持有并用 `WaitForMultipleObjects` 与"被唤醒"事件一起等），老系统上
  回退到 `timeBeginPeriod(1)`（~1.7 ms）。Windows 侧条件变量随之改成"计数信号量 +
  等人数"实现，语义不变（没有等待者时信号同样丢弃）。
  本机实测：`ncl_sleep_millis(1)` 由 14.1 ms → **1.56 ms**，
  `ncl_cond_wait_timeout(_, _, 1)` 由 15.5 ms → **1.56 ms**，`ncl_sleep_millis(100)`
  不变；示例的 `EdgeSersors`（1 ms 槽位 / 100 ms 上报）从 ~1.4 s 一条变成 ~222 ms
  一条（剩下的开销是每槽 12 次完整 Query）。Windows 链接多一个 `winmm`
  （MSVC 由源码里的 `#pragma comment` 自动带上，MinGW 需 `-lwinmm`；Go 绑定的 cgo
  LDFLAGS 已加）。

### 构建与发布

- **HTTP / REST 端点进了三份托管绑定**（`ncl_rest_attach` 早就在库里）：
  `start_http(port, with_config)` + 自定义路由（`method` 支持 `"*"`、`/api/` 开头是
  前缀匹配）+ `request_count` / `set_cors`；C# 是 `NclServer.StartHttp()` /
  `NclHttpEndpoint.Route()`，Java 是 `Server.startHttp()` / `HttpEndpoint.route()`，
  Python 是 `Server.start_http()` / `HttpEndpoint.route()`。端点内容全在库里：
  `GET /api/schema`（OpenAPI 3.0）、`GET /swagger-ui`、`POST /api/<工具>/<方法>`
  （等价于 methodCall，走 Result 信封），配置端点（SN / 模型 / 驱动 / mqtt.cfg /
  服务器列表）。
- **C# 绑定补齐设备端**：`NclServer`（工具注册 / 路径绑定 / 采样通道 / 事件推送 /
  离线 dispatch / 自研传输）+ `NclHttpEndpoint` + `Nclink.Parse()` / `NclMessage`
  （报文解码，`AsSample()` / `AsEvent()` 拿快照），`NclOperation` / `NclToolBinding` /
  `NclToolHandler` 与 Java / Python 一套语义；新增设备端示例
  `samples/Nclink.Demo.Device`（`broker` 写 `-` 就是离线：出站报文走自研传输打到
  控制台，REST 端点照常可用）与一键构建 `bindings/csharp/build.ps1`。
- **C# 自检工程** `tests/Nclink.SelfTest`（98 项，不需要 broker）：JSON / 模型 /
  报文解析 / 设备端（离线 dispatch、工具注册、采样通道、事件、自研传输、关闭语义）/
  HTTP（REST、配置端点、swagger-ui、自定义路由、错误路径、幂等关闭）；net472 与
  net8.0 两个目标都跑通。Java 自检补上 HTTP 用例（75 → 99 项），Python 32 → 36 项。
- **借用视图的 `Dispose` 改成空操作**（C#）：`NclJson` 的下标/成员视图、设备端的
  `NclServer.Model` 这类借用对象以前 `Dispose` 会把句柄置空、之后再用就抛
  `ObjectDisposedException`；现在与 Java / Python 一致——借用的东西不归你管，
  `Dispose` 什么都不做。
- Python 设备端示例的收尾计数：关闭后再读 `sample_upload_count` / `event_count` 会抛
  `ClosedException`，现在先读计数再关。
- `build.ps1 -Arch x86 -BuildDir build-x86`：新增 **32 位（Win32/x86）** 构建
  （库、示例、测试都是 x86，同一个 Ninja 工程换 `vcvars32` 即可）。
- 发布包新增 `lib/windows-x86-msvc/`、`examples/bin/{windows-x64-msvc,windows-x86-msvc,
  linux-x86_64-gcc}/`：**编好的示例可执行文件**随包交付，拿到即可跑（Windows 版是
  `/MD`，需要 VC++ 2015-2022 x64/x86 运行库；Linux 版需要 glibc 2.31+）。
- 包内同时带上语言绑定源码（`bindings/go`、`bindings/csharp`，都只放源码）。

### 文档

- 手册（`MANUAL.md` / `MANUAL.docx`）跟改：两个采样通道与各自的实测输出、按行消费、
  亚毫秒采样、设备端"一直运行 + Ctrl+C"、日志与编码说明。
- `RELEASE.md` 的包内清单、ABI 表与验证状态按本次实测更新。

## 3.0.0

首个 C 版本，零第三方依赖（仅可选的 zlib），Windows（MSVC）与 Linux（gcc）
双平台编译并跑通全部测试。

以 **MIT License** 授权（见 `LICENSE`），所有源文件带
`SPDX-License-Identifier: MIT` 头。

### MQTT 修复与互操作验证

- 新增 `tests/test_broker.c` 与 `tools/interop.sh`：对真实 broker（EMQX /
  Mosquitto，Docker 一键起）验证 QoS 0/1/2、通配订阅、40 KB 报文、退订、空闲
  保活、会话被顶替与重连后的订阅恢复。首次运行即发现并修掉下面两个问题。
- **修复：会话被顶替（0x8E）后不再自动重连**。此前两端都会立刻重连，导致同一
  clientId 的两个连接互相顶替、无限循环；现在把 0x8E 如实上报给回调并停止
  重连，由应用显式决定是否抢回身份（`ncl_mqtt_client_connect()`）。
- **修复：重连后的订阅恢复不再阻塞接收线程**。此前恢复订阅调用的是同步
  `subscribe()`，而 SUBACK 只能由接收线程处理，于是每恢复一个订阅就白等一个
  超时（10 s × 订阅数），期间报文不收、保活不发（会被 broker 以 0x8D 断开）。
  现在恢复订阅改为只发不等的异步路径，并在每次 connect 后恢复（显式重连不再
  静默丢订阅）。
- **修复：对端关闭连接立刻感知**。此前把"套接字被对端关闭"与"空闲超时"当成同
  一种情况，只能等保活看门狗（最多 2 × keepAlive）才发现掉线；现在 EOF/部分
  报文都会立即结束会话，不会在死连接上滞留，也不会因半截报文而错帧。
- **修复：服务器 DISCONNECT 立即结束会话**。协议规定服务器发完 DISCONNECT 就
  关连接，客户端不再继续读，直接进入断开/重连流程（0x8E 则按上面所述不重连）。
- `tests/test_mqtt_client.c` 的假 broker 改为可接受多次连接，并新增两个回归用例：
  「断开后自动重连并恢复订阅、且恢复过程不阻塞接收线程」与「服务器 0x8E 停止
  自动重连」——不装 Docker 也能挡住这两个问题。

### TLS（可选）

- 新增可选的 TLS 传输：`ncl_socket_connect_tls()` / `ncl_socket_tls_available()`
  与 MQTT 客户端上的 `ssl://`、`tls_ca_file`、`tls_verify_peer`、
  `tls_server_name`、`tls_client_cert/key`。默认构建仍然零依赖，
  用 `-DNCLINK_WITH_TLS=ON`（CMake）或 `NCL_WITH_TLS=1 ./build-linux.sh` 打开，
  链接 `-lssl -lcrypto`。校验链与主机名默认开启（IP 与 DNS 名都支持），
  可显式关掉用于自签调试。
- 新增 `tests/test_tls.c`（第 21 个套件，未启用 TLS 时自动跳过）：内置 TLS
  服务端，覆盖握手、CONNECT/SUBSCRIBE、8 KB 报文跨记录、服务端推送、陌生 CA
  与错误主机名必须失败、`verify_peer=false` 必须成功。
- `tools/interop.sh` 增加 Mosquitto 的 TLS 监听（18832），互操作套件再对
  `ssl://` 跑一遍（本机实测 44 检查全通过）。
- Windows 也可用：`.\build.ps1 -Tls`（或 `-DNCLINK_WITH_TLS=ON -DOPENSSL_ROOT_DIR=…`）
  自动定位 OpenSSL 3 并编译，两个平台的 TLS 构建均 21/21 通过；发布包附带
  `lib/windows-x64-msvc-tls/` 与 `lib/linux-x86_64-gcc-tls/`，Windows 版运行时
  需要 OpenSSL 3 的 DLL（`libssl-3-x64.dll`、`libcrypto-3-x64.dll`）。

### 协议与基础

- JSON DOM（有序对象、空值省略、忽略未知字段、数字保留原文）、
  字符串/容器、平台抽象（线程/互斥/条件变量/时间/熵）、日志、
  运行环境与路径（`conf/`、`bin/`、`log/`）、线程池与 TTL 缓存。
- NC-Link 常量与校验、全部主题构造与 `sn` 提取、18 种消息与 4 种消息项
  （字段顺序按规范固定）、完整数据模型（节点树、路径规则、采样通道绑定、
  映射表）、hex 与可选 zlib 编解码。
- MQTT 5.0：报文编解码逐字节对齐 OASIS 规范；传输层含 QoS 0/1/2、保活、
  自动重连与订阅恢复；TCP 套接字层（Winsock/BSD 双实现）。
- 日志的控制台镜像在 Windows 上按"真控制台 / 管道"分别解码（`WriteConsoleW`
  与本地 ANSI 代码页），中文不再乱码；`NCL_CONSOLE_ENCODING=utf8` 可强制按
  UTF-8 写（`ncl_console_write()`）。日志文件始终是 UTF-8。

### 客户端与服务端

- `ncl_client`：请求/响应关联、5 分钟响应缓存、getValue/getLength/setValue
  （含索引与区间）/probe/methodCall；`ncl_client_holder`：按 SN 的客户端表、
  30 分钟空闲过期、MQTT 连接与 FTP 端点生命周期。
- `ncl_server`：工具注册与 `<operation>#<path>` 绑定、Query/Set/MethodCall/Probe/Ping
  分发、线程池异步处理、采样通道（定时采集 + 聚合上报）、事件推送、
  参数 JSON Schema 校验（`check` 语义）、无 broker 的发布钩子。
- HTTP/REST：HTTP/1.1 基础层（解析、路由、应答、CORS，含前缀兜底路由）、
  统一应答封装、OpenAPI 3.0 生成、`/api/schema`、`/swagger-ui`、
  12 个配置接口，以及 `POST /api/<工具>/<方法>` 工具入口。
- 配置：SN、模型、驱动、服务器列表、`conf/mqtt.cfg`、`ipConf.json` 的读写。

### 文件传输

- 自带 FTP 服务端与客户端（RFC 959/2389 子集：主动/被动数据连接、
  STOR/RETR/LIST/MKD/RMD/DELE/RNFR/SIZE/MDTM 等），登录根隔离。
- 文件属性与工具函数（SHA-256 校验和、分片数、压缩判定）、
  FTP 文件工具两端、`file` 工具（`/CONTROLLER/FILE`）、
  methodCall 的 `@file` 标记与 `fileKeys` 替换；设备端与客户端两个 FTP 端点。

### 校验与事件

- JSON Schema 校验器（draft-07 子集，含自带正则引擎），用于 methodCall 的
  参数校验与 `check` 快速失败。
- 事件：`ncl_server_push_event()` 发布 + `ncl_client_subscribe_events()` /
  `ncl_client_set_event_handler()` 接收（主题 `Event/<sn>`）。
- 采样：客户端订阅 `Sample/<sn>/#` 与回调；**亚毫秒采样**支持"值本身是数组"
  （外层 1 ms 槽位、内层批次），报文不加字段；发布前校验**内外层都要对齐**，
  不完整即丢弃。
- 采样补齐：设备端省掉表头 `paths`、或用 `[]` 给"本周期该项没有数据"占位时，
  客户端先用设备模型补回规范形状再交给回调（`ncl_message_sample_normalise()`，
  见 4.5）；通道查不到、项数对不上、形状统一不了就原样交过去，绝不猜对应关系。

### 文档与工程

- `MANUAL.md` / `MANUAL.docx` 使用手册（构建、核心概念、逐模块 API、
  典型任务、排错表、API 索引）、`README.md` 工程说明、`RELEASE.md` 发布包说明。
- `examples/`：设备端与客户端两个可运行示例（模型、工具与 schema、采样、
  事件、文件、参数校验全覆盖）。
- 设备端示例可以指向一个**空目录**首次启动：目录不存在就建，缺什么按出厂默认值
  补齐 —— 随机 9 位 SN（`bin/sn.txt`）、默认机床模型（`conf/model/nclink.json`）、
  本机 broker 配置（`conf/mqtt.cfg`，`tcp://127.0.0.1:1883` 匿名）；已存在的
  文件一律不动，换模型/换 broker 直接改文件即可（见手册 3.4）。
- `tests/`：19 个测试套件（含假 MQTT broker 与许可头检查），Windows ctest 与
  Linux `build-linux.sh` 均 19/19；ASan 构建全绿。
- `tools/`：markdown → docx 转换与校验、发布打包脚本。

### 未实现部分

HTTP multipart、驱动层 Modbus/串口、`Edge/*` 主题、`ssl://`（TLS）暂未提供，
调用时会返回明确错误码。
