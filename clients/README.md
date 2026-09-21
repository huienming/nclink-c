# 协议实现（clients/）

一个协议一个目录：帧怎么组、会话怎么建、错误码怎么分级，以及**给现场用的语义函数**
（`ncl_focas_part_count()` 这种"名字就是它读回来的东西"的函数）。适配器（`plugins/*.c`）
引用它们去跟机床说话；它们自己不认识"适配器"，也不认识 NC-Link。

现场怎么用它，就两种姿态：

```c
/* ① 语义层已经有的协议（目前是 FOCAS）：一行一个绑定 */
#include "nclink/clients/focas.h"
NCL_DATAITEM_I64_SAMPLED("/PART_COUNT", ncl_focas_part_count)

/* ② 只有驱动层的协议（寄存器类）：构造 client，用一个 dispatch 按地址读 */
#include "nclink/clients/modbus.h"
static void *box_open(const ncl_json *params, char **err)
{
    ncl_driver *driver = ncl_modbus_tcp_create();

    (void)ncl_driver_ops_of(driver)->create(driver, params);  /* host/port/unit/… */
    return driver;
}
NCL_DATAITEM_SAMPLED("/TEMP", box_dispatch, "4x12")   /* 地址怎么写见下表 */
```

`ncl_driver_ops` 里除 `read_batch` 外都可留空，骨架对空缺的位回 `NCL_ERR_NOT_SUPPORTED`；
错误一律分三层（transport / protocol / business，`ncl_driver_error_tier()`），
所以"电缆断了"和"协议码不对"在宿主那一侧分得开。
## 协议一览

| 协议 | client 构造函数 | 端口 | 现场地址写法 | 状态 |
|---|---|---|---|---|
| 内存靶机 | `ncl_mock_driver_create()` | —— | 任意 `area` + 偏移 | ✅ 测试用 |
| Modbus TCP / RTU / RTU-over-TCP | `ncl_modbus_tcp_create()` / `ncl_modbus_rtu_create()` / `ncl_modbus_rtu_tcp_create()` | TCP 502 / RS-485 / TCP 任意 | `coil`/`discrete`/`input`/`holding` 或 `0x`/`1x`/`3x`/`4x`，如 `4x12` | ✅ 二进制 |
| 三菱 MC/SLMP | `ncl_mc_tcp_create()` | TCP 5534 | 设备名 + 号：`D100`、`M10`、`M10.3` | ✅ 3E/4E |
| 欧姆龙 FINS | `ncl_fins_tcp_create()` | TCP 9600 | 区名 + 字：`D100`、`CIO12.3`、`E0_0` | ✅ 内存区读写 |
| 西门子 S7comm | `ncl_s7_tcp_create()` | TCP 102 | 区名 + 字节偏移：`M10.3`、`DB1`、`MB10` | ✅ ISO-TSAP + COTP |
| MTConnect | `ncl_mtconnect_create()` | HTTP 7878 | **数据项 id 就是区名**：`{"area":"Xabs","offset":0}` | ✅ 只读 |
| 三菱 CNC M70/M80 | `ncl_meldas_create()` | TCP 683 | **命令名就是区名**，偏移是轴号/IO 地址 | ✅ 只读 |
| 海德汉 LSV2 | `ncl_lsv2_create()` | TCP 19000 | **区名是要读的东西**，偏移是地址 | ✅ 版本/状态/PLC 内存 |
| 新代 SYNTEC RemoteCNC | `ncl_syntec_create()` | TCP 8000 | **命令号就是区名**（名字或裸号）+ 偏移是 `dwCode` | ✅ 只读 |
| 凯恩帝 KND | `ncl_knd_create()` | HTTP 80 | **模型项名就是区名**：`/STATUS`、`/PART_COUNT` | ✅ 只读 |
| FANUC FOCAS | `ncl_focas_open()`（语义层）/ `ncl_focas_create()`（驱动层） | TCP 8193 | 语义函数优先；驱动层用 item 名（`ACTF`、`RDCOUNT`、`STATINFO`）+ 应答块号 | ✅ 只读 + **语义层** |

其余协议按 `protocal/docs/README.md` 的优先级推进：第一批 MC/SLMP → FINS → S7 → MTConnect
已完成；第二批 MELDAS → 新代 → LSV2 → FOCAS 已完成。下一步按规格最全的先做：
**GSK（HTTP 端点已齐）→ Brother → 科德/精雕 → RMI**。

**暂缓/不做的，以及原因**（避免以后重复踩）：

| 协议 | 结论 | 依据 |
|---|---|---|
| 新代 SYNTEC 的写操作 | 只读 | 交付包里没有写端点（`EFunctionID` 只有 7 个命令号），要写 PLC 寄存器时用控制器自带的 Modbus 从站 |
| Modbus ASCII（15 册） | 暂缓 | 15 册只给了轮廓，没有字节级样本；等一次抓包 |
| MC 的 ASCII 编码、FINS/UDP、S7 的 UDP | 暂缓 | 核心 socket 层目前只有 TCP，ASCII 缺原始样本 |
| 科德/精雕/海康（20 册） | 不做 | 规格书标 🔴 缺，无可用资料 |

