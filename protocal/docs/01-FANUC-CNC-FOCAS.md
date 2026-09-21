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

## 2. 连接建立（典型调用序列）

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

> **2026-09 更正（见 §2.8）：这两条连接不是"重试"。** 上面那段当时是**对着假机床**抓的，
> 机床一条都不回，于是把"两条连接"读成了 SDK 的重试；真机（应答齐全的那台）上，
> 官方 SDK **照样开两条**：第一条是控制通道（hello 计数器 1，只发 hello），第二条是
> 数据通道（计数器 2，命令全走这条）。分开的判据见 §2.8。

- 没有应答时 SDK 返回 **-16（EW_SOCKET）**，并且**会重试一次** —— 这只解释"为什么会
  出现两组 hello"，不解释"为什么计数器是 1 和 2"（重试的话两条都是同一段代码，
  计数器不会自己 +1）。
- 应答格式（🟢 2026-09 第九/十轮，反汇编 `libfwlib32.so` 的 `Pdu::send`/`Pdu::receive`/
  `getRbPos`/`getRb` + 假机床实测）：帧格式与四道判据见 §2.2，
  **应答体取值见 §2.3** —— 第十轮已把"体 = 块个数 + 变长块"这套结构解出来并实测
  `cnc_allclibhndl3` **rc=0**，本册**不再需要真机**。
- 现场（`cfg/driver_def.json`）用的就是这一家：`module: focas`、`8193`。

### 2.2 帧格式与校验规则（🟢 2026-09 第九轮反汇编）

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
**这一格 2026-09 第十轮已经补上**，见 §2.3（不需要真机了：按解出来的块结构回，
`cnc_allclibhndl3` 实测 **rc=0**）。

### 2.3 应答体的取值（🟢 2026-09 第十轮：反汇编 + 假机床实测 rc=0）

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

### 2.4 item 码表（🟢 2026-09 用**官方 SDK** 逐条问出来的）

§2.3 那十二个码是"反汇编 + 假机床"解的。2026-09 拿到 FANUC 官方 SDK 包（`Fwlib64.dll`
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
| `cnc_srvdelay` | 0x26 | **d = 9**，e = -1 | 每轴一条 **8 字节记录**：值 = 记录第 0 个 int32（BE32）；**后 4 字节官方库不读**，小数位走 `cnc_getfigure`（= 该轴 `POSELM` 的 `dec`） | 🟢 已进 client（跟踪误差；`POSITION@CMD` = 实际 − 这一条，见 §2.5.2） |
| `cnc_rdprgnum` | 0x1c | 0 | 载荷 **@2 运行程序号（BE16）**、@6 主程序号 | 🟢 已进 client |
| `cnc_rdseqnum` | 0x1d | 0 | 载荷 **@0 顺序号（BE32）** | 🟢 已进 client |
| `cnc_rdcount` | 0x8b | **0 / 0** | `ODBTLIFE3`：`datano` **@2**、件数 **@20**（BE32）—— 见 §2.6 | 🟢 已进 client（原来写成 1/1，是寿命那一支；值也从 @0 改到 @20） |
| `cnc_rdlife` | 0x8b | **1 / 1** | `ODBTLIFE3`：`datano` **@2**、寿命 **@12**（BE32）—— 与件数**不是一个偏移**，别串用 | 🟢 形状已核（client 侧还没挂点位） |
| `cnc_alarm2` | 0x1a | 0 | 载荷 **@0 报警状态位（BE32）**，0 = 无报警 | 🟢 已进 client |
| `cnc_rdngrp` | 0x4a | 0 | 载荷 **@0 刀具组数（BE32）** | 🟢 已进 client |
| `cnc_rdtimer` | 0x120 | d = 类型（0 通电/1 运行/2 切削/3 循环/4 自由） | 载荷 **@0 分钟、@4 毫秒（BE32）** | 🟢 已进 client |
| `cnc_rdalmmsg2` | 0x23 | d = 报警类型（-1 = 全部），e = 条数 | ODBALMMSG2 数组（编号/类型/轴/文本 64B） | 🟡 码已核 |
| `cnc_rdsvmeter` | 0x56 + 0x89 | d = 1 | LOADELM 数组（伺服负载） | 🟡 码已核 |
| `cnc_rdspmeter` | 0x40（d=4 负载 / 5 转速）+ 0x8a | e = -1 | LOADELM 数组 | 🟡 码已核 |
| `cnc_rdblkcount` | 0x35 | 0 | 载荷 **@0 的 BE32**（原来的"不是 @0"是错的，§2.6 反查出来的） | 🟢 已核（原来表里记成 0x06，是错的） |
| `cnc_rdopmode` | 0x57 | 0 | short 数组（主轴调整模式） | 🟡 码已核 |
| `cnc_exeprgname2` | 0xfc | 0 | 程序名文本 | 🟢 已进 client |
| `cnc_rdprogdir3` | 0x06 | d = 0x13，e = 1 | PRGDIR3 数组 | 🟡 帧有了、字段待核 |
| `cnc_rdtofs` | 0x08 | d = 形状（0 磨耗/1 形状…）、e = 编号 | `ODBTOFS` 的 `data` = 载荷 **@0 的 BE32**（`datano`/`type` 是请求回显） | 🟡 值的位置已核 |
| `cnc_rdparam` | 0x0e | d = 参数号、e = 轴号 | `IODBPSD`：`datano` **@2**、`type` **@4**、`ldata` **@8**（BE32） | 🟡 字段位置已核 |
| `cnc_rdmacro` | 0x15 | d = 变量号、e = 1 | `ODBM`（`mcr_val` + `dec_val`），**长度要给对**（给 12 仍回 `EW_LENGTH`=2） | 🟡 码已核、长度待试 |
| `cnc_rdtofsinfo` | **0x0a** | 0 | `ODBTLINF`：`use_no` **@2**、`ofs_type` **@4**（都是 BE16） | 🟢 码 + 切法都新核出来（§2.6） |
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

