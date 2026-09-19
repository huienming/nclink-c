# 30 · 外部资料：这批数据到底是什么（NC-Link 与西门子）

> 用途：回答"这些数据到底是怎么回事"。这一册只放**外部可查证的出处 + 原文摘录**，
> 内部实测结论在 03（S7）、28（现场 API）、29（模型与驱动）三册。
> 证据分级：🟡 公开（文章/问答）· 🔵 标准（团体标准/官方手册）· 🟢 实测。
> 检索方式：本机 SearXNG（`localhost:8080`）+ `tools/site-probe/websearch.py`。

---

## 1 标准：NC-Link（🔵 T/CMTBA 1008.1…1008.7-2020）

现场那 32 份模型文件、网关的数据项名（`Execution`、`Mode`、`FeedActual`…），
**不是厂商自创的**，而是 NC-Link 这套团体标准里的"数据项"。标准全称
**《数控装备工业互联通讯协议》**（NC-Link），中国机床工具工业协会（CMTBA）
归口，2020-12-01 发布、2021-01-01 实施，共 7 部分：

| 部分 | 名称 |
|---|---|
| T/CMTBA 1008.1-2020 | 通用技术条件 |
| T/CMTBA 1008.2-2020 | 联网参考模型 |
| T/CMTBA 1008.3-2020 | 数控装备模型定义 |
| **T/CMTBA 1008.4-2020** | **数据项定义（= 数据字典）** |
| T/CMTBA 1008.5-2020 | 终端及接口定义 |
| T/CMTBA 1008.6-2020 | 安全性 |
| T/CMTBA 1008.7-2020 | 评价规范 |

`1008.4` 的适用范围原文：**"本部分给出了 NC-Link 数控装备模型中设备对象、组件对象
和数据对象的数据项定义。本部分适用于数控机床、工业机器人、自动搬运车、清洗设备、
检测设备、自动化生产线和自动料库等数控装备。"**
起草单位：华中科技大学、科德数控股份有限公司、中国机床工具工业协会、
武汉华中数控股份有限公司、天津大学、福建省嘉泰智能装备有限公司。

标准给的三层角色与现场交付包**一一对应**：

```
数控装备 → 适配器 → 代理器 → 应用系统
             nclink-service + lib*.so 插件    hp2x_box200 (:33123)
```

原文（工业互联网产业联盟案例页）："NC-Link 协议体系架构设计为**适配器、代理器和
应用系统**三个部分……适配器负责将从数控设备采集到的数据转换为 NC-Link 协议格式
并发送到代理器……代理器负责适配器与应用系统之间的数据转发。"
同页对"数据项定义"的解释：**"数据项定义也称为『数据字典』，是 NC-Link 数据设计
规范，描述了数据的层次结构和语义表达。"**

出处：

- 标准文本条目（含适用范围、起草单位、7 部分清单）：
  <https://www.bzchaxun.com/view/6053020102000000.html> ·
  <https://www.wendang.net/bz/tb/183077.html>
- NC-Link 标准构成与实施（中国机床工具工业协会稿）：
  <https://www.sohu.com/a/460924601_583949>
- 华中数控项目案例（架构、适配器/代理器定义）：
  <https://www.aii-alliance.org/resource/c334/n1479.html>

> 标准正文（PDF）没有免费全文，`1008.4` 的数据项明细表要么买，要么从设备端
> 反推——**我们走的是后者**：模型文件给类型名，网关二进制给 23 个项，
> 现场报文给取值。

---

## 2 数据项：模型文件与网关两侧的名字

**两侧名字对得上**（🟢 实测）。`cfg/models/s7_ncu.json` 用的是 NC-Link 类型名，
网关 `/S7NCU/*` 的 23 个项是驱动内部的叫法：

| 网关项（`ncu.*Req`） | NC-Link 类型 / 中文名（模型 id） |
|---|---|
| `Execution` | `STATUS` 机床状态 `010302` |
| `FeedOverride` | `FEED_OVERRIDE` 进给倍率 `010303` |
| `FeedSet` | `FEED_SET` 进给设定值 `010304` |
| `FeedActual` | `FEED_SPEED` 进给速度 `010305` |
| `SpeedOverride` | `SPINDLE_OVERRIDE` 主轴倍率 `010306` |
| `PartCount` | `PART_COUNT` 加工件数 `010307` |
| `SpeedSet` | `SPEED_SET` 主轴设定值 `010308` |
| `SpeedActual` | `SPINDLE_SPEED` 主轴转速 `010309` |
| `CycleTime` / `LastRunTime` | `CYCLE_TIME` 循环时间 `010311` / `LAST_RUN_TIME` 上次运行时间 `010312` |
| `PlcType` | `PLCTYPE` PLC 类型 `010313` |
| `S1Load` | `S1LOAD` 主轴参数 `010320`（`LIST`） |
| `NckName` / `NckNo` / `NckVer` | `NCK_NAME` / `NCK_NO` / `NCK_VER` 数控系统 名称/编号/版本 |
| `Program` | `PROGRAM` 主程序名 `01036009` |
| `Alarm` | `WARNING` 报警 `01036012` |
| `ToolNo` | `TOOL_NUMBER` 刀具号 `01036013` |
| `Mode` | `MODE` 模式 `01036014` |
| `CoordinateName` / `CoordinateAbsolute` / `CoordinateRelative` / `CoordinateMachine` | 轴名 `NAME` / 绝对 `ABSOLUTE` / 相对 `RELATIVE` / 机器 `MACHINE`（每轴一组） |

