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
├── include/nclink_adapter/   # 宿主侧接口（模块作者看的是 include/nclink/ 下的头）
│   ├── ncl_driver.h          # ncl_driver_ops / ncl_address / ncl_driver_result
│   ├── ncl_audit.h           # 审计（§6）：请求/会话/写操作/错误直方图
│   ├── ncl_adapter.h         # 宿主：配置 → 设备（含 MQTT 会话）
│   └── ncl_module.h          # 模块 ABI 与装载器（两代，见下）
├── src/core/                 # 与协议无关的骨架（驱动接口、地址解析、模块装载、审计）
├── src/app/                  # 宿主主体（声明/点表 → 模型、操作注册、轮询、MQTT、采样）
├── src/main.c                # ncl_adapter 可执行文件（一台 NC-Link 设备程序）
├── plugins/<适配器>.c        # 适配器本体：一个文件一个 tool，编成 plugins/ncl_driver_<名字>.dll|.so
└── tests/                    # 装载器、宿主、黄金报文与 mock 靶机的集成测试
```

协议客户端（帧构造/解析 + 会话状态）在 `clients/<协议>/`，公开头是
`clients/include/nclink/clients/<协议>.h`；`adapters/` 只管宿主与模块装载。

## 写一个适配器：一个文件（推荐）

适配器作者只看一个头 —— `nclink/ncl_tool.h` —— 在 **`adapters/plugins/`** 下写一个
`.c` 文件：连接开一次，一个 dispatch 服务全部点位，最后一行交出模块入口。文件放进那个
目录就会被编成 `plugins/ncl_driver_<文件名>.dll|.so`（`file(GLOB adapters/plugins/*.c)`），
**不用改 CMake**——这是"一个文件一个适配器"的另一半。协议字节不进这个文件：它在
`clients/<协议>/` 里，适配器只调用它。

```c
#include "nclink/ncl_tool.h"

static void *open_box(const ncl_json *params, char **err) { /* 连接开一次 */ }
static void  close_box(void *ctx) { }
static ncl_err dispatch(void *ctx, const ncl_tool_point *self, ncl_operation op,
                        const ncl_json *params, ncl_json **result, char **reason) { }

NCL_TOOL_BEGIN("mybox", "某品牌机床（只读）", 1000, 1000, open_box, close_box)
    NCL_POINT_SAMPLED_ARG("/BOX/RUN", dispatch, &k_run)    /* 可读 + 进采样通道 */
    NCL_POINT_ARG("/BOX/NAME", dispatch, &k_name)          /* 只按需读 */
    NCL_POINT_RW_ARG("/BOX/MODE", dispatch, &k_mode)       /* 可读可写 */
    NCL_METHOD_NAMED("/BOX/RESET", dispatch, &k_reset, "RESET")
NCL_TOOL_END()

