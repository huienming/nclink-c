# 适配器（厂商协议驱动）

把设备自带的通信协议接进 NC-Link 的一层。核心库（`src/`）只认 NC-Link 主题与
消息；适配器负责另一侧——按厂商协议跟机床/PLC/机器人说话，把读到的数据挂到
设备模型上，把方法调用转成协议命令。

协议规格书在 [`../protocal/docs/`](../protocal/docs/)：先读
`00-通用-实现约定.md`（统一地址模型、错误分级、重连、批量合并、审计），
再读具体协议分册。编码顺序见 `protocal/docs/README.md` 的优先级排序。

## 目录

```
adapters/
├── include/nclink_adapter/   # 对外接口（驱动实现者要 include 的头）
│   ├── ncl_driver.h          # ncl_driver_ops / ncl_address / ncl_driver_result
│   ├── ncl_driver_manager.h  # 驱动配置、点位表、path→驱动 分派
│   └── ncl_adapter.h         # 守护进程：配置 → 设备
├── src/core/                 # 与协议无关的骨架（类型、错误分级、地址解析、注册表）
├── src/registry/             # 配置加载 + 点位表 + 最长前缀分派
├── src/app/                  # 守护进程主体（模型生成、操作注册、轮询）
├── src/main.c                # ncl_adapter 可执行文件
├── drivers/<协议>/           # 每个协议一个目录：帧构造/解析 + 会话状态
└── tests/                    # 黄金报文 + mock 靶机的集成测试
```

构建产物是 `libnclink_drivers.a`（CMake 目标 `nclink::drivers`）与可执行文件
`ncl_adapter`，与 `nclink::core` 分开：设备端不带任何厂商驱动时可以直接不编译
这一层（`-DNCLINK_BUILD_ADAPTERS=OFF`）。

## 驱动接口

一个驱动 = 一张 `ncl_driver_ops` 表：

| 回调 | 语义 |
|---|---|
| `protocol` | 配置里写的协议名（`"mock"`、`"modbus_tcp"`…） |
| `create` | 用配置的 `parameters` 对象初始化 |
| `open` / `close` / `is_connected` | 会话生命周期（连接 + 握手 / 断开） |
| `read_batch` / `write_batch` | 统一地址（`area` + `offset` + `bit` + `length` + `dtype`）的批量读写 |
| `read_raw` / `write_raw` | 逃生舱：直发原始报文（诊断、厂商私有命令） |
| `call` | 方法类操作（启程序、MDI、刀补…），对应 NC-Link 的 Method 调用 |
| `attach_event` | 报警、程序结束一类的事件回调 |
| `destroy` | 释放私有状态与驱动结构本身 |

三条共同的约定：

1. **统一地址模型**：`ncl_address` 只有 `area / offset / bit / length / dtype`
   五个字段，七种类型 `bit/byte/int16/int32/float32/float64/string`。协议里的
   怪类型（MELDAS 的 10 字节 double、FOCAS 的 4 字节串）在驱动内部消化。
2. **三类错误分开**：`NCL_DRV_ERR_TRANSPORT`（超时、断开 → 重连）/
   `NCL_DRV_ERR_PROTOCOL`（协议错误码 → 按码表映射）/
   `NCL_DRV_ERR_BUSINESS`（数据无效 → 上报，不改协议状态）。调用方用
   `ncl_driver_error_tier()` 判断该不该重连。
3. **会话按需打开**：`ncl_driver_read_one()` / `ncl_driver_write_one()` 会在
   未连接时先 `open()`。实现了 `open` 的驱动必须同时实现 `is_connected`。

响应统一走 `ncl_driver_result`：`code / success / value / message / raw`，
其中 `raw` 是原始应答字节，供审计与排障（`00-通用-实现约定.md` §6）。

## 写一个新驱动

1. 建 `drivers/<协议>/`，实现 `ncl_driver_ops` 里的回调。
2. 提供一个工厂 `ncl_driver *ncl_<协议>_create(void)`，头文件放在同目录。
3. 在 `src/core/driver.c` 的 `ncl_driver_register_builtin()` 里注册；
   第三方驱动也可以自己调用 `ncl_driver_register_protocol()` 挂进注册表。
