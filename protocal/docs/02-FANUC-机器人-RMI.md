# 02 · FANUC 机器人（RMI 新接口 + 老接口）实现规格书

> **证据**：🟢 RMI 24 指令（fanuc_rmi/fanucpy 源码）· 🔵 老接口设备码（underautomation-fanuc 反解）· 参考实现侧 `/Fanuc/Robot/*` 端点

---

## 1. 速查

| 项 | 值 |
|---|---|
| 端口 | **TCP 8193**（RMI 与老接口共用；老接口另有 FTP-TP）；**本包网关的 56 字节二进制帧默认 60008**（§2.2） |
| 两套接口 | **RMI**（新，推荐，R-30iB+）· **老 PC Interface**（SNPX/设备码读写） |
| RMI 库 | `FRC_*` 指令集（**24 条**） |
| 老接口 | `FRC_*` 设备码（GI/GO/UI/UO/SI/SO/RDI/RDO/PMCR2） |
|参考实现| `fanuc_rmi`（Python）· `fanucpy` · `underautomation-fanuc`（.NET） |

---

## 2. RMI 连接建立

```
① 机器人侧：启用 RMI（MENU → SETUP → RMI，配置允许的客户端 IP + 端口 8193）
② TCP connect(robot_ip, 8193)  —— RMI 为**主动连接**模式
③ FRC_Connect → 建立 RMI 会话
④ 业务指令（FRC_ReadCartesianPosition 等）
⑤ FRC_Disconnect
```

**老接口（主动上报模式）**：机器人主动连上位机（上位机监听），或上位机作为 TCP 客户端连机器人。

---

### 2.1 实测请求帧（🟢 本地仿真抓取，2026-09）

现场 Go 网关里的机器人模块（OpenAPI 里是 `FANUC/ROBOT`，**21 个方法**）走的是
**56 字节定长二进制帧**，不是 RMI 的 ASCII。跑网关 + 假机床（端口 8193）逐项抓
（`tools/site-probe/gateway_probe.sh`）：

| 项 | 帧首 2 字节 | 区分字段（尾部） |
|---|---|---|
| `Init` | `00 00` | 全零 |
| `Init2` | `00 00` | 偏移 2 起 `04 00` |
| `GetData` / `GetAlarmList` | `02 00` | 尾部含 ASCII **`CLRASG`**（取数前的清报警） |
| `ReadGI` / `ReadGO` | `02 00` | 端口字 `00 00`、计数 `08 00` |
| `ReadUI` / `ReadUO` | `02 00` | 端口字 **`70 17` = 6000** |
| `ReadSI` / `ReadSO` | `02 00` | 端口字 **`58 1b` = 7000** |
| `ReadRDI` / `ReadRDO` | `02 00` | 端口字 **`88 13` = 5000** |
| `ReadSDI` / `ReadSDO` / `ReadPMCR2` / `PrvReadBit` | `02 00` | 端口字 `00 00`、计数 `08 00` |
| `PrvReadUWord` / `ReadRShort` | `02 00` | 端口字 `00 00`、计数 `0a 00` |

结论：**这家的"数据点"就是端口字**（5000/6000/7000 分别对应 RDI-RDO、UI-UO、
SI-SO 的段起点，与公开约定一致），一次读几个点由计数域决定。应答字段语义仍待
按回包确认——这是本册目前唯一缺的一格。

### 2.2 应答闭环（🟢 2026-09 第八轮，`tools/site-probe/rmi_probe2.sh`）

**没有模板比对的那一层**：`RequestHexData`（`0x618fc8`）＝ 十六进制字符串 →
`hex.Decode` → 发出去 → `GetResponse3`，**只查"应答长度 ≥ 56"**（`0x619084`），
不够就是 `unexpected response length`。

**`Init`（`0x617854`）/ `Init2`（`0x617c00`）要两次交换，应答必须逐字节等于
二进制里内嵌的模板**（都是 `memequal(应答, 模板, 56)`，不等就是
`unexpected response data`）：

| 交换 | 请求（都是 56 字节） | 应答必须等于 | 模板地址 |
|---|---|---|---|
| `Init` 第 1 次 | 56 个 `00` | **模板 A** | `0x819bff` |
| `Init` 第 2 次 | **模板 B** | **模板 C** | `0x819b1f` / `0x819b8f` |
| `Init2` 第 1 次 | 56 个 `00`，但 `[2] = 04` | **模板 A** | 同上 |
| `Init2` 第 2 次 | **模板 B** | **模板 C** | 同上 |