## 各协议笔记

下面是每个协议的"实机前必看"：地址怎么拼、握手要什么、哪些坑只能靠现场经验。
### Modbus


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

HTTP 客户端（`clients/http/ncl_http_client.c`）与 MTConnect 驱动共用：一次请求
一个连接，支持 `Content-Length`、chunked 和"读到关闭"三种回包。

## 加一个新协议

1. 建 `clients/<协议>/`，实现 `ncl_driver_ops`：**只有 `read_batch` 是必需的**，
   其余留 `NULL`；一起给出 `create/open/close/is_connected/destroy` 就能跑。
2. 导出一个工厂 `ncl_driver *ncl_<协议>_create(void)`；头文件放同目录（内部头）
   或 `clients/include/nclink/clients/<协议>.h`（现场要看的放这里）。
3. **不要**做注册表：适配器直接调用你的工厂（`ncl_modbus_tcp_create()` 这样），
   协议名不进配置。
4. 错误分三层：`NCL_DRV_ERR_TRANSPORT/PROTOCOL/BUSINESS(code)`，别把协议码当传输错。
5. 批量读里的临时表**按这一批的规模分配**，不要按协议上限先要一大块：元素 40 字节时
   `2048` 项就是 80 KiB，静态池版本里一次三点的读就会被拒（`modbus`/`mc`/`fins`
   都踩过，现在按"字符串地址算一项、其余每个元素算一项"算容量）。
6. 测试放 `clients/tests/test_<协议>.c`：报文级黄金样本（字节 diff，真实抓包做 case）
   ＋对着自建靶机跑连接—读写—重连；`tests/test_point_map.h` 是那座桥。
7. `clients/mock/` 是最小样板：内存点位模型 + 错误注入 + 事件触发，零网络代码。

### 语义层（可选，但推荐给"能叫出名字的量"）

FOCAS 是样板（`clients/focas/focas_values.c` + 公开头 `nclink/clients/focas.h`）：语义面按
域铺开 —— 状态/模式（`ncl_focas_status` / `mode` / `emergency`）、报警（`alarm_status` /
`alarm`）、轴与主轴（`axis_feedrate` / `spindle_speed` / `axis_position` 一族 /
`axis_load` / `spindle_load`）、程序（`program_name` / `program_number` /
`line_number` …）、计数与计时（`part_count` / `tool_group_count` / `timer`）、刀具与参数
（`tool_list` / `tool_offset` / `macro_variable` / `parameter` …）。
**请求码已核、应答还没核的那几条**（位置、负载、报警消息、刀补、宏变量…）回
`NCL_ERR_UNAVAILABLE`，`ncl_focas_last_error()` 里写明要抓哪个调用；帧补上时只改函数体。
另外长了**程序上下行**：`ncl_focas_program_download()`（`cnc_dwnstart4` 三件套，
下行已按官方库实测验通）/ `ncl_focas_program_upload()`（请求码已核、应答待核）。
把"读某个 item 的某一块、按什么类型解、怎么由位域推成三态"这类知识从现场搬到 client，
对外只留 `ncl_err ncl_focas_part_count(ncl_focas *, long long *)` 这种函数，
适配器写 `NCL_DATAITEM_I64_SAMPLED("/PART_COUNT", ncl_focas_part_count)` 一行。

寄存器类协议（Modbus/MC/FINS/S7）补语义层时，粒度到"半语义"就够：
`ncl_modbus_read_word(inst, addr)`、`ncl_modbus_write_word(inst, addr, value)`，
现场再按需覆盖。

## 测试

```sh
.\build.ps1                      # Windows：配置 + 编译 + 42 套测试
sh build-linux.sh                # Linux：同样全跑一遍
```

42 套 = **31 套核心与工具层**（`tests/`）+ **11 套协议客户端**（`clients/tests/`）。
本目录的套件：`modbus` `mc` `fins` `s7` `mtconnect` `meldas` `lsv2` `syntec`
`syntec_driver` `knd` `focas`（`ctest -R <名字>` 单跑）。

小池回归（最容易踩的是"按协议上限要临时表"这类固定大块，见上一条）：

```sh
NCL_STATIC_MEM=1 NCL_MEM_POOL_BYTES=65536 NCL_MEM_REPORT=1 ./build-linux.sh build-linux-64k
```

64 KiB 池下应当只有 `file` 一套失败（它自己的整块读回比较要 1 MiB 连续块）；
32 KiB 池会再少 `ftp` 一套。池的实测峰值与边界见 `../MANUAL.md` 4.9。
