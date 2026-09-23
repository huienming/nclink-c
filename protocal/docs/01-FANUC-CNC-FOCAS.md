# 01 · FANUC CNC（FOCAS / Fwlib32）实现规格书

> **证据**：🟢 全实证 —— `libfwlib32.so.1` 导出 **881 个函数**（cnc 758 / pmc 95 / flnt 14 / ds 8 / pbm 3 / hmgr 1 / smgr 1 / loglevel 1）
> **定位**：FANUC 全系（0i/16i/18i/21i/30i/31i/32i）标准数据接口

---

## 1. 速查

| 项 | 值 |
|---|---|
| 端口 | **TCP 8193**（FOCAS 以太网；部分系统 8192） |
| 库 | `Fwlib32.dll`（Win）/ `libfwlib32.so.1`（Linux）/ `fwlib32.x64.dll` |
| 函数前缀 | `cnc_*`（CNC 数据）· `pmc_*`（PMC/梯形图）· `flnt_*`（Focas2 Logger）· `ds_*`（数据服务）· `pbm_*`（程序块管理） |
| 前置条件 | 机床侧需开通"以太网功能"并授权 |
|参考实现| `pyfocas`（ctypes 绑定）· `pyfanuc`（含 fwlib 头文件） |
| 完整表 | 881 函数原名（原始素材未随本目录提供） |

---

## 2. 连接建立

```c
// ① 分配句柄（最多 2 个并发句柄）
unsigned short h;
cnc_allclibhndl3("192.168.1.10", 8193, 10 /*秒*/, &h);

// ② 校验连接与系统信息
ODBSYS sys;  cnc_sysinfo(h, &sys);        // 系统型号/系列/轴数
ODBMDL mdl;  cnc_rdmodel(h, &mdl);        // 机床型号

// ③ 业务调用
ODBACT pos;  cnc_absolute(h, 1 /*X轴*/, &pos);      // 绝对坐标
ODBACT2 p2;  cnc_absolute2(h, 1, &p2);
ODBST  st;   cnc_statinfo(h, &st);                  // 运行状态

// ④ 释放
cnc_freelibhndl(h);
```

### 2.1 线协议握手（🟢 实测，非文档）

用现场交付包里的 `libfwlib32.so` 在 ARM 仿真环境里对着假机床跑
`cnc_allclibhndl3("127.0.0.1", 8193, 3)`，抓到的**设备侧**字节序列如下
（复现工具：`tools/site-probe/`）：

```
连接 1 →  a0 a0 a0 a0 00 01 01 01 00 02 00 01     (12 字节)
连接 2 →  a0 a0 a0 a0 00 01 01 01 00 02 00 02     (12 字节，计数器 +1)
          a0 a0 a0 a0 00 01 21 01 00 00           (10 字节)
          a0 a0 a0 a0 00 01 02 01 00 00           (10 字节)
```

> **2026-09 更正（见 §10.4）：这两条连接不是"重试"。** 上面那段当时是**对着假机床**抓的，
> 机床一条都不回，于是把"两条连接"读成了 SDK 的重试；真机（应答齐全的那台）上，
> 官方 SDK **照样开两条**：第一条是控制通道（hello 计数器 1，只发 hello），第二条是
> 数据通道（计数器 2，命令全走这条）。分开的判据见 §10.4。

- 没有应答时 SDK 返回 **-16（EW_SOCKET）**，并且**会重试一次** —— 这只解释"为什么会
  出现两组 hello"，不解释"为什么计数器是 1 和 2"（重试的话两条都是同一段代码，
  计数器不会自己 +1）。
- 应答格式（🟢 2026-09 第九/十轮，反汇编 `libfwlib32.so` 的 `Pdu::send`/`Pdu::receive`/
  `getRbPos`/`getRb` + 假机床实测）：帧格式与四道判据见 §3.1，
  **应答体取值见 §3.2** —— 第十轮已把"体 = 块个数 + 变长块"这套结构解出来并实测
  `cnc_allclibhndl3` **rc=0**，本册**不再需要真机**。
- 现场（`cfg/driver_def.json`）用的就是这一家：`module: focas`、`8193`。

---

## 3. 帧格式

FOCAS 的报文是 `Fwlib32` 内部那套 **PDU**（请求头 + 业务码 + 载荷），一收一发就是
"10 字节头 + 体"。下面三节分别是：帧与四道判据（3.1）、**应答体怎么取值**（3.2，
这一节是本地闭环的关键）、每个 API 用的 **item 码**（3.3）。

### 3.1 帧格式与校验规则（🟢 2026-09 第九轮反汇编）

**帧 = 10 字节头 + 体**，头和体都是"大端 u16 字段"的堆叠：

```
[0..4)   a0 a0 a0 a0          魔数（Pdu::receive #23fbc 做 32 位字节交换后跟
                              字面量 0x24ac0 = a0 a0 a0 a0 比）
[4..6)   be16 类型            请求恒 00 01；应答决定体长上限（见下表）
[6]      功能码               Pdu::send 写调用方给的"功能/方向"字节；
                              Pdu::receive 要求它 == 调用方期望值（0 = 不检查）
[7]      方向                 请求恒 01；应答必须 1..4（#25230）
[8..10)  be16 体长度（字节）
[10..)   体
```

`Pdu::send`（`0x234b8`）写头就是：magic、`[4]=0 [5]=1`、`[6]=func`、`[7]=dir`，
然后 `[8..10)=10+现有体长`。请求侧能一一对上：

| 抓到的帧 | 功能码 | 体 |
|---|---|---|
| `a0a0a0a0 0001 01 01 0002 0001` | `01` | `00 01`（2 字节） |
| `a0a0a0a0 0001 21 01 0000` | `21` | 无 |
| `a0a0a0a0 0001 02 01 0000` | `02` | 无 |

**`Pdu::receive`（`0x23a94`）的四道校验**（任一条不过就抛 `ErrObj(-17)` =
`EW_PROTOCOL`，SDK 层看到的就是 **rc = -17**）：

1. `#23fe0`：magic 必须是 `a0 a0 a0 a0`；
2. `#24488`：`header[6]` 必须等于调用方传入的期望功能码（传 0 则不查）；
3. `#25230`：`header[7] ∈ 1..4`；
4. `#25600`–`#25634`：**体长上限**——`be16(header[4..6)) <= 2` 时 1450 字节、
   `== 3` 时 2910、`> 3` 时 3470，`be16(header[8..10))` 不能超；
5. `#25ecc`–`#25f40`（`header[7]==2` 且 `header[6]==1` 时）：**体长必须是
   `n*8 + 16`**（`n = be16(体[8..10))`，type ≤ 2）或 `n*32 + 40`
   （`n = be16(体[20..22))`，type > 2）；
6. `#2601c`–`#26120`（`header[6]==0x21` 时）：体长必须 **> 1**，且
   `be16(体[0..2)) != 0`。

**应答块（rb）**：体是按 32 字节切的一串块，`Pdu::getRb(i)`（`0x26b78`）取第 i 块，
块内 `[8..10)` = 返回码（大端 int16），**非 0 直接抛异常**
（`ErrObj(code, detail1=块[10..12), detail2=块[12..14), ecode=块[2..4))`）。
所以"机床说 OK" = 这些块的 `[8..10)` 全 0。

**谁要应答**：`SockPair::request(Socket&, int, Pdu&, u8 dir)`（`0x1b3a0`）——
`dir == 2` 才轮询收应答（并要求应答 `[6] == 2`），否则只收一次、要求应答
`[6] == dir`。握手那四条都是 `dir = 1`。

**假机床实测（`focas_handshake_probe.sh`）**：给这些帧回
`a0a0a0a0 0001 01 02 0010 <16 字节体>`（体 `[8..10)=0`，正好满足第 5 条），
`cnc_allclibhndl3` 从 **-16（EW_SOCKET，什么都不回）** 变成
**-17（EW_PROTOCOL）**——**帧被收下了**，只剩应答体字段值没对上。复盘表：

| 试法 | rc |
|---|---|
| 不回任何应答 | -16（socket/超时） |
| 四个请求一律回 16 字节体 | -17，且**驱动只发 2 条请求**（`func 21` 那步没了） |
| 系统信息 `[2..4)=2` + `func 21/02` 回空体 | -17（`func 21` 的应答体长必须 > 1，见第 6 条） |
| 只回 `func 01` 那两条 | -16（说明其余请求也在等应答） |
| 类型 `[4..6)` 改 `0002`（其余同 A） | -16 |
| 方向 `[7]` 改 `01`（其余同 A） | -16 |

结论：**格式与判据已经全解**，剩下的是"应答体里那 16/32 字节放什么"——
**这一格 2026-09 第十轮已经补上**，见 §3.2（不需要真机了：按解出来的块结构回，
`cnc_allclibhndl3` 实测 **rc=0**）。

### 3.2 应答体的取值（🟢 2026-09 第十轮：反汇编 + 假机床实测 rc=0）

**体不是一块连续数据，而是一串"块"。** `Pdu::getRbPos`（`0x26ab0`）就是靠块自己的
长度字段往后走的：

```
体
  [0..2)   块个数 N（BE16）—— Pdu::getRbPos 的第一道检查：i 必须 < N
  [2..)    N 个块，逐个首尾相接：
             [0..2)   本块字节数 L（BE16，getRbPos 用 i 走到第 i 块就是靠它累加）
             [2..4)   ecode
             [8..10)  返回码（BE16，**非 0 → Pdu::getRb 抛 ErrObj**）
             [10..12) detail1      [12..14) detail2
             [14..16) 载荷字节数（数据块才有意义）
             [16..)   载荷
```

**三条硬规则（全部实测）**：

1. **块个数 = 请求里 Cb 的个数**。请求体的 `[0..2)` 就是这个数，应答必须一致——
   少一个，驱动取 `getRb(最后一个)` 时 `N <= i` 直接抛 → 上层看到 **-17**。
   无体的 10 字节请求（0 个 Cb）按 **1** 个块回（`Pdu::receive` 判据 6 也要求
   `0x21` 的应答块个数非 0）。
2. **块长 ≥ 34 字节**：`SockPair::system_info_v1` 要读块 `[16..34)`，
   `cnc_statinfo` 要读块 `[14..16)` 与 `[16..]`。
3. 每个块的 `[8..10)` 必须是 0，否则 `getRb(i)` 抛异常。

**func 01 的应答是另一套布局**（不是块，是"8 字节记录表"）：

```
[0..2) → this+116      [2..4) → this+118（==2 / ==3 走特殊分支）
[4..6) → this+122/144  [6..8) → this+124
[8..10) = 记录数 n     [10..12) [12..14) [14..16) → this+138/146/148
[16..)  n 个 8 字节记录（每条 4 个 BE16：A/B/C/D）
```

长度必须 `== 16 + 8n`（`Pdu::receive` 判据 5）。记录 A != 0 时驱动会为它加一个
`Cb(1, i, 24)` 并 `getRb(顺序号)`，所以"记录数 / 记录 A"要和后面的 Cb 个数对上。
`this+118 == 3` 时分支里会**段错误**（驱动自身的坑，实测必崩），填 0 或 2 都能走通。

**实测结果**（`tools/site-probe/focas_data_probe.sh`；`mock.py` 新增了 `CBREP:` 规格，
按请求的 Cb 个数自动生成同样多个块）：

| SDK 调用 | 结果 |
|---|---|
| `cnc_allclibhndl3` | **rc=0**，handle=32769 |
| `cnc_statinfo` · `cnc_rdcount` · `cnc_actf` · `cnc_acts` · `cnc_rdparam` · `cnc_rdtofs` · `cnc_exeprgname2` · `cnc_rdlife` | **rc=0** |
| `cnc_rdaxisdata` / `cnc_rdalmmsg2` / `cnc_rdexecprog` / `cnc_rdblkcount` | rc=1/2/6/6 —— 假机床给的是全零载荷，形状不够，不是协议不明 |
| `cnc_rdmacro` | 载荷长度 0 时驱动段错误（真机有真实长度，不会） |
| `cnc_machine` | 探针的调用签名不对（少一个 axis 参数），rc=4，与协议无关 |

> ⚠️ **"rc=0" 只说明形状能给 SDK 用，不等于字段布局已核**：本节坐实的只有四项——
> `STATINFO` 的 ODBST 拆分、`ACTF`/`ACTS` 的 float 数组、`RDCOUNT` 计数器、
> `EXEPRGNAME2` 的名字。`RDLIFE` / `RDPARAM` / `RDMACRO` / `RDTOFS` / `RDPROGDIR3`
> 在假机床里回的是全零载荷，**哪几个字节是哪个字段还没核**（31 册 §1 #7）；
> 我们的适配器（`adapters/README.md` 的 FANUC 一节）因此**默认不把它们写进点表**，
> 要用就自己在 `conf/fanuc.json` 里加一条、并且先别开采样（`"sample": false`）。

**数据是怎么切的（以 `cnc_statinfo` 为例，反汇编 `0x27a38`，110 行读完）**：

```
Cb(1, hd[0x8c0], 25) → addCb        Cb(1, hd[0x8c0], 225) → addCb
Cb(1, hd[0x8c0], 152) → addCb       Handle::request(hd, pdu, 0x21)
st[0] = 0; if (getRbCode(1) == 0) st[0] = be16(块1[16..18))
st[2] = 0; if (getRbCode(2) == 0) st[2] = be16(块2[16..18))
n = be16(块0[14..16)) / 2 ; 从 块0[16..] 拷 n 个 u16 到 st[4..]     ← getRb(0)
```

对上 `ODBST { dummy, aut, manual, run, edit, motion, mstb, emergency, alarm,
spindle, oper }`：**块 1 = dummy、块 2 = aut、块 0 的载荷 = manual…oper 这 9 个
u16**。也就是"标量各占一个块、数组走块 0 的载荷"——这就是 FOCAS 应答的通用切法。
（块 1/2 的返回码非 0 只是把该字段填 0，**不抛异常**；块 0 的返回码非 0 才抛。）

**Cb 码表**（每个数据项发的就是**一条** func `0x21` 请求，`c` 字段是它的码）：

| SDK 调用 | 请求条数 | Cb 的 `c` | d/e 取值 |
|---|---|---|---|
| 握手 `system_info_v1` 第 3 条 | 1 | `0x0e` | d=e=`0x26f0` |
| `cnc_statinfo` | 3 | `25` / `225` / `152` | 0 |
| `cnc_rdcount` | 1 | `0x8b`(139) | d=e=1 |
| `cnc_rdlife` | 1 | `0x8b`(139) | d=e=1 |
| `cnc_actf` | 1 | `0x24`(36) | 0 |
| `cnc_acts` | 1 | `0x25`(37) | 0 |
| `cnc_rdparam` | 1 | `0x0e`(14) | d=e=1，载荷含 `40 7f fc 90` |
| `cnc_rdmacro` | 1 | `0x15`(21) | d=e=1 |
| `cnc_rdtofs` | 1 | `0x08`(8) | d=e=1，另带 `0x78`(120) |
| `cnc_rdprogdir3` | 1 | `0x06`(6) | d=`0x13`, e=1 |
| `cnc_exeprgname2` | 1 | `0xfc`(252) | 0 |
| `cnc_rdaxisdata` `cnc_rdalmmsg2` `cnc_rdexecprog` `cnc_rdblkcount` | 0 | — | 本地先失败，没进到协议层 |

**这家实际用到的 29 个 SDK 调用**（`libfocas.so` 的导入表，🟢）：

```
cnc_allclibhndl3 cnc_freelibhndl cnc_statinfo cnc_machine cnc_rdcount
cnc_rdaxisdata cnc_rdparam cnc_rdtofs cnc_rdtofsinfo cnc_rdmacro
cnc_rdalmmsg2 cnc_rdlife cnc_rdexecprog cnc_exeprgname2 cnc_actf cnc_acts
cnc_rdblkcount cnc_rdprogdir3 cnc_pdf_slctmain cnc_pdf_del cnc_download4
cnc_dwnstart4 cnc_dwnend4 cnc_upload4 cnc_upstart4 cnc_upend4
cnc_startupprocess cnc_exitprocess cnc_rdparam
```

---

### 3.3 item 码表（🟢 2026-09 用**官方 SDK** 逐条问出来的）

§3.2 那十二个码是"反汇编 + 假机床"解的。2026-09 拿到 FANUC 官方 SDK 包（`Fwlib64.dll`
+ 官方头 `Fwlib64.h` + 每函数一页的 `Document/SpecE/*.xml` + 手册 `FWLIBPM.TXT`），
方法换成**"官方库自己对着一台假机床跑"** —— 那条调用发什么帧、收什么载荷，直接看得见：

```
python tools/site-probe/focas_sdk_mock.py 8193 --size 0x40 --ramp   # 假机床（可辨识载荷）
tools/site-probe/focas_sdk_probe.exe 127.0.0.1 8193 cnc_rdprgnum     # 探针：印 rc/出参/Cb 码
tools/site-probe/focas_sdk_probe.ps1 -Dll <Fwlib64.dll 所在目录> -Calls "…"   # 一次跑一串
```

要点（两次踩坑记下来）：

1. `Fwlib64.dll` 是**前端**，同目录还要有 `fwlib30i64.dll` 等按机型的实现，否则
   `cnc_allclibhndl3` 直接回 **-15 `EW_NODLL`**（一个字节都不发）。
2. 好几个调用对"数据块长度/条数"查得很严：`cnc_absolute` 的第二个 short 是
   **length**（给 0 回 `EW_ATTRIB`=4）、`cnc_rdposition`/`cnc_rdalmmsg2` 的
   `short *num` 给 0 回 `EW_LENGTH`=2 —— 都是**本地就拒**，不发帧。
3. 假机床的载荷用"斜坡"（第 i 块第 j 字节 = `i*16+j`）或"可辨识组"（0x1111/0x2222…），
   出参结构里哪个字段落在哪个偏移**一眼就看出来**。

**核出来的码（`c` = Cb 的 item 码；d/e = 随块发的两个 long）**：

