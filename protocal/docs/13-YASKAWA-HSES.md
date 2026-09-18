# 13 · 安川 Yaskawa（HSES）实现规格书

> **证据**：🟢 HSES 驱动实测 + `underautomation-yaskawa 2.2`（173 方法反解）+ 商业库 `YRCHighEthernet`
> **定位**：安川机器人（YRC1000 / DX200 / FS100）高速以太网接口

---

## 1. 速查

| 项 | 值 |
|---|---|
| 端口 | **UDP 10040**（High Speed Ethernet Server，HSES 默认） |
| 传输 | **UDP**（无连接，需自处理丢包/重传） |
| 字节序 | 小端 |
| 变体 | HighEthernet（UDP 10040，快）· EthernetServer（TCP，慢但可靠）· 老 HSES |
|参考实现| `underautomation-yaskawa`（.NET）· 商业库 `YRCHighEthernet` |
| 参考实现侧 | `/Yaskawa/*` 驱动（10040 UDP） |

---

## 2. 连接建立

```
① 机器人侧启用 High Speed Ethernet Server 功能（维护模式 → 选项功能）
② 无连接握手（UDP）
③ 发命令帧 → 收响应帧
④ 保活：定期读状态寄存器
```

**UDP 要点**：需自己做**超时重传**（建议 3 次，间隔 50-100ms）+ 序号校验防乱序。

---

## 3. 帧格式（HSES）

```
偏移  长度  内容            说明
0     1    0x01            数据区标识（0x01 = 单区）
1     1    0x00            保留
2     1    命令号           见 §4
3     1    实例号           递增
4     1    属性            0x00
5     1    服务             0x01 = 读，0x02 = 写
6     2    起始地址（小端）  数据区地址
8     2    数量（小端）      读写元素个数
10    N    数据（写时）      读时无
```

**数据区标识**：`0x01`=单区 · `0x02`=多区（可一次读多个不同区）

---

## 4. 数据区与命令

### 4.1 常用数据区

| 区 | 说明 |
|---|---|
| `S` | 系统寄存器（状态/模式/报警） |
| `I` | 输入（IO） |
| `O` | 输出（IO） |
| `B` | B 寄存器（字节） |
| `D` | D 寄存器（机器人数据，**常用**） |
| `R` | R 寄存器 |
| `C` | C 寄存器（IO 状态） |
| `M` | M 寄存器（机器人内部） |
| `P` | P 变量（位置） |

### 4.2 API 能力（173 方法摘要，按域）

| 域 | 方法（节选） |
|---|---|
| 连接 | `Connect` / `Disconnect` / `IsConnected` |
| 状态 | `GetStatusInformation` / `GetSystemInformation` / `GetConfigurationInformation` |
| 报警 | `GetAlarm` / `GetAlarmExtended` / `AlarmReset` / `AlarmResetType` |
| 位置 | `GetRobotPosition` / `GetRobotCartesianPosition` / `GetRobotJointPosition` |
| 伺服 | `GetTorque` / `GetPositionError` |
| 作业 | `GetExecutingJobInformation` / `GetJobStack` |
| 变量 | `ReadVariable`（格式：区+编号+类型） |
| IO | `ReadIO`（IOUSV PYOQ 区码） |
| 运动 | `MoveCartesian` / `MoveJoints`（+ 坐标类型/操作坐标/命令类型） |
| 文件 | `GetFile` / `GetFileList` / `GetFileProgressDelegate` |
| 时间 | `GetManagementTime` / `GetCreationTimeUtc` |

---

## 5. 变量读写格式

```
格式：<区码><编号>
  区码：B(字节) I(整型) D(双精度) R(实数) S(字符串) P(位置) BP/BI/… （位）
  例： "I000"（整型变量 I0）· "D010"（D 寄存器 10）· "M010"
数据类型：BYTE/INTEGER/DOUBLE/REAL/STRING/POSITION
```

---

## 6. 实现坑

1. **UDP 无连接** —— 必须实现超时重传 + 序号校验，否则丢包表现为"数据跳变"。
2. **数据区标识与地址不通用**（`I` 是 IO 区还是整型变量，取决于上下文）—— 按 API 语义区分。
3. **高频采集建议**：坐标类 50-100ms（UDP 快），报警/状态 500ms。
4. **写运动指令风险极高**（`MoveCartesian`）—— 生产环境禁写。
5. **机器人姿态**：安川是脉冲/RPY 表示，与 FANUC 的 W/P/R 语义不同，跨品牌抽象需转换。
6. **老 HSES 与 HighEthernet 报文不同**（老接口走 TCP，报文格式差异大）—— 按控制器型号分支。

---

## 7. 参考实现

| 来源 | 说明 |
|---|---|
| 商业库（.NET，未随本目录提供） | 173 方法，已抽出 77 个 API 名记录在本册 |
| 商业库 | `YRCHighEthernet`（**UDP 10040，与参考实现同端口**）+ `YRC1000TcpNet` |
| 参考实现侧 | `libhsr3` 系（华数）+ 安川 HSES 驱动 |