### 2.5 真机实测：NCGuide（FS0i-F 模拟器）能当"没有真机的真机"

#### 2.5.0 "正确环境"的配方 + 本机现在卡在哪（🟢 2026-09，手册 + 自写调试器）

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
   > （见 §2.7）。要核那一族就得在 NCGuide 的 GUI 里用 `OptionSetting.exe` 勾上它
   > （勾完必须重启 NCGuide）。

**实测结果**（`focas_sdk_probe32.exe --dll Fwlib32.dll --hssb 127.0.0.1 8193 <调用>`，
默认机床 `1path-3axis-M`）：

| 调用 | rc | 拿到的形状 |
|---|---|---|
| `cnc_statinfo` | 0 | ODBST 十一个 u16（块 1/2 + 块 0 载荷的那套切法 ✓ 与 §2.3 一致） |
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

#### 2.5.1 库内部：`cnc_srvdelay` 那一族怎么切（🟢 反汇编 + 假机床实测）

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

#### 2.5.2 以太网库 `fwlibe64.dll` 里的同一族（🟢 反汇编，2026-09）

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
和 §2.5 的 NCGuide 实测完全对上；**小数位不在这条载荷里**（官方库一个字节都不多看），
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

#### 2.6 用官方 SDK 反查"载荷第几字节是哪一格"（🟢 2026-09 新方法）

§2.4/§2.5 核 item 用的是"铺斜坡载荷、人眼看结构体" —— 对 `ODBST` 那种十来个 short 的
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

#### 2.7 官方库那条"控制轴数"闸门（🟢 2026-09 把范围钉清楚了）

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
（`AXIS@*/TORQUE`、`CURRENT`、`TEMPERATURE`、主轴负载、`FEED_SPEED`）就一起打开 ——
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