| SDK 调用 | c | d / e | 应答（载荷里怎么切） | 状态 |
|---|---|---|---|---|
| `cnc_statinfo` | 25 / 225 / 152 | 0 | 块1→dummy、块2→aut、块0 载荷→其余 9 个 u16 | 🟢 已进 client |
| `cnc_actf` | 0x24 | 0 | 每轴一个 **float32**（第 k 轴 @4k） | 🟢 已进 client（进给速度 F） |
| `cnc_acts` | 0x25 | 0 | 每个主轴一个 float32 | 🟢 已进 client（主轴转速 S） |
| `cnc_absolute` / `cnc_machine` / `cnc_relative` / `cnc_distance` | 0x26 | **d = 0/1/2/3**，e = 轴号或 `-1`(ALL_AXES) | ODBAXIS（dummy/type/data[]），**切法待核** | 🟡 码已核 |
| `cnc_rdposition` | 0x26 ×4 | d = 0..3，e = -1 | 同上（一次四条，四种位置） | 🟡 码已核 |
| `cnc_srvdelay` | 0x26 | **d = 9**，e = -1 | 每轴一条 **8 字节记录**：值 = 记录第 0 个 int32（BE32）；**后 4 字节官方库不读**，小数位走 `cnc_getfigure`（= 该轴 `POSELM` 的 `dec`） | 🟢 已进 client（跟踪误差；`POSITION@CMD` = 实际 − 这一条，见 §10.1.3） |
| `cnc_rdprgnum` | 0x1c | 0 | 载荷 **@2 运行程序号（BE16）**、@6 主程序号 | 🟢 已进 client |
| `cnc_rdseqnum` | 0x1d | 0 | 载荷 **@0 顺序号（BE32）** | 🟢 已进 client |
| `cnc_rdcount` | 0x8b | **0 / 0** | `ODBTLIFE3`：`datano` **@2**、件数 **@20**（BE32）—— 见 §10.2 | 🟢 已进 client（原来写成 1/1，是寿命那一支；值也从 @0 改到 @20） |
| `cnc_rdlife` | 0x8b | **1 / 1** | `ODBTLIFE3`：`datano` **@2**、寿命 **@12**（BE32）—— 与件数**不是一个偏移**，别串用 | 🟢 形状已核（client 侧还没挂点位） |
| `cnc_alarm2` | 0x1a | 0 | 载荷 **@0 报警状态位（BE32）**，0 = 无报警 | 🟢 已进 client |
| `cnc_rdngrp` | 0x4a | 0 | 载荷 **@0 刀具组数（BE32）** | 🟢 已进 client |
| `cnc_rdtimer` | 0x120 | d = 类型（0 通电/1 运行/2 切削/3 循环/4 自由） | 载荷 **@0 分钟、@4 毫秒（BE32）** | 🟢 已进 client |
| `cnc_rdalmmsg2` | 0x23 | d = 报警类型（-1 = 全部），e = 条数 | ODBALMMSG2 数组（编号/类型/轴/文本 64B） | 🟡 码已核 |
| `cnc_rdsvmeter` | 0x56 + 0x89 | d = 1 | LOADELM 数组（伺服负载） | 🟡 码已核 |
| `cnc_rdspmeter` | 0x40（d=4 负载 / 5 转速）+ 0x8a | e = -1 | LOADELM 数组 | 🟡 码已核 |
| `cnc_rdblkcount` | 0x35 | 0 | 载荷 **@0 的 BE32**（原来的"不是 @0"是错的，§10.2 反查出来的） | 🟢 已核（原来表里记成 0x06，是错的） |
| `cnc_rdopmode` | 0x57 | 0 | short 数组（主轴调整模式） | 🟡 码已核 |
| `cnc_exeprgname2` | 0xfc | 0 | 程序名文本 | 🟢 已进 client |
| `cnc_rdprogdir3` | 0x06 | d = 0x13，e = 1 | PRGDIR3 数组 | 🟡 帧有了、字段待核 |
| `cnc_rdtofs` | 0x08 | d = 形状（0 磨耗/1 形状…）、e = 编号 | `ODBTOFS` 的 `data` = 载荷 **@0 的 BE32**（`datano`/`type` 是请求回显） | 🟡 值的位置已核 |
| `cnc_rdparam` | 0x0e | d = 参数号、e = 轴号 | `IODBPSD`：`datano` **@2**、`type` **@4**、`ldata` **@8**（BE32） | 🟡 字段位置已核 |
| `cnc_rdmacro` | 0x15 | d = 变量号、e = 1 | `ODBM`（`mcr_val` + `dec_val`），**长度要给对**（给 12 仍回 `EW_LENGTH`=2） | 🟡 码已核、长度待试 |
| `cnc_rdtofsinfo` | **0x0a** | 0 | `ODBTLINF`：`use_no` **@2**、`ofs_type` **@4**（都是 BE16） | 🟢 码 + 切法都新核出来（§10.2） |
| `cnc_rdmacroinfo` | **0x17** | 0 | `ODBMVINF`：头两个 short 在 **@2** / **@6** | 🟡 码新核出来、字段名待对 |
| `cnc_rdexecprog` | **0x20** | arg0 = 0x594（缓冲长度） | 程序段文本**从载荷 @4 原样拷**（不是大端字） | 🟡 码 + 起点新核出来 |
| `cnc_rdgcode` | **0x96** | d = 类型、e = 段号 | `ODBGCD` 数组 | 🟡 码新核出来 |
| `cnc_rdwkcdshft` | **0x63** | d = 轴号、e = 长度 | `IODBWCSF` | 🟡 码新核出来 |
| `cnc_loadtorq` | **0xfd** | d = motor、e = 轴号 | `ODBLOAD`（长度要给对，给 12 回 `EW_LENGTH`） | 🟡 码新核出来 |
| `cnc_rdopnlsgnl` | **0x5d** | d = 读哪几路的位掩码（bit 5 = 进给倍率、bit 3 = 快移倍率、bit 6 = 主轴倍率但**只有 15i**） | `IODBSGNL`：载荷就是 **@0 起的 BE16 数组**（`mode`@0、`hndl_ax`@2、`hndl_mv`@4、`rpd_ovrd`@6、`jog_ovrd`@8、**`feed_ovrd`@0xa**、`spdl_ovrd`@0xc、`blck_del`@0xe…）；`feed_ovrd` 的**码 × 10 = %**（0..20 → 0..200%） | 🟢 已进 client（进给倍率） |
| `cnc_sysinfo`（= 连接期能力块，Cb `0x0e` d=e=`0x26f0`） | (0x0e) | 0x26f0 | `ODBSYS`，全 ASCII/大端：`addinfo`@0（BE16，bit0 上料器 / bit1 i 系列 / bit8..15 MODEL A..F）、`max_axis`@2（BE16 二进制）、`cnc_type`@4、`mt_type`@6（" M" 加工中心 / " T" 车床）、`series`@8、`version`@12、`axes`@16（都是**空格补齐**的 ASCII） | 🟢 已进 client（型号 = `cnc_type`+`mt_type`+`series`、版本 = `version`） |
| `cnc_rdaxisdata` | — | — | **本地就拒**：对假机床/我们的假机床一律 `rc = 1 (EW_FUNC)`、一个字节都不发（见下面那段） | 🔴 官方库自带的闸门 |

> 表里的"🟡 码已核"= **请求帧已经确定**（照着发就行），差的是**应答怎么切**（值不在
> 载荷 0 处，或是结构体数组）。真机抓一次就能把 🟡 变 🟢；client 里这些函数的
> `ncl_focas_last_error()` 已经写明"要抓哪一个调用"。

**程序上/下行是另一套帧**（不是 Cb 块，是定长体 + 裸数据；同一轮实测）：

| 调用 | func | dir | 体 | 应答 |
|---|---|---|---|---|
| `cnc_dwnstart4(h, type, dir)` | `0x11` | 1 | **516 字节定长**：`[0]=0`、`[1]=type`、`[2..4)=1`、`[4..6)="N:"`、`[6..)=目录名/程序名` | 块形状（同 0x21），块码 0 = OK |
| `cnc_download4(h, &len, buf)` | `0x12` | **4** | 就是程序文本本身（一块 1024–1400 字节） | **机床不应答**（回了反而把后续带歪） |
| `cnc_dwnend4(h)` | `0x13` | 1 | 无（10 字节帧） | 块形状；**下载的错误（数据错/溢出）都在这条上回** |
| `cnc_upstart4(h, type, name)` | `0x15` | 1 | 同 `0x11` 那 516 字节 | 块形状 |
| `cnc_upload4(h, &len, buf)` | `0x18` | **4** | 8 字节（`[0..4)` = 想要的字节数、`[4..8)` = 偏移，实测全 0） | 程序文本（**切法待核**：官方库在内部 `0x14fe70` 里解，反汇编到那层没再往下） |
| `cnc_upend4(h)` | — | 1 | 无 | 收尾 |

实测序列（`tools/site-probe/focas_sdk_probe.*` 的 `dwn4/up4`，接假机床）：
**下行** `0x11 → 0x12 → 0x13` 全程 rc=0（fake 端**必须不对 0x12 应答**）；
**上行** `0x15 → 0x18 →`（应答形状还没对上，`cnc_upload4` 一直回 `EW_BUFFER`=10：
"一个字符都没拿到"，与"应答不是驱动认的那个形状"一致）。

---

## 4. 常用函数表（按域）

### 4.1 连接与系统（cnc_*）

| 函数 | 说明 |
|---|---|
| `cnc_allclibhndl3(ip, port, timeout, &h)` | 建立连接（最常用） |
| `cnc_allclibhndl2/4` | 变体（多句柄/超时细控） |
| `cnc_freelibhndl(h)` | 释放 |
| `cnc_sysinfo(h, &ODBSYS)` | 系统信息（型号/系列/轴数/主轴数） |
| `cnc_rdmodel(h, &ODBMDL)` | 机床型号 |
| `cnc_statinfo(h, &ODBST)` | **运行状态**（模式/运行/急停/报警） |
| `cnc_rddt(`…`)` / `cnc_rdtime` | 日期时间 |
| `cnc_rdopmode(h, &ODBOPM)` | 工作模式 |

### 4.2 坐标与轴（★ 采集核心）

| 函数 | 说明 |
|---|---|
| `cnc_absolute(h, axis, &ODBACT)` | 绝对坐标 |
| `cnc_absolute2` | 绝对坐标（扩展） |
| `cnc_relative(h, axis, &ODBREL)` | 相对坐标 |
| `cnc_machine(h, axis, &ODBM)` | 机械坐标 |
| `cnc_distance(h, axis, &ODBDIS)` | 剩余距离 |
| `cnc_rdposition(h, type, …)` | **批量坐标读取**（一次取多轴多类型） |
| `cnc_acts(h, &ODBACT)` / `cnc_actf` | 全部轴绝对位置（short/float） |
| `cnc_rdsvmeter` / `cnc_rdspmeter` | **伺服/主轴负载表** |
| `cnc_rdaxisdata(h, …)` | 轴数据（速度/负载/电流，可批量）—— 它只是把 `0x24`/`0x56`/`0x40` 几条老调用拼起来（§11.19.3）|
| `cnc_rdaxisname` | 轴名称 |

### 4.3 主轴

| 函数 | 说明 |
|---|---|
| `cnc_rdspdata` | 主轴数据 |
| `cnc_rdspmeter` | 主轴负载/速度 |
| `cnc_rdspindle` / `cnc_rdspload` | 主轴转速/负载 |
| `cnc_rdspspeed` | 主轴速度 |

### 4.4 报警

| 函数 | 说明 |
|---|---|
| `cnc_alarm(h, &ODBALM)` | **当前报警**（数量+内容） |
| `cnc_rdalmmsg2(h, type, …)` | 报警消息（可分类/分页，推荐） |
| `cnc_rdalmmsg` | 报警消息（旧版） |
| `cnc_rdopmsg` / `cnc_rdopmsg2/3` | 操作员消息 |
| `cnc_rdalmhis` 类 | 报警历史（部分系统） |

### 4.5 程序

| 函数 | 说明 |
|---|---|
| `cnc_rdprgnum(h, &ODBPRO)` | 当前主/子程序号 |
| `cnc_rdseqnum(h, &ODBSEQ)` | 当前程序行号（序列号） |
| `cnc_rdprogdir` / `cnc_rdprogdir2/3` | 程序目录 |
| `cnc_upstart4` / `cnc_upend4` / `cnc_upload` | 程序上传（分块） |
| `cnc_dwnstart4` / `cnc_download` / `cnc_dwnend4` | 程序下载 |
| `cnc_delprogram` / `cnc_rdproginfo` | 删除 / 信息 |
| `cnc_rdpdf_*`（pbm_*） | 程序块管理（按块读写，大程序更高效） |

### 4.6 刀具补偿

| 函数 | 说明 |
|---|---|
| `cnc_rdtofs(h, type, &ODBTOFS)` | 刀补读（形状/磨损，长度/半径） |
| `cnc_wrtofs` | 刀补写 |
| `cnc_rdtool` / `cnc_rdtoolgrp` | 刀具信息/刀具组 |
| `cnc_rdngrp` | 刀具组数量 |

### 4.7 参数与宏变量

| 函数 | 说明 |
|---|---|
| `cnc_rdparam(h, num, len, &ODBPARA)` | **CNC 参数读** |
| `cnc_wrparam` | 参数写 |
| `cnc_rdmacro(h, num, len, &ODBM)` | **宏变量读** |
| `cnc_wrmacro` | 宏变量写 |
| `cnc_rdparainfo` / `cnc_rdparaminfo` | 参数信息 |

### 4.8 PMC（PLC 层，95 函数）

| 函数 | 说明 |
|---|---|
| `pmc_rdpmcrng(h, adr_type, data_type, start, end, len, buf)` | **PMC 区读**（R/Y/X/G/F/E/A/C/D 等） |
| `pmc_wrpmcrng` | PMC 区写 |
| `pmc_rdpmcinfo` | PMC 区信息 |
| `pmc_rdladder` | 梯形图读取 |
| `pmc_getdtailerr` | 详细错误 |
| `pmc_setpcmcntl` / `pmc_startladdermonitor` | 梯形图监控 |

### 4.9 其它

| 域 | 前缀 | 说明 |
|---|---|---|
| 时间 | `cnc_rdtimer` / `cnc_rdcount` | 运行时间/工件计数 |
| 伺服/主轴参数 | `cnc_rdservo` / `cnc_rdspindle` | 伺服/主轴参数 |
| 波形 | `cnc_startwave` / `cnc_rdwave` / `cnc_stopwave` | 波形采集（诊断） |
| 数据服务 | `ds_*`（8 函数） | FOCAS2 Data Service |
| 变量/PMC 日志 | `flnt_*`（14 函数） | Focas2 Logger |

---

---

## 5. 地址、数据类型与数据字典

### 5.1 结构体（选摘）

```c
typedef struct { short data; short dec; short unit; short disp; short name; short suff; } ODBACT;
// 坐标值 = data / 10^dec  —— ★ 必须按 dec 缩放，否则数值差 10^n 倍

typedef struct { short dummy; short num; struct { short no; short type; short data; } msg[10]; } ODBALM;
typedef struct { short datano; short type; long data[2]; } ODBTOFS;   // 刀补（形状/磨损 × 长度/半径）
typedef struct { short datano; short type; long data; } ODBM;          // 宏变量
typedef struct { char name[36]; char cnc_type[2]; ... } ODBSYS;
```

**⚠️ 坐标缩放是第一坑**：`cnc_absolute` 返回的 `data` 是整数，必须 `data / pow(10, dec)` 才是实际值（dec 通常 3 或 4）。

---

### 5.2 数据字典映射（册 4 的数据项 / 册 7 的集合类对象 → FOCAS 调用）

| 标准（册 4/7） | 模型路径 | FOCAS 调用 | 状态 |
|---|---|---|---|
| `STATUS` | `/STATUS` | `cnc_statinfo`（`ODBST`）| 🟢 rc=0 |
| `PART_COUNT` | `/PART_COUNT` | `cnc_rdcount`（`ODBTLIFE3`：`datano`@2、件数@20）| 🟢 假机床 rc=0；字段位置修正过一次 |
| `WARNING` | `/WARNING` | `cnc_rdalmmsg2`（`ODBALMMSG2` 数组）| 🟡 码 `0x23` 已核，块形状还差 |
| `PROGRAM` | `/CONTROLLER/PROGRAM` | `cnc_exeprgname2`（`0xfc`）| 🟢 rc=0 |
| `PROGRAM_NUMBER` | `/CONTROLLER/PROGRAM_NUMBER` | `cnc_rdprgnum`（`0x1c`）| 🟢（`SUBPROGRAM` 不要了，§11.20.3）|
| `LINE_NUMBER` | `/LINE_NUMBER` | `cnc_rdseqnum`（`0x1d`，载荷 @0 BE32）| 🟢 |
| `TOOL_NUMBER` | `cnc_rdcommand`（`0x97`）| 指令值里 `adrs='T'` 那条的 `cmd_val` | 🟢 NCGuide（§11.19.2）|
| `POSITION` | `/AXIS@<轴>/POSITION@REAL` | `cnc_rdposition`（`0x26` d=0，`POSELM` 每轴 12 字节）| 🟢 NCGuide 闭环 |
| `POSITION@CMD`（指令位置）| `/AXIS@<轴>/POSITION@CMD` | **实际位置 − `cnc_srvdelay`（d=9，每轴 8 字节）** | 🟢 NCGuide 闭环 |
| `PATH_LEFT_LENGTH` | `/AXIS@X/PATH_LEFT_LENGTH` | `cnc_rddynamic2` 的剩余距离 | 🟢 |
| `SPEED` / 主轴转速 | `/FEED_SPEED`、`/SPINDLE_SPEED` | `cnc_actf`（`0x24`）、`cnc_acts`（`0x25`）——每个轴/主轴一个 **float32** | 🟢 rc=0 |
| `FEED_OVERRIDE` / `SPINDLE_OVERRIDE` | `/FEED_OVERRIDE`、`/SPINDLE_OVERRIDE` | `cnc_rdopnlsgnl`（`0x5d`，`IODBSGNL` 的 `feed_ovrd`@0xa，码 ×10 = %）| 🟢 进给倍率闭环 |
| 负载 / 电流 | `/AXIS@X/TORQUE`、`CURRENT` | `cnc_loadtorq`（`0xfd`）、`cnc_rdsvmeter`（`0x56`，`d=1` 负载表 / `d=3` 安培）| 🟢 NCGuide（轴温没有这条 API，点位已删；§11.19.3）|
| `PARAMETER`（dict）| `/CONTROLLER/PARAMETER` | `cnc_rdparam`（`0x0e`，`IODBPSD`：`datano`@2、`type`@4、`ldata`@8）| 🟡 字段位置已核 |
| `VARIABLE`（list，宏变量）| `/CONTROLLER/VARIABLE` | `cnc_rdmacro`（`0x15`，`ODBM`）| 🟡 码已核，**长度要给对**（给 12 回 `EW_LENGTH`）|
| `TOOL`（list）| `/CONTROLLER/TOOL` | `cnc_rdtooldata` / `cnc_rdtoolrng` | ⛔ 真机 rc=1/3，**机床不提供**（官方 SDK 同样被拒）|
| `TOOLPARAM` | `/CONTROLLER/TOOLPARAM` | `cnc_rdtofs` / `cnc_rdtofsinfo` | 🟢 字段 @0 已核 |
| `COORDINATE` | `/CONTROLLER/COORDINATE` | **`cnc_rdzofs`（`0x0b`）/ 写 `0xc`**（`IODBZOFS`）| 🟢 读 + 写都实测过（§11.19）|
| `FILE`（程序上下行）| `/CONTROLLER/FILE` | `cnc_dwnstart4`/`cnc_download4`/`cnc_dwnend4`；取回是 `cnc_upstart4`/`cnc_upload4`/`cnc_upend4` | 🟢 下行链路码已核；**上行回 `EW_BUFFER`=10**（§10.4.6）|
| 刀补写 | `/CONTROLLER/TOOLPARAM` 的 `set_value` | `cnc_wrtofs` | 🟢（权限在外面）|
| 参数写 / 宏变量写 | `/CONTROLLER/PARAMETER`、`VARIABLE` 的 `set_value` | `cnc_wrparam` / `cnc_wrmacro` | 🟢（权限在外面）|

> 两套编号要分清：**册 4/7 的 `type`** 是数据对象的名字（`PARAMETER`、`TOOL`…），
> **FOCAS 的 item 码**（`0x0e`、`0x15`…）是协议里的业务码；两者在 §3.3 的表里对应。

### 5.3 错误码（错误分类函数）

```c
short err = cnc_getdtailerr(h, &ODBERR);   // ODBERR 结构 getdtailerrkind/dta1/dta2
// 通用返回：EW_OK=0；非 0 时用 cnc_getdtailerr 取细节
```
常见：`EW_FUNC`(功能不支持) · `EW_LENGTH`(数据长度错) · `EW_PARAM`(参数错) · `EW_HANDLE`(句柄无效) · `EW_SOCKET`(网络错) · `EW_BUSY`

---

---

## 6. 典型流程

### 6.1 采集循环

```c
cnc_allclibhndl3(ip, 8193, 10, &h);            // ① 一次会话，句柄最多 2 个：单进程复用
cnc_statinfo(h, &st);                          //   状态（STATUS）
cnc_rdposition(h, 0, -1, &POSELM);             //   位置：一条拿全部轴（比逐轴 cnc_absolute 快）
cnc_actf(h, -1, &feed);   cnc_acts(h, -1, &spdl);   //   速度：每轴/每主轴一个 float32
cnc_rdcount(h, 0, &count);                     //   计件
cnc_rdopnlsgnl(h, 0x20, &IODBSGNL);            //   进给倍率（bit5）
/* 轮询周期到了再来一遍；退出时 */
cnc_freelibhndl(h);
```

### 6.2 报警

状态位用 `cnc_alarm`（`ODBALM`，最多 10 条）；要文本用 `cnc_rdalmmsg2`（`0x23`，
`d` = 类型、`e` = 条数，`arg2=2` / `arg3=64` 才填文本，见 §10.4.2）。

### 6.3 程序下发 / 取回

```c
/* 下发：三步必须成对，中途退出会锁住机床侧 */
cnc_dwnstart4(h, type, dir);
  循环: cnc_download4(h, &len, buf);   /* 一块一块 */
cnc_dwnend4(h);

/* 取回：同一套路（本仓库这条还没通，见 §10.4.6）*/
cnc_upstart4(h, type, path);  … cnc_upload4(h, &len, buf);  … cnc_upend4(h);
```

### 6.4 写（刀补 / 参数 / 宏变量）

`cnc_wrtofs` / `cnc_wrparam` / `cnc_wrmacro`。**权限在外面**：改刀补会直接改变加工结果，
本仓库只在设备侧的 `set_value` 上把能力摆出来，谁能写由部署侧决定。

---

## 7. 同包附带能力（Fwlib32 之外）

`libfwlib32.so.1` 导出 **881 个函数**，不只有 `cnc_*`：

| 前缀 | 个数 | 能干什么 | 本仓库用了吗 |
|---|---|---|---|
| `cnc_*` | 758 | CNC 数据、坐标、程序、刀补、参数、宏变量、文件 | ✅ 主力 |
| `pmc_*` | 95 | PMC/梯形图：位、字节、字、定时器、计数器、I/O 链接 | ❌（册 4 没有对应的数据对象，先不进模型）|
| `flnt_*` | 14 | Focas2 Logger（高速采样日志）| ❌ |
| `ds_*` | 8 | 数据服务（服务器侧）| ❌ |
| `pbm_*` | 3 | 程序块管理 | ❌ |
| `hmgr_*` / `smgr_*` / `loglevel_*` | 1 / 1 / 1 | 句柄管理、服务器管理、日志级别 | ❌ |

> PMC 那 95 个函数是"梯形图侧"的读写口（`pmc_rdpmcrng` / `pmc_wrpmcrng` 等）。
> 现场如果要 PLC 信号，走的是这一族；本仓库现在只做 `cnc_*`，PMC 一格空着。

## 8. 实现坑

1. **句柄数限制**：FOCAS 默认最多 2 个并发句柄 —— 多进程采集会互相踢掉。**必须单进程复用句柄**。
2. **坐标缩放**（见 §4）—— 最常见的数值 bug 来源。
3. **`cnc_rdaxisdata` 优于逐轴 `cnc_absolute`**：批量读一次拿多轴，吞吐高 3-10 倍。
4. **报警消息分页**：报警多时 `cnc_alarm` 只给 10 条，遍历要用 `cnc_rdalmmsg2`。
5. **程序上传/下载必须成对调用**（`*start` → 分块 → `*end`），中途退出会锁住机床侧。
6. **32/64 位库不同**（`Fwlib32.dll` vs `fwlib32.x64.dll`），结构体对齐要用 `#pragma pack`。
7. **写类函数（`cnc_wrtofs` / `cnc_wrmacro` / `pmc_wrpmcrng`）风险高**：改刀补会导致撞刀。