4. 在 `tests/` 里加：报文构造/解析的黄金样本（真实抓包做 case）＋ 用
   `mock` 或自建靶机跑一遍连接与读写。

`drivers/mock/` 是最小样板：内存点位模型 + 错误注入 + 事件触发，没有一行
网络代码，测试里可以直接用（`ncl_mock_driver_create()`）。

## 已实现的协议

| 协议 | 注册名 | 传输 | 点位地址写法 | 状态 |
|---|---|---|---|---|
| 内存靶机 | `mock` | 无 | 任意 `area` + 偏移 | ✅ 完成 |
| Modbus TCP | `modbus_tcp` | TCP 502 | `coil`/`discrete`/`input`/`holding`，或 `0x`/`1x`/`3x`/`4x` | ✅ 完成 |
| Modbus RTU | `modbus_rtu` | RS-485/232 | 同上 | ✅ 完成 |
| RTU over TCP | `modbus_rtu_tcp` | TCP 任意 | 同上 | ✅ 完成 |
| 三菱 MC/SLMP | `mc_tcp` | TCP 5534 | 设备名 + 号：`D100`、`M10`、`M10.3`（字软元件的位） | ✅ 二进制 3E/4E |
| 欧姆龙 FINS | `fins_tcp` | TCP 9600 | 区名 + 字：`D100`、`CIO12.3`、`E0_0`… | ✅ 内存区读写 |
| 西门子 S7comm | `s7_tcp` | TCP 102 | 区名 + 字节偏移：`M10.3`、`DB1`、`MB10` | ✅ ISO-TSAP + COTP |
| MTConnect | `mtconnect` | HTTP 7878 | **数据项 id 就是区名**：`{"area":"Xabs","offset":0}` | ✅ 只读 |
| 三菱 CNC M70/M80 | `meldas` | TCP 683 | **命令名就是区名**，偏移是轴号/IO 地址 | ✅ 只读 |
| 海德汉 LSV2 | `lsv2` | TCP 19000 | **区名是要读的东西**，偏移是地址 | ✅ 版本/状态/PLC 内存 |

其余协议按 `protocal/docs/README.md` 的优先级推进
（第一批 MC/SLMP → FINS → S7 → MTConnect 已完成；第二批 MELDAS → 新代 → LSV2
→ FOCAS 进行中）。

**暂缓/不做的，以及原因**（避免以后重复踩）：

| 协议 | 结论 | 依据 |
|---|---|---|
| 新代 SYNTEC RemoteCNC（10 册） | **暂缓**：实现不了裸协议 | 10 册 §3 明确写"本协议为 .NET 对象 API，非裸字节协议"，且"如需自实现裸协议：抓包…尚未逆向"。控制器同时支持 Modbus 主机（`MODBUS_FC01~FC16`），那部分用本仓库已有的 `modbus_tcp` 就能覆盖 |
| Modbus ASCII（15 册） | 暂缓 | 15 册只给了 `:` + 十六进制 + LRC + CRLF 的轮廓，没有字节级样本；等一次抓包 |
| MC 的 ASCII 编码、FINS/UDP、S7 的 UDP | 暂缓 | 核心 socket 层目前只有 TCP（UDP 要加一层原语），ASCII 缺原始样本 |
| 科德/精雕/海康（20 册） | 不做 | 规格书标 🔴 缺，无可用资料 |

### Modbus

```json
{
  "id": "plc1", "path": "/PLC1", "type": "modbus_tcp",
  "parameters": {
    "host": "10.0.0.5", "port": 502, "unit": 1,
    "timeoutMs": 1000, "retries": 1, "wordOrder": "CDAB", "base1": false
  },
  "points": [
    { "path": "/PLC1/TEMP",  "addr": "4x12" },
    { "path": "/PLC1/SPEED", "addr": {"area":"holding","offset":30,
                                      "dtype":"float32"}, "writable": true },
    { "path": "/PLC1/READY", "addr": "0x3" },
    { "path": "/PLC1/NAME",  "addr": {"area":"holding","offset":40},
      "dtype": "string", "length": 8 }
  ]
}
```

