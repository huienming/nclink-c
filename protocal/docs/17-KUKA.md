# 17 · 库卡 KUKA 实现规格书

> **证据**：🟢 **本交付包实际实现 = 那智（Nachi）ASCII 命令**（`nclink-service/lua/lua_mod/kuka_task.lua`
> 与 `kuka_mod.lua` 全文可读）；下面 §2/§3/§4 的 KUKAVARPROXY / EKI / RSI 是
> **官方通道**（本包没用），留作换通道时的参考。
> **定位**：KUKA KRC4 控制器 —— **官方无开放数据接口，靠第三方 KUKAVARPROXY 或 EKI**

---

## 1. 速查

| 通道 | 端口 | 说明 |
|---|---|---|
| **KUKAVARPROXY** | TCP **7000** | 第三方开源程序（KRC4 上运行），提供变量读写 |
| **EKI**（EthernetKRL） | 自定义（XML 配置） | KUKA 官方以太网接口，需写 KRL 程序 |
| RSI | 自定义 | 实时接口（周期 4-12ms，用于力控/视觉引导） |
| 参考实现侧 | `/Kuka/*` 驱动（7000） |

---

## 2. 通道 A：KUKAVARPROXY（推荐）

### 2.1 部署

```
① 在 KRC4 控制器上安装 KUKAVARPROXY（开源，需 KUKA 工程师模式）
② 配置监听端口（默认 7000）
③ 上位机 TCP 连接
```

### 2.2 报文格式（文本协议）

```
请求：<命令> <参数>\r\n
响应：<结果>\r\n
```

| 命令 | 说明 |
|---|---|
| `READ <变量名>` | 读变量（返回类型+值） |
| `WRITE <变量名> <类型> <值>` | 写变量 |
| `READSTATE` | 读控制器状态 |
| `READGLOBAL` | 读全局变量 |
| `GETVAR` / `SETVAR` | 变量操作变体 |
| `READVAL` / `WRITEVAL` | 值操作变体 |

**类型码**：BOOL / INT / REAL / DOUBLE / CHAR / STRING / E6POS（位置结构）

### 2.3 变量命名

```
$POS_ACT      当前笛卡尔位置（E6POS 结构，12 个字段）
$AXIS_ACT     当前轴角
$OV_PRO       速度倍率（0-100）
$MODE_OP      操作模式
$ALARM_STOP   急停状态
$PRO_STATE1   程序状态
$TIMER[1]     定时器
```

---

## 2.5 本交付包实际实现：那智（Nachi）ASCII 命令 🟢 2026-09

交付包的 `lua/lua_mod/kuka_task.lua`（5.4 KB，纯 Lua，可直接读）里，KUKA 这一路
**实际走的是那智（Nachi）机器人的 ASCII 命令**——`nclink-service` 侧复用
`/Foxconn/Chengdu/Robot/Nachi/*` 这条 HTTP 路由，把结果按 `KUKA/*` 的键塞进共享内存，
再由 `kuka_mod.lua` 映射成模型项。也就是说：**这一家的"设备侧"是行文本协议**。

```
连接：POST /Foxconn/Chengdu/Robot/Nachi/Open/TCP   {ipAddress, port, timeout}
读值：POST /Foxconn/Chengdu/Robot/Nachi/GetResponse {connectionId, data:"XH\r\n"}
      → data.value = 一行文本，字段用逗号分隔
```

任务循环里依次发 4 条命令（每条 `\r\n` 结尾），失败就 Close + 重连：

| 命令 | 字段（逗号分隔，下标从 1 开始） | 映射到 |
|---|---|---|
| `XH` | [1] 型号 · [2] 控制器版本 | `/TYPE` · `/CONTROLLER/VERSION` |
| `JSL` | [1] CT · [2] WT · [3] 总工件数（取整）· [4] NG（取整）· [5] 线体状态 · [6] 工位状态 · [7] 料架状态 | `/VARIABLE@CT` `/WT` `/TOTAL_PART_COUNT` `/NG` `/WITH_LINE_STATUS` `/WITH_STAGE_STATUS` `/WITH_MAG_STATUS` |
| `TPM` | [1] 状态 · [2] 报警号（`"0"` = 无报警）· [3..8] 轴 0..5 马达位置 · [9] 当前程序名（去掉 `\r\n`） | `/STATUS@FSK` `/CONTROLLER/WARNING` `/AXIS@N/MOTOR/POSITION` `/CONTROLLER/PROGRAM` |
| `JK` | [1..6] 轴 0..5 马达电流 · [7..12] 轴 0..5 马达力矩 · [13] 自动倍率（取整） | `/AXIS@N/MOTOR/CURRENT` `/TORQUE` `/VARIABLE@AUTO_OVERRIDE` |

复现要点：

1. 四个命令在**同一个 TCP 会话**里轮流发，每条一个请求-应答；
2. 应答是**一行文本**（逗号分隔），驱动只做 `tonumber` / `math.floor`，没有二进制结构；
3. 报警：`TPM` 的第 2 个字段不为 `"0"` 才算一条报警（`number` = 该字段、`text` 恒空）；
4. 位置/电流/力矩的**轴序**就是 `AXIS@0..5`，与 KUKA 的 A1..A6 对应关系要现场核一次。

---

## 3. 通道 B：EKI（官方）

```
原理：KRC 侧运行 KRL 程序，用 EKI 函数收发字符串
报文：XML（可自定义 schema）
配置：Config.xml（连接方向、端口、超时）
```
**优势**：官方支持、稳定。
**劣势**：需改写机器人程序 + 工程投入。

---

## 4. 通道 C：RSI（实时）

```
周期：4-12ms（硬实时）
用途：力控、视觉引导、外部轴同步
报文：XML（结构固定）
```
**仅用于实时闭环**，不适用于常规数据采集。

---

## 5. 实现坑

1. **KUKA 无官方数据采集接口** —— 生产环境必须走 KUKAVARPROXY（第三方，需客户接受）或 EKI（需改机器人程序）。
2. **KUKAVARPROXY 有并发限制**，且非官方支持 —— 掉线要能自恢复。
3. **`$POS_ACT` 是结构体**（X/Y/Z/A/B/C/S/T 共 12 字段），解析要按 E6POS 布局。
4. **写变量可能触发机器人动作**（如写 `$OV_PRO` 改变速度）—— 必须白名单。
5. **姿态表示 A/B/C**（绕 Z/Y/X 的欧拉角）与 FANUC W/P/R、安川 RPY 语义不同，跨品牌转换必须显式处理。
6. **KRC2 与 KRC4 差异大**（老系统用 CrossComm/其它第三方）。

---

## 6. 参考实现

| 来源 | 说明 |
|---|---|
| 商业库 | `KukaAvarProxyNet`（端口 7000）· `KukaTcpNet` |
| 开源 | KUKAVARPROXY（GitHub，KRC4 上用） |
| 参考实现侧 | `/Kuka/*` 驱动 |