模板就是 56 字节的定长帧（二进制里存成 112 字符的十六进制字符串）：

```
A  01 00 00 00 00 00 00 00  01 00 00 00 00 00 00 00  00 ...（后 40 字节全 0）
B  08 00 01 00 00 00 00 00  00 01 00 00 00 00 00 00  00 01 00 00 00 00 00 00
   00 00 00 00 00 00 01 c0  00 00 00 00 10 0e 00 00  01 01 4f 01 00 00 00 00
   00 00 00 00 00 00 00 00
C  03 00 01 00 00 00 00 00  00 01 00 00 00 00 00 00  00 01 00 00 00 00 00 00
   00 00 00 00 00 00 01 d4  10 0e 00 00 30 3a 00 00  01 01 00 00 00 00 00 00
   01 01 ff 02 00 00 7c 21
```

**读数据那一层**（`PrvReadUWord` `0x618584` / `PrvReadBit` `0x617fb4` /
`ReadRShort` `0x618aa4` …）：请求是按参数**现场拼的 43 字节帧**
（`binary.Write` 写 1 个字节 + 2 个 uint16 = `selector`/`index`/`count`，
`bytes.Buffer` 收尾），应答 **= 56 字节头 + 数据**，数据从 **`[56..]`**
（`count < 4` 时是 **`[44..]`**，见 `0x6187f0`）：

| 端点 | 数据解释 |
|---|---|
| `PrvReadUWord` | **小端 uint16 数组**，每项 2 字节 |
| `ReadRShort` | 小端 uint16 数组 |
| `PrvReadBit` | 布尔数组（按位） |
| `GetData` | **整段 `[56..]` 的 base64** |
| `GetHexResponse` | **整个应答的十六进制**（56 字节头 + 载荷都回） |

假机床实测（`rmi_probe2.sh`：`A + 00 01 02 … 1f` 当应答）：

| 端点 | 返回值 |
|---|---|
| `Init` / `Init2` | `success` ✅ |
| `GetData` | `'AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8='`（就是载荷的 base64）✅ |
| `PrvReadUWord` | `[256, 770, 1284, 1798, 2312, 2826, 3340, 3854, 4368, 4882]`（小端 u16）✅ |
| `ReadRShort` | 同上 |
| `PrvReadBit` | `[false × 8]` |
| `GetHexResponse` | 整个应答的 hex ✅ |

失败对照：`Init` 第 1 帧回全 `00` → `unexpected response length`；
回 A、第 2 帧回全 `00` → `unexpected response data`。

**参数表**（从网关自己的 `/api.json` 抄，`tools/site-probe/gateway_schema.sh`）：

| 端点 | 参数（默认值） |
|---|---|
| `Open/TCP` | `ipAddress` / `port`（默认 **60008**）/ `timeout`（15） |
| `Init` / `Init2` / `Close` / `GetAlarmList` | `connectionId` |
| `GetData` | `connectionId` + `setasgs`（默认 `["SETASG 1 1000 ALM[1] 1","SETASG 1001 50 POS[0] 0.0"]`） |
| `PrvReadUWord` | + `selector`(12) / `index`(1) / `count`(10) |
| `PrvReadBit` | + `selector`(70) / `index`(1) / `count`(8) |
| `ReadRShort` | + `address`(1) / `count`(10) |
| `ReadGI/GO/UI/UO/SI/SO/RDI/RDO/SDI/SDO/PMCR2` | + `index`(1) / `count`(8) |

**驱动短板（留档）**：应答短于 64 字节时 `PrvReadUWord` 会 panic
（实测 56 字节 → `HTTP 500 … slice bounds out of range [:66] with capacity 64`）；
`Init` 的模板比对只比前 56 字节，所以**更长的应答（头 + 载荷）也能过握手**。

**真机抓包口径**：`POST /FANUC/ROBOT/Open/TCP` → `/Init`（我方的会话帧），然后
逐项 `POST /FANUC/ROBOT/<项>`；想一次看全就用 `POST /FANUC/ROBOT/GetHexResponse
{"hexData":"…"}`，它把机床回的**整包**原样吐成 hex，照着上表就能把字段填满。

---

## 3. RMI 指令表（24 条，全套）

### 3.1 会话

| 指令 | 说明 |
|---|---|
| `FRC_Connect` | 建立 RMI 会话 |
| `FRC_Disconnect` | 断开 |
| `FRC_Initialize` | 初始化（清错、置位） |
| `FRC_Abort` | 中止当前运动 |
| `FRC_Reset` | **复位报警** |