---

## 9. 参考实现与待办

| 来源 | 说明 |
|---|---|
| `pyfocas` / `pyfanuc` | ctypes 绑定 + fwlib 头文件（可直接抄结构体定义） |
| **881 个函数原名**（按前缀分类） | 原始素材未随本目录提供 |
| 商业库对照 `FanucSeries0i` | 高层 API 分组参考（59 成员，坐标/报警/负载/刀补/宏变量/程序列表） |
| 参考实现侧 | `驱动定义目录/` FANUC 驱动（端口 8193） |

**待办（按"差什么"排）**：

| # | 差什么 | 卡在哪 | 下一步 |
|---|---|---|---|
| 1 | 报警文本（`WARNING`）| `cnc_rdalmmsg2` 的**块形状**没对上（假机床铺的形状被 SDK 判无效）| 用 §10.2 那套反查工具按官方头 `ODBALMMSG2` 的尺寸试块长；有报警的真机再抓一次做终验 |
| 2 | 宏变量（`VARIABLE`）| `cnc_rdmacro` 的**载荷长度**没试出来（给 12 回 `EW_LENGTH`）| 按官方头 `ODBM` 的尺寸扫一轮长度 |
| 3 | 程序取回 | `cnc_upload4` 一直回 `EW_BUFFER`=10 | 把 `0x18` 那条的"想要多少字节/偏移"试对 |
| 4 | 坐标状态变量 / 轴状态变量 | 册 4 里没有对应项 | 先不进模型 |
| 5 | 负载 / 电流 | ✅ 通了（`0x56`：`d=1` 负载表、`d=3` 安培；轴温 FOCAS 没有这条 API）| — |
| 6 | 刀具列表（`TOOL`）| 真机 `cnc_rdtooldata` rc=1、`cnc_rdtoolrng` rc=3 | 同上，机床侧不提供 |
| 7 | PMC（95 个函数）| 没有对应的数据对象 | 等现场提出信号需求 |
| 8 | `TOOL_NUMBER` | ✅ `cnc_rdcommand`（`0x97`）的 `'T'` 记录（§11.19.2）| — |


---

## 10. 线协议解剖（这些结论是怎么钉出来的）

这一节是**档案**：§1–§9 里每一句话的出处。方法是三层——
① 反汇编现场交付包里的 `libfwlib32.so` / `fwlibe64.dll`；
② 拿**官方 SDK** 对着"每个字节都可辨识"的**假机床**跑，出参哪一格等于载荷哪一格是自动反查出来的；
③ NCGuide（FANUC 自己的模拟器）当真机用；**④ 真机**（以太网 0i-MD）做终验。
复现脚本都在 `tools/site-probe/` 下（`focas_*.sh` / `focas_*.py` / `mock.py`）。

### 10.1 真机实测：NCGuide（FS0i-F 模拟器）能当"没有真机的真机"

#### 10.1.1 "正确环境"的配方 + 本机现在卡在哪（🟢 2026-09，手册 + 自写调试器）

配方就在 NCGuide 自带的手册 `Document/NCG/NCGuide FOCAS2 Function.pdf`（官方 SDK 包里，
21 页）：

| 手册 | 说的 |
|---|---|
| §3.1 | 只有**一个**选项要求：**`Extended driver and library function`**（消息表 `Message.xml` 里编号 `OPTPRM_401`）；用 `OptionSetting.exe` —— 手册强调要**在 NCGuide 起来之后**开、勾上、**再重启 NCGuide** |
| §3.1 | **以太网不用再开别的选项**（"Ethernet connection is equal to embedded Ethernet function"）；屏幕里那个 **Ethernet function 勾是给机床侧以太网用的，别开**（FS0i-F 就是勾了它之后起不来的） |
| §4.2 | **HSSB 不用装驱动**（原文 "The installation of the HSSB driver is unnecessary"） |
| §4.3 | 以太网要的是**跑 NCGuide 那台机器的网卡 + IP** |
| §4.4 | 取句柄：HSSB → `cnc_setdefnode(9)` + `cnc_allclibhndl()`（节点号 **9**）；以太网 → `cnc_allclibhndl3(IP,…)`，IP 用跑 NCGuide 那台的（手册特意 NOTE：**屏幕上那个以太网设置对 FOCAS2/Ethernet 无效**） |
| §5 | 一张**每函数的 HSSB / Ether 可用性矩阵**（O / X / -），核某条前先查它 |

**本机现状（2026-09-21 17:00 前后实测）**：所有 NCGuide 机型（FS0i-F、FS0i-F Plus、
FS31i-B、…) **都是起来十几秒后自己死**，SIM.LOG 只写 `CNCSIMULATOR STARTED`、没有
`FINISHED`，事件日志里是 `APPCRASH`。本机没装 cdb/windbg、也没管理员权限开 WER 的
LocalDumps，所以我写了 `tools/site-probe/win_minidbg.py` 自己当调试器（`CreateProcess`
+ `DEBUG_ONLY_THIS_PROCESS`，只报**二次异常**——WER 记的就是那一枪），抓到的现场是：

- **FS31i-B / FS0i-F Plus**：致命异常 `ACCESS_VIOLATION`、**写**到**页对齐**地址
  （`0x21050000`、`0xef670004` 这种），指令是 `MSVCR80!memset` 里的
  `movdqa [edi], xmm0`；调用链 `USER32 → System.Windows.Forms.ni.dll →
  mscorwks(.NET 2.0) → msvcr80` —— 即**画 CNC 屏幕那一步 memset 写过了区域边界**。
- **FS0i-F** 另有第二种：`ns.dll+0x74e2c1`，那段紧跟"往 `0x125e2402/0x125e2404` 写
  `0x50/0x1e`"之后按 0..3 分支 —— 像**显示尺寸**处理。
- **时好时坏**：连开 3 次大约活 1 次；活下来的一路涨到 160→409 MB 之后死（正好在
  "开始画屏"那一步）。把 `SimBaseSetting.xml` 删掉死得更早（那文件是必需的）。

所以卡的是**它自己的显示这一路**，不是机床数据、不是 FOCAS2/协议。要把它变成
"客户端发帧 / 服务器回帧"的环境，得先在 GUI 侧：① 把它起稳（或换个显示/缩放/分辨率），
② 用 `OptionSetting.exe` 勾上 `OPTPRM_401`，③ 需要以太网就用 NCGuide 的以太网设置把
服务开到 8193（之前那台 FS0i-F 确实听过 8193）。

NCGuide 自带 **FOCAS2 服务**，所以那批 🟡（"码已核、应答待核"）不用等真机 —— 起一个
模拟机床就能把应答抓全。2026-09 在本机装好的 `C:\Program Files (x86)\FANUC\NCGuide
FS0i-F` 上实测通了，**配方**（每一条都是踩出来的）：

1. **走 HSSB，不走以太网**：手册（NCGuide FOCAS2 Function §4.4）说 HSSB 用节点号 **9**，
   处理库是 NCGuide 自带的 **`Fwlib32.dll` + `fwlibNCG.dll`**（"HSSB connection:
   Exclusive use for NCGuide of FS31i/32i/35i and FS0i-F"）。调用序列：
   `cnc_setdefnode(9)` → `cnc_allclibhndl(&h)`。实测 `rc=0`、handle=18433。
   以太网那条（`cnc_allclibhndl3(127.0.0.1, 8193)`：Simbase 确实在听 8193）**用官方 SDK
   的库一律 -17 EW_PROTOCOL** —— NCGuide 的以太网服务要它自己那套握手，别在这条上耗。
2. **必须 32 位**：NCGuide 随包的是 32 位库（`Fwlib32.dll`）。探针要按 `vcvars32` 编，
   而且**函数指针要标 `WINAPI`（__stdcall）** —— 不标就栈坏，症状是 `0xC0000409`
   （探针里已经修好：`NCL_PROBE_CALL` + `--hssb`）。
3. **不用改 NCGuide 的选项**：手册 §3.1 说要开 "Extended driver and library function"
   再重启；实测 HSSB 这条路**不开也能用**（没有 GUI 自动化时省事）。
   > 2026-09 补：这一句只对**基本函数**成立。那条选项（消息表里的 **`OPTPRM_401`**）
   > 卡的是**扩展驱动/库功能**——也就是 `cnc_rdaxisdata` 那一族（伺服/主轴负载、电流、
   > 速度）：官方库对没开这个选项的机床**本地就回 `EW_FUNC`(1)、一个字节都不发**
   > （见 §10.3）。要核那一族就得在 NCGuide 的 GUI 里用 `OptionSetting.exe` 勾上它
   > （勾完必须重启 NCGuide）。

**实测结果**（`focas_sdk_probe32.exe --dll Fwlib32.dll --hssb 127.0.0.1 8193 <调用>`，
默认机床 `1path-3axis-M`）：

| 调用 | rc | 拿到的形状 |
|---|---|---|
| `cnc_statinfo` | 0 | ODBST 十一个 u16（块 1/2 + 块 0 载荷的那套切法 ✓ 与 §3.2 一致） |
| `cnc_rdposition 0`（绝对） | 0 | **每轴一个 `POSELM`（12 字节）**：`int32 data` + `dec=3` + `unit=0`(mm) + `disp=1` + `name='X'` + `suff` ✓ 位置值 = `data / 10^dec` |
| `cnc_rdsvmeter` | 0 | **每轴一个 `LOADELM`（12 字节）**：`int32 data` + `dec` + `unit` + `name='X'` |
| `cnc_rdspmeter -1` | 0 | 每主轴一个 `LOADELM`：`name='S'` + `suff1='1'` → "S1"（负载/转速各一） |
| `cnc_rdalmmsg2 -1` | 0 | `ODBALMMSG2` 数组（这条机床没报警，全 0）；形状与手册一致 |
| `cnc_rdblkcount` | 0 | 就是一个 **int32**（原来"取值不在载荷 0 处"的判断作废） |
| `cnc_rdopmode` | 0 | short 数组（内容随主轴状态，值域见手册） |
| `cnc_srvdelay -1 --len 132` | 0 | `ODBAXIS`：`type = -1`（ALL_AXES）+ `data[0..31]`（机床静止，全 0）。**长度规则 = `4 + 4×轴数`**：单轴给 `8`，ALL_AXES 要用机床的最大轴数（这台报 32 → `132`）——给 20 / 36 / 68 都回 `EW_LENGTH` |
| `cnc_absolute -1 --len {12,16,36}` | **2** | `EW_LENGTH`：长度必须是"这台机床的轴数"对应的那个值；**位置建议直接走 `cnc_rdposition`**（一条拿四种） |
| `cnc_rdtofs` / `cnc_rdmacro` / `cnc_rdparam` | **2** | `--len` 给得不对（要按各结构的实际长度给），下一轮按结构体尺寸补 |
| `cnc_upstart4` | —— | 探针在这条上没返回（取程序那条要真程序/超时处理），下一轮补 |

> 结论：`POSITION`/`ANGLE`（`cnc_rdposition`）、`TORQUE`+伺服负载（`cnc_rdsvmeter`）、
> 主轴负载/转速（`cnc_rdspmeter`）、`WARNING`（`cnc_rdalmmsg2`）、`cnc_rdblkcount`
> 这五组**已经从 🟡 变 🟢**（形状实测过了），client 侧照 `POSELM`/`LOADELM` 解码即可。
> 跟踪误差（`cnc_srvdelay`）走的是同一族的 `0x26`，但机床静止时它恒为 0，形状靠
> **反汇编**钉（下一节）。

#### 10.1.2 库内部：`cnc_srvdelay` 那一族怎么切（🟢 反汇编 + 假机床实测）

用 `tools/site-probe/focas_dis_range.py` 把 32 位 `fwlibNCG.dll` 里那一层读出来
（`cnc_srvdelay` 的 RVA `0x1bde0` → 内部函数 `0x1b700`）：

```
cnc_machine   (d=1)  ┐
cnc_absolute  (d=4)  ├─ 都是同一个内部函数的薄壳：push out, len, axis, <kind>, h
cnc_relative  (d=6)  │   —— **那个 <kind> 就是 Cb 的 d**（srvdelay = 9）
cnc_distance  (d=7)  │
cnc_srvdelay  (d=9)  │   调用前两道检查（都在本地，不发帧）：
cnc_accdecdly (d=10) │     axis > 轴数            → EW_ATTRIB (4)
cnc_skip      (d=8)  ┘     length < 4 + 4×轴数    → EW_LENGTH (2)

取值（0x1b878 起）：
    out->data[i] = *(u32*)(staging + i*8)   ← **每轴步长 8 字节**，取记录第 0 个 dword
    out->type    = axis                     ← 轴号由库自己填
```

对上假机床实测的请求帧（`0x26`、d = 9、e = `0xffffffff`），client 侧的口径就是
**每轴 8 字节、值在记录第 0 个 int32**。记录后 4 字节是什么，这一节没定 —— 下一节拿
**以太网**库（我们 client 真正对的那条协议）把它问清楚了。

#### 10.1.3 以太网库 `fwlibe64.dll` 里的同一族（🟢 反汇编，2026-09）

上面那一节读的是 32 位 HSSB 库；**我们 client 对的是以太网协议**，所以又去 x64 的
以太网处理库 `fwlibe64.dll` 里核了一遍 —— `cnc_srvdelay` 同样是薄壳，把 `kind = 9`
交给 `sub_180059180`（`cnc_absolute` = 4、`cnc_machine` = 1、`cnc_relative` = 6、
`cnc_distance` = 7、`cnc_accdecdly` = 10，与 32 位那张表一致）。共享函数里三件事一次说清：

```
18005923a  movsx r8, [handle+0x96e] / shl rax,5 / movzx eax,[rax+rbx+0x66c]
           ; 从连接期缓存的"轴表"取控制轴数；axis > 轴数 → 本地回 EW_ATTRIB(4)
18005928a  mov r9d, 0x26 / mov edx, <kind>        ; 组请求：Cb 码 0x26 + d
180059304  movzx ecx, [块+0xe] + bswap16          ; 块里 [14..16) 的载荷长度
180059321  shr ax, 3                              ; 轴数 = 载荷长度 / 8 → **每轴 8 字节**
180059343  lea rcx, [rax*4 + 4]                   ; 长度规则 = 4 + 4×轴数（不够 EW_LENGTH）
18005938c  mov ecx, [载荷 + i*8 + 0x10]           ; 取记录第 0 个 dword
180059391  call bswap32                           ; 线上是**大端**
180059396  mov [out + i*4 + 4], eax               ; → ODBAXIS.data[i]；type 在 +2 = 轴号
```

两条结论：**线上每轴 8 字节、值在记录第 0 个 dword（大端）、长度规则 `4 + 4×轴数`** ——
和 §10.1 的 NCGuide 实测完全对上；**小数位不在这条载荷里**（官方库一个字节都不多看），
位数走 `cnc_getfigure`，也就是我该轴显示小数位 —— 同一条 `POSELM` 里的 `dec`。所以
`focas_values.c` 的 `srv_delay_raw()` 借 POSELM 的 dec 缩放，记录的 `[4..8)` 当保留位、
不解释（早先按 POSELM 一族猜它是 dec/unit，这轮改掉了）。

工具：`tools/site-probe/focas_dis_range.py`（按 RVA 反汇编 PE，64 位也能用）、
`tools/site-probe/elf_dis.py`（按符号反汇编 ELF，ARM/Thumb 自动 —— 用来开交付包里那份
`libfwlib32.so.1`）。

> 顺带记一笔：官方 SDK 对这几条**单轴**调用是**在本地**回 `EW_ATTRIB` 的 —— 它从连接期
> 缓存里取"控制轴数"，而假机床的握手没把这一项喂对（握手 `func 01` 的头 16 字节、记录
> A/B/C/D、每条记录的 `0x18` 详情、能力块 `0x0e` 的载荷都试过，都没喂动）。这只影响
> "拿官方 SDK 当裁判"这条路；我们 client 自己解，形状由上面这段反汇编定死。

---

### 10.2 用官方 SDK 反查"载荷第几字节是哪一格"（🟢 2026-09 新方法）

§3.3/§10.1 核 item 用的是"铺斜坡载荷、人眼看结构体" —— 对 `ODBST` 那种十来个 short 的
结构还行，对"值藏在 @12 还是 @20"这类问题就很容易看岔（本轮就抓到一处：`cnc_rdcount`
原来按 @0 读，其实是 @20）。这轮把它做成**自动反查**：

```
python tools/site-probe/focas_sdk_layout.py cnc_rdcount 0          # 自动
python tools/site-probe/focas_sdk_layout.py --calls "cnc_rdlife:1,cnc_rdtofs:0+0+8"
python tools/site-probe/focas_sdk_layout.py --payload 01020304... cnc_rdtofsinfo
```

做法（`tools/site-probe/focas_sdk_layout.py`）：给假机床铺一份**每个字都不一样**的载荷
（第 i 个字 = `0x1000 + i*0x101`），跑一次官方 SDK 的调用，把 SDK 填进出参的字节抠出来，
再对出参的每一格在载荷里**反查**它是从哪儿来的（大端/小端 × 16/32 位各试一遍，唯一命中
才报）。于是"结构体第 n 格 ← 载荷 @m"是机器算出来的，不靠眼力。配套两个小工具：

```
python tools/site-probe/fwlib_struct.py <Fwlib64.h> ODBTLIFE3 ODBALMMSG2   # 官方头的结构体
python tools/site-probe/fwlib_proto.py  <SpecE 目录> cnc_rdtofsinfo        # 官方文档的原型
```

- 结构体/原型从**官方 SDK 包**里拿（`Fwlib64/30i/Fwlib64.h`、`Document/SpecE/**/*.xml`，
  每份 XML 都有 `<prottype>` 一行）。原型很关键：`focas_sdk_probe.c` 里那些通用形状
  （`s1/s2/s3/s1_n/s2_n/…`）就是照它配的，配错了出参落在哪一格就全是噪声。
- 反查只报"唯一命中"；对不上就标 `?`（SDK 自己按语义改写过、或者那一格不是从载荷来的）。

**交叉印证**：同一条 item 再对着**现场包里那份 Linux 实现**（`libfwlib32.so.1`，x86 版
在官方 SDK 包的 `Fwlib/Linux/x86/` 下）反汇编一遍，两边必须一致。`cnc_rdcount` 就是这么
钉死的：

```
0009d627  mov dword ptr [esp+0xc], 0x8b     ; Cb 码
0009d6a7  mov edx, dword ptr [eax+0x10]     ; 块 +0x10 = 载荷起点
0009d6aa  bswap edx
0009d6af  mov word ptr [ecx], dx            ; out->datano = 载荷 @2 的 BE16
0009d6f8  mov eax, dword ptr [eax+0x24]     ; 载荷 @20
0009d6fb  bswap eax
0009d6fd  mov dword ptr [ecx+4], eax        ; out->data   = 载荷 @20 的 BE32
```

（用 `python tools/site-probe/elf_dis.py <libfwlib32.so.1> cnc_rdcount`；这一轮给
`elf_dis.py` 补了 x86 / x64，之前只认 ARM。）

**这一轮反查出来的东西**（都已并进上表）：

| 事实 | 出处 |
|---|---|
| `cnc_rdcount` = `datano`@2、件数 **@20**；`cnc_rdlife` = `datano`@2、寿命 **@12** | SDK 反查 + `libfwlib32.so` 反汇编，两边一致 |
| `cnc_rdtofsinfo` 的 Cb 码是 **0x0a**（不是 0x0e），`use_no`@2、`ofs_type`@4 | SDK 反查 |
| `cnc_rdmacroinfo` 的 Cb 码是 **0x17** | SDK 反查 |
| `cnc_rdexecprog` 的 Cb 码是 **0x20**、文本从载荷 @4 起（原样字节，不是 BE16） | SDK 反查 |
| `cnc_rdgcode` = **0x96**、`cnc_rdwkcdshft` = **0x63**、`cnc_loadtorq` = **0xfd** | SDK 反查（后两条还差长度） |
| `cnc_rdblkcount` 就是**载荷 @0 的 BE32**（上一版写"不是 @0"，反了） | SDK 反查 |
| 进给倍率不在 `cnc_rddynamic2` 里（`ODBDY2` 没有倍率字段），在 `cnc_rdopnlsgnl` 的 `IODBSGNL.feed_ovrd`；主轴倍率那一格现代系列 "(Not used)" | 官方头 + 官方文档的反查 |
| `cnc_rdparam` = `datano`@2、`type`@4、`ldata`@8；`cnc_rdtofs` = `data`@0 | SDK 反查 |
| `cnc_rdprgnum`@2/@6、`cnc_rdseqnum`@0、`cnc_alarm2`@0、`cnc_rdngrp`@0、`cnc_rdtimer`@0+@4 **都对**（client 原来的读法没问题） | SDK 反查（顺带把已进 client 的几条复核了一遍） |