RTU 用 `"serial": "COM3"` / `"/dev/ttyUSB0"` 加 `baud`/`parity`/`dataBits`/
`stopBits`/`interFrameMs`（3.5 字符静默的近似），RTU over TCP 用 `host`/`port`
但保留 RTU 的 CRC 帧（串口服务器场景）。

实现上遵循 15 册：单次 ≤125 寄存器（位区 2000），相邻点位间隔 ≤ `mergeGap`
（默认 8）合并成一次请求，传输层失败按 `retries` 重发（RTU 默认 2 次、TCP 1 次），
异常码按 §6 分级（非法地址/功能码 → 协议层；从站忙/确认 → 业务层不重连），
多寄存器数值按 `wordOrder` 的 ABCD/CDAB/BADC/DCBA 组装，1x/3x 区写操作直接拒绝。
所有交换串行化（485 总线半双工，TCP 设备也不希望两个请求交叉）。
`loopback` 方法对应诊断功能 0x08/0x0000，用来判断"线还活着"。
原始报文逃生舱收发的是 PDU（功能码 + 数据）。

### 三菱 MC / SLMP

```json
{
  "id": "plc1", "path": "/PLC1", "type": "mc_tcp",
  "parameters": { "host": "10.0.0.5", "port": 5534, "frame": "3e",
                  "timeoutMs": 1000, "network": 0, "plc": 255, "station": 0 },
  "points": [
    { "path": "/PLC1/TEMP",  "addr": "D100" },
    { "path": "/PLC1/READY", "addr": "M10" },
    { "path": "/PLC1/FLAG",  "addr": "D100.3" },
    { "path": "/PLC1/SPEED", "addr": {"area":"D","offset":200,
                                      "dtype":"float32"}, "writable": true }
  ]
}
```

二进制 3E（Q/L/FX）与 4E（iQ-R，多一个序列号字段）都实现了；命令码覆盖成批读
0x0401、成批写 0x1401（字/位子命令都走），方法有 `loopback`（0x0619，适合做保活）、
`remoteRun`/`remoteStop`/`clearError`/`cpuType`/`cpuStatus`。数据是小端：
32 位值低字在前，字符串每字两个字符、低字节在前。位软元件按"一点一字节"
（0x00/0x01）传输；**字软元件的位**按 `号 × 16 + 位` 编码（`D100.3` → 1603），
这正是 06 册 §8.2 的那条坑。单次读 ≤960 字，超了分片；相邻点位间隔 ≤ `mergeGap`
（默认 8）合并成一次请求。设备返回的结束代码按 §7 分级映射（地址越界/命令未找到
→ 协议层；CPU 错误、远程 RUN/STOP 未受理 → 业务层）。

**还没做**：ASCII 编码（06 册里没有字节级原始样本，等一次抓包再补）、UDP
（核心的 socket 层目前只有 TCP）。端点字节序转换等长尾项也留到实机验证时再定。

### 欧姆龙 FINS

```json
{
  "id": "plc1", "path": "/PLC1", "type": "fins_tcp",
  "parameters": { "host": "10.0.0.5", "port": 9600, "clientNode": 1,
                  "timeoutMs": 1000 },
  "points": [
    { "path": "/PLC1/TEMP",  "addr": "D100" },
    { "path": "/PLC1/READY", "addr": {"area":"CIO","offset":12,"dtype":"bit"} },
    { "path": "/PLC1/FLAG",  "addr": "D100.3" },
    { "path": "/PLC1/SPEED", "addr": {"area":"D","offset":200,
                                      "dtype":"float32"}, "writable": true }
  ]
}
```

FINS/TCP 会**先做节点地址分配握手**（§2：`FINS` 头 + 命令 0x00000000，PLC 回
分配到的客户端节点号与自己的节点号），之后每个请求都是"传输头 + FINS 帧"。
命令覆盖内存区读 `01 01`、写 `01 02`，方法有 `run`/`stop`/`controllerStatus`/
`readClock`/`cycleTime`；区码含 CIO/W/H/A/D/P/C/T/TS/CS/CF/IR/DR/TK 与 EM 库
`E0_0`–`E0_15`、`E1_0`–`E1_15`（§4，别按 0x90 硬编码）。全部大端；位访问时
"位号"字节填实际位号（字访问填 0x00），位读回一点一字节。单次 ≤999 字，
相邻点位间隔 ≤ `mergeGap`（默认 8）合并。结束码分级映射：重发超限/超时
（0x03/0x04）算传输层（重连重发），读写不可能/越界（0x20/0x21/0x24）算协议层，
CPU 忙或被拒（0xA5 等）算业务层。