NCL_TOOL_MODULE("1.0.0", "某品牌机床适配器")
```

宿主拿这份声明生成模型、OpenAPI schema 与绑定：`path` 就是模型路径，采样通道取
`NCL_TOOL_BEGIN` 的周期，方法调用地址是 `<tool 名>/<点位名>`（点位名默认取路径尾段；
同一条路径下重名时用 `*_NAMED` 宏显式给名字）。`sampled` 只要求点位可读，周期给 0
就表示"这个声明不生成采样通道"（现场仍可在模型文件里加、调）。

**哪些点位进默认采样通道，是声明说了算**：`NCL_POINT_SAMPLED_*` 的点位进通道，
`NCL_POINT_*`（不带 SAMPLED）只按需读。现场口径常常是"只报状态、计件、程序名、报警"
这类少量点位，那就只把那几行写成 `*_SAMPLED_*`，别的保持按需读 —— 改一行、重编模块即可。

**已经定下来、但协议调用还没抓到帧的点位**用 `NCL_POINT_PENDING[_SAMPLED](路径, 理由)`
声明（重名的用 `NCL_POINT_PENDING_NAMED(路径, 名字, 理由)`）：它在模型里看得见，
客户端 `Query` 它会拿到"还读不了 + 理由"（不是"没有这个点位"），宿主在自检里把它报成
`<待抓包>` 而不是失败、在轮询里直接跳过。`*_PENDING_SAMPLED` 会占住采样通道的位置
（抓包补上之前那一列是 `null`），所以现场一开始就看得见"报警这一列将来会有"。
抓包补上以后，把那一行换成普通宏、别的什么都不用改。

参数有三条通道，别混：

| 参数 | 从哪来 |
|---|---|
| 配置里的 `parameters`（IP/端口/超时/unit…） | `open(params, err)` 拿一次，返回的 ctx 传给每个点位 |
| 客户端的请求参数（Query 的 params、Set 的 `"value"`、方法调用的 arguments） | handler 的 `params` |
| **点位自己的数据**（寄存器地址、映射表条目、协议项名） | 声明时挂在点位上，handler 从 `self->arg` 取 |

配置里对应的一段只有参数，点位不在这里：

```json
{ "tools": [ { "name": "mybox", "parameters": { "host": "10.0.0.5" } } ] }
```

模块文件按 `ncl_driver_<tool 名>.dll`（Linux/macOS 是 `libncl_driver_<名字>.so`）放进
`plugins/` 即可。现场三条命令：`ncl_adapter --plugins`（列工具与点位/方法个数）、
`--probe <路径>`（单点试读）、`--once`（跑一遍自检）；`--model` 把这份声明生成的
**设备模型 JSON** 打出来（设备对外发布的就是它，要现场调采样周期就存成文件、在配置里
用 `"model"` 指过去）。可抄的样板：
`adapters/plugins/focas.c`（FANUC，30 个点位 + 2 个方法，其中 6 个"待抓包"）、
`adapters/tests/module_tool_basic.c`（最小夹具）。

## 驱动接口（内部一层：协议客户端，以及老式驱动模块）

构建产物是 `libnclink_drivers.a`（CMake 目标 `nclink::drivers`）、宿主可执行文件
`ncl_adapter`，以及插件目录里的适配器模块（`plugins/ncl_driver_<协议>.dll|.so`）：
厂商协议默认编成**可动态装载的模块**（`-DNCLINK_BUILD_PLUGINS=OFF` 可以关掉，
把它们放回内置注册表，做成一个自包含的可执行文件）。驱动层与 `nclink::core` 分开：
设备端不带任何厂商驱动时可以直接不编译这一层（`-DNCLINK_BUILD_ADAPTERS=OFF`）。

### 一个驱动 = 一张 ncl_driver_ops 表

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
| `last_raw` | 最近一次交换的请求/应答字节，给审计用（可选，见 §6） |

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

## 审计（§6）

`00-通用-实现约定.md` §6 要四样东西，`src/core/audit.c` 一次给全。**记账点是
`ncl_audit_request()` / `ncl_audit_write()` / `ncl_audit_session()`**：谁读写了
协议，就在那里调用一次（适配器宿主不再有一层点表替你记——见下方"已知缺口"）：

| §6 要求 | 实现 |
|---|---|
| 会话生命周期 | `ncl_audit_session()`：`open_all` / `close_all` 各记一条，原因随行 |
| 每次请求的原始报文 + 耗时 + 响应码 | `ncl_audit_request()`：一行一条（hex 只在打开 `raw` 时打） |
| 协议层错误码直方图 | `note_code()`：按 tier 与具体码计数，`ncl_audit_stats()` 出 JSON |
| 写操作全量审计 | `ncl_audit_write()`：改前先读旧值，记「路径、地址、旧值、新值、操作者」 |

```c
ncl_audit_options options;