还没啃下来的：`cnc_rdalmmsg2`（**往前推了一格**：每条记录 80 字节、`alm_no` 在 +0、
文本 `alm_msg[64]` 在 +0x10 —— 依据是 Linux `libfwlib32.so` 里"`条数 = 载荷长度 / 80`"
外加官方 SDK 出参里 `[0..4)`/`[12..)` 两格正好对上；**中间三个字段 type/axis/msg_len
还没钉死**：Linux 库读 +4/+8/+0xc（+6/+0xa/+0xe 各空 2 字节，正好铺满 80），官方 SDK
的出参没跟这三格对齐，所以 client 的 `WARNING` 先不开。假机床那边可以用
`focas_sdk_mock.py --almmsg2` 复现，`ALMMSG2_MARK=1` 会给每个字段可辨识的值）、
`cnc_rdsvmeter`/`cnc_rdspmeter`/`cnc_rdposition` 那几条
**一条请求带多个块**的（`0x89`/`0x88`/`0x0e` 那几块的形状要逐块对），以及
`cnc_rdprogdir3`/`cnc_rdmacro` 的**长度**（给 12 仍回 `EW_LENGTH`，要按结构体尺寸试）。
这些都不再需要真机 —— 接着拿这套反查工具磨就行。

### 10.3 官方库那条"控制轴数"闸门（🟢 2026-09 把范围钉清楚了）

前面几轮一直说"单轴调用回 `EW_ATTRIB`"，这轮顺着多块调用往下一挖，发现它其实是**同一
条闸门**，而且影响面比"单轴"大得多 —— 官方库在连接期把"这台机床几根控制轴"记在上下文
里，之后凡是**按轴数决定长短**的调用都吃这一份缓存。我们假机床上它记成 **0**：

| 现象 | 例子（同一台假机床） |
|---|---|
| 指定轴号 → 本地就回 `EW_ATTRIB`(4)、不发帧 | `cnc_rdwkcdshft(h, 1, 8, …)` → rc=4；换成 `ALL_AXES(-1)` → rc=0 |
| `ALL_AXES` → 帧发得出去，但**一条轴的数据都没有** | `cnc_rdwkcdshft(h, -1, …)`：出参只有 `type` 被填成 `0xffff`，`data[]` 全 0（0 根轴） |
| 请求里的"长度"也由它算 → 变成 0 | 同一条调用的 Cb `e`（length）发的是 **0**，机床那边"按长度切"的解析全落空 |
| 整族调用直接本地拒 `EW_FUNC`(1)、一个字节都不发 | `cnc_rdaxisdata`（伺服/主轴负载、电流、速度那一族的入口） |

也就是说：**多块那几条（`cnc_rdsvmeter` / `cnc_rdspmeter` / `cnc_rdaxisdata`）之所以
核不出来，根子在这条闸门，不在"应答怎么切"**。反过来，哪天把连接期那一格喂对，这一族
（`AXIS@*/TORQUE`、`CURRENT`、主轴负载、`FEED_SPEED`）就一起打开 ——
写 client 需要的东西官方文档已经给全：

```
cnc_rdaxisdata(h, cls, short *type, short num, short *len, ODBAXDT *axdata)
  cls = 1 位置 / 2 伺服 / 3 主轴 / 4 选中的主轴 / 5 速度
  ODBAXDT = { char name[4]; long data; short dec; short unit; short flag; short reserve; }  // 16 字节
```

（反汇编 `fwlib30i64.dll` 看到 `cnc_rdsvmeter`/`cnc_rdspmeter` 都是薄壳，内部
`call cnc_rdaxisdata`（RVA `0x18d60`）—— 这几条在官方库里是同一个入口。）

**另外记一笔**：这轮把假机床改成"`0x89` 回像样的轴表、`0x0e/0x26f0` 回 `ODBSYS`"之后，
`cnc_rdsvmeter`（`0x56` + `0x89`）从 `-17 EW_PROTOCOL` 变成 `rc = 0` —— 与上一条对照
说明"闸门"和"多块形状"是两件事：多块调用的每一块都得铺对，`0x89` 回填充字节直接判
协议错。以后调多块调用先加 `focas_sdk_mock.py --axis-table N`。

### 10.4 真机（以太网 0i-MD，2026-09-21）：会话是**两条 TCP**，`code 24` 才是 ODBSYS

现场有了一台能连的 FOCAS2 服务端（`192.168.110.192:8193`，`cnc_sysinfo` 报
**0i-MD / series `D4G3` / 版本 `28.0` / 3 轴**）。**官方 SDK**（`Fwlib64.dll` +
`fwlibe64.dll`，`cnc_allclibhndl3` rc=0）与**这一份 client** 一起对着它跑，
抄包用 `focas_tap.py`，逐条读语义接口用 `tools/site-probe/focas_live.*`。

**会话的形状**（官方 SDK 的字节，两条 TCP 都开着）：

```
#1（控制通道）  >> a0a0a0a0 0001 01 01 0002 0001        hello，计数器 1
                << a0a0a0a0 0003 01 02 0168 …           应答 370 字节（体 360）
#2（数据通道）  >> a0a0a0a 0001 01 01 0002 0002        hello，计数器 2
                << a0a0a0a 0003 01 02 0168 …           同样的应答
                >> a0a0a0a 0001 21 01 001e 0001 <Cb>   探针：一个 code 24 的块
                << a0a0a0a 0003 21 02 0024 0001 <rb>   载荷 = ODBSYS（18 字节）
                >> a0a0a0a 0001 02 01 0000              bye（关闭时，两条各一条）
```

判据（都是本轮实测，不是推测）：

| 试法 | 结果 |
|---|---|
| 控制通道上发 `func 0x21` | **连接被 RST**（命令只能走数据通道） |
| 只开一条连接、只发 hello(1) 再发命令 | 同上，RST |
| 一条连接上连发 hello(1) 和 hello(2) | 第二条 hello 不应答，之后 RST（**一条连接一次 hello**） |
| 新连接上直接 hello(**2**) | 收下，命令能发（数据通道不看有没有前一条） |
| 数据通道上不发那条 `code 24` 探针，直接读数据 | 照样 rc=0 —— 探针**不是**会话前提 |

**handshake 应答的形状**：体 **360 字节**，而 `[8..10)` 写的是 **8**——`16 + 8n`
（§3.1 判据 5）对不上，可 SDK 自己收下并 rc=0。所以那条判据是**对反汇编的误读**，
client 里已经改成"至少 16 字节就收下"（`ncl_focas_hello_reply`）。同理，
"按记录数发一串 `code 24` 的块、再跟一条 `code 14`（`0x26f0`）"那套 step 2/step 3
（§3.2）在这台机器上一条都不成立：SDK 只发**一个** `code 24` 的块。

**ODBSYS 从哪来**（`cnc_sysinfo` 这一格）：

```
code 24  → rc=0，载荷 18 字节  06 62 | 00 20 | " 0" | " M" | "D4G3" | "28.0" | "03"
                              addinfo  max_axis cnc_type mt_type series  version axes
code 0x0e（d=e=0x26f0，老写法）→ **rc=1**（机床拒了）
```

client 里 item `ODBSYS`（`code 24`）是主路径，`VERSION`（`code 0x0e`）留作**退路**
（只有机床明确拒了 24 才用，见 `focas_values.c` 的 `odbsys_payload`）。

**其余几条的对照**（这一份 client 的语义层读到的值，与官方 SDK 逐条一致）：

| item | 请求 | 应答 | 语义 |
|---|---|---|---|
| `RDPRG` | `0x1c` | 载荷 @2 / @6 = `07 d1` | 运行程序号 = 主程序号 = 2001 |
| `RDSEQ` | `0x1d` | 载荷 @0 = 0 | 顺序号 |
| `EXEPRGNAME2` | `0xfc` | 载荷 256 字节 `"//CNC_MEM/USER/PATH1/O2001"` | 主程序名 |
| `RDNGROUP` | `0x4a` | **rc=6** | 这台不认这条（SDK 也 rc=6） |
| `STATINFO` | `{0x19,0xe1,0x98}` | 块载荷 14 / 4 / 2 字节，全 0 | ODBST：机床在"free" |

**改了什么**（同一轮，见 CHANGELOG 3.4.x）：会话改成两条 TCP（控制 + 数据），
握手应答的体长判据放宽，`code 24` 探针进握手、`ODBSYS` 成为 `system`/`model`/
`version` 的主路径。假机床（`clients/tests/test_focas.c`）跟着改成"一条连接一个
线程"，42 个套件全绿。

**改完之后对着这台机器跑一遍**（`focas_live.ps1`，同一份 client，不是 SDK）：
先是 **18 条读得到**（状态/模式/程序名/程序号/行号/时钟/型号/版本/倍率/主轴转速/
轴进给），`RDPOSITION` 一族 6 个点位与 `RDCOUNT`/`RDNGROUP` 失败。按下面的帧证据
改掉 `RDPOSITION` 之后**变成 24 条**：`status=free`、`mode=manual`、
`program_name="//CNC_MEM/USER/PATH1/O2001"`、`model="0M D4G3"`、`version="28.0"`，
以及**六个位置点位全部读通**（绝对/机械/相对/剩余/指令/跟踪误差，机床静止 → 全 0）。
剩 3 条失败：`RDCOUNT`、`RDNGROUP`（机床自己不认，见下）与 `RDMACROR` 的参数检查；
另外 25 条是点位表里本来就"帧待抓包"的。

> 记一笔地址：这台机器在 Wi-Fi 上是 DHCP（当时从 `.192` → `.193` → `.195` 变过两次，
> 一变会话就断）。同时宿主机的 VMware NAT 上挂着 `8193 = 192.168.79.128:8193` 的
> 端口转发，但那条转发当时**不通**（`127.0.0.1:8193` 无人应答），所以探针按当前地址打。

**这三族被拒的原因**（同一轮逐帧对照出来的）：

- `RDCOUNT`（`0x8b`，d=e=0）与 `RDNGROUP`（`0x4a`）：**机床自己回的 rc=6**，
  SDK 也一样（`cnc_rdcount` 在这台机器上就是 rc=6）—— 不是我们的帧错，这台不提供。
- `RDPOSITION`：**是我们多发了一个块**。SDK 的 `cnc_rdposition` 在这台机器上 rc=0，
  它的请求体是 **198 字节 = 2 + 7 个块**：

  ```
  code 0xa4  d=0          e=0
  code 0x89  d=0xffffffff e=0
  code 0x88  d=1          e=0
  code 0x88  d=2          e=0
  code 0xa3  d=0          e=0xffffffff
  code 0x26  d=1          e=0xffffffff     ← 真正取位置那一条（d = 位置类型，这里是"机械"）
  code 0xa4  d=0          e=0
  ```

  我们表里原来是 **9 个块**（`0x19`/`0x26`×4/`0x89`/**`0x0e`**/`0x88`/`0x19`，
  照假机床定的）。逐块试的结果：**8 个块（去掉那条 `0x0e` + `d=e=0x26f0`）全部
  rc=0**，四种位置的数组就在应答的下标 1..4（与 Cb 一一对应）；只留那一条 `0x0e`
  被拒时，块返回码是 **1**（上层就是 `NCL_FOCAS_ERR_RB_CODE`）。所以改法是把那条
  `0x0e` 拿掉（8 块帧），不是照抄 SDK 的 7 块帧 —— 两种都行，8 块这条保留了原来
  "一条请求取四种位置"的好处（见 `focas_codec.c` 里 `RDPOSITION` 的注释）。

> 两种帧的差别也说明**官方库是按机型选帧的**：对这台 0i-MD 它发 7 块（带 `0xa4`
> 轴数），对假机床（hello 应答是填充字节）它发 9 块。所以"照 SDK 的某一帧"要看清
> 是**对哪台机器**发的。

#### 10.4.1 每轴一条 **8 字节记录**（🟢 2026-09-21 真机实测；推翻了"12 字节 POSELM"）

位置/伺服负载/主轴负载/主轴转速/进给速度这几族的应答载荷**都是同一个形状**：

```
每轴（每主轴）一条 8 字节记录：
  [0..4)  data  (BE32)
  [4..6)  预留 —— 这台机器恒 0x000a，官方库一个字段都不取
  [6..8)  dec   (BE16，该轴的小数位)
载荷长度：0x26 / 0x56 = 256 字节 = 32 根轴 × 8；0x40 = 64 字节 = 8 根主轴 × 8。
```

判据（同一条链，都是真机）：

1. **256 = 32 × 8**（不是 21.33 × 12）—— 0x26/0x56 都是这个长度，0x40 是 64；
2. 官方 SDK 的 `cnc_rdaxisdata`（cls=1，位置）报 **X=116583、dec=3**，而载荷里那一格
   正好在记录 **+6** 处（+4 是恒定的 `00 0a`）→ 值 = `116583 / 10^3 = 116.583` ✓；
3. 主轴转速：`0x40 d=5` 的载荷 `00 00 08 98 00 0a 00 00` → data=2200、dec=0 →
   **2200 rpm**，与 SDK 的 `cnc_rdspmeter` 出参一致 ✓；
4. 逐字节 dump 支持轴 Z 的 `FFFFB244` = −19900 → −19.900 ✓。

> **原来按 12 字节 POSELM 切**（data@0 + `dec`@4 + `unit`/`disp` + 轴名@10）—— 那是照
> 假机床定的。真机上这条会把 dec 读成 10，于是任何值都被除成 0；从第 2 根轴起偏移也
> 全错。改法与依据都写进 `focas_values.c` 的 `record_read()`。

**"一条块就够"**（真机逐条试出来的，官方库会捎上下文块，但机床单收也认）：

| 量 | 请求 | 应答 |
|---|---|---|
| 位置 | `0x26` d = 0/1/2/3（绝对/机械/相对/剩余）、e = ALL | 256 字节 |
| 伺服负载 | `0x56` d=1 | 256 字节 |
| 主轴负载/转速 | `0x40` d=4 / d=5 | 64 字节 |
| 进给速度 / 主轴转速 | `0x24` / `0x25` | 8 字节 |
| 报警消息 | `0x23` d=−1（全部）、e=条数 | **没报警就是 0 字节** |

#### 10.4.2 报警：`arg2=2` / `arg3=64` 才填文本（🟢 实测，含一条真报警）

先说结论：**`cnc_rdalmmsg2` 的 Cb 有两个"暗格"**，不填就永远看不到报警文本 ——

```
0x23  d = 0xffffffff（-1 = 全部报警类型）  e = 条数（10）
      arg2 = 2      ← 不给 2，机床**不填消息文本**
      arg3 = 64     ← 文本要多少字节（一条记录的长度就是 16 + arg3）
```

同一条报警、三种请求（真机同一分钟里跑的对照）：

| 请求 | 应答载荷 |
|---|---|
| `d=-1 e=10`（arg2/arg3 = 0） | **16 字节**：只有抬头，没有文本 |
| `d=-1 e=10 arg2=0 arg3=64` | 80 字节，但**文本区是 0** |
| `d=-1 e=10 arg2=2 arg3=64`（官方 SDK 的形状） | 80 字节，**文本在里面** |

**一条报警记录**（80 = 16 + 64，真机那条是 SV 报警）：

```
[0..4)   报警号    (BE32) = 75
[4..8)   报警类型  (BE32) = 3
[8..12)  轴号/保留 (BE32) = 0
[12..16) 文本长度  (BE32) = 4
[16..)   文本（GB2312）—— 4 字节 = 0xB1A3 0xBBA4 = "保护"
```

两次实测把"有没有报警"这条路也钉住了：

- **没报警**时：`cnc_alarm2`（`0x1a`）数据全 0、`cnc_rdalmmsg2` 载荷 **0 字节**。
  client 对 0 字节回**空数组**（"没有报警"），不是"读不到"。
- **有报警**时（用户在那台仿真器上触发了一次）：`cnc_alarm2` = `0x8`（SV 位）、
  `ODBST.emergency` = 1、三态变 **holding**，`cnc_rdalmmsg2` 回上面那条记录 ——
  这一份 client 现在报 `{"number":75,"type":3,"text":"保护"}` ✓。

> **文本是 GB2312**，而 NC-Link 的 JSON 是 UTF-8 —— 出门前过一道
> `ncl_gb2312_to_utf8()`（`nclink/ncl_charset.h`，纯查表、7445 个码位、无第三方依赖、
> Windows/Linux 一个行为）。所以上面那条在真机上是
> **`{"number":75,"type":3,"text":"保护"}`**（机床给 4 个 GB2312 字节，出门 6 字节
> UTF-8）。ASCII 原样过去，非法字节变成 U+FFFD —— 半截汉字不会把整条报警带没。

**计时器**：`cnc_rdtimer`（`0x120`）的载荷是 **小端**（`48 01 00 00` = 328 分钟），
而线上其余各族的整数都是大端。官方 SDK 也按大端解，于是它自己读出来的也是
`1208025088` 分钟（两千多年）—— 机床侧的字节序问题，client 按**官方口径（大端）**
读，遇到这种机器就是把机床的毛病照实报出来（现场真要这个量的话，先跟机床厂对一下）。

#### 10.4.3 "同一个 item、每次问不同的号"那一族（刀补 / 宏变量 / 参数）🟢 实测

这三条都是**一个块、`d` = 号**，应答就是 §10.4.1 那条 8 字节记录（值@0 + 小数位@6）：

| 量 | item | 请求 | 真机应答 | client 出门 |
|---|---|---|---|---|
| 刀补 | `RDTOFS` | `0x08`，`d` = 刀补号、`e` = 1、**`arg2` = 1000** | 8 字节 `…00 0a 00 03` | `{"number":1,"value":0.0}` |
| 宏变量 | `RDMACRO` | `0x15`，`d` = 变量号、`e` = 1 | 8 字节 `…00 0a ff ff` | 变量 1 → `0.0` |
| 参数 | `RDPARAM` | **`0x8d`**（不是 `0x0e`！），`d` = 参数号、`e` = 1 | **264 字节**，头 4 字节就是值 | 参数 1 → `{"number":1,"value":1.0}` |

**`0x0e` 不是参数那条码**：官方 SDK 对这台机器发的是 `0x8d`，`0x0e` 那条被机床拒
（rc=1）—— 原来表里写 `0x0e` 是照假机床定的。

**"没有这个号"是方向 3 的帧**（这一轮新钉出来的，比"读到 0"重要）：

```
请求 宏变量 100  →  a0a0a0a0 0003 21 03 0008  00 00 ff ef 00 01 00 00
                              ↑ func  ↑ dir=3      ↑ 块数 0   ↑ −17
```

机床对**不存在的号**（宏变量 100、刀补 2、参数 2）回一条 **`dir = 3`** 的帧，体里
块数 0、带一个 `-17`。这不是协议错（原来 client 判成 `NCL_FOCAS_ERR_HEADER`），
也不是"读到 0"：语义层把它翻成 **`NCL_ERR_NOT_FOUND`**（"这台机床没有这一号"），
上层拿去就能说清是"没配"还是"读不到"。

#### 10.4.4 程序目录（`cnc_rdprogdir3`）🟢 实测

请求：**一个 `0x06`，`d = 0`、`e = 8`（一次要几条）、`arg2 = 1`**（官方 SDK 对这台
机器发的就是这个形状；原来表里写 `d = 0x13` 是照假机床定的）。应答是**72 字节一条**
的记录，一条一个程序：

```
[0..2)   空
[2..4)   程序号  (BE16)   例 2001
[4..8)   4 字节属性（这台第一个程序是 "M 0 "，其余是 0）
[8..72)  注释（NUL 结尾、后面补 0）   例 "(DEMOMAINGEAR)"
```

真机四个程序一次读全，client 出门：

```json
[{"number":2001,"comment":"(DEMOMAINGEAR)"},{"number":3000,"comment":"(SUBGEAR)"},
 {"number":3001,"comment":"(SUBPOCKET)"},{"number":3002,"comment":"(SUBCENTER)"}]
```

**模态（`cnc_rdgcode`，`0x96`）还差一步**：应答 12 字节 = `{short datano; short type;
char gcode[8]}`，文本在 **@4**（SDK 解出来的是 "G00"）—— 但这台机器只在**显示模态的
那一刻**才填这 8 个字节，其余时候回全 0（同一组参数 `d=0..7` 试了 8 遍都是空）。所以
模态 / 刀号（T 码）这两条先留在"待抓包"：等它真显示的时候再对一次，或者换成
`cnc_rddynamic2` 那条路（要先把 OBDDY2 的长度给对）。

#### 10.4.5 模态（`cnc_rdgcode`）与执行中的程序段（`cnc_rdexecprog`）🟢 实测

上面那段"模态先放着"的说法**这一轮推翻了**：模态那条一直是好的，是我把**两格看错**了。

**模态**：`0x96` 的 `d` = **第几组**（0..23，24 以上回 rc=3），应答 12 字节：

```
[0..2)   组号（回显）
[6..8)   代码   (BE16)
[10..12) 小数标志：非 0 → 代码 = 值 ×10（带一位小数），0 → 整数
```

判据是 24 组全扫一遍、跟官方 SDK 的渲染逐条对上：

| 载荷 | 渲染 |
|---|---|
| 17 / 40 / 54 / 80 / 98 / 160（flag=0） | `G17` `G40` `G54` `G80` `G98` `G160` |
| 401 / 131 / 501 / 542 / 805（flag=1） | `G40.1` `G13.1` `G50.1` `G54.2` `G80.5` |

（全是真 G 码：G17 平面、G90 绝对、G94 每分进给、G21 公制、G49 长度补偿取消、G54 工件
坐标系、G54.2 附加坐标系……）client 出门就是这一串：`{"gcodes":["G0","G17","G90",…]}`。
**"文本在 @4、只在显示时才填"那条是误读**：那是 SDK **自己渲染**的 ASCII 写进了它的
出参结构，线上根本没有那段文本。