**还没做**：FINS/UDP、HostLink/C-Mode；`run`/`stop` 的两个参数字节按 "00 00"
发出，实机若要求运行模式需再调。

### 西门子 S7comm

```json
{
  "id": "plc1", "path": "/PLC1", "type": "s7_tcp",
  "parameters": { "host": "10.0.0.5", "port": 102, "rack": 0, "slot": 2,
                  "timeoutMs": 1000, "pduSize": 960 },
  "points": [
    { "path": "/PLC1/READY", "addr": "M10.0" },
    { "path": "/PLC1/BYTE",  "addr": "MB10" },
    { "path": "/PLC1/WORD",  "addr": "MW20" },
    { "path": "/PLC1/REAL",  "addr": {"area":"DB1","offset":0,
                                      "dtype":"float32"}, "writable": true },
    { "path": "/PLC1/NAME",  "addr": {"area":"DB1","offset":40},
      "dtype": "string", "length": 8 }
  ]
}
```

三层握手都实现了：TCP → COTP 连接请求/确认（TSAP = 0x0300 + rack×0x20 + slot，
所以 S7-300/400 是 `slot:2`、S7-1200/1500 是 `slot:1`）→ Setup Communication
协商 PDU 尺寸（默认要 960，取设备给的上限）。之后每个请求都是 TPKT + COTP DT +
S7 PDU，PDU 引用回显校验；协商出的 PDU 尺寸决定一次 Read Var 能带多少个 Item
（本项目里一个批读就是**一次请求**，不是一点一次）。

区名可写 `I`/`Q`/`M`/`T`/`C` 或 `DB1`（DB 号写在区名里），也可以写带宽度后缀的
`MB`/`MW`/`MD`/`IB`/`QW`… —— 后缀同时定下读写类型（B 字节、W 字、D 双字、X 位）。
数据大端；REAL 是 IEEE754 大端；字符串按 S7 格式带两个头字节（最大长度/当前长度）。
地址是"字节偏移 × 8 + 位号"。

**实机前记得**：S7-1200/1500 默认禁止 PUT/GET（TIA 里要勾"允许来自远程对象的
PUT/GET 通信访问"），DB 关了"优化块访问"才能按绝对地址读 —— 03 册 §7.1/§7.2 说的
那两条，90% 的"连得上读不到"都是它们。

### MTConnect

```json
{
  "id": "cnc", "path": "/CNC", "type": "mtconnect",
  "parameters": { "host": "10.0.0.9", "port": 7878, "timeoutMs": 3000 },
  "points": [
    { "path": "/CNC/X",    "addr": {"area":"Xabs","offset":0,"dtype":"float64"} },
    { "path": "/CNC/EXEC", "addr": {"area":"exec","offset":0,"dtype":"string"} },
    { "path": "/CNC/ALARM","addr": {"area":"alarm","offset":0,"dtype":"string"} }
  ]
}
```

MTConnect 是**只读**标准接口（§1）：写操作返回"不支持"。点位用数据项的 id 寻址，
所以统一地址模型里**区名就是 dataItemId**，偏移恒为 0（id 只含字母时可以写成
字符串简写，含数字的用对象形式）。

实现：会话建立时读一次 `/probe`（拿到数据项表，于是每个点位的 category 是"知道"
而不是"猜"），每次读发一个 `GET /current`；`/probe` 不可用的 agent 也能用（category
退化为按元素名判断，且只问一次）。HTTP 客户端支持 `Content-Length`、`chunked` 与
"读到连接关闭"三种正文形态，可带 Basic 认证。`UNAVAILABLE` 的点位返回 JSON null
（§6.1：不要假设每帧都有全部点位，缺项也是 null）；CONDITION 变成 Fault/Warning 时
**推一次事件**（记住上次状态，轮询不重复报）。方法有 `probe`（数据项表）与
`sequence`（最后一次读到的 sequence）；原始报文逃生舱的入参是 HTTP 路径，返回文档
本身。

