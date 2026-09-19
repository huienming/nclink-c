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

   前 16 项读法已定。剩下 7 项再喂"数据段 = 0x10 0x11 0x12 …"这种**唯一字节**
   （`S7R:`），从返回值反查它们读哪儿：

   | 项 | 实测返回 | 读法 |
   |---|---|---|
   | `Alarm` | 319951120 = 0x13121110 | 数据段**开头 4 字节的小端 uint32** |
   | `PlcType` | 4368 = 0x1110 | 开头 **2 字节的小端 uint16** |
   | `CoordinateName` | `["\x10\x11…", …]` | **字符串数组**（按字节读成文本） |
   | `S1Load` | `[2.86e-29]` | double 数组（改 item 数后会 `error response`，说明它按请求的项数校验） |
   | `FeedActual` | `x²/100` | 见下 |
   | `Mode` | `"AUTO"` | 项长 **4 字节**时返回模式**名字**（整数码 → 文本） |
   | `Execution` | `error response` | 仍待定（它要 2 项，两项各自长度可能不同） |

   `FeedActual` 的换算已实测出来——把数据段偏移 0 的 double 记为 x，返回 **x²/100**：

   | 输入 x | 返回 |
   |---|---|
   | 42 | 17.64 |
   | 24 | 5.76 |
   | 100 | 100 |
   | 10 | 1 |
   | 1 | 0.01 |

   即网关是"**进给 × 倍率%**"两个字段相乘，而我们只给了一段数据、两个字段都读到
   同一个 x，于是表现为平方除以 100。要让它回一个指定进给，把倍率字段填 100 即可
   （两项分别给不同的值，用 `S7S:<hex1>,<hex2>`）。

   `Mode` 的两个子项**都必须正好 4 字节**（5/6/8 字节一律 `error response`），但
   返回的模式名**与这 4 字节的取值无关**：0..13、位模式（1/2/4/8/…/0xFFFF）、
   文本（JOG/MDI/AUTO/TEACH/REPOS/REF）、大小端，以及"只改第一项/只改第二项"
   三种组合各扫一遍，**每次都回 `AUTO`**（`tools/site-probe/s7ncu_mode_exec_probe.sh`）。
   所以这一项在网关里是**半成品**：形状校验有了、码表没接上，`AUTO` 是常量出口
   ——之前"4 字节码 → 模式名"的说法只对了一半。`Execution` 到本轮为止仍未找到
   它能接受的两项形状（等长 1~32、以及 1/2/4/8 的各种组合都回 `error response`），
   它请求里的两个 SZL 子项不同（`…0b…` 与 `…0d…`），下一步按请求里的子项推导长度。

   顺手把 `Execution` 的**请求**也从网关自己的构造函数里读了出来
   （`hp2x/protocols/siemens/plc/s7v2.(*S7).Execution`，0x648074）：TPKT 总长
   **39**（0x27）、COTP `02 f0 80`、S7 头 `32 01`（Job）+ PDU 引用 `00 14` +
   参数长 `00 16`（22）+ 数据长 `00 00`，参数 = `04 02`（Read Var，**2 项**）+
   两个 10 字节 SZL 子项（`12 08 82 41 00 0b 00 01 7f 01` 与 `…0d…`）。
   所以它的应答必须是**两项**、且两项各自的长度要与这两个子项匹配——剩下就是
   把这 10 字节子项里的 SZL-ID/索引解出来（`41 00 0b 00` / `41 00 0d 00`）。

   另外把**应答的校验规则**从网关自己的解析函数里读了出来
   （`hp2x/protocols/siemens/plc/s7v2.(*S7).ResolveRes`，0x64a918）——逐条如下，
   与我们实现的假机床完全一致（这也是大多数项能通过的原因）：

   ```
   buf 长度 >= 19        buf[0]==0x03 (TPKT)     buf[1]==0x00
   buf[4]==0x02 (COTP)   buf[7]==0x32 (S7)       buf[8]==0x03 (Ack_Data)
   buf[9]==0x00  buf[10]==0x00（冗余）
   buf 长度 >= 20，buf[20]==0x00（错误类）        buf 长度 >= 21
   每个数据项首字节 == 0xFF（成功）
   ```

   `Execution` 的 `error response` 出在这些之后的**项数与项长**检查上（它的两个
   子项不同长），下一步按上面那 10 字节子项解出各自的期望长度即可。

   再把 `(*S7).Execution` 自己的解析段读了一遍：它在应答里反复做
   `cmp …, #2` 与 `ldrb …, [sp, #16]` —— 也就是**按某个字节的取值分派状态**，
   说明 `Execution` 不只是"把数据搬出来"，还要把状态码映射成文字。据此把
   第二项换成单字节的 **0 / 1 / 2 / 3**、第一项换 1/2/4/8 字节再试，
   仍然全 `error response`。所以这一项要么还需要继续读它的映射表，
   要么等一次真机抓包——23 项里其余 22 项都已定。

   也就是说 23 项里已有 **21 项的读法确定**（8 标量 + 4 数组 + 4 文本 +
   Alarm/PlcType/CoordinateName/S1Load + Mode），只剩 `FeedActual` 的换算系数
   与 `Execution` 的项形状；答案侧的机制（项数回显、返回码、错误类/码）都已就位。

   至此 S7NCU 这条链**闭环**：COTP CC + Setup ack（含错误类/错误码）+ Read ack
   （长度按字节、数据段头 8 字节是小端 float64）。25 个数据项只剩"逐个确认
   类型（double/数组/文本）"，用 `S7R:<bytes>` 换值即可。