**执行中的程序段**（`cnc_rdexecprog`，`0x20`，`d` = 要多少字节，SDK 给 0x594）：
应答体 = 4 字节 + ASCII 文本。**这台机器回的不是"当前那一段"，而是从执行位置起的整段
程序**（真机 515 字节：`M98P3001 / G49 / T01 / D1 / G0G43H1Z100. / M3S800 / …` 一直到
程序末尾），所以：

  * `ncl_focas_executed_block()` 老实把这段文本交出去（多行、尾部 0 与空白去掉）；
  * **刀具号先不接**：T 码在整段程序里会出现很多次（最后一个换刀不是当前刀具），
    编一个数比"读不到"更糟 —— 那条通道（`cnc_rdgcode` 的 T 组 / ODBDY2）找到再上。

**合成进给速度**：`cnc_rddynamic2` 死活回 rc=4（长度从 4 试到 192），但每轴的
`cnc_actf`（`0x24`）是通的 —— 官方 SDK 的 `cnc_rdaxisdata(cls=5 速度)` 最后也是发
`0x24`×N（抄包确认）。所以按"三根直线轴里最大的那个"给。注意**这台机器 `0x24` 只回
一根轴的 8 字节**，第 2、3 根读不到就跳过。

#### 10.4.6 这台机器**做不到**的那些（🟢 逐条实测，官方 SDK 同样被拒）

扫完一遍，剩下读不到的分两类：**我们还没核**的（照旧 `NCL_ERR_UNAVAILABLE`），和
**机床自己不给**的（下面这些 —— 官方 SDK 用同样的参数也被拒，所以不是我们的帧错）。
现场排障时先看这张表，别去查 client：

| 量 | 调用 | 机床回的 |
|---|---|---|
| 加工件数 | `cnc_rdcount`（`0x8b` d=e=0） | **rc=6** |
| 刀具组数 | `cnc_rdngrp`（`0x4a`） | **rc=6** |
| 刀具寿命 | `cnc_rdlife`（`0x8b` d=e=1） | **rc=6** |
| 工件坐标 | `cnc_rdwkcdshft`（**type 0..20 全试**） | **rc=1** |
| 动态数据 | `cnc_rddynamic2`（长度 4/8/24/48/64/128/160/192） | **rc=4** |
| 轴扭矩 | `cnc_loadtorq`（长度 4/8/24） | **rc=4** |
| 伺服那一类（`cls=2` 的负载/电流） | `cnc_rdaxisdata 2` | ✅ 通了：`cls=2 type=1` → `0x56 d=1`、`type=2` → `d=3`（2026-09-23 抓帧，§11.19.3）|
| 程序上行 | `cnc_upstart4` rc=0 → `cnc_upload4` | **rc=10**（数据那一步不给） |

能读的都在上面几节里；`cnc_rdaxisdata` 一族里 **cls=1（位置）** 与 **cls=3（主轴）、
cls=5（速度）** 是好的（`cls=3/5` 见 §10.4.1 那条 8 字节记录）。

**整表那几条也在这台机器上试过**（为它们给探针加了 `n_n`/`s4_n` 两种原型）：

| 调用 | 参数 | 机床回的 |
|---|---|---|
| `cnc_rdparanum` | — | rc=0 但数量是 0 |
| `cnc_rdparar` | 范围 1..1 | **直接崩**（SDK 内部，探针没 rc 就退出）|
| `cnc_rdmacror` | `1 1 1 1` | rc=2 |
| `cnc_rdtooldata` | type=0、5 条 | rc=1 |
| `cnc_rdtoolrng` | 5 条 | rc=3 |

所以"参数表 / 宏变量表 / 刀具表"这三条**在这台机器上做不了**：不是我们的帧问题，
是机床不提供（官方 SDK 一样被拒）。

**轴名是好的**（`cnc_rdaxisname`）：一个 `0x89`，应答**每轴 4 字节** = 名字 2 字节 +
2 字节代码（真机 X/Y/Z 都是 `58 00 94 06` / `59 00 …` / `5a 00 …`，那个代码三根轴一样，
分不出类型）。所以：

  * 轴名从这一条读得到 ✓；
  * **轴类型（linear / rotary）按 FANUC 命名约定推**（X/Y/Z/U/V/W 直线、A/B/C 回转）——
   这是**约定**不是机床上读到的一格，现场命名不按套路时覆盖档里自己改。

#### 10.4.7 收尾那一轮的几个确认（🟢 实测）

  * **`cnc_rddynamic2` 是通的那条**（之前一直 rc=4）：**`axis` 参数必须 ≥ 1**（给 0 就是
    rc=4），长度给 ≥ 48 就行（48/64/96/128/160/192/224/256 都 rc=0）。它**不是一条命令，
    而是九条捆在一起**（抄包看到的 Cb 序列）：

    ```
    0x1a(报警) 0x1c(程序号/主程序号) 0x1d(顺序号) 0x24(进给速度)
    0x25(主轴转速) 0x26 d=4 0x26 d=1 0x26 d=6 0x26 d=7
    ```

    也就是**我们已经逐条读到的那些量**（报警/程序号/顺序号/actf/acts/位置），所以这一份
    client 不需要为它单开接口 —— 顺带确认 **ODBDY2 里确实没有倍率字段**（那九条里没有
    `0x5d`），倍率只能从操作面板信号（`0x5d`）来。
  * **操作面板信号（`0x5d`）在这台机器上是桩**：载荷 32 字节，除 `@2 = 0xffff` 全是 0。
    所以本机上 `feed_override = 0`、主轴倍率无从谈起（**不是我们的偏移错**，是机床没填）。
    现场遇到"倍率一直是 0"，先查这一条。
  * `cnc_rdaxisdata` 的类：**`cls=4`（主轴）rc=0**（记录名 `S`），`cls=0/6/7/8` rc=3，
    `cls=1/3/5` 是好的（见 §10.4.1）、`cls=2` 是桩（见 §10.4.6）。
  * `cnc_rdexecprog3`（子程序号）与 `cnc_rdspdata` **这套官方 SDK 根本没导出** ——
    这台机器上这两条到此为止。
  * 两条"表信息"调用是通的、内容也像样：`cnc_rdtofsinfo` → `{type=2, 条目=400}`、
    `cnc_rdmacroinfo` → `{33, 1}` —— 哪台机器支持整表，就从这两条拿范围。

---

## 11. 本仓库实现（2026-09 落地）

能力面在 `plugins/focas.c`（点位声明）+ `clients/focas/`（协议与语义）。这一节按能力
记：**模型路径 → client 函数 → 帧/码 → 验证到什么程度**。

### 11.1 会话与验证环境

| 环境 | 是什么 | 用来验什么 |
|---|---|---|
| 假机床（`tools/site-probe/mock.py`）| 自己按抓到的帧铺应答 | 帧形状、块语义、字段位置（§10.2 的反查就靠它）|
| **NCGuide**（FS0i-F 模拟器）| FANUC 自己的模拟器，能用官方 SDK 直连 | 坐标一族、`cnc_srvdelay`（§10.1）|
| **真机**（以太网 0i-MD，2026-09-21）| 现场那台 | 终验：两条 TCP / `code 24` / 8 字节记录 / 报警 / 目录（§10.4）|

会话：`cnc_allclibhndl3()` 起两条 TCP（§10.4），本仓库的 client 保持**一个句柄复用**
（FOCAS 默认只给 2 个），轮询与按需读都走它；`--once` 自检会把模型里每个点位读一遍，
"读不了"的点位报原因而不是假装成功。

### 11.2 点位一览（模型 → client）

| 模型路径 | client 函数 | FOCAS 调用 | 验证 |
|---|---|---|---|
| `/STATUS` | `ncl_focas_status()` | `cnc_statinfo` | 🟢 假机床 + 真机 |
| `/WORK_MODE` | `ncl_focas_mode()` | `cnc_statinfo`（`aut`/`motion`）| 🟢 |
| `/PART_COUNT` | `ncl_focas_part_count()` | `cnc_rdcount` | 🟢（字段 @20 修正过一次）|
| `/WARNING` | `ncl_focas_alarm()` | `cnc_alarm` + `cnc_rdalmmsg2` | 🟡 状态位 🟢 / 文本待块形状 |
| `/MANUFACTURER` `/MODEL` `/VERSION` | `ncl_focas_manufacturer()` 等 | `cnc_sysinfo`（`ODBSYS`）| 🟢 |
| `/CONTROLLER/PROGRAM` | `ncl_focas_program_name()` | `cnc_exeprgname2` | 🟢 |
| `/CONTROLLER/PROGRAM_NUMBER` | `ncl_focas_program_number()`、`_main_program_number()` | `cnc_rdprgnum` | 🟢（`SUBPROGRAM` 不要了）|
| `/LINE_NUMBER` | `ncl_focas_line_number()` | `cnc_rdseqnum` | 🟢 |
| `/FEED_SPEED` `/SPINDLE_SPEED` | `ncl_focas_feed_speed()`、`_spindle_speed()` | `cnc_actf` / `cnc_acts` | 🟢 rc=0 |
| `/FEED_OVERRIDE` `/SPINDLE_OVERRIDE` | `ncl_focas_feed_override()` / `_spindle_override()` | `cnc_rdopnlsgnl`（`0x5d`，@0xa/@0xc）| 🟡 帧已核；本机这块是空的（§11.20.2）|
| `/AXIS@<轴>/POSITION@REAL` | `ncl_focas_axis_position()` | `cnc_rdposition`（`POSELM`）| 🟢 NCGuide |
| `/AXIS@<轴>/POSITION@CMD` | `ncl_focas_axis_position_cmd()` | 实际 − `cnc_srvdelay` | 🟢 NCGuide |
| `/AXIS@<轴>/MOTOR/VARIABLE@ABSOLUTE\|RELATIVE\|DISTANCE` | `_axis_position_machine/_relative/_distance` | `cnc_rdposition` 的四种（d=0..3）| 🟢 |
| `/AXIS@X/PATH_LEFT_LENGTH` | `ncl_focas_axis_distance()` | `cnc_rddynamic2` | 🟢 |
| `/AXIS@X/TORQUE` `/CURRENT` | `ncl_focas_axis_torque()` / `_axis_current()` | `cnc_loadtorq` / `cnc_rdsvmeter`（`0x56` `d=3`）| 🟢 NCGuide（轴温没有这条 API，点位已删）|
| `/MOTOR@S1/SPEED` | `ncl_focas_spindle_speed()` | `cnc_acts` | 🟢 |
| `/CONTROLLER/PARAMETER`（HASH）| `ncl_focas_parameter_table()` | `cnc_rdparam` / `cnc_rdparar` | 🟡 字段已核 |
| `/CONTROLLER/VARIABLE`（LIST）| `ncl_focas_variable_table()` | `cnc_rdmacror`（`0x15` 号段读）| 🟢 NCGuide（§11.20.1）|
| `/CONTROLLER/TOOL`（LIST）| `ncl_focas_tool_list()` | `cnc_rdtooldata` / `cnc_rdtoolrng` | ⛔ 机床不提供 |
| `/CONTROLLER/TOOLPARAM` | `ncl_focas_tool_param_table()` | `cnc_rdtofs` / `cnc_rdtofsinfo` | 🟢 |
| `/CONTROLLER/COORDINATE` | `ncl_focas_work_offset/…s()` | `cnc_rdzofs`（`0x0b`）/ 写 `0xc` | 🟢 读 + 写（§11.19）|
| `/SESSION` `/ITEMS`（方法）| `ncl_focas_system()`、`ncl_focas_read_item()` | 会话自检 / 按 item 码裸读 | 🟢 调试用 |

### 11.3 位置一族（采集核心）

`POSELM` 每轴 **12 字节**（`int32 data` + `dec=3` + `unit` + `disp` + `name` + `suff`），
值 = `data / 10^dec`；一条 `cnc_rdposition` 就把四种位置都拿到（d=0 绝对 / 1 机械 /
2 相对 / 3 剩余），比逐轴 `cnc_absolute` 快得多。**指令位置**没有直接接口：
`cnc_srvdelay`（`0x26`，d=9）给的是**每轴跟随误差（8 字节记录）**，`指令 = 实际 − 误差`。

### 11.4 负载 / 电流 / 转矩（轴温没有这条 API）

码在（`cnc_rdsvmeter` 的 `0x56`+`0x89`、`cnc_rdspmeter` 的 `0x40`/`0x8a`），但这台机床
回"功能不支持"——**官方 SDK 同样被拒**（§10.4.6），所以点位如实回 `读不了`（`NCL_ERR_UNAVAILABLE`），
不编造数值。

### 11.5 参数与宏变量

参数走 `cnc_rdparam`（`IODBPSD`：`datano`@2、`type`@4、`ldata`@8），表结构先问
`cnc_rdparainfo`/`cnc_rdparar`（“一次能读几个”由它说）；宏变量走 `cnc_rdmacro`（`ODBM`），
**长度要给对**（给 12 回 `EW_LENGTH`）——这一条还差最后一步（§9 待办 #2）。

### 11.6 刀补 / 刀具参数 / 刀具表

`cnc_rdtofs`（字段 @0）与 `cnc_rdtofsinfo` 已进 client；`cnc_rdlife`（寿命）字段 @12 已核。
机床的**刀具列表**（`TOOL`）它不提供（rc=1/3），模型里那一条点位因此标"机床不提供"。
写刀补走 `cnc_wrtofs`（§11.9）。

### 11.7 报警

状态位 `cnc_alarm`（`ODBALM`，最多 10 条）🟢；文本要 `cnc_rdalmmsg2`（`0x23`，`arg2=2`、
`arg3=64` 才填），**块形状还差**——所以 `WARNING` 现在答得出"有没有报警"，答不出文本
（§9 待办 #1）。

### 11.8 程序与文件

**下发**（`cnc_dwnstart4` → `cnc_download4` → `cnc_dwnend4`）🟢 2026-09-23 通了：
帧形状（516 字节定长、`[2..4)=1`、`[4..6)="N:"`）+ 正文格式（`\nO<号>\n…\n%`）都对上，
**关键在连接**：这三条帧必须走 hello 计数器 1 那条 TCP（§11.18，发到命令通道上机床
直接断连接）。设备侧 `/CONTROLLER/FILE` 的 `push` 就是这条链路（`pull` 见下、`remove`
走 Cb `0xb6`）。

**取回**：流式上行（`cnc_upstart4`/`cnc_upload4`/`cnc_upend4`）在这台机器上被机床拒
（`0x18` 不答、`0x19` 回 `EW_REJECT=13`，§11.18.3），所以走的是**按文件名按行读**
（`cnc_rdpdf_line`，Cb `0xf0`，§11.16）—— 那条通了，程序正文原样取回。

程序目录 `cnc_rdprogdir3` 🟢（**要翻页**：一次只回 8 条，见 §11.17.2）。

### 11.9 写这一侧

| 能力 | 函数 | 本仓库 |
|---|---|---|
| 刀补写 | `cnc_wrtofs` | `/CONTROLLER/TOOLPARAM` 的 `set_value` |
| 参数写 | `cnc_wrparam` | `/CONTROLLER/PARAMETER` 的 `set_value` |
| 宏变量写 | `cnc_wrmacro` | `/CONTROLLER/VARIABLE` 的 `set_value` |
| 程序下发 | `cnc_dwnstart4` 链 | `/CONTROLLER/FILE` 的 `push` |
| 程序删除 | `cnc_delprogram` | `/CONTROLLER/FILE` 的 `remove` |

**权限在外面**：这些 `set_value` 只声明"能不能做"，谁能写由部署侧的白名单决定。

### 11.10 两条口径（和 10 册同一套）

1. **取值形状决定操作**：dict/HASH 答 `get_keys`，list/LIST 答 `get_length`，两者不同时声明。
2. **没验过的不假装**：帧形状没核出来的点位（报警文本、刀具列表、负载那一族）如实回
   "读不了"或"机床不提供"，不编数值、不猜结构。

### 11.11 缺项核对：以 10 册（新代）的代码为参照（2026-09-22，逐条按代码对）

核对方法：把两边的 **client 头文件 + 适配器点位声明**摆在一起对，**不看注释看代码** ——
client 里落到 `not_yet()` 的算"桩"，适配器里没声明的算"没摆出来"，两边都没的算"整格缺"。
（注释与代码已经有漂移：`feed_speed`/`feed_override`/`axis_load` 的头注释写着"还没实现"，
实现其实在，这一轮一并纠正。）

**A. 新代做了、FOCAS 缺**（按"缺在哪一层"分）

| 能力域 | 新代（10 册）| FOCAS 现状 | 缺在哪一层 |
|---|---|---|---|
| **PLC / 寄存器 / 位** | `/CONTROLLER/REGISTER@{R,I,O,C,S,A}` 读 + 写 | `/CONTROLLER/REGISTER@{X,Y,G,F,R,K}`（位）+ `@D`（字）**读通了**（§11.21）；写还没做 | 帧已核、client + 点位都在 |
| 变量（宏变量）| `/CONTROLLER/VARIABLE` 读 + **写** | 点位在，整表是桩；**单条 `ncl_focas_macro_variable` 已实现** | 表可以像新代那样**逐号读**；写要新写 |
| 参数 | `/CONTROLLER/PARAMETER` 读 + **写** | 点位在，整表是桩；**单条 `ncl_focas_parameter` 已实现** | 同上 |
| 刀补（TOOLPARAM）| 读 + **写**（读回打底再覆盖）| 点位在，整表桩；单条 `ncl_focas_tool_offset` 已实现 | 同上；写是桩 |
| 刀具表 `TOOL` | 读 + 写 | 点位在，client 桩（**这台机床也不提供**，§10.4.6）| 口径一致（都答"不提供"）|
| 位置：机械 / 相对 / 剩余 | 每轴 6 格（实际/指令/机械/相对/剩余…）| `axis_position_machine` / `_relative` / `_distance` **都实现了，只是没声明** | **只差声明** |
| 轴覆盖 | X/Y/Z/A/B/C/U/V/W 九轴 | client 枚举只有 X/Y/Z/A/C | client 加轴 + 声明 |
| 轴名 / 轴数 | 参数区动态查轴名（`/AXES`）| 没有（`cnc_rdaxisname` 只在注释里提过）| **client 缺** |
| **G 代码文件** | exist / dir_exist / file_new / dir_create / delete / copy / move / list + push + pull | 只接了 push / pull / remove | client 缺那六条；**`pull` 还不落盘（见 D）** |
| 报警历史 | 无（只有当前报警）| 无 | 两边都缺，先不动 |

**B. FOCAS 比新代多**（顺手记下，别只往一边看）：`/WORK_MODE`、`PROGRAM_NUMBER` / `SUBPROGRAM`、
执行中的程序段、模态（`cnc_rdgcode`）、坐标系、计时器、刀具寿命 / 组数、主轴负载、`/ITEMS` 裸读。

**C. 只差"接线"就能补上的**（client 已实现、模型没摆或写没开）：

1. `/AXIS@<轴>/MOTOR/POSITION`、`MOTOR/VARIABLE@RELATIVE`、`MOTOR/VARIABLE@DISTANCE` 三个位置格；
2. `/CONTROLLER/PARAMETER`、`/CONTROLLER/VARIABLE`、`/CONTROLLER/TOOLPARAM` 改成**逐号读**
   （单条读都已实现），这三个"桩点位"立刻变成真点位；
3. 执行中的程序段 / 程序目录 / 模态（`executed_block` / `program_directory` / `modal` 都在）。

**D. 顺手修掉的一个真 bug**：`focas_file_pull()` 拿到程序字节后是 `(void)path;` +
`ncl_free_safe(bytes);` —— **根本没往本地写**（新代那条是 `ncl_file_write_all(path, …)`）。
本轮按新代的写法修好了（`plugins/focas.c`）；程序取回本身还是桩，等 `program_upload` 落地这条就通。

**E. client 里仍是桩的 22 条**（要新写的）：

```
axis_current  axis_temperature  axis_torque  spindle_load  spindle_override
macro_variables  macro_write  parameter_table  parameter_write  program_delete
program_select_main  program_upload  subprogram_number  tool_life  tool_list
tool_number  tool_offset_write  tool_param  tool_param_table  variable_table
work_offset  work_offsets
```

> 其中 **主轴倍率 / 刀具寿命/组数** 这台机床本来就不提供（§10.4.6），补了也是
> 继续报"不提供"；真正值得按新代补齐的顺序是：**参数写 → 变量写 → 刀补写 → 程序取回 →
> PLC/寄存器（要连 `pmc_*` 一起做）**。

#### 11.11.1 现状盘点（2026-09-23 复测：把公开接口挨个问一遍）

上面那张 E 表是 09-22 的，之后程序文件、参数/刀补写、目录翻页、传输那几轮都补上了，
所以这里重新盘一次 —— 方法不是看注释，是**把 `focas.h` 的 68 个公开函数挨个调一遍**，
逐条打 `rc` + `last_error`（工具：**`tools/site-probe/focas_sweep.c`** 挨个调；要逐条看某个 item 的原始应答用
`tools/site-probe/focas_item.c`（`focas_item 127.0.0.1 8193 RDPARAM 6711 6711`）；
试连接/帧走哪条用 `tools/site-probe/focas_channel_probe.py`）：

**已经通了（这台 0i-MF 模拟器上 `rc=0`，逐条实测）**