**还没做**：流式 `/sample`（§5 推荐用于高频）与 `/assets`；`/current` 的按行
增量缓存（现在每读一次就全量解析一次，点位多时值得加）。

### 三菱 CNC M70/M80（MELDAS/GIOP）

```json
{
  "id": "cnc", "path": "/CNC", "type": "meldas",
  "parameters": { "host": "192.168.0.10", "port": 683, "axisMode": "bit",
                  "timeoutMs": 1000 },
  "points": [
    { "path": "/CNC/XABS", "addr": {"area":"machine_position","offset":1,
                                    "dtype":"float64"} },
    { "path": "/CNC/LOAD", "addr": {"area":"spindle_load","offset":0,
                                    "dtype":"int32"} },
    { "path": "/CNC/PART", "addr": {"area":"part_count","offset":0,
                                    "dtype":"int32"} },
    { "path": "/CNC/MODE", "addr": {"area":"work_mode","offset":0,
                                    "dtype":"byte"} }
  ]
}
```

机床侧要先开网：`#1925=1`（网络使能）、`#1926/1927/1928` 是 IP/掩码/网关、
`#1929` 是端口（改完要重启网络）——05 册 §1，这是"连不上"的第一嫌疑。

MELDAS 是 **CORBA GIOP 1.0 之上的私有 mocha 操作集**：请求固定 80 字节、全程
小端（GIOP flags=0x01）、响应按请求 ID 匹配，无握手。§8 那几条坑都落在代码里：
长度字段是"总长 − 12"、操作名 13 字节（含结尾 NUL，且这个 NUL 同时是紧随其后的
`00 00 00 03` 的首字节）、坐标是 CString 且长度在 `[36]`、正文从 `[40]` 起。

点位把"要什么数据"和"从哪个轴要"分开写：**区名是命令**（§4 的名字表，或
`"0x3b/0x7f"` 这样的原始命令/子码），**偏移是轴号或 IO 地址**。坐标类命令的
轴号按 C# 交付实测的位编码（X=1、Y=2、Z=4，第 4 轴 8），`"axisMode":"index"`
可切换成 1..n 的序号制（05 册 §5 明确两种都存在，以机床实测为准）。应答按
自己的类型标记解码（BYTE/INT16/INT32/DOUBLE/CString），文本形式的坐标会自动
转成点位声明的数值类型；"IDL" 应答表示该操作没有数据，点位读成 JSON null。

`0x03` 那一族是"通用设备数据"命令，语义完全由子码决定，所以表里把
`(命令, 子码)` 成对登记（§8.4）。`raw` 逃生舱收五个小端 32 位字段
（命令、子码、数量、地址、期望类型），用来在真机上试一条文档里没有的命令。

**只读**：`mochaSetData` 在交付材料里没有抓到帧格式，§8.7 也要求生产环境默认
禁用写，所以写操作返回"不支持"，等一次抓包再补。**未验证**：10 字节扩展
double（`0x06`）按 x87 布局解析，需要实机确认；坐标建议用 CString 形式读。

### 海德汉 HEIDENHAIN（LSV2）

```json
{
  "id": "tnc", "path": "/TNC", "type": "lsv2",
  "parameters": { "host": "10.0.0.20", "port": 19000, "user": "INSPECT",
                  "timeoutMs": 3000 },
  "points": [
    { "path": "/TNC/VER",  "addr": {"area":"version","offset":0,
                                    "dtype":"string"} },
    { "path": "/TNC/ST",   "addr": {"area":"remote_status","offset":0,
                                    "dtype":"string"} },
    { "path": "/TNC/M100", "addr": {"area":"plc_memory","offset":100,
                                    "dtype":"int16"} }
  ]
}
```

帧格式是本项目里最友好的一个：`[4 字节大端 payload 长度][4 字符命令名][payload]`，
请求与应答同形，所以一个构造函数一个拆分函数就够。§7 的坑都在代码里：长度是
大端（与 PLC 类协议相反）、payload 里的字符串含结尾 NUL 且长度算上它、大文件靠
`S_FL` 分块循环。