项名可以直接从 Go 二进制里一次抓出来（🟢）：

```sh
python tools/site-probe/gateway_meta.py <现场包>/app1/hp2x/hp2x_box200   # 路由元数据
python - <<'PY'   # ncu 包的 23 个数据项类型
import re; b=open("<现场包>/app1/hp2x/hp2x_box200","rb").read().decode("latin-1")
print(sorted(set(re.findall(r"\*ncu\.([A-Za-z0-9_]+)", b))))
PY
```

得到 `AlarmReq … ToolNoReq` 共 26 个名字，其中 `NCU`（驱动本身）、`CloseReq`、
`TcpReq` 不是数据项，**其余 23 个就是 §5.1 里逐帧列出的那 23 项**。

---

## 3 西门子：两个关键状态量的取值表（🟡 公开问答）

出处：西门子中国"找答案"《如何获取西门子 828D 以及 840Dsl 机床运行状态 cncStatus》
<https://www.ad.siemens.com.cn/service/answer/solve_237967_1044.html>

原文（提问者陈述，问题已被标记"已解决"）：

> 通过 OPC UA 对机床参数进行数据采集，目前已知 **ProgramStatus：程序运行状态
> (1 = 中断　2 = 停止　3 = 运行　4 = 等待　5 = 取消)** 是
> 『`/Channel/State/progStatus[u1]`』；**CNC 当前模式 (0 = JOG　1 = MDI　2 = AUTO)**
> 是『`/Bag/State/opMode[u1]`』……
> 问题补充：机床状态可以通过以下信号组合判断：
> 1. 运行，程序正常运行：`DB21.DBX35.0` 与 `DB21.DBX35.5`
> 2. 待机，程序已选择但未启动：`DB21.DBX35.4` 与 `DB21.DBX35.7`
> 3. 报警，通道 NC 报警 `DB21.DBX36.6` 或 `DB21.DBX36.7`
> 6. 急停，NC 内部的急停激活状态信号 `DB10.DBX106.1`

**旁证（位序表）**：广泛流传的《840D 常用信号表》里，操作方式是**按位**给的，顺序
与上面的枚举不同——PLC→NC（状态组 1）：`DB11.DBB0.0 = AUTOMATIC`、
`.1 = MDA`、`.2 = JOG`、`.3 = TEACH IN`；NC→PLC（状态组 2）：`DB11.DBB6.x` 同序。
（<https://max.book118.com/html/2017/1002/135533668.shtm>）
所以 `opMode` 到底是"枚举 0/1/2"还是"位掩码"——**真机确认前不要写死**。

**权威出处还没拿到**：西门子《SINUMERIK 840D sl NC variables》List Manual
（FC5397-3CP40）里才有 `progStatus` / `opMode` 的官方取值表，目前只能查到目录与
摘要（doc88 / book118 均为付费预览）。要落地到"零风险"，得等一次真机或一本手册。

---

## 4 实测回填与两处更正（🟢）

这一轮的实测（`tools/site-probe/s7ncu_mode_exec_probe.sh`）推翻了两个早先的判断：

1. **更正**：网关二进制里的 `Auto` / `Stopped` / `waiting` **不是**西门子状态表的
   文案——它们来自 Go 运行时（`AutoClose`、`syscall.WaitStatus.Stopped`、
   `gcwaiting`…）。别拿关键字命中当证据。
2. **更正**：`Mode` 不是"4 字节码 → 模式名"，而是"**两项各 4 字节是形状要求，
   取值不影响输出**"：0..13、位模式、文本、大小端、"只改第一项/只改第二项"
   各扫一遍，**全部回 `AUTO`**。这一项在网关里是半成品。
3. **新增**：`Execution` 的请求是**两项** SZL 子项，所以应答也必须回两项——
   用 `S7R:`（参数只声明 1 项）打它必然 `error response`，这就是它一直不通的原因之一。

---

## 5 检索方法（可复用）

```sh
python tools/site-probe/websearch.py "查询词1" "查询词2"   # 需要本机 SearXNG
```

本轮真正见效的关键词：

- `SINUMERIK 840D sl NC variables list manual progStatus opMode`
- `"progStatus" 840D 中断 停止 运行 等待 取消`
- `T/CMTBA 1008.4-2020 数据项定义`（标准号是最强的检索锚点）
- `NC-Link 数控装备工业互联通讯协议 架构 适配器 代理器`
- 反查厂商文档的办法：拿**标准号 / 手册号（FC5397-3CP40）**去搜，比搜功能名有效