| 域 | 通的 |
|---|---|
| 会话 / 系统 | 两条 TCP + hello、`session`、`read_item`、`system`（型号/系列/版本/轴数）、`emergency` |
| 状态 | `status`、`mode`、`alarm_status`、`alarm`（这台无报警 → 空数组）、`part_count`（参数 6711）、`timer` |
| 程序 | `program_name` / `program_number` / `main_program_number` / `line_number` / `executed_block` / `modal`（24 组）/ `program_directory`（**14 条，翻页**）/ `program_download` / `program_upload`（0xf0）/ `program_create` / `program_delete` |
| 轴（**一根轴通就算通**，这台是 3 轴机） | `axis_position`（实际）/ `_machine` / `_relative` / `_cmd` / `_distance` / `_srv_delay` / `_feedrate` / `_load` / `_torque` / `_type` |
| 主轴（**单算**） | `spindle_speed`（S1）、`spindle_load`（S1） |
| 进给 | `feed_speed`、`feed_override` |
| 参数 / 刀补 | `parameter`（单条）、`parameter_table`（逐号读）、`tool_offset` / `_typed` / `_count`、`tool_list`（64 号）、`tool_param` / `tool_param_table`、`tool_offset_write` / `_typed`、`tool_param_write` |

**还没通的，分四类（这是重点）**

| 类别 | 条目 | 卡在哪 |
|---|---|---|
| ① 模型里看得见、client 是桩（`rc=-15`）| 只剩 `/SPINDLE_OVERRIDE` 一格（帧有了、机床那块空着）；`SUBPROGRAM` 已删、`/CONTROLLER/VARIABLE` 已做（§11.20）| 见下 |
| ② 写这一侧 | **写参数**（`0xa0`）、**写宏变量**（`0x16` 刻度）| 帧形状/刻度没对 |
| ③ 整格缺（点位都没有）| **G 代码文件族的其余几条**（exist / copy / move / list…）、报警历史（PLC/寄存器**读已通**，§11.21）| 要新做 |
| ④ 口径要改（不是"没实现"）| 刀具表上限 64 vs 机器 400 | 改声明 |

① 里每条的落点（都核过一遍）：

| 点位 | 现在 | 该怎么做 |
|---|---|---|
| `/CONTROLLER/VARIABLE`（宏变量表）| ✅ **已做**：`0x15` 的**号段读**（`d` = 起始号、`e` = 结束号），空号不列（§11.20.1）| — |
| `/CONTROLLER/COORDINATE`（工件坐标系）| ✅ **已通**：`cnc_rdzofs`（`0x0b`）读、`cnc_wrzofs`（`0x0c`）写，读 + 写都实测过（§11.19）。原来问错的调用是 `cnc_rdwkcdshft` |
| `/AXIS@X/CURRENT`（轴电流）| ✅ **已通**：`cnc_rdsvmeter`（`0x56`）`d=3` = 安培、`d=1` = 负载表 %（§11.19.3）|
| `/AXIS@X/TEMPERATURE`（轴温）| ✅ **点位已删**（FOCAS 没有这条 API，见 §11.19.3）|
| `/TOOL_NUMBER`（当前刀号）| ✅ **已通**：`cnc_rdcommand`（`0x97`）里 `adrs='T'` 那条的 `cmd_val`（§11.19.2）|
| `/CONTROLLER/SUBPROGRAM` | ✅ **点位已删**（用户口径不要了；官方库对 `cnc_rdexecprog3` 一帧不发，§11.20.3）| — |
| `/SPINDLE_OVERRIDE`（主轴倍率）| 桩 | 现代系列 `IODBSGNL.spdl_ovrd` 没这格；试 `cnc_rdspdata` 或参数 |

② 写这一侧的具体卡点：

* **写参数**（`0xa0`）：官方 SDK 发的是 **20 字节载荷**（`IODBPSD`：`datano`/`type`/`data` 各占一格）+
  独立的 `length` 参数；本实现按写刀补那个形状补了 8 字节，机床回 `dir 3`（"没有这一号"）——
  载荷形状还没对。
* **写宏变量**（`0x16`）：机床**收下**了、读得回来，但**刻度差 10 倍**（写 1.0 读回 10）。写后
  复核如实回 `NCL_ERR_UNAVAILABLE`（不假装成功）—— 差的是"值那一格的刻度/dec 该怎么送"。

### 11.12 对着 VM 里的 FANUC 模拟器核对：**读全通、写还没通**（2026-09-23）

这一轮接上了一台真在跑的 FANUC：VMware 里那台 **CNC Guide & NC Trainer plus**
（Windows 7 + CNC Guide，机床显示 `Series 0i-MF Plus`、`0M D4G3` / `28.0`）。

**怎么接上的**（VM 那一侧按用户给的地址是 169.254.178.13，主机够不着）：

| 现象 | 处理 |
|---|---|
| `169.254.178.13` 从主机**没有路由**（`ping` 直接 general failure）| 那是客户机自己的 APIPA：VM 的网卡是**桥接**、所在链路没有 DHCP |
| 迁到 NAT 就有现成转发（`vmnetnat.conf` 里早写着 `8193 = 192.168.79.128:8193`）| 用 `vmrun stop/start`（不占 GUI、不要管理员）把 `ethernet0.connectionType = "nat"` 写进 vmx 后重启客户机 |
| 起来后 | 客户机 DHCP 拿到 **192.168.79.128**，主机 **127.0.0.1:8193** 就通了（`a0a0a0a0 0003 0102 0168 0011 …` 的 FOCAS2 hello）|

**读这一侧逐条过了一遍**（`tools/site-probe/focas_live.c`，全对着真模拟器）：

| 结果 | 条目 |
|---|---|
| 🟢 通（有值）| `STATUS`=free、`WORK_MODE`、`MODEL`=`0M D4G3`、`VERSION`=`28.0`、`PROGRAM`=`O0`、`LINE_NUMBER`、`PROGRAM_NUMBER`、`FEED_SPEED`、`FEED_OVERRIDE`、`SPINDLE_SPEED`、**X/Y/Z 的 position / machine / relative / cmd / distance / srv_delay / load / feedrate**、`axis_type`=linear、`executed_block`=`O0000%`、**`PARAMETER` #1**、**`TOOL_OFFSET` #1**、`program_directory`、`modal`、`alarm` |
| ⛔ 这台机床没有（如实报）| `POSITION A/C` 一族（`-6` = 没有这根轴）|
| 🟡 有码但结果异常 | `PART_COUNT` / `TOOL_GROUP_COUNT`（`0x200000b4` = 应答块返回码非 0：这两条的码或 `d/e` 在这台 0i-MF 上还要再看）、`feedrate Z`（`-11` 越界）、宏变量 #500（`-6` 这台没有这个号）|

**写这一侧：试出来的形状被机床拒了**。按"读的码 + 1"猜了三条 ——
`cnc_wrparam` = `0x8e`（读是 `0x8d`）、`cnc_wrmacro` = `0x16`（读是 `0x15`）、
`cnc_wrtofs` = `0x09`（读是 `0x08`）—— 值放在命令块后面的载荷、长度写进块的 `tag0`；
三条都**读得到、写回去被拒**（`NCL_FOCAS_ERR_RB_CODE` = 应答块返回码非 0）。
所以那三条**退回 `not_yet()`**（宁可不做，也不留一个形状没核的写入口）。

**下一步的抓包路子已经打通**（这一轮把工具链也修好了）：

```
# 1) 官方 SDK 要**在它自己的目录里跑**（不然依赖的 fwlibe64/fwlib0DN64 找不到，
#    cnc_allclibhndl3 直接回 EW_SOCKET=-15）：
cd D:\downloads\focas-test2x64
focas_sdk_probe64.exe 127.0.0.1 8193 cnc_statinfo --dll .\Fwlib64.dll      # rc=0 ✓

# 2) 中间插一层 tap，把 SDK 发的字节原样抄下来：
python tools/site-probe/focas_tap.py 8194 127.0.0.1 8193
focas_sdk_probe64.exe 127.0.0.1 8194 cnc_wrparam --dll .\Fwlib64.dll
```

`tools/site-probe/focas_sdk_probe.c` 的 `kCalls` 里这一轮补了 `cnc_wrparam` /
`cnc_wrmacro` / `cnc_wrtofs` / `cnc_rdparanum` / `cnc_rdparar` / `cnc_rdwkcdshft` /
`cnc_rdtoolnum` / `cnc_rdtooldata` 几条；它们现在是 `void` 形状、入参是一块**全零**的
缓冲区 —— 所以 SDK 会先本地判 `EW_NUMBER`（号 0 不合法）**不发帧**。再给探针加一个
`--in HEX`（按结构体铺初值）就能把写帧抓出来，照抄进 client。

### 11.13 写这一侧打通：`cnc_wrtofs` 写进去又读回来 + 这台机床"有什么/没什么"清单（2026-09-23）

接着 §11.12 往下：把写帧抓齐、把缺的那些读补上，逐条对着同一台 NCGuide 模拟器核。
**这一轮之后，`not_yet()` 只剩机床真不给的那几条**（下面有清单），没有"码没抓".

#### 11.13.1 工具链：三条命令就够

```powershell
# 探针（官方 SDK）： cwd 必须在 DLL 目录，否则依赖 DLL 找不到 → cnc_allclibhndl3 回 -15
cd D:\downloads\focas-test2x64

# 一次抓一串：内部起代理（默认 8194 → 机床），一条调用一个进程，把请求/应答摊开印
python D:\codex\nclink-c\tools\site-probe\focas_capture.py `
    "cnc_wrtofs 1 1 8 8" "cnc_rdtofs --shape s3p 1 1 8"

# 两个客户端的会话逐帧对齐（SDK vs 本仓库 client）：一帧一行，命令帧打 Cb 与 d/e/a2/a3
python D:\codex\nclink-c\tools\site-probe\focas_tap.py 8199 127.0.0.1 8193 sdk.log
python D:\codex\nclink-c\tools\site-probe\focas_log_summary.py sdk.log
```

这一轮给探针补了 `--in HEX`（把出参那块 4 KiB 缓冲**先铺初值**——写的那几条全靠它，
全 0 时 SDK 自己就在本地判非法、一帧都不发）与 `--shape`（临时改调用形状，不用重编），
并照官方头 `FWLIB32.H` 的形状表补了 `s2_l / s3_l / s2_l1 / s1p / s2p / s3p / s_np / np /
s2_np / parar / parar2` 几种调用形状。

#### 11.13.2 请求块的载荷：长度写在 **`tag1`**，不是 `tag0`

写这一类（`0x09` / `0xa0` / `0x16`）是**一条命令块 + 紧跟一段载荷**。块里两格是这样用的
（逐字节对过官方 SDK 的帧）：

```
块 = size(2) first(2) index(2) code(2) d(4) e(4) arg2(4) arg3(4) tag0(2) tag1(2)
       ^ size = 0x1c + 载荷长度                                   ^ 0     ^ = 载荷长度
```

§11.12 那一轮把长度写进了 **`tag0`**，机床回 `EW_LENGTH=2`（`resp 0x09 rc=2`），
所以三条写全被判"被拒"。改到 **`tag1`** 之后同一帧机床回 `rc=0` —— 差的就这一格。
（应答块这一格位置也对得上：应答块头 16 字节、长度在 `[14..16)`，同样是"块尾"。）

#### 11.13.3 写刀补：`0x09`，写进去又读回来

```
Cb      0x09、d = 刀补号、e = 1、arg2 = 1000 + 刀补类型、块长 = 0x1c + 8
载荷    8 字节 = BE32 值（最低输入单位，0.001mm 那一档）+ BE16 0 + BE16 0xffff
```

`tools/site-probe/focas_write.c`（现编现跑）对模拟器实测：

| 步骤 | 结果 |
|---|---|
| 读 1 号刀补（type 1 半径）| `0.008` |
| 写 `2.2345` 进去 | `rc=0` |
| 再读 | `2.234` —— **一致** |
| 还原 `0.008` | `rc=0` |

刀补类型（`arg2 = 1000 + type`）与四个字段的对应，两边老代码一致：官方头
`cnc_rdtofs(h, ofs_num, type, len, ODBTOFS*)` + 现场那份服务
`NCLINK-SERVICE/mods/focas/focas.cpp` 的 `getToolParams`（type 1 = 半径、3 = 长度、
0 = 半径磨损、2 = 长度磨损）。client 里 `ncl_focas_tool_offset_typed()` /
`ncl_focas_tool_offset_write_typed()` / `ncl_focas_tool_param[_write]()` 就是这四格。

#### 11.13.4 这一轮补上的读（都对着模拟器核过）

| 项 | 帧 | 结果 |
|---|---|---|
| `TOOL` / `TOOLPARAM`（刀具表）| `cnc_rdtofsinfo` = **`0x0a`**（载荷 @2 = `use_no` = **400**）+ 每号 4 条 `0x08` | 🟢 逐号读；元素 = `{id, kind, radius, length, radius_wear, length_wear}` |
| 轴扭矩 | `cnc_loadtorq` = **`0xfd`**，d = 电机号（0 = 伺服）、e = 轴号（1 起）| 🟢 机床收支（`d=3 e=7` 回 `EW_RANGE`），本机静止恒 0，**量纲未定标** |
| 参数表 | 逐号 `0x8d` 读（`RDPARAM`）| 🟢 交前 64 号（一段一条，本实现的约定）|
| 删程序 | `cnc_delete` = **`0x05`**，d = 程序号 | 🟡 帧照官方 SDK 抄；**这台模拟器回 `EW_ATTRIB=5` 不收**，所以如实报模块错 |
| `cnc_rdparar`（参数段读）| 复用 `0x8d`：`d = 起始号、e = 起始类型、arg2 = 结束号、arg3 = 结束类型` | 🟡 码核到了，这台机器回 `EW_RANGE`（段不合法），先不用 |

#### 11.13.5 机床自己的返回码：`1`/`6` = "这台没有"，不是"调用出错"

以前 `EW_FUNC=1`、`EW_NOOPT=6` 一路冒到上层成了**模块错**
（`NCL_FOCAS_ERR_RB_CODE`），点位表里看着像 client 坏了。现在 `ncl_focas_check_blocks()`
把这两个码翻成 **`NCL_ERR_UNAVAILABLE`**（"声明了、但这台机床读不到"）。
这台模拟器上因此变成如实回话的几条：

| 点位 | 以前 | 现在 |
|---|---|---|
| `PART_COUNT`、`TOOL_GROUP_COUNT` | `0x200000b4` 模块错 | **`-15` 机床不提供**（官方 SDK 同样被拒）|
| `cnc_rdtooldata`（刀具管理数据）| — | **`EW_FUNC=1`**：这台机型没有 |
| `cnc_rdlife` / `cnc_rdtoolrng`（寿命管理）| — | **`EW_NOOPT=6`**：寿命管理选件没开 |
| `cnc_rdmacro` / `cnc_wrmacro`（用户宏变量）| — | 读 **`EW_NOOPT=6`**、写 **`EW_NUMBER=3`**：这台没开用户宏变量 |
| `cnc_rdwkcdshft`（工件坐标系）| `rc=1` | **`-15`**（0x63 帧，机床明说不支持）|

#### 11.13.6 单条读/写：`d` 与 `e` 都是**号**

这一格以前写死 `e = 1`，于是**只有 1 号读得出来**：

| 调用 | 官方 SDK 的帧 | 说明 |
|---|---|---|
| `cnc_rdparam 6711` | `0x8d d=6711 e=6711` | 单条读时 `d` = `e` = 号 |
| `cnc_wrtofs 2 …` | `0x09 d=2 e=2` | 写也一样 |
| `cnc_rdmacro 500` | （SDK 不发）本实现发 `0x15 d=500 e=500` | 发下去机床回 `rc=0` 并给记录 |

改过来之后**整片都通了**：刀补 2 号、参数 6711、宏变量 500 以前都回"块返回码非 0"，
现在是正常值。顺带纠正两条**以前读错**的：

* **加工件数**（`PART_COUNT`）：以前走 `cnc_rdcount`（`0x8b`，刀具寿命计数器）—— 机床要
  开寿命管理选件才答，这台回 `EW_NOOPT`。改成读 **6711 号参数**（加工总件数 6712），
  与现场那份服务的 `getPartCount`/`getPartTotal` 一致。
* **参数的值在载荷 `@8`**，不是 `@0`：载荷前三格都是 BE32（`@0` 号、`@4` 条数、`@8` 值）。
  以前按 `@0` 读，读到的是**参数号自己** —— 只有 1 号参数看着对（1 == 1 巧合），
  6711 会读出 6711。用 20 号（值 4）与 6000 号（值 4）交叉核过。

#### 11.13.7 还没通的：写参数（`0xa0`）、写宏变量的刻度、程序上传

* **写参数**：官方 SDK 对这台机器发的是 **`0xa0`**、d = 参数号、e = 1、**不带载荷**；
  机床回块返回码 `0` 而 SDK 自己回 `EW_LENGTH=2` —— 值没送出去（看着像"这条命令还要
  后续块"）。本实现按写刀补那个形状补 8 字节载荷试：机床**收下（块码 0）但值不变**
  （写 0 到 1 号参数、再读还是 1）。所以 `ncl_focas_parameter_write()` 做了
  **写后复核**：值没变就回 `NCL_ERR_UNAVAILABLE`（"这台写不进去"），**绝不回成功**。
  载荷形状还没对，等真机或反汇编那条 `0xa0` 的后续块。
* **写宏变量**（`0x16`）：帧机床**收下**（块返回码 0，读也读得到），但值的刻度对不上 ——
  写 500 号 625、读回来是 6250（差 10 倍），写 501 号 626.25、读回来 6260。
  这台模拟器的宏变量小数位（dec）这一格与实际刻度不自洽（`@6` 回 0，值却是 ×10 的）。
  所以 `ncl_focas_macro_write()` 做了**写后复核**：读回来的值与要写的不在一个刻度上就回
  `NCL_ERR_UNAVAILABLE`（"机床收了帧但读回来是 …"），**绝不回成功**。
* **程序上传**（`cnc_upstart4` / `cnc_upload4`）：SDK 在这台机器上 `start4` 回
  `EW_ATTRIB=5`/`EW_NOOPT=6`，**一帧都没发**；应答里程序文本的切法仍旧没核到，
  所以这一格继续 `NCL_ERR_UNAVAILABLE`。

#### 11.13.8 复现清单

```
python tools/site-probe/focas_capture.py "cnc_rdtofsinfo" "cnc_rdtofs --shape s3p 1 1 8"
D:\downloads\simulators\tools\focas_write.exe 127.0.0.1 8193        # 写刀补 写→读→还原
D:\downloads\simulators\tools\focas_live.exe  127.0.0.1 8193        # 读这一侧逐条
ctest --test-dir build -C Release                                    # 43/43
```

### 11.14 程序上/下行：两条现场口径 + 传输的状态回执在**方向 3**（2026-09-23）

用户给的现场口径（**这一条是 FANUC 的规矩**）：

> 程序正文的**第一行必须是程序号** —— `O0001` / `O00001` 都行；机床是从这一行认
> 程序号的，正文里没有这一行，这份程序机床不收。

```
O0001
G01 X100 Y100
M30
```

#### 11.14.1 帧（官方 SDK 实测，`cnc_dwnstart4/download4/dwnend4`）

| 步骤 | 帧 | 体 | 应答 |
|---|---|---|---|
| start | `func 0x11 dir 1` | 定长 **516** 字节：`[1] = 种类`（0 = NC 程序）、`[3] = 1`、`[4..6) = "N:"`、`[6..)` = **目录** | `dir 2`、256 字节（全 0） |
| data | `func 0x12 dir 4` | 就是程序正文（一块 ≤ 1400 字节；SDK 一块 1024） | **不回** |
| end | `func 0x13 dir 1` | 空 | **`dir 3`**、体前 4 字节 = 机床返回码 |

**start 那一格给的是目录**（`//CNC_MEM/USER/PATH1/`），**不是文件名**：给成
`//CNC_MEM/USER/PATH1/O0001` 这种文件路径，机床在 start 那一步就回
`EW_DATA=5` + **细码 1**（= 目录名不对，见 §11.18.2）。程序号不从路径来，从正文
第一行来（也就是上面那条口径）。

#### 11.14.2 状态回执在**方向 3**（以前把这一族当"没有这个数"了）

`0x13`（end）的应答长这样（SDK 抓的真帧）：

```
a0a0a0a0 0003 13 03 0008  | 00 00 00 05 00 04 00 00
            ^func ^dir=3    ^^^^^^^^^^^ ^^^^^ ^^^^^
                            返回码 4 字节 = 5    细码   err_dtno
                            （EW_DATA）      （4 = 这个程序号已经登记过）
```

普通调用里**方向 3 是"没有这个数"**（§2.2 rule 3，宏变量/刀补/参数里不存在的号就是
这条）；可传输这一族里它是**状态回执**。client 以前一律按"没有这个数"处理，于是
"机床为什么不收"整条信息丢了。现在 `focas_transfer_exchange()` 单独认这一族：
方向 3 + 码 0 = 这一步成了；码非 0 = 翻成 `NCL_FOCAS_ERR_TRANSFER2(码, 细码)`，
`last_error` 里出人话 —— 连**细码**一起（`机床回码 5（EW_DATA（数据错）），细码 4
（这个程序号已经登记过（下行 end））`）。

> ⚠️ **返回码表以前抄错了一格**：`5` 不是 `EW_ATTRIB` 而是 **`EW_DATA`**（`4` 才是
> `EW_ATTRIB`）。表和细码表都是从官方头文件 `Fwlib64.h` + spec 的
> `cnc_dwnstart4/cnc_dwnend4/cnc_upend4` 里逐条对出来的，见 §11.18.2 ——
> 这一格错着的时候，现场明明看到的是"数据错"却按"属性错"去查，白绕了一圈。