#### 2.8 真机（以太网 0i-MD，2026-09-21）：会话是**两条 TCP**，`code 24` 才是 ODBSYS

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
（§2.2 判据 5）对不上，可 SDK 自己收下并 rc=0。所以那条判据是**对反汇编的误读**，
client 里已经改成"至少 16 字节就收下"（`ncl_focas_hello_reply`）。同理，
"按记录数发一串 `code 24` 的块、再跟一条 `code 14`（`0x26f0`）"那套 step 2/step 3
（§2.3）在这台机器上一条都不成立：SDK 只发**一个** `code 24` 的块。

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

#### 2.8.1 每轴一条 **8 字节记录**（🟢 2026-09-21 真机实测；推翻了"12 字节 POSELM"）

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

#### 2.8.2 报警：`arg2=2` / `arg3=64` 才填文本（🟢 实测，含一条真报警）

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

#### 2.8.3 "同一个 item、每次问不同的号"那一族（刀补 / 宏变量 / 参数）🟢 实测

这三条都是**一个块、`d` = 号**，应答就是 §2.8.1 那条 8 字节记录（值@0 + 小数位@6）：

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

#### 2.8.4 程序目录（`cnc_rdprogdir3`）🟢 实测

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

#### 2.8.5 模态（`cnc_rdgcode`）与执行中的程序段（`cnc_rdexecprog`）🟢 实测

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

#### 2.8.6 这台机器**做不到**的那些（🟢 逐条实测，官方 SDK 同样被拒）

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
| 伺服那一类（`cls=2` 的负载/电流/温度） | `cnc_rdaxisdata 2` | rc=0 但**载荷是桩**（`08 00 09 00` 这种，不是数据） |
| 程序上行 | `cnc_upstart4` rc=0 → `cnc_upload4` | **rc=10**（数据那一步不给） |

能读的都在上面几节里；`cnc_rdaxisdata` 一族里 **cls=1（位置）** 与 **cls=3（主轴）、
cls=5（速度）** 是好的（`cls=3/5` 见 §2.8.1 那条 8 字节记录）。

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

## 3. 常用函数表（按域）

### 3.1 连接与系统（cnc_*）

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

### 3.2 坐标与轴（★ 采集核心）

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
| `cnc_rdaxisdata(h, …)` | 轴数据（速度/负载/温度，可批量，推荐） |
| `cnc_rdaxisname` | 轴名称 |

### 3.3 主轴

| 函数 | 说明 |
|---|---|
| `cnc_rdspdata` | 主轴数据 |
| `cnc_rdspmeter` | 主轴负载/速度 |
| `cnc_rdspindle` / `cnc_rdspload` | 主轴转速/负载 |
| `cnc_rdspspeed` | 主轴速度 |

### 3.4 报警

| 函数 | 说明 |
|---|---|
| `cnc_alarm(h, &ODBALM)` | **当前报警**（数量+内容） |
| `cnc_rdalmmsg2(h, type, …)` | 报警消息（可分类/分页，推荐） |
| `cnc_rdalmmsg` | 报警消息（旧版） |
| `cnc_rdopmsg` / `cnc_rdopmsg2/3` | 操作员消息 |
| `cnc_rdalmhis` 类 | 报警历史（部分系统） |

### 3.5 程序

| 函数 | 说明 |
|---|---|
| `cnc_rdprgnum(h, &ODBPRO)` | 当前主/子程序号 |
| `cnc_rdseqnum(h, &ODBSEQ)` | 当前程序行号（序列号） |
| `cnc_rdprogdir` / `cnc_rdprogdir2/3` | 程序目录 |
| `cnc_upstart4` / `cnc_upend4` / `cnc_upload` | 程序上传（分块） |
| `cnc_dwnstart4` / `cnc_download` / `cnc_dwnend4` | 程序下载 |
| `cnc_delprogram` / `cnc_rdproginfo` | 删除 / 信息 |
| `cnc_rdpdf_*`（pbm_*） | 程序块管理（按块读写，大程序更高效） |

### 3.6 刀具补偿