会话按 §7.4 建：连上先 `R_VR` 问型号（成功即证明链路可用），配置了登录名再发
`A_LG` + 用户名 + NUL（可选口令）。登录名只接受 §4.1 的分级名单
（INSPECT/FILE/MONITOR/DIAGNOSTICS/PLCDEBUG），写错了是配置错误而不是发出去试。
已实现的能力：版本（R_VR/S_VR）、远程状态保活（R_ST/S_ST，`keepAlive` 方法）、
**PLC 内存读**（R_MB：4 字节地址 + 1 字节长度）、登录状态查询、`A_LO` 登出，
以及 raw 逃生舱（4 字符命令 + payload → 原样返回应答）。PLC 内存的字节数由点位
类型与长度决定（`int16` 读 2 字节、`"abc"` 读 3 字节）。

**只读**：§7.6 明确 `C_EK`（模拟按键）与 `C_MC`（改机器参数）要挡在权限墙后，
而抓包材料里没有它们的 payload，所以写操作返回"不支持"。**没做**：`R_RI`
（采集主命令）的 16 位选择码与 `S_RI` 的布局在 07 册里没有列出，需要一次抓包；
PLC 内存的大端解释同样待实机确认。

## 配置与守护进程

一条「链路」= 一个驱动实例。配置文件（单个文件、目录里的多个 `*.json`，或
直接内嵌 `drivers` 数组都行）：

```json
{
  "id": "plc1",                     // 链路 id（唯一）
  "path": "/PLC1",                  // 这条链路负责的模型路径前缀；"/" 为兜底
  "type": "modbus_tcp",             // 驱动注册表里的协议名
  "parameters": { "host": "10.0.0.5", "port": 502, "unit": 1 },
  "points": [
    { "path": "/PLC1/STATUS", "addr": "D100" },
    { "id": "POWER", "addr": {"area": "D", "offset": 101},
      "dtype": "float32", "writable": true }
  ]
}
```

- 点位路径可写绝对路径（`/PLC1/STATUS`）或相对路径（`STATUS`），内部统一
  按「链路前缀之后的相对路径」存表；查找时先取最长前缀，再回退到 `"/"`
  那条兜底链路（`/PLC10` 不会被 `/PLC1` 抢走）。
- `writable` 默认 **false**（§7：默认只读，写能力要显式开）；`sample`
  默认 true，设 false 可把高频/大流量点位排除在采样通道之外。

适配器的配置：

```json
{
  "sn": "V2AABBCCDD1",
  "driverDir": "conf/driver",        // 或 "driverFile"，或内嵌 "drivers"
  "model": "conf/model.json",        // 可选；不给就按点位表生成
  "device": { "type": "MACHINE", "id": "01", "name": "数控机床" },
  "methods": [ { "path": "/PLC1/START", "operation": "startProgram" } ],
  "sample": { "intervalMs": 1000, "uploadMs": 1000 }
}
```

守护进程为每个点位注册一条操作：`get_value#<路径>` 读、`set_value#<路径>` 写
（只有 `writable` 的点位才注册），`call#<路径>` 走驱动自己的方法。核心的采样
任务发的就是普通 Query，所以采样通道不需要额外机制。

```sh
./ncl_adapter -c conf/adapter.json            # MQTT + REST + 轮询
./ncl_adapter -c conf/adapter.json --once     # 读一遍全部点位就退出（自检）
```

自动生成的模型里，每个数据项的 `source` 就是点位路径的父级，因此
**模型路径与配置里的点位路径严格一致**，配置和模型不会漂移。
可运行的配置样例见 `tests/test_adapter.c` 里的 `kConfig`。

## 测试

```sh
.\build.ps1                      # Windows：配置 + 编译 + 28 个测试套件
sh build-linux.sh build-linux    # Linux：同样全跑一遍
```

适配器层的测试三件套：`tests/test_driver.c`（驱动接口：注册表、地址解析、
错误分级、mock 的读写/位寻址/批量/事件/原始报文）、
`tests/test_driver_manager.c`（配置加载、点位表、前缀分派）、
`tests/test_adapter.c`（配置 → 设备：生成模型、操作、读写、方法、轮询）。
协议驱动的测试以两段为主：报文级的黄金样本（字节级 diff），以及对着 mock
靶机的连接—读写—重连流程。