#### 11.14.3 这台模拟器上：下行**通了**，上行被机床拒（2026-09-23 更新）

| 动作 | 结果 | 说明 |
|---|---|---|
| 下行到**空号**（`//CNC_MEM/USER/PATH1/` + 正文 `\nO0303\n…\n%`）| start `rc=0` → data 发出 → **end `rc=0`** ✓ | 程序真进去了：目录里多出 `303`，按文件名读回来一字不差（§11.18.3）|
| 下行到**已存在的号** | end **`EW_DATA` + 细码 4** | "这个程序号已经登记过" —— 换一个空号即可（这不是错，是机床的规矩）|
| 下行**目录名写错** | **start 就回** `EW_DATA` + 细码 1 | "目录名不对" |
| 上行（`0x15` 流式）| start 收 256 字节应答，`0x18` **一声不响**，`0x19` upend 回 **`EW_REJECT=13`** | 机床明确说"这个状态下不给你上行"（spec：`Uploading is disable in the current CNC status`）。**取程序请用 §11.16 的 `cnc_rdpdf_line`（0xf0）**，那条通了 |

上行的请求帧已经和 SDK 逐字节对齐过（`0x15` 与下行同形、`0x18 dir 4` 体 8 字节、
`0x19` 收尾），换了"裸文件名/目录名/各种体长（4/8/1024/1400）"**六种形状**，
`0x18` 一律不答、`0x19` 一律 `EW_REJECT` —— 所以流式上行在这台机器上就是被拒的，
不是我们发错。哪天真机肯答 `0x18`，再回来把应答里文本的切法定下来。

#### 11.14.4 没解决的一条（**2026-09-23 已解决，见 §11.18**）

> 🟢 **结论**：`0x11` 不是"帧不对"，也不是"TCP 层面/会话来源"——是**发错了一条 TCP**。
> 机床把两条连接分了工：**第一条只收传输帧、第二条只收命令帧**，发错那一条它就
> 一声不响地断连接。本 client 当时把三条传输帧发在第二条上（命令通道），所以逐字节
> 相同也被关。§11.18 有全套实测。下面这一节保留原样，当**排查过程的记录**看 ——
> 它把"不是帧、不是时序、不是通道数、不是机床忙"这几条都钉死了，正是这些排除把
> 范围逼到了"那还剩什么"上。

本仓库 client 发的 `0x11` 与官方 SDK **逐字节相同**（`focas_log_summary.py` 逐帧对过：
526 字节、连 `tag` 都一致），可机床**对 SDK 那份回 256 字节、对本 client 这份直接把连接
关掉**。更硬的一条证据：把 SDK **发过的原始字节**从 tap 日志里抠出来、一个字节都不改地
重放（`focas_replay_verbatim.py`），机床**还是关连接** —— 而几秒前 SDK 亲自发同样的字节
它却答了。会话里的每一条前序帧（hello×2、`0x18` 探针）同样逐字节相同。

也就是说：**这一条不是"帧写错了"**，机床是在帧以外（TCP 层面/会话来源）区分的，
机理还没找到，记在这儿当开口项：

已经排除掉的（都试过）：

| 试法 | 结果 |
|---|---|
| 逐字节对齐（含 `tag`），用 `focas_log_summary.py` 对过 | 帧一模一样 |
| **原样重放 SDK 的字节**（`focas_replay_verbatim.py`）| 一样被关 |
| 会话形状：只开一条连接 / 两条顺序反过来 / 不发探针 / 先读程序目录再传 | 一样被关 |
| 发法：start 帧拆成"头 10 字节 + 体"两次 send | 一样被关 |
| 时间：探针应答后停 600 ms（client）/ 2 s（重放，帧间全停）| 一样被关 |
| 通道：把下行那三条发到控制通道 / 另开第三条连接 | 一样被关（第三条连接 hello 回 `dir 3` + 码 4 = EW_RANGE）|
| **重启 CNC GUIDE 之后，先用 client 试**（在 SDK 之前）| 一样被关 —— 所以不是"机床卡在传输忙" |

> ☝️ 表里"通道"那两行的口径要按 §11.18 更正：角色跟着 **hello 里的计数器**走
> （计数器 1 = 传输通道、2 = 命令通道），而本 client 当时把三条传输帧发在
> **计数器 2 那条**上（代码里叫"数据通道"、业务调用也走它）—— 那正是机床会断的
> 一条。当时那两行一次发在第二条、一次发在新开的第三条连接上，所以怎么试都被关。

给 tap 加时间戳后看到的两边差别只有快慢（SDK：探针应答后 9 ms 发 start、机床 16 ms
后回 256 字节；client：0.35 ms 就发、机床**立刻**关连接）—— 但把延时拉长也一样被拒，
所以时间不是判据。

```
python tools/site-probe/focas_capture.py "cnc_dwnstart4 --shape dwn4 0 --name //CNC_MEM/USER/PATH1/"   # SDK：start rc=0
D:\downloads\simulators\tools\focas_transfer.exe 127.0.0.1 8193 //CNC_MEM/USER/PATH1/O0001           # client：0x11 无应答
python tools/site-probe/focas_replay_verbatim.py <SDK 那次的 tap 日志>                                # 原样重放：同样被关
```

（client 这一侧**不会假装成功**：拿不到应答就是 `NCL_DRV_ERR_TRANSPORT`，上层看到的是
"程序下行没通"。）

### 11.15 "操作文件要做的配置"就是**数据格式**：开头 LF + 结尾 `%`（2026-09-23）

官方 spec 里写得很死（`CNC Application Development Kit` 的
`Document/SpecE/Program/cnc_download4.xml`，用户机器上那份 `FANUC_Focas_API-main.zip`
里就有全套）：

> **NC data format**: NC data to be registered to CNC is a string composed of ASCII
> characters as the following format: `LF Block1 LF Block2 LF ... LF %`（LF = `\n`）
> **'LF' must be placed at the top of the whole program, and `%` at the end.
> Data before the first 'LF' are ignored.**
> In case of NC program, **address 'O' and program number must be placed in the program**
> to be registered.
> 例子：把 `O1234 / G1 F0.3 W10. / M30 / %` 注册进去，就发
> **`"\nO1234\nG1F0.3W10.\nM30\n%"`**。

**现场实测（同一台 NCGuide 0i-MF）**：

| 发出去的正文 | 结果 |
|---|---|
| `O0001\nG01 X100 Y100\nM30\n`（少开头 LF、少结尾 `%`）| start `0` → download4 `0` → **end `5`（EW_ATTRIB）**，程序目录不变化 |
| **`\nO0001\nG01 X100 Y100\nM30\n%`**（按 spec）| start `0` → download4 `0` → **end `0`**，程序目录**立刻**出现 `{"number":1,"comment":"()"}` |

所以上一节那个"EW_ATTRIB 收不下"根本不是配置开关，而是**帧里的正文没按 spec 的格式**。
`ncl_focas_program_download()` 现在自己补齐并校验（`program_frame()`）：开头的 LF、结尾的
`%`、`O<号>` 那一行（缺 `O` 号行本地就回参数错）；调用方给一份普通程序文件即可。

#### 11.15.1 spec 里另外几条"现场要配/要注意"的（都跟程序文件有关）

| 出处 | 原文/要点 |
|---|---|
| `cnc_dwnstart4` | `dir_name` = **目标文件夹**（例 `//CNC_MEM/USER/PATH1/`），最长 241 字符，NULL = 当前文件夹；**只有 type = 0 用得到** |
| `cnc_upstart4` | 上传给 `file_name`：**文件名**（`O1234`，读当前文件夹里的）/ **文件夹**（`//CNC_MEM/USER/PATH1/`，读该文件夹里**所有**程序）/ **路径+文件名**（`//CNC_MEM/USER/PATH1/O1234`）；`cnc_upload4` 的 `*length` **必须 ≥ 256 且是 256 的倍数** |
| `cnc_upload4` | 读回来的文本是 `% LF Block1 … LF %`，**最后一个字符是 `%`**；再读就是 `EW_RESET`。受保护的程序（O8000-/O9000- 保护、编码）读回来是 `"% LF % LF"`，然后 `cnc_upend4` 回 `EW_PROT` |
| `cnc_dwnend4` | 错误在 end 这条上回（"error status for some cnc_download4 callings just before the finish is returned by cnc_dwnend4"），这正是我们先前看到 `EW_ATTRIB` 的位置 |
| 错误码 | `EW_ATTRIB` 数据类型非法；`EW_DATA` 细码 1 = 文件夹名不对 / 字符非法、2 = TV check、3 = **程序数满**、4 = **同号程序已注册**、5 = 同号程序正在被选中；`EW_PROT` = **O8000-/O9000- 保护或编码**；`EW_OVRFLOW` = 内存不够；`EW_BUFFER` = 缓冲满/空，重试；**`EW_REJECT` = 加工中/复位中/换模式中不能传**；`EW_ALARM` = PW000；`EW_PARAM` = **参数要在屏幕上开"参数写入使能"** |
| `GENERAL.HTM` | 文件夹名要以 `/` 结尾，否则当成文件名；**0i-D 上 CNC 内存里用不了"文件夹"这一层**；频繁注册/删除程序要用 `cnc_saveprog_start` / `cnc_saveprog_end`（否则每次都写非易失存储） |
| 会话条数 | 第三条 TCP 连接 hello 得到的是 **`dir 3` + 码 4（EW_RANGE）** —— 一条会话就是**两条** TCP，别多开 |

#### 11.15.2 上行取程序：请求帧都核完了，卡在机床不答 `0x18`

上行三件套的请求侧已经逐条核过（官方 SDK + spec 对得上）：

```
cnc_upstart4(h, 0, file_name)   func 0x15、体与下行同形那 516 字节
                                （[1] = 种类、[4..6) = "N:"、[6..) = 文件名/目录/路径+文件名）
cnc_upload4(h, &len, buf)       func 0x18、dir 4、体 8 字节（全 0）；*len 必须 ≥256 且是 256 的倍数
cnc_upend4(h)                   收尾
读回来的文本                    `% LF Block1 LF … LF %`，最后一个字符是 `%`（再读就是 EW_RESET）
```

**这台模拟器上取不到文本**：start 回 256 字节 ✓，但 `0x18` 那条机床**一声不响**
（SDK 自己回 `EW_DATA=10`，`cnc_getdtailerr` 的细码是 **0** —— 机床连理由都没给）。
试过并且都一样的：

| 试法 | 结果 |
|---|---|
| `file_name` 三种写法（`O3001` / `//CNC_MEM/USER/PATH1/` / `//CNC_MEM/USER/PATH1/O3001`）| 都是 `upload4 rc=10`、缓冲里一个字都没有 |
| 上行的**三代**都试（`cnc_upstart` 第一代 / `cnc_upstart3` 第三代 / `cnc_upstart4`）| 一样 |
| 把 **O3001 选成主程序**后再试 | 一样 |
| 紧跟在 `cnc_upload4` 之后问 `cnc_getdtailerr` | `err_no = 0`（没细码）|

所以"回读程序文本"这一格仍旧是 `NCL_ERR_UNAVAILABLE`，但**缺的只剩机床那一侧的应答**：
哪天在真机（或补上这块的模拟器）上抓一次 `0x18` 的应答，切法就能定。

### 11.16 取程序改用 `cnc_rdpdf_line`：**按文件名按行读**，模拟器上通了（2026-09-23）

§11.15.2 那条路子（`cnc_upstart4`/`cnc_upload4` 的流式上行）在这台模拟器上卡死在
`0x18` 不答。换到**文件那一家族**试，通了：

#### 11.16.1 帧（官方 SDK 抓的）

```
请求   func 0x21、一个块：size = 284 = 0x1c + 256、first = 1、index = 1、code = 0xf0
                          d = 0（起始行号，程序头是 0）、e = 32（读几行）、
                          arg2 = arg3 = 0、tag0 = 0、**tag1 = 256**（载荷长度）
       载荷 = **256 字节**的程序路径（NUL 补齐）→ //CNC_MEM/USER/PATH1/O3001
应答   块 size = 400 = 16 + 384、code = 0xf0、返回码 0，体 = **程序正文**：
       O3001(SUBPOCKET)\nG1990(GROUPSTART)\n…\nM99\n%
```

要点：

* 路径必须是"**盘名 + 路径 + 文件名**"：给裸名 `O3001` 机床回 `EW_DATA`（细码 1 =
  程序路径错）；`//CNC_MEM/USER/PATH1/O3001` 才对（"只给文件名 = 当前文件夹"那条
  是**手册**的口径，这台机器照它回错，所以 client 自己把默认文件夹补上前缀）。
* **一次读几行**（`e`）+ 起始行号（`d`）就能分页把整份程序读完；末行没有 EOB（`'\n'`）
  的那一段就是结尾（spec："最后一行没读到 EOB 不算一行"）。
* 手册把 `cnc_rdpdf_line` 标成 **HSSB 专用**（支持表 `O-O-`，以太网列是 `-`），
  **可这台 NCGuide 模拟器的以太网照答** —— 真机上要复核（改用 §11.15.2 的
  `cnc_upload4`，或退回 HSSB）。

#### 11.16.2 client 怎么实现的

`ncl_focas_program_upload(type = 0, name, &program, &len)`：

* 用 item `RDPDFLINE`（Cb `0xf0`）+ `call("payload")` 带上 256 字节路径载荷
  （块的 `tag1`/块长由 `ncl_focas_body_add_payload()` 写对）；
* 一次要 `FOCAS_PDF_LINES_PER_CALL`（64）行，按"回了几行"推进 `d`，末行没有 `'\n'`
  或机床报错就收尾；上限 `FOCAS_PDF_PROGRAM_MAX`（256 KiB）；
* `name` 带 `/` 就原样用，裸文件名自动补 `//CNC_MEM/USER/PATH1/`；
* 一段都没读到 → `NCL_ERR_NOT_FOUND`（不编内容）。

**实测**（`focas_transfer.exe`，模拟器）：

| 调用 | 结果 |
|---|---|
| `ncl_focas_program_upload(f, 0, "//CNC_MEM/USER/PATH1/O3001", …)` | `rc=0`、**384 字节正文** |
| `ncl_focas_program_upload(f, 0, "O3001", …)` | `rc=0`、384 字节（自动补文件夹）|
| `type != 0` | `NCL_ERR_UNAVAILABLE`（只有 NC 程序能这么读）|

#### 11.16.3 顺带：怎么"借 SDK 的会话"扫传输帧

（当时）本仓库 client 发传输 start（`0x11`/`0x15`）会被这台机床直接关连接（§11.14.4；
真因是发错了一条 TCP，见 §11.18 —— 现已修），所以那一轮**自己的客户端扫不了这一族帧**。
办法是让官方 SDK 去建会话、代理在中间**插帧**：

```
python tools/site-probe/focas_inject.py 8290 127.0.0.1 8193 inj.log 0.6
focas_sdk_probe64.exe 127.0.0.1 8290 cnc_upstart4 --shape up4 0 --name O3001
```

就是靠这个扫出 `0x19` 会被答（回 `dir 3` + 码 13 = `EW_REJECT`、11 = `EW_PATH`）——
机床**是**实现了文件/程序那一族的，只是状态不对时它就回"拒绝执行"。

### 11.17 程序**文件**那一族（建 / 读 / 删）在以太网上都通了（2026-09-23）

`cnc_pdf_*` 这一族把机床的程序当**文件**操作，帧都是"**一个块 + 256 字节路径载荷**"
（`tag1` = 256、块长 = `0x1c + 256`）。实测（NCGuide 0i-MF）：

| 动作 | 函数 | Cb | 块里的参数 | 结果 |
|---|---|---|---|---|
| 读内容（按行）| `cnc_rdpdf_line` | **`0xf0`** | `d` = 起始行号、`e` = 读几行 | 应答体 = 程序正文 ✓ |
| 建文件/文件夹 | `cnc_pdf_add` | **`0xb5`** | `d` = 0 文件 / 1 文件夹 | `rc=0`；机床**自己写**程序号那行（内容是 `O2200%` —— 末尾 `%`、**没有换行**）|
| 删文件/文件夹 | `cnc_pdf_del` | **`0xb6`** | — | 路径存在 `rc=0`；不存在回 `EW_ATTRIB` |

> 上一轮把 `0xb6` 判成"码不对"，其实是**拿不存在的路径试的**（机床回 `EW_ATTRIB`）——
> 换成存在的路径就 `rc=0`。教训：判"码不对"之前先确认参数是对的。

**踩到的坑（已修）**：`cnc_pdf_add` 建出来的内容末尾是 `%` 而**不是换行**，
client 原来用"数到的换行数"当"有没有读到东西" → 这种单行内容数出来 0 行 → 误报
"这台没有这个程序"。现在判据改成**文字长度**，行数只用来推进。

client 侧现在有（都与上面三条对上）：

```
ncl_focas_program_upload(type = 0, name, &program, &len)   // 读（0xf0，分页读整份）
ncl_focas_program_create(name, folder)                     // 建（0xb5）
ncl_focas_program_delete(name)                             // 删（0xb6）
```

#### 11.17.1 写**内容**还剩的最后一块

| 路子 | 状态 |
|---|---|
| `cnc_wrpdf_line`（按行写）| ✗ 官方库**连帧都不发**（手册 `O-O-`，HSSB 专用）|
| 流式下行 `cnc_dwnstart4`（格式已核：`\nO<号>\n…\n%`）| ✅ **通了**（2026-09-23 修：传输帧得走 hello 计数器 1 那条连接，见 §11.18）|

最后那条的排查过程留在 §11.14.4（当时排到"帧逐字节相同也被关"，正说明问题不在帧上），
根子是**发错了一条 TCP**；§11.18 有全套实测。

#### 11.17.2 程序目录要**翻页**（一次只回 8 条，2026-09-23 修）

`cnc_rdprogdir3`（item `RDPROGDIR3`，Cb `0x06`）**一次只回 8 条记录**（72 字节一条），
要拿全就得顺着号问：`d` = 起始程序号、`e` = 一次要几条；下一轮从这一页**最大号 + 1**
接着问，问到"这一页没有新号"为止。

**症状**：库里当时有 12 个程序（`O0001…O0005`、`O0101/O0102`、`O2001/O2100/O2200`、
`O3000…O3002`），原来只发一条请求，列表里**只有号最小的 8 个** —— 现场"程序列表不全"
就是这么来的。修完这台机器列出 14 条（含机床自带的 101/102/2001/2100/2200/3000/3001/3002）。

两条实现口径：

* **不认 `d` 的机床**（每一页都把同一批号重铺）也会把同一批号重复列出来 —— 所以收记录时
  滤掉"号 ≤ 上一页已收的最大号"，这样最坏情况也只是少列，不会重复；
* 最后那次"空页"是**必须**的（机床不告诉你一共几条）：12 条记录 = 三次请求（8 + 4 + 0）。

### 11.18 传输三件套跑通的真正原因：两条 TCP 是**按 hello 计数器分工**的（2026-09-23）

§11.14.4 那个开口项（"帧逐字节相同、机床就是不收"）破在这里：**不是帧写错了，也不是
TCP 层面有什么猫腻，是发错了一条连接**。

#### 11.18.1 分工：一条收传输帧、一条收命令帧

会话是两条 TCP，两条都得 hello（§2.1），但机床给它们派了不同的活：

| 连接（看 hello 里那个计数器）| 只收 | 发错那一族会怎样 |
|---|---|---|
| **计数器 1** 那条 | **传输帧**：`0x11/0x12/0x13`（下行）、`0x15/0x18/0x19`（上行）| 往它上面发 `func 0x21`：**连应答都不给，直接断连接** |
| **计数器 2** 那条 | **命令帧**：`func 0x21`（和收尾那条 `0x02`）| 往它上面发传输帧：同上 |

**分的是计数器，不是"先连上的那条"**：把两条连接 hello 的计数器对调（第一条报 2、第二条
报 1），角色跟着对调 —— 报 2 的那条收命令、报 1 的那条收传输。所以口径要记成
"**谁是传输通道看 hello 里那个数**"，别记成"第几条"。

钉住它的一帧都不用改，只换连接（`tools/site-probe/focas_channel_probe.py`）：

```
# 会话照官方库的样子建：两条 TCP + hello(1) / hello(2) + 命令通道上那条 code 24 探针
python tools/site-probe/focas_channel_probe.py <host> <port> ardg    # 0x21 发计数器 1 那条 → 无应答、连接被断
python tools/site-probe/focas_channel_probe.py <host> <port> a11     # 0x11 发计数器 1 那条 → 应答 dir 2、体 256 ✓
python tools/site-probe/focas_channel_probe.py <host> <port> b11     # 同一条 0x11 发计数器 2 那条 → 无应答、连接被断
python tools/site-probe/focas_channel_probe.py <host> <port> swap    # 计数器对调 → 角色跟着对调
```

官方库自己的抓包（tap 带连接标签）也是这个分工：

```
#1  hello(0001) → 360 字节；之后 0x11 start → 0x12 数据帧 → 0x13 end、收尾 0x02 全在这里
#2  hello(0002) → 360 字节；连接期那条 func 0x21（code 24 探针）在这里
```

本仓库 client 当时把三条传输帧跟业务调用一样发在**计数器 2 那条**上（代码里叫"数据通道"），
所以帧逐字节相同也照样被关 —— §11.14.4 里那一长串排除（时序、通道数、重放、机床忙……）
的最后一块拼图就是它。

