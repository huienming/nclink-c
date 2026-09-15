# 变更记录

本文件记录 NC-Link C 实现（`nclink-core-c`）的版本变更。版本号跟随
NC-Link 规范版本：**3.0.0** 对应 GB/T 41970-2022 协议 3.0.0。

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

### 协议与基础

- JSON DOM（有序对象、空值省略、忽略未知字段、数字保留原文）、
  字符串/容器、平台抽象（线程/互斥/条件变量/时间/熵）、日志、
  运行环境与路径（`conf/`、`bin/`、`log/`）、线程池与 TTL 缓存。
- NC-Link 常量与校验、全部主题构造与 `sn` 提取、18 种消息与 4 种消息项
  （字段顺序按规范固定）、完整数据模型（节点树、路径规则、采样通道绑定、
  映射表）、hex 与可选 zlib 编解码。
- MQTT 5.0：报文编解码逐字节对齐 OASIS 规范；传输层含 QoS 0/1/2、保活、
  自动重连与订阅恢复；TCP 套接字层（Winsock/BSD 双实现）。

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

### 文档与工程

- `MANUAL.md` / `MANUAL.docx` 使用手册（构建、核心概念、逐模块 API、
  典型任务、排错表、API 索引）、`README.md` 工程说明、`RELEASE.md` 发布包说明。
- `examples/`：设备端与客户端两个可运行示例（模型、工具与 schema、采样、
  事件、文件、参数校验全覆盖）。
- `tests/`：19 个测试套件（含假 MQTT broker 与许可头检查），Windows ctest 与
  Linux `build-linux.sh` 均 19/19；ASan 构建全绿。
- `tools/`：markdown → docx 转换与校验、发布打包脚本。

### 未实现部分

HTTP multipart、驱动层 Modbus/串口、`Edge/*` 主题、`ssl://`（TLS）暂未提供，
调用时会返回明确错误码。
