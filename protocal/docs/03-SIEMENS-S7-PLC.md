# 03 · 西门子 S7 PLC（S7comm over ISO-TSAP）实现规格书

> **证据**：🟢 python-snap7 / pycomm3 源码（COTP + S7comm 完整实现）
> **定位**：S7-200/300/400/1200/1500 全系标准协议（840D 的 PLC 层同源）

---

## 1. 速查

| 项 | 值 |
|---|---|
| 端口 | **TCP 102**（ISO-TSAP） |
| 协议栈 | `TCP → TPKT(ISO 8073) → COTP → S7comm` |
| 字节序 | **大端**（S7comm 内部） |
| 机架/槽位 | S7-300/400：rack=0, slot=2；S7-1200/1500：rack=0, slot=1 |
| 字节序协商 | PDU 大小协商：`COTP_PARAM_PDU_SIZE = 0xC0` |
|参考实现| `python-snap7`（**官方 C++ 库封装**）· `pycomm3`（纯 Python） |
| 完整函数表 | snap7 全表（原始素材未随本目录提供） |

---

## 2. 连接建立（三步握手）

```
① TCP connect(ip, 102)
② COTP Connection Request (CR, 0xE0):
   ISO-TSAP 参数：CALLING_TSAP=0x0100, CALLED_TSAP=0x0302(rack0/slot2) 或 0x0301(slot1)
   协商 PDU 大小（S_512=0x09 / S_1024=0x0A / S_2048=0x0B …）
③ COTP Connection Confirm (CC, 0xD0) ← 设备返回
④ S7comm 通信建立：Job = Setup Communication (0xF0 0x00)
   参数：MaxAmQCalling / MaxAmQCalled / PDU Length（协商结果，常见 480 / 960）
```

**COTP 报文类型**（`snap7/connection.py`）：
| 值 | 名称 |
|---|---|
| `0xE0` | `COTP_CR` 连接请求 |
| `0xD0` | `COTP_CC` 连接确认 |
| `0x80` | `COTP_DR` 断开请求 |
| `0xF0` | `COTP_DT` 数据传输 |

**PDU 尺寸协商值**：`S_128=0x07, S_256=0x08, S_512=0x09, S_1024=0x0A, S_2048=0x0B, S_4096=0x0C, S_8192=0x0D`

---

## 3. 帧格式

### 3.1 TPKT（ISO-on-TCP，4 字节）

```
0  1  版本（0x03）
1  1  保留（0x00）
2  2  总长度（大端，含 TPKT 头）—— **注意：按总长发送，接收侧要按此分包**
```

### 3.2 S7comm 头（10 字节）

```
0   1  协议 ID（0x32）
1   1  ROSCTR：0x01=Job 0x02=Ack 0x03=Ack-Data 0x07=UserData
2   2  冗余标识（0x0000）
4   2  PDU 引用（自增，响应回显）
6   2  参数长度（大端）
8   2  数据长度（大端）
10  N  参数区
..  M  数据区
```

### 3.3 常用功能码（参数区首字节）

| 功能 | 码 | 说明 |
|---|---|---|
| Read Var | `0x04` | **读**（可一次多地址） |
| Write Var | `0x05` | **写** |
| Setup Communication | `0xF0` | 建链 |
| Read SZL | `0x1C` | 系统状态列表（型号/序列号） |
| PLC Stop | `0x29` | **停机（危险）** |
| Request Download / Download Block / Download Ended | `0x1A`/`0x1B`/`0x1C` | 程序下载 |
| Start Upload / Upload / End Upload | `0x1D`/`0x1E`/`0x1F` | 程序上传 |

### 3.4 数据项（Item）格式（Read/Write Var 内）

```
0x12          变量规格（VARIABLE_SPECIFICATION）
0x0A          后续长度
0x10          Syntax ID = S7ANY
0x02          传输尺寸：01=BIT 02=BYTE 03=CHAR 04=WORD 05=INT 06=DWORD 07=DINT 08=REAL
2 bytes       长度（位访问时为位数）
2 bytes       DB 号（非 DB 区填 0）
1 byte        区域：0x81=I 输入 0x82=Q 输出 0x83=M 标志 0x84=DB 0x1C=计数器 0x1D=定时器
3 bytes       地址（位×8 + 字节偏移）
```

---

## 4. 地址格式与数据类型

| 地址 | 区域码 | 示例 |
|---|---|---|
| I / E（输入） | `0x81` | `I0.0`, `IB10`, `IW20`, `ID30` |
| Q / A（输出） | `0x82` | `Q0.0`, `QB10` |
| M（标志） | `0x83` | `M0.0`, `MB10`, `MW20`, `MD30` |
| DB（数据块） | `0x84` | `DB1.DBX0.0`, `DB1.DBB10`, `DB1.DBW20`, `DB1.DBD30` |
| T（定时器） | `0x1D` | `T1` |
| C（计数器） | `0x1C` | `C1` |

**数据类型**：BIT · BYTE · WORD(16) · DWORD(32) · INT/DINT（有符号）· REAL(32 浮点，IEEE754 大端) · STRING（S7 格式：2 字节头 + 字符）