### 3.2 读状态

| 指令 | 说明 |
|---|---|
| `FRC_ReadCartesianPosition` | **笛卡尔位置**（X/Y/Z/W/P/R） |
| `FRC_ReadJointAngles` | **关节角**（J1..J6） |
| `FRC_ReadDIN` | 读数字输入 |
| `FRC_ReadError` | 读错误码 |
| `FRC_GetUFrameUTool` | 读用户坐标系/工具坐标系 |
| `FRC_ReadUFrameData` | 读用户坐标系数据 |
| `FRC_ReadUToolData` | 读工具坐标系数据 |

### 3.3 运动（写）

| 指令 | 说明 |
|---|---|
| `FRC_LinearMotion` | **直线运动**（笛卡尔目标） |
| `FRC_LinearRelative` | 直线相对运动 |
| `FRC_JointMotionJRep` | **关节运动**（关节角目标） |
| `FRC_JointRelativeJRep` | 关节相对运动 |

### 3.4 写状态

| 指令 | 说明 |
|---|---|
| `FRC_WriteDOUT` | **写数字输出** |
| `FRC_SetOverRide` | **设置速度倍率** |
| `FRC_SetUFrame` / `FRC_SetUTool` / `FRC_SetUFrameUTool` | 设坐标系 |
| `FRC_WriteUFrameData` / `FRC_WriteUToolData` | 写坐标系数据 |
| `FRC_WaitTime` | 等待 |

> 指令清单来源：`fanuc_rmi-0.3.1`（client.py / motions.py / pose_reader.py）

---

## 4. 老接口设备码（SNPX / PC Interface）

| 设备码 | 区 | 说明 |
|---|---|---|
| `GI=12` | 通用输入 | 机器人→上位机 |
| `GO=10` | 通用输出 | 上位机→机器人 |
| `UI=72+6000` | 用户输入 | 系统级 |
| `UO=70+6000` | 用户输出 | 系统级 |
| `SI=72+7000` | 系统输入 | — |
| `SO=70+7000` | 系统输出 | — |
| `RDI=72+5000` | 机器人数字输入 | 机器人 IO |
| `RDO=70+5000` | 机器人数字输出 | 机器人 IO |
| `PMCR2=76` | PMC 保持寄存器 | 共享区（**最常用**） |

**典型用法**：上位机写 `GO`/`PMCR2`，机器人 KAREL/TP 程序读；反之用 `GI`。这是**无需 RMI 授权**的老路，兼容性好。

---

## 5. 数据类型

```
笛卡尔位置：{ x, y, z, w, p, r }（mm / 度），6 个 float
关节角：{ j1..j6 }（度）
坐标系：UF(用户) { x,y,z,w,p,r } + UT(工具) { x,y,z,w,p,r }
数字 IO：1 bit 或 8 bit（按字节打包）
```

**注意**：FANUC 机器人姿态是 **W/P/R**（非 RX/RY/RZ），与 KUKA 的 A/B/C、安川的 RX/RY/RZ 不同 —— 统一抽象层需做转换。

---

## 6. 实现坑

1. **坐标系与姿态表示各家不同**（FANUC W/P/R · KUKA A/B/C · 安川 R/P/Y）——统一抽象层必须显式转换，别直接透传。
2. **运动指令必须考虑运动组/UF/UT 生效顺序**：先设 UF/UT 再发运动。
3. **RMI 需要机器人侧配置 IP 白名单**，否则连接被静默丢弃（无错误码）。
4. **老接口（GI/GO）不占用 RMI 授权**，但需要机器人侧写 KAREL/TP 程序对接 —— 工程上更重但更稳。
5. **`FRC_Abort` 必须在异常路径调用**，否则机器人保持运动状态。
6. **速度倍率（`FRC_SetOverRide`）是全局的**，调试期改动要复位。

---

## 7. 参考实现

| 来源 | 说明 |
|---|---|
| `fanuc_rmi-0.3.1-py3-none-any.whl` | 24 指令的完整 Python 封装（报文构造可直接抄） |
| `fanucpy-0.1.14-py3-none-any.whl` | 另一套 Python 实现 |
| 商业库（.NET，未随本目录提供） | RMI/SNPX/FTP-TP/KAREL 全接口 |
| 商业库对照 | `FanucInterfaceNet`（PC Interface / R-30iB mate plus 实测通过） |
| 参考实现侧 | `/Fanuc/Robot/*` 端点 |