**改法**（`clients/focas/focas_driver.c`）：握手时把两条都 hello 完，再**按计数器**定角色 ——
`ctx->socket` = 命令通道（所有 `func 0x21` 走它）、`ctx->transfer` = 传输通道
（`focas_transfer_exchange()` / `focas_send_only()` 走它）。默认 `helloCounter = 1`，
与官方库一致；配置把计数器改成 2 起头也照样对得上（角色跟着计数器走，不跟着顺序走）。

#### 11.18.2 顺带校准：EW 返回码表 + 状态回执里的"细码"

状态回执（应答方向 3）的体是 **8 字节**：`[返回码 4][细码 2][err_dtno 2]`（都是大端）。
返回码就是 `Fwlib64.h` 里的 `EW_xxx`，**老文档把 `5` 写成 `EW_ATTRIB` 是错的**（`5` 是
`EW_DATA`、`4` 才是 `EW_ATTRIB`）—— 表照官方头文件逐条重抄：

| 码 | 名字 | 码 | 名字 |
|---|---|---|---|
| 0 | EW_OK | 8 | EW_OVRFLOW（内存放不下）|
| 1 | EW_FUNC（这机型/状态没这条功能）| 9 | EW_PARAM（参数不让写）|
| 2 | EW_LENGTH | 10 | EW_BUFFER（缓冲空，再要一次）|
| 3 | EW_NUMBER / EW_RANGE | 11 | EW_PATH（路径不对）|
| **4** | **EW_ATTRIB / EW_TYPE** | 12 | EW_MODE（模式不对）|
| **5** | **EW_DATA（数据错）** | 13 | EW_REJECT（这状态下不干）|
| 6 | EW_NOOPT（选件没开）| 15 | EW_ALARM |
| 7 | EW_PROT（写保护）| 21 | EW_RD_RSTFIN（读到程序尾）|

**细码**（`[4..6)`）= `cnc_getdtailerr` 的 `ODBERR.err_no`，含义**按是哪一条帧**查 spec：

| 帧 | 细码 | 含义 |
|---|---|---|
| `0x11` start | 1 | 目录名不对 |
| `0x13` end | 1 / 2 / 3 / 4 / 5 | 正文有非法字符 / TV check 块内字符数为奇数 / 已登记的程序数满了 / **这个程序号已经登记过** / 这个号正被机床选中 |
| `0x19` upend | 2 / 3 | 指定范围里没有程序 / 程序内存坏了 |

同一台机器上实测到的三条（`focas_transfer.exe`）：

```
0x11 start → 00000005 0001 0000   目录名写错（拿 //CNC_MEM/USER/NOPE/ 试的）
0x13 end   → 00000005 0004 0000   程序号已存在（同一个号再下一次）
0x19 upend → 0000000d 0000 0000   EW_REJECT：上行被机床拒
```

client 现在把这些一起带到 `last_error`：

```
PROGRAM_DOWNLOAD: 机床回码 5（EW_DATA（数据错）），细码 4（这个程序号已经登记过（下行 end））
```

#### 11.18.3 修完之后的实测矩阵（NCGuide 0i-MF Plus，2026-09-23）

| 能力 | 帧 | 结果 |
|---|---|---|
| 会话（两条 TCP + hello 1/2）| `0x01` | ✓ 两条都回 360 字节 |
| 会话探针 | `func 0x21` + 一个 `code 24` 块（走命令通道）| ✓ 回 ODBSYS（载荷 36 字节，`MD4G328.003`）|
| **下发程序（写内容）** | `0x11`（体 516）→ `0x12 dir 4` 数据帧 → `0x13` | ✓ `rc=0`；程序真进机床：目录里多出这个号、读回来一字不差 |
| 按文件名读程序 | `func 0x21` + Cb `0xf0`（256 字节路径）| ✓ 正文逐字节一致 |
| 建文件 | Cb `0xb5` | ✓ `rc=0`（机床自己写 `O<号>%`）|
| 删文件 | Cb `0xb6` | ✓ 存在的路径 `rc=0`；不存在的回块码 5 |
| 程序目录（**翻页**）| Cb `0x06`（`d` = 起始号、`e` = 8）| ✓ 14 条全列出来（修前只有 8 条，见 §11.17.2）|
| 流式**上行** | `0x15` → `0x18` → `0x19` | ✗ `0x18` 一声不响、`0x19` 回 `EW_REJECT=13` —— 机床拒绝（SDK 在同样状态下也一样）；**取程序请用 `0xf0` 那条**（§11.16）|
| 按行写 | `cnc_wrpdf_line` | ✗ 官方库连帧都不发（手册 `O-O-`，HSSB 专用）|

复现三行：

```
python tools/site-probe/focas_channel_probe.py 127.0.0.1 8193 a11 //CNC_MEM/USER/PATH1/ "\nO0303\nG01 X1 Y2\nM30\n%"
D:\downloads\simulators\tools\focas_transfer.exe 127.0.0.1 8193 //CNC_MEM/USER/PATH1/ nolist O0303   # 本 client 下发
D:\downloads\simulators\tools\focas_read.exe    127.0.0.1 8193 O0303                                # 读回来对一遍
```

#### 11.18.4 这台模拟器的两个怪癖（别当成自己的 bug）

1. **目录表不是即时的**：`cnc_pdf_del` 回 `rc=0` 之后，按文件名读**已经读不到**（确实删了），
   可 `cnc_rdprogdir3` 还把它列着；反过来，那个号再"下载"一次机床**不报"已存在"**
   （说明登记表里已经没了）—— 两个表各更新各的。**对账要以"读文件"为准**，别拿目录
   当存在性判据（本实现里也没拿它当判据）。
2. **上行一族的拒绝不看时机**：`0x18` 六种形状（`dir 1`/`dir 4`、体 4/8/1024/1400）
   都不答，`0x19` 一律 `EW_REJECT` —— 所以这台机器的上行是"能力上没有"，不是"时机不对"。

### 11.19 一轮抓帧补三条：工件坐标系 / 当前刀号 / 轴电流（2026-09-23）

用户口径：**坐标系肯定有、刀具号也要、温度不要了**。于是拿官方 SDK（
`tools/site-probe/focas_sdk_probe.c`，配 `cap.py` 把每条调用的收发帧打出来）对同一台
NCGuide 0i-MF 把这几条抓了个遍 —— 结论是"坐标系一直都在，只是我们**问错了调用**"。

#### 11.19.1 工件坐标系 = `cnc_rdzofs`（**0x0b**），不是 `cnc_rdwkcdshft`

`cnc_rdwkcdshft`（`0x63`）type 0..20 全试过一律 rc=1（那是"工件坐标**平移**"，不是
偏移表）；真正读 G54…G59 的是 **`cnc_rdzofs`**：

```
请求   func 0x21，一个块：code = 0x0b、d = 偏移号、e = 偏移号、arg2 = 轴号（-1 = 全轴）
应答   载荷 256 字节 = 32 条 8 字节记录（MAX_AXIS = 32），第 i 根轴的值在 @8×(i-1)
       记录 = [值 BE32][00 0a][dec BE16]   ← 与位置/负载那一族同一形状
```

    number：**0 = 外部零点偏移、1..6 = G54..G59、7..306 = G54.1P1..**

**写**（`cnc_wrzofs`，码按"读 + 1"推 = **0x0c**，机床认）：载荷与写刀补/宏变量同形
（`[值 BE32][00 00][ff ff]`），**一次只写一根轴**（`arg2` = 轴号，1 起；给 -1 机床不收）。
"对拍"验过：写 12345 进 G54 的 X，读回载荷 `@0` 就是 `00003039`。

两个坑（都踩过）：
* **写进去不是立刻读得到**：写完紧跟着读回的是旧值，隔一会儿才是新值 —— 所以 client 的
  写后复核**读 6 遍、每遍隔 50 ms**，别把成功报成"没写进去"；
* 偏置量是**最低输入单位**的整数，小数位要拿机床报的 `dec`（记录 `@6`）换算
  （这台 X 是 `dec=3` → 12345 就是 12.345 mm）。

#### 11.19.2 当前刀号 = `cnc_rdcommand`（**0x97**）里 `adrs = 'T'` 那条

`cnc_rdgcode`（0x96）只报 G 组，`0x2e` 那条回的是整段程序 —— 当前刀号在**指令值**里：

```
请求   func 0x21：code = 0x97、d = -1（全部模态非 G 码）、e = 1
应答   载荷 = 12 字节一条记录，一条一个地址：
       [adrs(1)][num(1)][flag(2)][cmd_val(4 BE)][dec_val(4 BE)]
```

这台一次回了 **D/E/F/H/L/M/N/O/S/T** 十条，字母就是地址（`'T'` = 0x54）——
**取 `'T'` 那条的 `cmd_val` 就是刀号**。`d = 100..129` 可以逐条要。

#### 11.19.3 轴电流 = `cnc_rdsvmeter`（0x56）的 `d = 3`；**轴温 FOCAS 没有**

官方 `cnc_rdaxisdata` 其实只是**把几条老调用拼起来**（前面带上轴数/轴名上下文块），
把它的 `cls/type` 对着抓到的帧摊开就是一张现成的表：

| `cls` | `type` | 实际发的 item |
|---|---|---|
| 2 Servo | 1 负载电流（%）| **0x56** `d = 1`（就是 client 现在的 `/AXIS@X/TORQUE` 那条负载表）|
| 2 Servo | 2 负载电流（安培）| **0x56** `d = 3` ← 轴电流用这条 |
| 3 Spindle | 0 主轴负载表 | **0x40** `d = 4` |
| 5 Speed | 0 进给率 F | **0x24**（ACTF）|

**轴温不做了**：官方头 `Fwlib64.h` + spec 全文搜过，FOCAS 里**没有**读轴温的调用
（只有"智能终端高温报警"那种报警码），所以那个点位直接从模型里删掉，不留"待抓包"。

#### 11.19.4 没抓到的两条（如实记）

* **子程序号**：`cnc_rdexecprog3` 官方库**自己就回 EW_PROT、一帧都不发**
  （`cnc_rdexecprog2` 那条还没试，probe 里没编）；所以 `/CONTROLLER/SUBPROGRAM`
  继续回"还没有"。顺带一条口径：表 4 里 `SUBPROGRAM` 是 **string（子程序名）**，
  本适配器声明成了 int64 —— 等这条通了要一起改。
* **主轴倍率**：`cnc_rdspdata` **这份 SDK 里没有这个导出**（0i-D 那套 DLL 查过）；
  而 `IODBSGNL` 的 spec 写明 **bit6 = 主轴倍率信号只有 Series 15i 用**，
  0i-D/F/30i 那几格是 "Not used" → 现代系列上这条得另找入口（真机或别的系列再定）。

#### 11.19.5 client 落地 + 实测（同一台模拟器）

| 能力 | 帧 | 实测 |
|---|---|---|
| 读坐标系 | `0x0b` | ✓ `ncl_focas_work_offset("G54")` → `{"x":…,"y":…,"z":…,"number":1}`；整套 7 个（EXT + G54..G59）|
| 写坐标系 | `0x0c` | ✓ 写 G54 的 X = 12.345 → rc=0，读回 12.345（写后复核 6 次重试）|
| 当前刀号 | `0x97` | ✓ `ncl_focas_tool_number()` 取 `'T'` 那条；机床没给 T 那条时如实回 NOT_FOUND，**不报 0** |
| 轴电流 | `0x56 d=3` | ✓ `ncl_focas_axis_current()`（安培）；X/Y/Z 三根都读得到 |
| 轴温 | — | 点位删掉（FOCAS 没这条 API）|

### 11.20 宏变量表（号段读）与主轴倍率（2026-09-23）

用户口径：`SUBPROGRAM` 不要了、`SPINDLE_OVERRIDE` 需要、`/CONTROLLER/VARIABLE` 做完。

#### 11.20.1 宏变量表 = `0x15` 的**号段**读（不是"一次最多 5 个"）

`cnc_rdmacro`（`0x15`）一条码两种问法，抓帧（同一台 NCGuide 0i-MF）：

```
单条读   >> code=0x15 [d=100][e=100] → 载荷 8 字节 = 号 100 那一条
号段读   >> code=0x15 [d=1][e=5]     → 载荷 40 字节 = **5 条 8 字节记录**（一条一个号）
         >> code=0x15 [d=1][e=20]    → 载荷 160 字节 = 20 条
```

记录形状与单条读一样：`[值 BE32][00 0a][dec BE16]`，第 j 条就是 `起始号 + j`。
**空号**（spec 的 vacant：`值 0` 且 `dec = -1`）**不进表** —— "运行变量"里不该出现没定义的
号，也不硬凑一个 0。

一段能要多少个号**别开太大**：本机实测 `d=33 e=64` 只回 **1 条**、`d=1 e=40` 回 33 条
（264 字节封顶）—— 所以 client 按 **16 个号一段**问，1..999 一共 63 次调用。

> ⚠️ 这台模拟器的宏变量区**不太自洽**：有些没定义的号回 `0 + dec 0`（看着像"定义了、
> 值是 0"），有些回 `0 + dec -1`（vacant）。所以本机上这张表有 487 条 —— 按 spec 判
> 没问题，真机上空的号一般就是 vacant。别拿条数当"这台只有这么多变量"。

#### 11.20.2 主轴倍率 = `0x5d` 的 `spdl_ovrd`（就在进给倍率后面那一格）

`IODBSGNL` 的偏移图（client 的 item 表里那张）：`… rpd_ovrd@6、jog_ovrd@8、feed_ovrd@0xa、
spdl_ovrd@0xc、blck_del@0xe …` —— **主轴倍率与进给倍率同一条 `cnc_rdopnlsgnl`（`0x5d`）、
同一张码值表**（0..20 = 0%..200%，每级 10%），只是格子在 @0xc。

两点如实记：
* spec 的 `slct_data` 位表上写着"bit6 = 主轴倍率信号**只有 Series 15i**"，而 0i-D 那版
  头文件 `Fwlib64.h` 的 `IODBSGNL` **有** `spdl_ovrd` 这一格（15i 那版才标 "(not used)"）
  —— 字段在、能不能填看机型；
* **本机这块是空的**（`0x5d` 回的是没初始化的内容）：落在码表里就像"0%"、落在码表外
  就报错。**真机要复核**（现场看到"倍率一直 0"先查这里，与进给倍率同一类现象）。

#### 11.20.3 `SUBPROGRAM` 删掉

用户口径"不要了"：官方库对 `cnc_rdexecprog3` 自己就回 `EW_PROT`、一帧都不发（§11.19.4），
`cnc_rdexecprog2` 那条还没试 —— 与其挂一个永远"读不到"的点位，不如不摆。client 的
`ncl_focas_subprogram_number()` 与 `/CONTROLLER/SUBPROGRAM` 一并删除。

### 11.21 寄存器（PMC）读通了：`pmc_rdpmcrng` = **0x8001**（2026-09-23）

用户口径："FANUC 的 PLC 寄存器就是 PMC"。于是把 `pmc_*` 那一族里最要紧的读法拿官方
SDK 对同一台 NCGuide 0i-MF 抓了一遍，**读通了**（写这一轮没做，见 §11.21.5）。

#### 11.21.1 帧（官方向 SDK 抓的）

```
请求   func 0x21，一个块：code = 0x8001、块头第 2 格 = **2**（别的命令都是 1）
       载荷四格 = [起始号 BE32][结束号 BE32][族 adr_type BE32][宽度 data_type BE32]
应答   块 = [大小][2][1][0x8001][返回码 4 字节][数据长度 4 字节][数据…]
       数据 = 每个点 1(字节)/2(字)/4(长字) 字节，大端
```

* 族 `adr_type`：**0=G 1=F 2=Y 3=X 4=A 5=R 6=T 7=K 8=C 9=D**
* 宽度 `data_type`：0=字节、1=字、2=长字；**结束号 = 起始号 + 要几个**
  （spec 的例子：`D0100` 字型 → `s=100 e=101 length=10` 读 1 个）
* 号段上限（0i-D 那版 spec）：X/Y `0..127`（字节）、G/F `0..767`、
  R `0..7999`、K `0..99`、D `0..7999`（**字**）。位族在模型里按"字节 × 8 + 位"
  摊平成位号（`X0.0` = 0、`X1.0` = 8）。

**踩到的第一颗钉子**：块头第 2 格不对时，机床回的块头是乱的（客户端会把它当
"块返回码非 0"报错）。照官方库发 **2** 才对 —— 这一格是这一族专有的。

#### 11.21.2 客户端

`ncl_focas_pmc_read(focas, family, start, count, width, &json)`：一个号段一个数组，
每点一个值；`ncl_focas_pmc_bit(focas, family, bit, &on)` 读一位（读所在字节再取位）。
族认不出、号段越界、一段要太多（> 512）都在本地挡下来，不编数。

#### 11.21.3 这台模拟器的脾气：**一次只回一个点**

要 8 个字节它就回 1 个（`d=33 e=64` 那种也一样）。所以段读**先试一次，回得不够就
退化成一个号一个号读**补齐 —— 真机上次段读只会更快，两边都站得住。

实测（客户端直接打 127.0.0.1:8193）：

| 族 | 读法 | 结果 |
|---|---|---|
| F（CNC→PMC）| 字节 0..7 | `[64,128,0,8,4,0,0,0]` —— 真的信号位 |
| Y（PMC→机床）| 字节 0..7 | `[4,0,0,1,0,0,2,0]` |
| X（机床→PMC）| 字节 0..7 | `[0,0,0,0,0,0,73,1]` |
| G / R / K | 字节 0..7 | 全 0（这台没用到）|
| D（数据表）| 字 0..7 | `[260,1,0,…]`（`D0 = 260`）|
| X 的位 0..7 | 位读 | 全 false（与字节值对得上）|

#### 11.21.4 适配器侧：`/CONTROLLER/REGISTER@<族>`

声明了 7 条（与新代那几条寄存器点位同一个口径：LIST，答 `get_length` / `get_value`
（`keys` 给号）/ `get_attributes`；**这一轮只读**，所以不声明 `set_value`）：

```
/CONTROLLER/REGISTER@X  @Y  @G  @F  @R  @K   → 位（true/false），号 = 字节×8+位
/CONTROLLER/REGISTER@D                       → 字（u16），号 = 字号
```

`get_length` 用的是 spec 上 0i-D 那版的范围（X/Y 128 字节 → 1024 位、G/F 768 → 6144、
R 8000 → 64000、K 100 → 800、D 8000 字）。**机器实际支持多少可以用 `pmc_rdpmcinfo`
（`0x8003`）问**：那条帧已经抓到了（应答载荷 772 字节 ≈ 44 条记录、字母每 12 字节一条），
只是逐条布局还没核 —— 核出来就把 `get_length` 换成"问机床要"。

#### 11.21.5 还没做的

* **写**（`pmc_wrpmcrng`）：帧还没核（形状应该是"同一个 0x8001 的体、载荷里带上数据"），
  所以点位不声明 `set_value`。
* 定时器/计数器（`T`/`C` 那两族是"2 个字 + 状态"的结构体）：这轮没接。
* `pmc_rdpmcinfo` 的逐条布局（见上）。

### 11.22 PMC 写：`pmc_wrpmcrng` = **0x8002**（2026-09-23 抓帧）

用户口径："有可能有的寄存器不能写，但是肯定有能写的。" —— 写这一侧的帧抓到了，
而且**用官方库验证过确实能写进去**。

```
请求   func 0x21，一个块：code = 0x8002（读 + 1）、块头第 2 格 = 2
       载荷 = [起始号 BE32][结束号 BE32][族 BE32][宽度 BE32]
              **[数据长度 BE32][数据…]**（每个点 1/2/4 字节，大端）
应答   块 16 字节、返回码 0
```

实测（同一台 NCGuide 0i-MF，官方库）：

```
写 R0..R1 = 0x33 0x44（s=0 e=1 length=10）→ rc=0
读回 R 0..7 = [0x32, 0x44, 0, …]
```

两条要记的：

* **写这一侧 `e` 给"起始 + 1"**（官方库抓到的就是 `s=0 e=1`，而载荷里带了两个字节）
  —— 写几个由**载荷里的长度**说了算；
* **有的号写不进去**：读回 R0 是 `0x32` 而写的是 `0x33` —— R0 那一位被**跑着的 PMC
  梯形图**当场改回去了。这正是用户说的"有的寄存器不能写"：能写的族也不保证每一位都听
  你的（G/Y/R/K/D 这些系统/内部区一般能写，X/F 是机床与 CNC 驱动的信号，只读）。

client 侧：`ncl_focas_pmc_write(focas, family, start, values, count, width)` 与
`ncl_focas_pmc_bit_write(focas, family, bit, on)`（位写 = 读回所在字节、改那一位、写回）。
写不收就如实把返回码带上来（`NCL_FOCAS_ERR_RB_CODE` / `NCL_ERR_UNAVAILABLE`），**绝不
假装成功**。

适配器侧：`@Y @G @R @K @D` 声明了 `set_value`（`@X`/`@F` 只读 —— 它们是机床/CNC
驱动的信号），值与读同一个口径（位族 true/false、`@D` 是数）。

**还没核透的一条**：本机 `R0..R7` 被梯形图盯着，客户端发同样的帧时读回来的值不稳
（官方库那次能站住一下）—— 下一步按用户的提示，**用 VM 里的 PMC 画面（或 SERVO GUIDE
的 I/O 监视）**挑一个梯形图不碰的号（比如 `R9000` 之后的系统区外、或 `D` 的空白号）
再对一次，就能把"写"这一格彻底钉死。
