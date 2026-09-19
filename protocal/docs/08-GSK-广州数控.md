# 08 · 广州数控 GSK 实现规格书

> **证据**：🟡 公开文章（GSKRM.dll 函数签名）· 🔵 协议名级（Modbus/rmdata 两条替代路线）
> **定位**：国产 CNC 保有量大，但**官方 SDK 需从代理获取**

---

## 1. 速查

| 项 | 值 |
|---|---|
| 端口 | **TCP 6000**（GSK 联网默认） |
| 通道 A | **GSKRM.dll**（厂商 SDK，函数式 API） |
| 通道 B | **Modbus**（部分型号支持，寄存器映射） |
| 通道 C | **rmdata + gskdll**（988 系列走 UDP，其余走 TCP） |
| 参考实现侧 | `/GSK/*` 驱动（端口 6000） |

---

## 2. 通道 A：GSKRM.dll

### 2.1 已知函数签名（实测）

```csharp
[DllImport("gskrm.dll", EntryPoint = "GSKRM_CreateInstance", CallingConvention = StdCall)]
public static extern int CreateInstance(byte[] ipBytes, int protocolType);

[DllImport("gskrm.dll", EntryPoint = "GSKRM_ReadSystemSignal", CallingConvention = StdCall)]
public static extern int ReadSystemSignal(...);
```

**要点**：
- **32 位进程**（`x86` 编译；64 位需厂商 64 位版）
- IP 以 **字节数组**传入（`byte[4]`），非字符串
- 调用约定 **StdCall**
- `protocolType` 选择网络/串口

### 2.2 典型调用序列

```
① CreateInstance(ipBytes, protocolType)   → 返回句柄/状态
② 业务调用（Read/Write 系列）
③ 释放句柄（DestroyInstance 类函数）
```

---

## 3. 通道 B：Modbus（推荐自实现路线）

GSK 部分型号（如 GSK988/980 系列）支持 Modbus TCP —— **无需 SDK 授权**：

```
① TCP connect(ip, 502)（或厂商配置的 Modbus 端口）
② 标准 Modbus 功能码 0x03/0x04 读、0x06/0x10 写
③ 地址映射需厂商《Modbus 地址表》—— 常见：
   - 坐标、进给、主轴 → 保持寄存器区
   - 报警/状态 → 离散输入或保持寄存器
```

> ⚠️ **地址表是关键缺口**：没有厂商地址表就无法确定哪个寄存器是哪个数据。需向代理商索取，或现场用扫描法反推（不推荐，风险高）。

---

## 4. 通道 C：rmdata + gskdll

| 型号 | 传输 |
|---|---|
| GSK 988 系列 | **UDP** |
| 其余系列 | **TCP** |

通过 `rmdata`（数据）+ `gskdll`（库）组合访问，属于厂商私有协议栈。

---

## 4.5 设备侧报文（🟢 本地仿真抓取，2026-09）

现场交付包的 Go 网关（`hp2x_box200`，:33123）**自己实现**了 GSK 协议：把
`/GSK/CNC/Open/TCP` 的 `ipAddress` 指向我们的假机床，再逐项 POST
`/GSK/CNC/<项>`，设备侧请求就原样落到手上（复现：`tools/site-probe/` 的
`gateway_probe.sh`）。抓到 11 条（每个 POST 恰好一帧，按调用顺序）：

| 项 | 设备侧请求（十六进制） |
|---|---|
| `STATUS` | `93 00 0a 00 6f c8 1e 64 1e 17 10 17 11 00 55 aa` |
| `PART_COUNT` | `93 00 0a 00 6f c8 1e 64 1e 17 10 17 16 00 55 aa` |
| `PROGRAM` | `93 00 0a 00 6f c8 1e 64 1e 17 10 17 12 00 55 aa` |
| `LINE_NUMBER` | `93 00 0a 00 6f c8 1e 64 1e 17 10 17 23 00 55 aa` |
| `FEED_SPEED` | `93 00 0c 00 6f c8 1e 64 1e 17 10 17 1a 01 00 00 55 aa` |
| `SPDL_SPEED` | `93 00 0c 00 6f c8 1e 64 1e 17 10 17 1a 04 00 00 55 aa` |
| `FEED_OVERRIDE` | `93 00 0c 00 6f c8 1e 64 1e 17 10 17 1a 02 00 00 55 aa` |
| `SPDL_OVERRIDE` | `93 00 0c 00 6f c8 1e 64 1e 17 10 17 1a 05 00 00 55 aa` |
| `RAPID_OVERRIDE` | `93 00 0c 00 6f c8 1e 64 1e 17 10 17 1a 06 00 00 55 aa` |
| `TOOL_NUMBER` | `93 00 0a 00 6f c8 1e 64 1e 17 10 17 17 00 55 aa` |
| `WARNING` | `93 00 09 00 6f c8 1e 64 1e 17 10 17 81 55 aa` |

帧结构（先由这 11 条推出形状，再对着现场网关自己的解析函数逐条核对过——
`hp2x/protocols/gsk/cnc.packRequest` / `shouldResponseCmd`）：

```
93 | 00 | 长度(2B 小端) | 载荷(长度个字节) | 55 aa        总长 = 长度 + 6
载荷 = 会话 4B | 序号 4B | 命令 1B | 数据…
```

- `93 00` 帧头、`55 aa` 帧尾；**长度是小端 16 位**，数的正是"`93 00` 之后、
  `55 aa` 之前"的字节数（`93 00 0a 00 …` 共 16 字节 → 长度 10 ✓）。