**地址换算**：位地址 = `字节偏移 × 8 + 位号`（Read Var 的地址字段是位地址）

---

## 5. 读写流程

```
读 DB1.DBD0（REAL）:
  TPKT(03 00 00 1F) + S7(32 01 00 00 <pduRef> 00 0E 00 00)
  + 04 01                      # Read Var, 1 item
  + 12 0A 10 08 00 04 00 01 84 00 00 00    # S7ANY, REAL, len=4, DB=1, area=0x84, addr=0×8

写 M10.0 = 1:
  同结构，功能码 05，数据区：00 04 00 01 00 03 00 01 01  (bit, 1 byte, 0x01=ON)
```

### 5.1 S7NCU（840D NCU 的 S7 通道）实测帧 🟢

现场交付包里的 `S7NCU` 模块（**25 个数据项**，OpenAPI 里逐项列出）走的就是 S7comm：
跑现场 Go 网关 + 假机床（端口 102）逐项抓下来，**每项两帧**：

1. **Setup Communication**（22 字节，恒定）：
   `03 00 00 16 11 e0 00 00 00 48 00 c1 02 04 00 c2 02 0d 04 c0 01 0a`
   （PDU 480；模块每次读都重新握手，不留长会话）
2. **Read Var**（`03 00 00 XX 02 f0 80 32 01 …`）：读的**区域码 `0x12` = SZL**
   （系统状态列表），所以这些项统一是"读某个 SZL-ID / 索引"。

各数据项的第 2 帧（逐字节，供实现对照）：

```
Alarm         ... 04 01 12 08 82 01 00 01 00 01 77 01 ...
Program       ... 04 01 12 08 82 41 01 2a 00 01 7f 01 ...
Execution     ... 04 02 12 08 82 41 00 0b 00 01 7f 01 | 12 08 82 41 00 0d 00 01 7f 01
Mode          ... 04 02 12 08 82 21 00 03 00 01 7f 01 | 12 08 82 41 00 0c 00 01 7f 01
PlcType       ... 04 01 12 08 82 41 01 2a 00 01 7f 01
NckName       ... 04 01 12 08 82 01 46 78 00 04 1a 01
NckNo         ... 04 01 12 08 82 01 46 6e 00 01 1a 01
NckVer        ... 04 01 12 08 82 01 46 78 00 01 1a 01
CoordinateAbsolute .. 04 05 (5 个轴：SZL 41 00 03/02 与 74 01)
CoordinateMachine  .. 04 05 (SZL 41 00 02 ×5，74 01)
CoordinateRelative .. 04 05 (SZL 41 00 19 ×3 + 41 00 02 ×2)
CoordinateName .. 04 01 12 08 82 41 4e 70 00 01 1a 05
FeedActual    ... 04 01 12 08 82 41 00 02 00 01 7f 01
FeedSet       ... 同样（与 FeedActual 同帧）
FeedOverride  ... 04 01 12 08 82 41 00 03 00 01 7f 01
SpeedActual   ... 04 01 12 08 82 41 00 02 00 01 72 01
SpeedSet      ... 04 01 12 08 82 01 00 03 00 04 72 01
SpeedOverride ... 04 01 12 08 82 41 00 04 00 01 72 01
S1Load        ... 04 04 (SZL a1 00 1a/1e/21/25 四路，82 01)
ToolNo        ... 04 01 12 08 82 41 00 21 00 01 7f 01
PartCount     ... 04 01 12 08 82 41 00 79 00 01 7f 01
CycleTime     ... 04 05（与 CoordinateAbsolute 同形）
LastRunTime   ... （本次未取到读帧，按同样两帧结构重跑即可）
```

**实现要点**：区域码 `0x12` 的 S7ANY 变体里，`41/01/21/a1` 之后是 SZL-ID 与索引，
末 3 字节是"部分列表"标记；多个区域可以在一次 Read Var 里合并（`04 0N` 那个 N 就是
项目数），这正是坐标类项一次读 5 个轴的做法。

### 5.2 应答侧：仿真到哪一步了（🟡 2026-09）

`tools/site-probe/s7ncu_reply_probe.sh` + `mock.py` 的 `S7S:` 模式现在能扮演一个
最小 ISO-on-TCP / S7 服务器，**把三步握手走完**（实测序列）：

```
网关 → COTP CR (0xE0)           假机床 → COTP CC (0xD0)（回显 CR 的 TSAP 参数）
网关 → S7 Setup (0xF0)          假机床 → Ack_Data + Setup 参数（PDU 480）
网关 → S7 Read Var (0x04)       假机床 → Ack_Data + 参数 04 01 FF 04 000N
                                          + 数据项 FF 04 000N <值字节>
```

**网关已经收下应答了**（`code:0, success:true`）。两个关键点：

1. **Ack_Data 的头部比 Job 多两个字节**：`32 03 | 冗余 2 | PDU 引用 2 | 参数长 2 |
   数据长 2 | 错误类 1 | 错误码 1`。少了这两个字节，网关会把我方参数当成
   "错误类/错误码"直接判 `error response`——补上 `00 00` 就通过了。
