# 02 · FANUC 机器人（RMI 新接口 + 老接口）实现规格书

> **证据**：🟢 RMI 24 指令（fanuc_rmi/fanucpy 源码）· 🔵 老接口设备码（underautomation-fanuc 反解）· 参考实现侧 `/Fanuc/Robot/*` 端点

---

## 1. 速查

| 项 | 值 |
|---|---|
| 端口 | **TCP 8193**（RMI 与老接口共用；老接口另有 FTP-TP） |
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