- 校验顺序（网关侧原文）：`buf[0]==0x93` → `buf[1]==0x00` →
  `buf[总长-2]==0x55` → `buf[总长-1]==0xAA` → `总长 == 长度+6` → `长度 ≥ 12`。
- `6f c8 1e 64 1e 17 10 17` 在一次会话里各帧相同（会话标识 + 计数器），
  跨会话会变。
- 命令字节：`0x11` 状态、`0x16` 加工计数、`0x12` 程序名、`0x23` 行号、
  `0x17` 刀具号、`0x81` 报警，`0x1a` + 子码（`01` 进给速度、`02` 进给倍率、
  `04` 主轴转速、`05` 主轴倍率、`06` 快移倍率）。
### 4.6 应答侧（🟢 2026-09 第三轮，`tools/site-probe/gsk_reply_probe.sh`）

反汇编 `(*GskCnc).query` / `.cmd` / `.shouldResponseCmd` / `.GetCncState` +
假机床实测，应答的构造规则如下（**照这个造，11 项里 10 项立即出值**）：

```
93 | 00 | 长度(2B 小端) | 载荷 | 55 aa        总长 = 长度 + 6，且 长度 ≥ 12
载荷 = 期望头 8B | 命令 1B | 00 00 | 取值…
```

1. **壳**：`shouldResponseCmd`（`0x61a6e4`）查 `93`/`00`/`55`/`AA`、`总长 == 长度+6`、
   `长度 ≥ 12`；不对就报 `unexpected response length`。
2. **期望头**：它把"载荷前 8 字节"和驱动自己算的期望头做 `memequal`（8 字节）。
   读命令（`query`）的期望头是 **`00 64 1e c8 1e 17 10 01`**——对照请求头
   `6f c8 1e 64 1e 17 10 17`：第 1..3 字节**反序**、首字节清 0、末字节变 `01`。
   （`cmd` 写命令那条路用另一组：请求 `65 c8 0c 64 0c <sn> 00 00`、
   期望应答 `65 64 0c c8 0c <sn> 00 00`，`<sn>` = `getCmdSN()`；`Init` 实测就是这条。）
3. **命令 + 两个 0**：每个 getter 在拿到"载荷 [8..]"这段之后，还要把它的前 3 字节
   跟 `命令 00 00` 比一次（`GetCncState` `0x61d590`、`GetFeedSpeedAct` `0x61e1e4`、
   `GetAlarmCount` `0x61f3bc` …）——不等就报 **`unexpected response command`**
   （请求原样回就是死在这一步）。注意子码命令这里是 `1a 00 00`，**不带**请求里的子码。
4. **取值**从"载荷 [8..]"的第 3 字节之后开始（即帧偏移 **15**，载荷偏移 11）：

| 项 | 命令 | 取值 |
|---|---|---|
| `PART_COUNT` | `16` | 载荷 [11..14] = u32 → 实测 0x83828180 原样回 ✅ |
| `LINE_NUMBER` | `23` | 载荷 [11..14] = u32 ✅ |
| `TOOL_NUMBER` | `17` | 载荷 [11..12] = **u16 ToolNo**、[13..14] = **u16 OffsetNo** → `{'ToolNo':33152,'OffsetNo':33666}` ✅ |
| `STATUS` | `11` | 载荷 **[11]** = 状态字节：`0`/`1` → `'free'`、`2` → `'running'`、`3` → `'holding'`、其余 `'unknown'` ✅ |
| `FEED_SPEED`/`FEED_OVERRIDE`/`SPDL_SPEED`/`SPDL_OVERRIDE`/`RAPID_OVERRIDE` | `1a` | 载荷 **[11..14] = float32** → 摆 12.5 就回 12.5 ✅（五项共用同一解码） |
| `WARNING` | `81` + `82` | 先回 0x81 的"条数"（载荷 [11..14] = u32，全 0 → `[]` ✅），再对每条回 0x82：载荷 [11..14] = **u32 报警号** → `[{'number':'4660','text':''}]` ✅（文本由驱动按号查自己的表，报文里没有） |

**还差**：① `PROGRAM`——名字长度读的是"数据 [3..4] 的 u16"、名字在 [5] 起
（这样不再 panic，但回出来是空串，说明还有一处偏移/编码没对，`GetRunCncProgName`
`0x61d6fc` 0x61d7d8-0x61d830 再细读一遍）；② `cmd` 写命令那条路
（`Init` 已经能看到帧 `... 3f 03 00 01 00 00`，但"写"的语义没试）。

---

## 5. 实现坑

1. **缺 SDK 本体**：函数签名已知但 `gskrm.dll` 需从广数代理/系统光盘获取（**当前最大缺口**）。
2. **32 位限制**：DLL 为 x86，64 位采集程序需用独立 32 位进程桥接（IPC）。
3. **Modbus 地址表缺失**：自实现 Modbus 路线的唯一障碍。
4. **型号差异大**：980/988/218 系列协议差异明显，需按型号分支。
5. **端口 6000 是扫描推断值**，以实际机床配置为准。

---

## 6. 参考实现

| 来源 | 说明 |
|---|---|
| 参考实现侧 | `/GSK/*` 驱动（13 端点，端口 6000） |
| 公开文章 | 《手把手教你用 C# 调用 GSKRM.dll 采集广数 980MDI 数据》（含完整 DllImport 代码） |
| 待补 | GSKRM.dll 本体 + Modbus 地址表 |