**这些数据到底是什么（外部依据，🟢）**：西门子官方"找答案"里的一问一答把这个
讲清楚了（<https://www.ad.siemens.com.cn/service/answer/solve_237967_1044.html>）：

| 我们的项 | 西门子 NC 变量 | 取值 |
|---|---|---|
| `Execution` | `/Channel/State/progStatus[u1]`（程序运行状态） | **1 中断 / 2 停止 / 3 运行 / 4 等待 / 5 取消** |
| `Mode` | `/Bag/State/opMode[u1]`（CNC 当前方式） | **0 JOG / 1 MDI / 2 AUTO** |

用实测校一遍（`tools/site-probe/s7ncu_mode_exec_probe.sh`）：

1. `Execution` 的请求确实是**两个** SZL 子项，应答也必须回两项——用 `S7R:`
   （参数里只声明 1 项）打它一定 `error response`。这解释了为什么早先"单项形状"
   怎么试都不通：**项数必须一致**。
2. `Mode` 的形状要求与"两项各 4 字节"吻合（`0/1/2` 恰好是 4 字节整型），
   但**码表没接上**：取值怎么变都回 `AUTO`（见上）。
3. `FeedActual` 的 `x²/100` = **实际进给 = 设定进给 × 进给倍率%**——只喂一段
   数据时两个字段读到同一个 x，于是表现为 x²/100。

⚠️ **一处更正**：早先拿"网关二进制里有 `Auto` / `Stopped` / `waiting` 这几个词"
当作这两张状态表的文案，是**误判**。它们分别来自 Go 运行时的通用字符串
（`AutoClose` / `SetAutoWrapText` / `Debugmode`…、`syscall.WaitStatus.Stopped`、
`gcwaiting` 等），与西门子状态表无关；真正的码表不在这些字符串里，本文档下面
也不再拿它当证据。

同一问答还给"机床状态"的 DB21 信号组合（`DB21.DBX35.0`+`DB21.DBX35.5` = 运行、
`DB21.DBX35.4` = 待机），可作交叉验证。

注意 `0=JOG / 1=MDI / 2=AUTO` 出自**网友问答**（西门子"找答案"，非官方手册）。
另一路流传的 840D 接口信号表写的是**位序**：`DB11.DBB0.0 = AUTOMATIC`、
`.1 = MDA`、`.2 = JOG`、`.3 = TEACH IN`（`DB11.DBB6.x` 同序，NC→PLC）。
两者数值排列不同，真机确认前以问答为准、以位序表作旁证。

**这些项在标准里叫什么**：交付包的模型文件 `cfg/models/s7_ncu.json` 用的是
**NC-Link 标准**的类型名——`STATUS`（机床状态 `010302`）、`FEED_OVERRIDE`（进给倍率
`010303`）、`FEED_SET`（进给设定值）、`FEED_SPEED`（进给速度）、`SPINDLE_OVERRIDE`
（主轴倍率）、`PART_COUNT`（加工件数）、`SPEED_SET`/`SPINDLE_SPEED`（主轴设定值/
转速）、`CYCLE_TIME`/`LAST_RUN_TIME`、`PLCTYPE`、`S1LOAD`（主轴参数，`LIST`）、
`NCK_NAME`/`NCK_NO`/`NCK_VER`（名称/编号/版本）、`PROGRAM`（主程序名）、`WARNING`
（报警）、`TOOL_NUMBER`（刀具号）、`MODE`（模式），坐标类则是 `NAME`/`ABSOLUTE`/
`RELATIVE`/`MACHINE`。