| 函数 | 说明 |
|---|---|
| `cnc_rdtofs(h, type, &ODBTOFS)` | 刀补读（形状/磨损，长度/半径） |
| `cnc_wrtofs` | 刀补写 |
| `cnc_rdtool` / `cnc_rdtoolgrp` | 刀具信息/刀具组 |
| `cnc_rdngrp` | 刀具组数量 |

### 3.7 参数与宏变量

| 函数 | 说明 |
|---|---|
| `cnc_rdparam(h, num, len, &ODBPARA)` | **CNC 参数读** |
| `cnc_wrparam` | 参数写 |
| `cnc_rdmacro(h, num, len, &ODBM)` | **宏变量读** |
| `cnc_wrmacro` | 宏变量写 |
| `cnc_rdparainfo` / `cnc_rdparaminfo` | 参数信息 |

### 3.8 PMC（PLC 层，95 函数）

| 函数 | 说明 |
|---|---|
| `pmc_rdpmcrng(h, adr_type, data_type, start, end, len, buf)` | **PMC 区读**（R/Y/X/G/F/E/A/C/D 等） |
| `pmc_wrpmcrng` | PMC 区写 |
| `pmc_rdpmcinfo` | PMC 区信息 |
| `pmc_rdladder` | 梯形图读取 |
| `pmc_getdtailerr` | 详细错误 |
| `pmc_setpcmcntl` / `pmc_startladdermonitor` | 梯形图监控 |

### 3.9 其它

| 域 | 前缀 | 说明 |
|---|---|---|
| 时间 | `cnc_rdtimer` / `cnc_rdcount` | 运行时间/工件计数 |
| 伺服/主轴参数 | `cnc_rdservo` / `cnc_rdspindle` | 伺服/主轴参数 |
| 波形 | `cnc_startwave` / `cnc_rdwave` / `cnc_stopwave` | 波形采集（诊断） |
| 数据服务 | `ds_*`（8 函数） | FOCAS2 Data Service |
| 变量/PMC 日志 | `flnt_*`（14 函数） | Focas2 Logger |

---

## 4. 数据类型（结构体，选摘）

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

## 5. 错误码（错误分类函数）

```c
short err = cnc_getdtailerr(h, &ODBERR);   // ODBERR 结构 getdtailerrkind/dta1/dta2
// 通用返回：EW_OK=0；非 0 时用 cnc_getdtailerr 取细节
```
常见：`EW_FUNC`(功能不支持) · `EW_LENGTH`(数据长度错) · `EW_PARAM`(参数错) · `EW_HANDLE`(句柄无效) · `EW_SOCKET`(网络错) · `EW_BUSY`

---

## 6. 实现坑

1. **句柄数限制**：FOCAS 默认最多 2 个并发句柄 —— 多进程采集会互相踢掉。**必须单进程复用句柄**。
2. **坐标缩放**（见 §4）—— 最常见的数值 bug 来源。
3. **`cnc_rdaxisdata` 优于逐轴 `cnc_absolute`**：批量读一次拿多轴，吞吐高 3-10 倍。
4. **报警消息分页**：报警多时 `cnc_alarm` 只给 10 条，遍历要用 `cnc_rdalmmsg2`。
5. **程序上传/下载必须成对调用**（`*start` → 分块 → `*end`），中途退出会锁住机床侧。
6. **32/64 位库不同**（`Fwlib32.dll` vs `fwlib32.x64.dll`），结构体对齐要用 `#pragma pack`。
7. **写类函数（`cnc_wrtofs` / `cnc_wrmacro` / `pmc_wrpmcrng`）风险高**：改刀补会导致撞刀。

---

## 7. 参考实现

| 来源 | 说明 |
|---|---|
| `pyfocas` / `pyfanuc` | ctypes 绑定 + fwlib 头文件（可直接抄结构体定义） |
| **881 个函数原名**（按前缀分类） | 原始素材未随本目录提供 |
| 商业库对照 `FanucSeries0i` | 高层 API 分组参考（59 成员，坐标/报警/负载/刀补/宏变量/程序列表） |
| 参考实现侧 | `驱动定义目录/` FANUC 驱动（端口 8193） |