ncl_audit_options_default(&options);      /* enabled=true, raw=false */
options.raw = true;                       /* §6：报文 hex 按需采样，别长开 */
options.operator_name = "commissioning";  /* 写审计里的「操作者」 */
ncl_audit_init(&options);
...
ncl_json *stats = ncl_audit_stats();       /* 计数、直方图、最近 8 条写操作 */
```

- 日志走库内 logger，落在 `<root>/log/out.txt`，和其他日志同一份；写操作恒为
  `INFO`（§6 要求写操作必须留痕），请求行按结果分 `DEBUG` / `WARNING`。
- `raw` 打开时，驱动通过可选的 `last_raw` 回调把最近一次交换的请求/应答字节
  交出来（Modbus/MC/FINS/S7/MELDAS/LSV2/SYNTEC 都实现；mock 与 MTConnect
  没有帧，日志里就没有字节）。只显示前 96 字节，超出打 `...`。
- 「写能力必须显式开启」（§7）由声明把关：点位不声明 `writable` 就不注册
  `set_value` 操作（`ncl_tool_register()` 只注册声明过的操作）✓
- **声明式适配器的账由宿主记**：`ncl_tool_register()` 收一个 `ncl_tool_audit *`，
  core 里的 shim 在每次点位调用前后把「路径、操作、结果、耗时」交给它，写操作还会先
  经点位读一次旧值。模块唯一要做的是**可选**地交出原始帧：`NCL_TOOL_END_WITH_RAW(fn)`
  一行（见 `adapters/plugins/focas.c`）。适配器作者不写任何审计代码。

## 写一个新驱动

1. 建 `drivers/<协议>/`，实现 `ncl_driver_ops` 里的回调。
2. 提供一个工厂 `ncl_driver *ncl_<协议>_create(void)`，头文件放在同目录。
3. 在 `src/core/driver.c` 的 `ncl_driver_register_builtin()` 里注册；
   第三方驱动也可以自己调用 `ncl_driver_register_protocol()` 挂进注册表。
4. 在 `tests/` 里加：报文构造/解析的黄金样本（真实抓包做 case）＋ 用
   `mock` 或自建靶机跑一遍连接与读写。
5. 批量读里要用的临时表**按这一批的规模分配**，不要按协议上限先要一大块：
   元素 40 字节时 `2048` 项就是 80 KiB，静态池版本里一次三点的读就会被拒
   （`modbus` / `mc` / `fins` 都踩过这个坑，现在按"字符串地址算一项、其余每个
   元素算一项"算容量，上限仍由 `*_MAX_ITEMS` 把关）。

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
| 新代 SYNTEC RemoteCNC | `syntec` | TCP 8000 | **命令号就是区名**（名字或裸号）+ 偏移是 `dwCode`；也认 §10.12 的具名读数 | ✅ 只读（服务端无写端点） |
| 凯恩帝 KND | `knd` | HTTP 80 | **模型项名就是区名**：`STATUS`、`/PART_COUNT`、`/AXIS@0/SCREW/POSITION` | ✅ 只读（现场只映射了 get_value） |
| FANUC FOCAS | `focas` | TCP 8193 | **数据项名就是区名**（`ACTF`/`RDCOUNT`/`STATINFO`…或裸码 `0x24`），`offset` 是**应答块号**，名字里可带 `@<字节>` 取块内偏移 | ✅ 只读 + 模块（别名 `fanuc`） |

其余协议按 `protocal/docs/README.md` 的优先级推进
（第一批 MC/SLMP → FINS → S7 → MTConnect 已完成；第二批 MELDAS → 新代 → LSV2 → FOCAS
已完成）。下一步按规格最全的先做：**GSK（HTTP 端点已齐）→ Brother → 科德/精雕 → RMI**。

**暂缓/不做的，以及原因**（避免以后重复踩）：

| 协议 | 结论 | 依据 |
|---|---|---|
| 新代 SYNTEC 的写操作 | 只读 | 交付包里没有写端点（`EFunctionID` 只有 7 个命令号，没有写数据的路径），`write` 返回"不支持"；要写 PLC 寄存器时用控制器自带的 Modbus 从站 |
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

### 新代 SYNTEC RemoteCNC

```json
{
  "id": "cnc", "path": "/CNC", "type": "syntec",
  "parameters": { "host": "10.0.0.30", "port": 8000, "timeoutMs": 5000 },
  "points": [
    { "path": "/CNC/COUNT", "addr": "part_count", "length": 4, "dtype": "int32" },
    { "path": "/CNC/POS_X", "addr": {"area": "KrnlAPI", "offset": 0,
                                     "length": 8, "dtype": "float64"} },
    { "path": "/CNC/NCPATH", "addr": {"area": "RemoteProgExecute", "offset": 0} }
  ]
}
```

帧格式是从交付的 .NET 程序集里读出来的，不是猜的：`12 字节包头`
（`Length u4 | CmdID u2 | 2 字节填充 | Reserved u4`）+ `8 字节函数头`
（`uFuncID u2 | uSerial u1 | Reserved u1 | IHeader u4`）+ 体，小端，`Length`
只算包头之后的部分。`uFuncID == CmdID`（§10.9：服务端按 `uFuncID` 分派，回包把
`uSerial` 原样带回来）。

**区名是命令**：写 §10.6/§10.7 的名字（`KrnlAPI`、`FileExist`、`DirCreate`…）
或十进制裸号（`"200"`，§10.7 说编号空间是一个）；**偏移是 `dwCode`**，
`length` 是问控制器要的字节数。`KrnlAPI` 的体是
`uFuncID u2 | dwCode i4 | dwSizeIn i4 | dwSizeOut i4` + 输入字节（输入字节紧跟
结构体这一条是推断，§10.8 里标着待抓包确认）。

**具名读数**（§10.12）：客户端那 150 个 API 只是薄壳，真正的 worker 桩里各带
一个常量，这是**第三套编号**（1000 段是计数与时间、700 段是主轴），既不是
`EDataType` 也不是 `EDevice_Type`。这些名字可以直接当区名用，`length`/`dtype`
仍按点位写：

| 区名 | 码 | 说明 |
|---|---|---|
| `part_count` / `part_count_good` / `part_count_bad` | 1000 / 1002 / 1004 | 总/好/坏计数 |
| `spindle_700` / `spindle_771` | 700 / 771 | `READ_spindle` 的两个常量；差在哪未确证 |

名字大小写与下划线都不敏感，带不带客户端的 `READ_` 前缀都行（`READ_part_count`
= `partCount` = `part_count`）。**未确证**：这些码到底落在 `dwCode` 还是
`pBufferIn` 里的设备号（§10.12 结尾写明要一次实机验证），实现按 `dwCode` 走。

**只读**：服务端的 `EFunctionID` 只有 7 个命令号，没有写数据的路径，所以
`write` 返回"不支持"。要写 PLC 寄存器时走控制器自带的 Modbus 从站，
用本仓库的 `modbus_tcp` 就行。

### 凯恩帝 KND（机床自带 HTTP/JSON）

```json
{
  "id": "knd1", "path": "/CNC", "type": "knd",
  "parameters": { "host": "10.0.0.40", "port": 80, "timeoutMs": 3000 },
  "points": [
    { "path": "/CNC/STATE", "addr": "STATUS" },
    { "path": "/CNC/COUNT", "addr": "/PART_COUNT" },
    { "path": "/CNC/X",     "addr": "/AXIS@0/SCREW/POSITION" }
  ]
}
```

这台机床不需要帧编解码：控制器自己跑一个小 REST 服务，**每个端点回一个扁平
JSON**（`GET /workcounts/total` → `{"count": 1234}`），所以驱动里是一张表：
模型项 → 端点 → 取值字段 → 规整方式。表来自现场交付包的映射层
（`lua_mod/knd_mod.lua`，[09 册](../protocal/docs/09-KND-凯恩帝.md) §3），共 16 个
端点、17 个模型项 + 9 轴两路坐标。

**区名就是模型项**（`STATUS`、`PART_COUNT`、`CONTROLLER/PROGRAM`、
`VARIABLE@CUT_TIME`、`AXIS@2/SCREW/POSITION`…），前导 `/`、大小写随意。
地址里的 `/` 与 `@` 都算名字的一部分（`{"addr": "/AXIS@0/SCREW/POSITION"}` 里的
`@0` 不会被当成偏移）。

现场的三条规整规则直接写在代码里，别按直觉改：

- 倍率类（`/overrides/*`、`/sp/overrides/1`）机床给的是 0..2 的比值，模型要
  百分比，所以 **×100**；
- `run-status` 是 `0/1/2`，映射成 `free` / `holding` / `running`；
- `/CONTROLLER/WARNING` 是"报警类别 → 文本"的字典，按固定类别顺序展开成
  `[{number, text}]`，号码是 `100%02d`（第 8 类 `servo` → `10008`）。

**只读**：现场只注册了 `get_value`，没有写端点，所以 `write` 返回"不支持"。
读是 **HTTP GET**，不需要会话，`open()` 只是拿 `/status` 探一次（连错主机会在
开工前就报错）。同一个端点的多个点位（9 个轴都在 `/coors/machine`）在一次批量读里
**只请求一次**。

HTTP 客户端（`drivers/http/ncl_http_client.c`）与 MTConnect 驱动共用：一次请求
一个连接，支持 `Content-Length`、chunked 和"读到关闭"三种回包。

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
  "sample": { "intervalMs": 1000, "uploadMs": 1000 },
  "mqtt": {
    "url": "tcp://127.0.0.1:1883",
    "username": "", "password": "",  // 留空 = 匿名
    "clientId": "V2AABBCCDD1",       // 默认取设备 SN
    "keepAliveSeconds": 60,
    "automaticReconnect": true,
    "reconnectDelayMs": 1000, "reconnectMaxDelayMs": 30000,
    "offline": false                 // true = 不碰 broker
  }
}
```

`mqtt` 一段给了，适配器就自己管这条会话：建客户端 → 连 broker → 订阅本 SN 的
八个请求主题 → 采样上报与应答都走它。没给（或 `"offline": true`）＝ 离线：
驱动照读、REST 照开，只是不上总线——**库层面的默认是离线**，所以测试和不带
broker 的部署不受影响。**broker 没起来不致命**：`ncl_adapter_broker_poll()`
由主机在它自己的循环里调用，1 s 起、上限 30 s 退避重试，连上后补订阅；断线
交给库的自动重连（`automaticReconnect`），两处不会同时抢连。设备比 broker
先上电是常态，现场靠的就是这条。

守护进程为每个点位注册一条操作：`get_value#<路径>` 读、`set_value#<路径>` 写
（只有 `writable` 的点位才注册），`call#<路径>` 走驱动自己的方法。核心的采样
任务发的就是普通 Query，所以采样通道不需要额外机制。

```sh
./ncl_adapter -c conf/adapter.json            # MQTT + REST + 轮询
./ncl_adapter -c conf/adapter.json --once     # 读一遍全部点位就退出（自检）
./ncl_adapter -c conf/adapter.json -b tcp://10.0.0.9:1883   # broker 从命令行给
./ncl_adapter -c conf/adapter.json --offline  # 或 -b -：不接 broker
```

`-b` 省略时读 `<root>/conf/mqtt.cfg`（设备端示例用的也是那一份）；`-b -` 与
`--offline` 都是离线。`--once` / `--stats` 这类一次性自检固定走离线。优先顺序是
**命令行 → 配置里的 `mqtt` → `<conf>/mqtt.cfg`**：配置文件自己写全了 broker，
就不再去别的文件里找。

自动生成的模型里，每个数据项的 `source` 就是点位路径的父级，因此
**模型路径与配置里的点位路径严格一致**，配置和模型不会漂移。
可运行的配置样例见 `tests/test_adapter.c` 里的 `kConfig`。

## FANUC 适配器模块（`ncl_driver_focas`）

FANUC 现场要的是"一条链路、一个进程、一个设备"：一边是机床（FOCAS/TCP 8193），
一边是 NC-Link（MQTT + REST + 采样 + 审计）。程序就是宿主 `ncl_adapter`，FANUC 的
适配器是它启动时装载的一个模块，**点表声明在模块里**（`adapters/plugins/focas.c`），
配置里只有连接参数：

```sh
ncl_adapter -c conf/fanuc.json            # 一直跑，Ctrl+C 退出
ncl_adapter -c conf/fanuc.json --once     # 轮询一遍全部点位就退出（自检）
ncl_adapter -c conf/fanuc.json --plugins  # 看装载到了哪些模块，然后退出
ncl_adapter -c conf/fanuc.json -b tcp://10.0.0.9:1883
```

`conf/fanuc.json` 里与 FANUC 有关的就两处：`plugins.load`（装载哪个模块）、
`tools[0].parameters`（机床地址与超时）。点表不在这里 —— 它随模块走，一行一个点位。
现场手册是 [`FANUC-ADAPTER.md`](FANUC-ADAPTER.md)（打包时进包内 `README.md`）。

出厂点表（模块里 30 个取值 + 2 个方法，模型路径都挂在 `/MACHINE` 下）：

| 模型路径 | FOCAS 项 | 块 | 读法 | 默认采样 |
|---|---|---|---|---|
| `/MACHINE/STATUS` | `STATINFO@0…@16` | 0 | 块 0 负载的前十个 int16，取 RUN/EMERGENCY 推标准三态 `running`/`free`/`holding` | ✅ |
| `/MACHINE/PART_COUNT` | `RDCOUNT` | 0 | int32，按标准表 7 报成**字符串** | ✅ |
| `/MACHINE/CONTROLLER/PROGRAM` | `EXEPRGNAME2` | 0 | 字符串（`char name[36]` + 两个 long），挂在 CONTROLLER 组件下 | ✅ |
| `/MACHINE/WARNING` | 待抓包（`cnc_rdalmmsg2`） | — | 报警（标准 `WARNING`）；**占着采样通道，抓包补上之前是 `null`** | ✅ |
| `/MACHINE/AXIS@k/POSITION@REAL` | `ACTF@4k` | 0 | float32，轴 k 的字节偏移 `4k` | ❌ 按需读 |
| `/MACHINE/AXIS@k/POSITION@CMD` | 待抓包（`cnc_rdposition`） | — | 目标位置（待抓包：问它答"还没抓到帧"） | ❌ 按需读 |
| `/MACHINE/AXIS@k/SPEED` | `ACTS@4k` | 0 | float32，同上 | ❌ 按需读 |
| `/MACHINE/FANUC_ODBST@MANUAL` … `@AUTO`（11 个） | `STATINFO@0/2/4/6/8/10/12/14/16` 与块 1/2 | 0/1/2 | FANUC 私有状态位（标准里没这些名字，带厂商前缀另起） | ❌ 按需读 |
| `RDLIFE` `RDPARAM` `RDMACRO` `RDTOFS` `RDPROGDIR3` | 同名项 | 0 | 首个 int32，**字段布局待真机核对**（需自己加点位） | ❌ |

带 ✅ 的就是 01 册 §2.3 实证过的布局；最后一行**默认不写进点表**——按需读一个没
核对过的字段可以，每秒往总线上报一个没人核对过的名字不行，要用就自己加一条，
先别开采样（用 `NCL_POINT_ARG` 而不是 `NCL_POINT_SAMPLED_*`）。

**默认采样通道只有四样**（现场口径）：设备状态、加工计件、程序名称、报警。位置、速度、
私有位一律按需读（`NCL_POINT_ARG`）；要上报就把那一行换成 `NCL_POINT_SAMPLED_ARG`，
反过来不想上报就把 `*_SAMPLED_*` 换回普通宏。

四条现场经验写在这里：

- **块内字节偏移写在 `area` 里**（`"STATINFO@12"`、`"ACTF@4"`）：通用地址模型的
  `bit` 是**位**索引（`ncl_address_from_json()` 见到 `bit` 就把 dtype 变成 BIT），
  所以 FOCAS 的"负载第几字节"只能由驱动从名字里取。名字里没有 `@`、或 `@` 后面
  不是十进制数的（别的协议那种 `AXIS@0/SCREW`）按原样处理。
- **项名以数字结尾要小心**：地址解析把尾部的数字串当**偏移**（`"D100"` 是区 `D`
  偏移 100），所以 `{"area":"EXEPRGNAME2"}` 到手是区 `EXEPRGNAME` + 偏移 2。
  驱动在项表里查不到时会把这个数字再拼回去，所以两种写法都能用；显式写
  `"offset": 0` 或 `"EXEPRGNAME2@0"` 最不容易误读。
- **机床掉线时一轮只等一次超时**：`ncl_adapter_poll_round()` 逐点读、遇到传输层错误
  就结束这一轮（否则 N 个点位要各等一次连接超时）；`ncl_adapter_poll()` 相反，它把
  每个点位都试一遍，是给自检和测试用的。
- **轮询与采样会各读一遍机床**：采样通道发的是普通 Query（走 `get_value#…` 绑定，
  这条链路上是"读一次机床并上报"），而模型里的值只由 `ncl_adapter_poll_round()`
  刷新（REST 和读模型的客户端看的是它）。现场嫌报文多就把 `--interval` 调大，
  或者把不必要上报的点位改回按需读。这张表一轮是 24 次读（30 个点位里 6 个待抓包的
  跳过），采样通道另读它的 4 个。

现场部署：把 `bin/ncl_adapter.exe`、`plugins/ncl_driver_focas.dll`、`conf/fanuc.json`
与 `conf/mqtt.cfg` 放一份（`plugins/` 必须跟 `bin/` 同级），`bin/sn.txt` 会自动生成
或由 `-s` 指定，日志在 `<root>/log/out.txt`。

**打包给现场**：`.\tools\make_fanuc_release.ps1` 出一个
`dist\nclink-fanuc-adapter-<版本>-win-x64\`（+ zip + `.sha256`）：程序、模块、配置、
站端手册（`adapters/FANUC-ADAPTER.md` → 包内 `README.md`）、`run-once.ps1` /
`run.ps1` / `list-plugins.ps1`、`SHA256SUMS.txt`。加 `-WithProtocolDocs` 会把 01 册
与本文档一起塞进 `docs/`（内部资料版）；默认不带，因为那两份是逆向证据与工程笔记，
给机床厂看的包里不该有。

## 适配器模块（plugins）

厂商适配器默认不编进程序里，而是做成可动态装载的模块：**"这台机器会说哪种机床"
是部署决定，不是编译决定**。

```
plugins/
├── ncl_driver_focas.dll      ← 协议名 "focas"，别名 "fanuc"
└── ncl_driver_modbus.dll     ← 协议名 "modbus_tcp"
```

- **命名约定**：`ncl_driver_<协议名>.dll`（POSIX 是 `libncl_driver_<协议名>.so`）。
  配置里写协议名即可，装载器自己补文件名；直接写 `xxx.dll`、带路径的名字也认。
- **配置**：`"plugins": {"load": ["focas"]}`（也接受数组形式 `"plugins": ["focas"]`，
  或对象 `{"dir": "plugins", "load": [...], "auto": true}`：`auto` 表示先扫描
  整个目录）。不写这一段＝只扫目录。
- `-P/--plugin-dir <目录>`、`--plugin <名字|文件>`（可重复，最多 8 个）、
  `--plugins`（列出已装载的模块与协议）；`-r/--root` 决定 `conf/ bin/ plugins/ log/`
  的位置。
- **ABI**（`include/nclink_adapter/ncl_module.h`）：模块只导出一个入口
  `const ncl_adapter_module_desc *ncl_adapter_module(void)`，结构里带 ABI 代次、
  协议名、版本、说明、驱动工厂 `create()` 与可选别名。**宿主负责登记**：模块不碰
  自己的注册表副本（模块链的是静态核心，它有自己的一份）。加载失败会用平台自己
  的原因报出来（`LoadLibrary` / `dlerror` 的原文），不会变成没头没脑的
  "协议未注册"。
- **两条纪律**：模块必须与程序用同一套头文件编译（装载时核对 ABI 代次，不一致会
  拒绝并说原因）；`NCL_STATIC_MEM` 构建不要混用模块（两边各有一块内存池，谁也释放
  不了对方的内存块）。
- **关掉插件**：`-DNCLINK_BUILD_PLUGINS=OFF` 把驱动放回内置注册表，得到一个自包含
  的可执行文件（没有 `plugins/` 也照样跑）。

## 测试

```sh
.\build.ps1                      # Windows：配置 + 编译 + 42 个测试套件
sh build-linux.sh build-linux    # Linux：同样全跑一遍
```

其中 26 套是核心库的（`tests/`），16 套是适配器层的（`adapters/tests/`）。
适配器层的测试三件套：`tests/test_driver.c`（驱动接口：注册表、地址解析、
错误分级、mock 的读写/位寻址/批量/事件/原始报文）、
`tests/test_driver_manager.c`（配置加载、点位表、前缀分派）、
`tests/test_adapter.c`（配置 → 设备：生成模型、操作、读写、方法、轮询）。
协议驱动的测试以两段为主：报文级的黄金样本（字节级 diff），以及对着 mock
靶机的连接—读写—重连流程。
`tests/test_adapter_plugin.c` 把 "宿主 + 模块 + 配置点表" 这条链整根跑一遍：
装载 `plugins/ncl_driver_focas.dll` → 登记协议与别名 → 用配置里的 8 个点位生成
设备 → 读假 FANUC 机床的应答块（含 `@<字节>` 取偏移、块索引、字符串按长度截断、
项名尾巴数字拼回）→ 模型里的值；同时覆盖加载器的失败路径（外部 ABI 代次、
没有入口的文件、文件不存在、协议重名）。核心库那边多了 `tests/test_library.c`
（`ncl_library_*`：名字 → 文件名、装载夹具模块、取符号、错误文案）。

小池回归（适配器层最容易踩的是"按协议上限要临时表"这类固定大块，见上一条）：

```sh
NCL_STATIC_MEM=1 NCL_MEM_POOL_BYTES=65536 NCL_MEM_REPORT=1 ./build-linux.sh build-linux-64k
```

64 KiB 池下应当只有 `file` 一套失败（它自己的整块读回比较要 1 MiB 连续块，与适配器
无关）；32 KiB 池会再少 `ftp` 一套。池的实测峰值与边界见 `../MANUAL.md` 4.9。