2. **长度字段它按"字节"读**（不是标准 S7 的"位"）：它内部会 `slice(长度+4)`，
   声明成位就会 `slice bounds out of range [:548] with capacity 91`。

3. **值的读法：数据段头 8 字节 = 小端 float64**（🟢 已验证）。把 `S7 R:` 模式
   的数据段直接写成 `struct.pack('<d', X)`，网关就把 X 原样返回：

   ```
   raw: 42.0     →  value 42        （pack('<d', 42.0) = 00 00 00 00 00 00 45 40）
   raw: 1234.5   →  value 1234.5
   raw: 41.5     →  value 41.5
   ```

   逐项的读法也一并试出来了（同一段字节，不同项解释不同）：

   | 项 | 返回值 | 说明 |
   |---|---|---|
   | `PartCount` / `LastRunTime` | `7.25` | 单个 float64 |
   | `CycleTime` | `[7.25]` | **数组**（读多个 double，这也解释了之前 68 字节的越界） |
   | `ToolNo` | 字符串 | **文本项**（把字节按文字读） |

   用一段"double + 文本 + double + 文本"的模式把 23 个项一次过完（`S7R:`）：

   | 读法 | 项 |
   |---|---|
   | 单个 float64 | `Program` `FeedSet` `FeedOverride` `SpeedActual` `SpeedSet` `SpeedOverride` `PartCount` `LastRunTime` |
   | 数组（`[x]`） | `CoordinateAbsolute` `CoordinateMachine` `CoordinateRelative` `CycleTime` |
   | 文本 | `NckName` `NckNo` `NckVer` `ToolNo` |
   | 还需其它形状（返回 `0`/`[0]`/读到别的偏移/`error response`） | `Alarm` `PlcType` `S1Load` `FeedActual` `Execution` `Mode` `CoordinateName` |

   前 16 项读法已定；剩 7 项按各自的返回（例如 `FeedActual` 回 15239.9025 =
   从别的偏移读到的 double）继续调偏移即可。

   至此 S7NCU 这条链**闭环**：COTP CC + Setup ack（含错误类/错误码）+ Read ack
   （长度按字节、数据段头 8 字节是小端 float64）。25 个数据项只剩"逐个确认
   类型（double/数组/文本）"，用 `S7R:<bytes>` 换值即可。

**批量读**：一次 Read Var 可带多个 Item（受 PDU 尺寸限制，500 字节 PDU 约能放 20-30 个 Item）。

---

## 6. 错误码（S7 返回错误类）

| 错误类 | 码 | 含义 |
|---|---|---|
| 0x81 | Application relationship | 连接问题 |
| 0x82 | Object definition | 对象不存在 |
| 0x83 | No resources | 资源不足 |
| 0x84 | Error on service processing | **服务处理错误（含地址越界）** |
| 0x85 | Error on supplies | — |
| 0x87 | Access error | **访问错误（权限/不存在）** |
| 错误码 | 0x05 | Address out of range（地址越界） |
| | 0x06 | Data type not supported |
| | 0x0A | Object does not exist（**最常见：DB 不存在/地址错**） |

**S7-1200/1500 特例**：默认**禁止 PUT/GET**（需在 TIA Portal 里勾选"允许来自远程对象的 PUT/GET 通信访问"）；且 DB 需要关闭"优化块访问"才能按绝对地址读。

---

## 7. 实现坑

1. **S7-1200/1500 的 PUT/GET 开关**（见上）—— 90% 的"连得上读不到"都是这个。
2. **优化块访问（Optimized block access）**：DB 开了优化就不能按 DB1.DBD 绝对地址读，必须用符号名（需 S7comm-plus / 或改设置）。
3. **PDU 尺寸决定单次读取上限**：协商后 PDU=480 时单 Item 数据 ≤ ~222 字节；超了要分片。
4. **TPKT 长度包含自身 4 字节**，S7 头里的参数/数据长度**不含** TPKT 头 —— 三个长度别混。
5. **PDU 引用必须回显匹配**，否则乱序响应会错位（并发请求时尤其重要）。
6. **S7-300 的槽位是 2, S7-1200/1500 是 1**（TSAP 0x0302 vs 0x0301）。
7. **REAL 是大端 IEEE754**，跨平台解析注意；STRING 有 2 字节头（最大长度/当前长度）。

---

## 8. 参考实现

| 来源 | 说明 |
|---|---|
| `python_snap7-3.1.2` | 完整实现：`connection.py`（COTP/TPKT 分帧）· `client.py`（业务，135 个 `Cli_*` 函数）· `error.py`（错误码表） |
| `pycomm3-1.2.16` | 纯 Python 实现（含 CIP，可对照） |
| 商业库对照 | `SiemensS7Net`（含 S7-200/300/400/1200/1500 语义差异处理） |
| 参考实现侧 | `/Siemens/S7/*` 驱动（端口 102） |