标准本身：**T/CMTBA 1008.1…1008.7-2020《数控装备工业互联通讯协议》（NC-Link）**，
中国机床工具工业协会（CMTBA）团体标准，2020-12-01 发布、2021-01-01 实施；其中
**第 4 部分《数据项定义》就是"数据字典"**（给出设备对象/组件对象/数据对象的数据项）。
标准里的三层角色"**数控装备 → 适配器 → 代理器 → 应用系统**"正好对应本交付包：
`nclink-service` + 各家 `lib*.so` 插件 = 适配器，`hp2x_box200`（:33123）= 代理器。
出处与原文摘录见 `30-外部资料-NC-Link与西门子.md`。

按这些真值（mode 0/1/2、progStatus 1..5）再打一遍 `Execution` 仍是
`error response`——说明这一项剩的不是取值问题，而是**它两个子项的形状**；
但"数据是什么"已经由上面的外部依据定死。

**批量读**：一次 Read Var 可带多个 Item（受 PDU 尺寸限制，500 字节 PDU 约能放 20-30 个 Item）。

### 5.3 `Mode` / `Execution` 的第二轮：把代码读透了（🟢 实测 + 反汇编）

这一轮不再靠猜：先用 `.gopclntab` 还原函数地址（`tools/site-probe/go_pclntab.py`），
再用 objdump 出 ARM 汇编（`arm_analyze.sh`），配 `elf_vaddr.py` 解字面量池。

**（1）每个数据项都是"`bytes.Reader` + `encoding/binary.Read`"。**
各项函数反汇编里的调用点是 `encoding/binary.Read`（0xeb080），前两个参数是一个
`*bytes.Reader`（结构 = `{ptr,len,cap,i=0,prevRune=-1}`，12/16/20 三个字段在
栈上一个个写出来就能认出来）和小端 `binary.LittleEndian`，第三个参数是目标类型：

| 项 | 目标类型 | 需要的项长 |
|---|---|---|
| `FeedSet` | `float64` | 8 字节 |
| `Mode` | 4 字节小整数 | 4 字节 |
| `Execution` | 2 字节整数 | 2 字节 |

**（2）`FeedActual` = `FeedSet` × `FeedOverride` ÷ 100 —— 这次是从代码上坐实的。**
`(*S7).FeedActual`（0x648b9c，只有 352 字节）里就三件事：`bl 0x648540`（= `FeedSet`）、
`bl 0x64886c`（= `FeedOverride`）、`vmul.f64` 之后拿常量 `*(0x94ea0c)` 做 `vdiv.f64`；
把那 8 字节读出来就是 **100.0**。这就是之前实测 `x²/100` 的由来——不是"进给乘倍率"的
巧合，而是**驱动作者自己写的**。

**（3）`Mode` 的真码表（反汇编，值取每项的低 16 位）**：

```
v[0]==0 && v[1]==1  -> "REPOS"      (长度 5)
v[0]==0 && v[1]==3  -> "REFPOINT"   (长度 8)
v[0]==0 && v[1]==0  -> "JOG"        (长度 3)
v[0]==2             -> "AUTO"       (长度 4)
其它                 -> "AUTO"
```

**但实测到不了这张表**：喂 4 字节时 (`0,0`) 也回 `AUTO`（表里应是 `JOG`），
说明 `Mode` 真正比较的值不是我们给的 payload。声明长度扫描也佐证这项很脆：

| 第二项声明长度 | 1 | 2 | 3 | **4** | **5** | 6…19 | **20** |
|---|---|---|---|---|---|---|---|
| 结果 | err | err | err | **AUTO** | **AUTO** | 全 err | **AUTO** |

（第一项固定 4 字节；第一项换长度时规律一样。）——4/5/20 能过、6~19 全过不了，
这不是正常协议该有的边界，更像解析器按"项头 4 字节 + 声明长度"切完之后的偏移 bug。

**（4）`Execution` 基本可以判定为"当前版本不可达"。** 它的检查是
**每一项都必须正好 2 字节**（`cmp …, #2` 后接 `runtime.memequal` 与 2 字节常量
`01 00` / `02 00` / `03 00` / `00 00` / `05 00` 比），而同一份解析器喂给 `Mode`
的项长度却是 4 字节起步——两边的"项长度"口径对不上。实测也印证：

- 2 字节码全网格 `0..5 × 0..5`（小端、大端各一轮，共 72 次）→ **全部 `error response`**；
- 等长配对 `1..16 × 1..16`（64 组）→ 全 `error response`；
- 4/5/8/12/16 字节等长、以及"第一项 1 字节 + 第二项 4 字节"等形状 → 全 `error response`。

