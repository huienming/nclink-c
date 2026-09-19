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

帧结构（从这 11 条推出的形状）：

```
93 | 长度(2B 大端) | 会话+序号（同一连接内各帧相同） | 命令 | 参数 | 保留 | 55 aa
```

- `93` 帧头、`55 aa` 帧尾；**长度**数的是"帧头之后、帧尾之前"的字节数
  （`93 00 0a 00 …` 共 16 字节 → 0x000a = 10）。
- `6f c8 1e 64 1e 17 10 17` 在一次会话里各帧相同（会话标识 + 计数器），
  跨会话会变。
- 命令字节：`0x11` 状态、`0x16` 加工计数、`0x12` 程序名、`0x23` 行号、
  `0x17` 刀具号、`0x81` 报警，`0x1a` + 子码（`01` 进给速度、`02` 进给倍率、
  `04` 主轴转速、`05` 主轴倍率、`06` 快移倍率）。
- **应答还没拿到**：把请求原样回给网关会被判 `unexpected response command`，
  应答里的"命令"要按它的规则构造（大概率是命令字节带方向位，或应答位独立成
  字节）。这一格用同一个 `gateway_probe.sh` 迭代即可——网关的报错就是判据。

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
