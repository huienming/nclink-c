# 18 · 埃夫特 Efort 机器人实现规格书

> **证据**：🟢 **本交付包实际实现 = Modbus TCP**（`lua/lua_mod/efort_task.lua` 给了完整寄存器表，
> 见 §2.1）· 🟡/🔵 ER7B-C10 定制协议（`efort_driver.json`，仅 Modbus 路径激活）+ 商业库 `ER7BC10`
> （新旧两版报文）——定制协议本包**没用**，留作参考。

---

## 1. 速查

| 项 | 值 |
|---|---|
| 型号 | **ER7B-C10**（对应协议定制版） |
| 传输 | TCP（Modbus 或定制协议） |
| 两版报文 | **新版（对齐）** / **旧版（未对齐）** —— 必须按固件版本选 |
|参考实现| 商业库 `ER7BC10`（新）· `ER7BC10Previous`（旧） |
| 参考实现侧 | `efort_driver.json`（**仅 Modbus 路径激活**，httphandler 缺失） |

---

## 2. 通道 A：Modbus（参考实现实际使用）

```
① TCP connect(ip, 502)
② 标准 Modbus 功能码读写
③ 地址映射按埃夫特《Modbus 地址表》
```

**参考实现侧现状**：`efort_driver.json` 只定义了 Modbus 连接，**没有 httphandler** —— 即参考实现对埃夫特的 HTTP 端点未实现，只有 Modbus 一条路可用。

### 2.1 地址表：从 `lua/lua_mod/efort_task.lua` 读出来了 🟢 2026-09

交付包的 Lua 驱动层把这一路写得明明白白（5 KB，纯 Lua），**不用再向厂商索取地址表**：

```lua
open("/Modbus/Open/TCP", {ipAddress, port, timeout=15, slaveTd=1})
req("/Modbus/Function", {connectionId, functionCode=3, address=44, quantity=2})
req("/Modbus/Function", {connectionId, functionCode=3, address=38, quantity=1})
```

| 项 | 值 |
|---|---|
| 传输 | Modbus **TCP**，单元号 **1** |
| 功能码 | **3**（读保持寄存器） |
| 寄存器 **44..45** | 机器人状态/CT，**两寄存器拼成一个 32 位大端 float**：`(reg45 << 16) | reg44`，再按 IEEE754 大端解析 |
| 寄存器 **38** | 产出计数（取整） |

驱动的判定逻辑（同一份 Lua）：

```
floor(值) == 1 → 等待（记 waitCT）
floor(值) == 2 → 运行（记 runningCT）
```

即：**44/45 那个 float 的整数部分就是状态码**（1 = 待机、2 = 运行），小数部分是
该状态的计时；38 是当班/当件产出。要仿真一台埃夫特/该工位，按这两条读法回
Modbus 应答即可（`tools/site-probe/mock.py` 的裸 `hex` 应答够用）。

---

## 3. 通道 B：ER7B-C10 定制协议

| 版本 | 特征 |
|---|---|
| **新版** | 报文**对齐**（字段按 4 字节边界） |
| **旧版** | 报文**未对齐**（紧凑排布） |

**分界**：以机器人固件版本判断（商业库 用两个类区分）。

---

## 4. 实现坑

1. **新旧版报文不兼容** —— 必须先探测固件版本，或做双解析回退（先按新版解析，校验失败再按旧版）。
2. ~~Modbus 地址表缺失~~ → 已从 `efort_task.lua` 解出（§2.1）：FC3 读 44-45（状态/CT 的
   大端 float）与 38（产出计数）。**只剩**该表在 ER7B-C10 固件上是否随版本变动要核对一次。
3. **参考实现未实现 HTTP 端点** —— 若要与参考实现行为一致，只需实现 Modbus。
4. **国产机器人协议不公开** —— ER7B-C10 的定制协议需从 商业库 实现中反推（其类为混淆 DLL，仅作参考）。

---

## 5. 参考实现

| 来源 | 说明 |
|---|---|
| 商业库 | `ER7BC10` / `ER7BC10Previous`（新旧两版报文实现，可作格式对照） |
| 参考实现侧 | `驱动定义目录/efort_driver.json`（Modbus 连接定义） |
| ~~待补~~ | ~~埃夫特 Modbus 地址表 + ER7B-C10 协议文档~~ —— **2026-09 清账**：地址表已在 §2.1 从
`lua/lua_mod/efort_task.lua` 解出（FC3 / 寄存器 44-45 大端 float + 38），本包 **不使用**
ER7B-C10 定制协议；该协议只在"直连 ER7B-C10 控制器"时才需要，属外部资料 |