结论：**两项 `Mode` / `Execution` 在网关这一版里是半成品**——`Mode` 的码表写好了但
读值路径不通（恒 `AUTO`），`Execution` 的长度断言与解析器给的长度永远不一致（恒
`error response`）。想 100% 定死只剩一条路：**真机（840D sl NCU）抓一次这两项的
应答**，看真机的项头/长度长什么样。

### 5.4 第三轮：把 `ResolveRes` 与 `Execution` 的机器码读完（🟢 反汇编，机制已明）

这一轮把两份关键函数按地址反汇编出来（`tools/site-probe/arm_range.sh`，因为
`go_pclntab.py` 认不出这份 `hp2x_box200` 的 pclntab）。**地址对得上的那份网关是
`D:\03-开发代码\incbox200\app1\hp2x\hp2x_box200`（14.5 MB）**，不是另一份 17.9 MB 的
（后者在 0x648074 处没有代码）——这一条以前没写下来，害得下一轮容易摸错包。

**（1）`(*S7).Execution`（`0x648074`）的请求模板，逐字节读出来了**（46 字节缓冲，
只写前 39 字节）：

```
03 00 00 27            TPKT，总长 39
02 f1 80               COTP DT
32 01 00 00 00 14      S7 Job，PDU 引用 0x0014
00 16                 参数长 22
00 00                 数据长 0
04 02                 Read Var，2 项
   12 08 82 41 00 0b 00 01 7f 01     SZL 子项 1
   12 08 82 41 00 0d 00 01 7f 01     SZL 子项 2
```

即 `Execution` 读的是 SZL `0x0b` / `0x0d` 两项（和 `Mode` 的 `0x03` / `0x0c` 不同），
也解释了"为什么必须回两项"。

**（2）应答解析（同函数 `0x6481fc` 起）**：

```
bl 0x64a918                    ResolveRes
cmp r0, #0 ; bne -> 错误出口    r0 = 错误接口
cmp r1, #2 ; blt -> 错误出口    项数必须 >= 2
item0 = {ptr=[r2],   len=[r2+4] }      // Go 的 []byte 头 = 12 字节
item1 = {ptr=[r2+12], len=[r2+16]}
cmp item0.len, #2 ; 不等则"不匹配"    ← **每项载荷必须正好 2 字节**
memequal(item0.ptr, [01 00], 2)       ← 再按 2 字节常量派发文案
cmp item1.len, #2 ; memequal(item1.ptr, [03 00], 2)
cmp item0.len, #2 ; memequal(item0.ptr, [02 00], 2)
……逐条与 01 00 / 03 00 / 02 00 / 00 00 / 05 00 比
```

和 NC-Link 给的真值表对得上（progStatus `1 中断 / 2 停止 / 3 运行 / 4 等待 / 5 取消`）。
也就是说：**`Execution` 期望的应答是"两项、每项载荷 2 字节、载荷就是状态码"**，
形状是有定义的——问题出在下一步。

**（3）`ResolveRes`（`0x64a918`）读应答的方式，就是那条"口径对不上"的根源**：

```
buf 长度 < 19                       -> 错误
buf[0]!=03 · buf[1]!=00             -> 错误   (TPKT)
buf[4]!=02                          -> 错误   (COTP)
buf[7]!=0x32 · buf[8]!=03           -> 错误   (S7 Ack_Data)
buf[9]!=0 · buf[10]!=0              -> 错误   (冗余)
n = u16(buf[2..4))                  -> 这是 **TPKT 总长**，不是数据项长度
buf 长度 < n                        -> 错误
然后：bytes.NewReader(buf[2:] ) 与 bytes.NewReader(buf[15:]) 各建一个 reader，
      再用 encoding/binary.Read 往里读
```

**`buf[15]` 正好是 S7 头里"参数长度"字段的起始位置**（4 TPKT + 3 COTP + 2 协议/ROSCTR
+ 2 冗余 + 2 PDU 引用 + 2 参数长 = 15）。也就是说：解析器把**自己的头字段**当成了项
区来切，于是"某项声明长度"实际被解成头里的常数，而**同一份解析器给 `Mode` 的项长是
4 字节起步、给 `Execution` 的断言却要求正好 2 字节**——两边永远对不上，跟假机床喂什么
形状无关。

**结论（本轮不改上一轮的定性）**：`Mode` / `Execution` 仍然是**驱动半成品**，
`#1` 保持 🔴 等真机；但**机制现在是明确的**（不是"我们造不出应答"，是驱动的
`buf[15:]` 偏移把项区读歪了），真机回包能拿来做的是"反推它本该读哪个偏移"。

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
