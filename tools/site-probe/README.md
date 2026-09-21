# site-probe · 把现场包的 ARM 代码跑起来看协议

现场交付包（iNC-BOX-200）里全是 **ELF32 ARM hard-float**：Go 网关
`hp2x_box200`、`nclink-service`、以及 20 多个厂商协议插件 `lib*.so`。
Docker Desktop 自带 `binfmt`，加 `--platform linux/arm/v7` 就能让它们在本机
跑起来；再把 `ipAddress` 指向我们自己的"假机床"，就能把**设备侧报文**抓下来
——不必等现场机床，也不必等厂商文档。这套东西就是干这个的。

```sh
cd tools/site-probe
docker build --platform linux/arm/v7 -t ncl-arm-probe .

# 只看 FANUC FOCAS 握手（不需要任何 FOCAS 头文件，走 dlsym）
docker run --rm --platform linux/arm/v7 \
  -v <现场包>/app1/nclink-service:/svc:ro -v $PWD:/work \
  ncl-arm-probe sh /work/focas_run.sh

# 直接驱动某个插件，让它去连我们监听的 6000
docker run --rm --platform linux/arm/v7 \
  -v <现场包>/app1/nclink-service:/svc:ro -v $PWD:/work \
  ncl-arm-probe sh /work/plugin_drive.sh libgsk-http.so 6000 /GSK/CNC/Open/TCP

# 或者（推荐）直接跑 Go 网关，让它去连假机床——协议实现其实在它里面
docker run --rm --platform linux/arm/v7 \
  -v <现场包>/app1/hp2x:/hp2x:ro -v $PWD:/work \
  ncl-arm-probe sh /work/gateway_probe.sh
```

`gateway_probe.sh` 的第一个参数是假机床的应答（十六进制，可省）：起假机床 →
起 `hp2x_box200` → `POST /<模块>/Open/TCP` 拿 `connectionId` → 逐项 POST →
把设备侧请求打出来。**这条路已经通了**，GSK 的 11 条请求帧就是这么抓到的
（`protocal/docs/08-GSK-广州数控.md` §4.5）。换模块名/项键就能抓科德、三菱、
S7、Modbus、FOCAS、840D 的对应帧。

## 读网关自己的代码（Go 二进制，strip 过也不怕）

现场网关是 Go 编的，`hp2x_box200` 里 `.gopclntab` 完好，所以**函数名 + 起止地址**
都能还原；配上 objdump 就能读它的判定逻辑，比抓包猜快得多。

```sh
python go_pclntab.py <bin> --list 's7v2\.\(\*S7\)'      # 列函数
python go_pclntab.py <bin> --dump '(*S7).Mode' --out fn.bin   # 导出机器码
python elf_vaddr.py <bin> --words 0xdec9a8 4            # 看字面量池/全局变量
python elf_vaddr.py <bin> --gostr 0x64852c              # 池中 (ptr,len) → 字符串
sh arm_range.sh <bin> 0x648074 0x648400                # pclntab 认不出时直接按地址反汇编

docker run --rm --platform linux/arm/v7 -v <现场包>/app1/hp2x:/hp2x:ro \
  -v $PWD:/work ncl-arm-probe sh /work/arm_analyze.sh '(*S7).Mode'
```

> ⚠️ **本机有好几份现场包，别摸错**：`protocal/docs` 里的地址（如 `(*S7).Mode`
> `0x648074`）对应的是 **`D:\03-开发代码\incbox200\app1\hp2x\hp2x_box200`（14.5 MB）**；
> 另一份 17.9 MB 的 `hp2x_box200` 在这些地址上没有代码，`go_pclntab.py` 也解不了它的
> pclntab。`hp2x_pick.sh <候选...>` 一比对就知道是哪份。

`arm_analyze.sh` 会把 `runtime.memequal` 的比较、字面量池以及被页面的地址都列出来
（`Mode` 的 `JOG`/`REPOS`/`REFPOINT`/`AUTO` 表就是这么读出来的）。
`gateway_meta.py` 则从二进制里抽 `Meta=path:"…" tags:"…"` 这类路由元数据；
`websearch.py` 是查本机 SearXNG（找标准/手册出处）用的小脚本。

**要某家的请求参数（字段名 / 默认值 / 端口）别猜**：网关自己带 OpenAPI，
`gateway_schema.sh '/FANUC/ROBOT'` 把 `/api.json` 里该前缀的路径连同
`components.schemas` 里的请求结构一起打出来——RMI 的
`{"selector":12,"index":1,"count":10}`、默认端口 60008、
`setasgs:["SETASG 1 1000 ALM[1] 1"]` 都是这么抄到的，比反汇编快得多。

**反查"这句报错是谁报的"**：

```sh
python go_pclntab.py <bin> --xstring "error response length"
```

字符串常量 → `.text` 里引用它地址的字面量池格子 → 用 functab 翻回函数名。
三菱的 `error response length` / `error response data` 就是这么定到 `hp2x/common` 的。

`mock.py` 的 `EREP:` 还支持 `cut:N`（回声后截断到 N 字节），用来扫"应答该多长"——
三菱 M70 的"正确应答 = 40 字节"就是这么试出来的（`m70_reply_probe.sh`）。

`arm_bytes.py` 更进一步：把函数反汇编里那串 `mov rX,#imm` + `strb rX,[sp,#off]`
当成**栈上拼出来的字面量**读回来——Go 编译器就是这么落地 `[...]byte{…}` 的。
三菱 M70 的"应答模板"（`47 49 4f 50 01 00 01 01 …`）就是这么拿到的：

```sh
sh /work/arm_literals.sh '(*MitsubishiCncM70).GetPartCount'   # 只打"字面量"那一段
SECTION=逐字节 sh /work/arm_literals.sh '<函数名>'            # 每个 strb 的偏移与值
```

配合 `mock.py` 的 `ZREP:N:off:hex,…`（零填充 N 字节、拷进请求前 24 字节、再补丁）
和"`SEQ:` 里可以嵌套别的规格"，就能拼出**比请求长的应答**（M70 报警的 544 字节帧
就是这么发的）。

**一个数据项连发多帧**时（如新代 `FEED_SPEED` 会分别问寄存器 700 / 状态 12 / 状态 76，
而且**每帧都是新连接**，`SEQ:` 那种"按连接内序号挑"就不管用了）用 `MAP:`：

```
MAP:28:2:bc02=<应答A>|0c00=<应答B>|4c00=<应答C>|<兜底>
```

即"看请求的 `[28..29]` 等于哪个 hex，就回哪条应答"，天然适配按寄存器/子码分发的驱动。

**"一直读到机床说停"的循环**（M70 列目录 `mochaFSReadDirectory`、读文件
`mochaFSReadFile`）既不能按连接内序号（网关自己的后台轮询会插队把序号算歪），
也不能按固定项名（同一项要连发多帧）——用 `LOOPQ:`：

```
LOOPQ:6d6f6368614653526561644469726563746f7279/<填充帧>/<第1帧>|<第2帧>|…
```

即"**请求里含这段 hex 的**才按顺序取，其余请求一律回填充帧"，第 n 帧用完就重复
最后一帧（所以最后那帧要给"读完"）。M70 的用法见 `m70_fs_probe5.sh`/`probe6.sh`。

## 用 FANUC 官方 SDK 自己问（Windows，2026-09）

上面的 `focas_*_probe.sh` 是在 ARM 容器里对着现场包里的 `libfocas.so` 跑（Linux 版）。
拿到**官方 SDK**（`Fwlib64.dll` + `Fwlib64.lib` + 每函数一页的 XML 文档）以后，多了一条
更快、证据更硬的路：让官方库在 Windows 上对着一台假机床跑，逐条把 `Cb` 码与应答布局
问出来 ——

```
python tools/site-probe/focas_sdk_mock.py 8193 --size 0x40 --ramp
tools/site-probe/focas_sdk_probe.ps1 -Dll <Fwlib64.dll 所在目录> `
    -Calls "cnc_rdprgnum","cnc_absolute -1 --len 36","cnc_rdalmmsg2 -1 --count 10"
```

- `focas_sdk_mock.py`：假机床，`--ramp` 铺"斜坡载荷"（第 i 块第 j 字节 = `i*16+j`）、
  `--payload HEX` 铺指定字节、`--blocks N` 强推块数；每个请求都打 hexdump + `Cb` 表。
  程序上下行另配三个：`--body HEX`（数据帧回**裸体**）、`--silent 0x12`（下行数据帧
  机床**不应答** —— 回了会把驱动带歪）、`--reply-func HEX`（应答的 `[6]` 换成别处）。
- `focas_sdk_probe.c`：`LoadLibrary` + `GetProcAddress` 驱动指定调用（**不 include、
  不抄官方头**：出参给一块 4 KiB 零缓冲，跑完按 u16/i32 打出来），`--len/--count` 给
  "数据块长度/条数"（这两个给 0 会被本地拒掉，不发帧）。
- `focas_sdk_probe.ps1`：编译 + 起假机床 + 一次跑一串调用 + 打印每条的 Cb 码。
- `focas_item_scan.py`：静态那一路（PE 导出表 + 反汇编找小立即数），当交叉印证用 ——
  官方库的导出函数多是薄壳，真代码在内部调度里，所以**以线上探针为准**。
- `focas_dis_range.py`：把官方库里**某一段**反汇编出来（`rva` + 条数），call 目标自动标
  成导出名。问"应答怎么切"时用它——长度校验的常数、拷贝循环的步长都在这段里
  （`cnc_srvdelay` 那族每轴 8 字节的结论就是这么来的，见 01 册 §2.5.1）。
- `focas_tap.py`：TCP 抄包器，插在客户端与机床之间同时看两个方向的字节
  （`python focas_tap.py 8194 127.0.0.1 8193`，探针打 8194 就行）。官方库在**以太网**
  这条路上不发帧日志（`FWLIBETH.LOG` 只记错误文本），要看帧就用它。
- `focas_sdk_layout.py`：**自动反查**"出参的哪一格是从载荷第几字节来的"（本节下面
  专门有一段）。核"值在 @12 还是 @20"这类问题用它，不要靠看斜坡载荷的眼力。
- `fwlib_struct.py` / `fwlib_proto.py`：从官方 SDK 包里抠结构体（`Fwlib64.h`）与
  函数原型（`Document/SpecE/**/*.xml` 的 `<prottype>`）—— 核 item 之前先看这两个。

结果表（核出来的 item 码、哪些已进 client、哪些还差应答布局）写在
`protocal/docs/01-FANUC-CNC-FOCAS.md` §2.4 —— 包括**程序上下行的另一套帧**
（`0x11/0x12/0x13` 下行、`0x15/0x18` 上行：定长 516 字节 start 体、数据帧 dir=4 不应答、
错误在 end 那条回）。下行整条已经照这套验通；上行差"应答里程序文本怎么切"。

### 对着 NCGuide 跑（"没有真机的真机"，2026-09 实测通）

FANUC 自己的模拟器 **NCGuide** 带 FOCAS2 服务，所以"待真机核准"的那些应答可以直接在
本地抓全（§2.5 那张表就是这么做出来的）。配方与踩坑：

```
# 1) 用 NCGuide 自带的那对 32 位库（Fwlib32.dll + fwlibNCG.dll），放到探针目录
#    C:\Program Files (x86)\FANUC\NCGuide FS0i-F\{Fwlib32.dll,fwlibNCG.dll}
# 2) 32 位编译（vcvars32），函数指针要 WINAPI（__stdcall），否则栈坏 → 0xC0000409
cl /nologo /W4 /utf-8 /O2 focas_sdk_probe.c /Fe:focas_sdk_probe32.exe
# 3) 走 HSSB（节点号 9；NCGuide 手册 §4.4），不要走以太网
focas_sdk_probe32.exe --dll Fwlib32.dll --hssb 127.0.0.1 8193 cnc_statinfo
```

- **别走以太网那条**：Simbase 确实在听 8193（`Simbase.exe` 进程），但用官方 SDK 的库
  （含 `Fwlib64.dll` + `fwlibe64.dll` 那对）一律回 **-17 EW_PROTOCOL** —— NCGuide 的
  以太网服务要自己那套握手。HSSB 这条路 `cnc_setdefnode(9)` + `cnc_allclibhndl()` 实测
  `rc=0`、handle=18433。
- 不用去开手册 §3.1 说的那个选项（"Extended driver and library function"）：HSSB 这条路
  不开也能用（省得动 NCGuide 的设置）。
- 想看它到底发了什么：SDK 自己的日志在 `C:\ProgramData\FANUC\Fwlib\FWLIBETH.LOG`
  （一行一条：建 socket / 建 circuit / 收应答失败的原因）。

### 真 FOCAS2 假机床（`focas_machine.py`，2026-09）—— 拿它把 client 端到端跑起来

`focas_sdk_mock.py` 是"载荷随便铺、看 SDK 怎么解"的**取证**工具；`focas_machine.py`
是**按核出来的口径发正确字节**的假机床，我们自己的 client（`ncl_server` + focas 插件）
能直接连它：

```powershell
# 1) 起假机床（值随便给；跟踪误差可以给负数）
python tools/site-probe/focas_machine.py 8193 --pos 12.345,67.89,-3.5,0,0 `
    --feed 500.5 --spindle 3000 --count 952 --prog O1234 --srv-delay 1.234 `
    --status running --mode auto
# 2) 用我们自己的 client 读一遍（--offline = 不接 broker；conf 里 host 指向 127.0.0.1:8193）
build\Release\ncl_server.exe -c <conf> -P build\plugins\Release --offline --once
```

实测：`STATUS`、`WORK_MODE`（auto/manual 都对）、`PART_COUNT`、`PROGRAM` /
`PROGRAM_NUMBER`、`LINE_NUMBER`、`POSITION@REAL`、`SPEED` 全部按给的值出来；
**`POSITION@CMD` = 实际 − 跟踪误差**（`--srv-delay 1.234` → 11.111，`-2.5` → 12.5）。
还没接的那 17 条照旧报"还读不了"。

**每个字节的来源**（也是它的边界，文件头写了同样一段）：帧与块结构 = 参考实现反汇编；
每条 item 的 Cb 码 = 官方 SDK 实测；字段位置 = 官方 SDK 填它自己的结构体（`STATINFO`
的 ODBST 就这么钉的：块 1 → dummy、块 2 → aut、块 0 载荷 → manual/run/edit/…）；
数值形状 = NCGuide 实测（`POSELM` 12 字节、`ODBAXIS` 一族…）。**没有证据的 item 回错块，
不编字节。**

调试开关：`-v` 打每帧 hexdump + Cb 表；`--srv-shape rec8|bare4|hdr4` 换 `ODBAXIS` 那一族
的候选形状；`--hello-records` / `--rec-a` / `--rec18` / `--hello-hex` / `--cap-hex`
是"试驱动到底在哪一格读轴数"用的。

**接仿真器（`--protoforge`）**：给个 REST 基址就每 `--pf-interval` 毫秒拉一次点值，
映射到这台假机床（`x_abs/y_abs/z_abs` → 绝对位置、`feed_rate` → 进给、
`spindle_speed` → 主轴、`run_status` → 三态、`tool_number` → 刀具组数；件数/程序号/
跟踪误差这些没有对应点的保持命令行给的值）：

```powershell
python tools/site-probe/focas_machine.py 8196 --protoforge http://127.0.0.1:8000 `
    --pf-token-file $env:TEMP\pf-token.txt --srv-delay 0.25 --pf-interval 300
```

实测（2026-09，用同形状的桩验的链路）：桩里 `x_abs=77.25 / y_abs=-11.5 / z_abs=3.0 /
feed_rate=123.5 / run_status=2` → client 读到 `POSITION@REAL` = 77.25 / −11.5 / 3.0、
`STATUS="holding"`、`POSITION@CMD` = 77.0 / −11.75 / 2.75（各减 0.25）。要对着
ProtoForge 跑真值，把它起回来（`python app.py`，8000 + 8193）再照上面那条命令指过去即可。

`ODBAXIS` 那一族的形状已经**定死**（不再靠 SDK 试）：直接反汇编 x64 以太网库
`fwlibe64.dll` 的 `cnc_srvdelay` → `sub_180059180` 那段 —— `shr ax, 3`（轴数 = 载荷长度
/ 8）、`lea rcx, [rax*4+4]`（长度规则 4+4×轴数）、`mov ecx, [载荷 + i*8 + 0x10]`
（每轴 8 字节、值在第 0 个 dword）、`bswap32`（大端），而且**小数位不在这条载荷里**
（位数走 `cnc_getfigure`，也就是该轴 `POSELM` 的 `dec`）。见 01 册 §2.5.2。
用法：`python focas_dis_range.py <fwlibe64.dll> 0x59548`（`cnc_srvdelay` 的 RVA）、
`python focas_dis_range.py <fwlibe64.dll> 0x59180`（共享函数）；ARM 的
`libfwlib32.so.1` 用 `elf_dis.py <so> cnc_srvdelay`。

> 还留着的一条（只跟"拿官方 SDK 当裁判"有关）：SDK 对那几条**单轴**调用会**在本地**回
> `EW_ATTRIB` —— 它从连接期缓存取"控制轴数"，而假机床握手没把这一项喂对（`func 01` 的
> 16 字节头、记录 A/B/C/D、每条的 `0x18` 详情、能力块 `0x0e` 的载荷都试过）。我们 client
> 不吃这一套，所以不影响验证；桥里那些开关还留着，谁想接着试都行。
>
> 2026-09 又往下挖了一层（`fwlibe64.dll` 反汇编 + 桥里那些开关），把**机制**摸清了，只差
> 最后一格：
>
> - 单轴闸门读的是**每条路径一张轴表**：`[base + 0x66c + idx*32]`，`idx = [base+0x96e]`
>   （0x18005923a 起）。表项 32 字节，第一个字 = 这张表认的"轴数"。
> - 连接期只发三条数据请求：**能力块**（`0x21` 无 Cb）、`0x0e`（d = e = `0x26f0`）、
>   **`0x89`（轴表）**。
> - `0x0e` 的解析在 0x18008cbdc 那一段：把应答载荷**偏移 8 的 4 字节** bswap32 后取低字节
>   存进 `handle+0x37c`（试过 1/2/3/4，不是轴数）。
> - `cnc_rdaxisname` 的邻居函数（发 Cb **`0xa4`**）把应答载荷的**第一个字**当轴数
>   （0x1800316de 发、0x180031783 读）。
> - **`cnc_sysinfo` 的输出来自 `0x18`（握手记录详情）的载荷** —— 那一条就是 `ODBSYS`
>   （拿 `--rec18` 塞标记字节实测：标记原样出现在 sysinfo 输出里）。
>
> 还没钉死的是：**谁把那张 32 字节表填上**。全库反查 `[.. + 0x66c]` 只有读、没有写，
> 所以它是被**整块拷进去**的（下一步从"路径对象"的构造处找这个拷贝）。桥里现在的处理：
> `0x89` 按"每轴 16 字节、前 4 字节轴名"铺（`cnc_rdaxisname` 的取数循环就是这么读的），
> `0xa4` 报"第一个字 = 轴数"，`--rec18` 默认铺 NCGuide 实测的那串 `ODBSYS`。
>
> 2026-09 再往下挖了一层的**字段映射**（连接解析在 0x18008c7f0 起那一段，`rdi` = 某个
> 上下文、`rsi` = 握手载荷）：
>
> ```
> [rdi+0x68] ← bswap16(载荷[0xa..0xc])
> [rdi+0x6a] ← bswap16(载荷[0xc..0xe])      ← 32 字节表项 idx=0 的"轴数"那格
> [rdi+0x6c] ← 0
> [rdi+0x6e] ← bswap16(载荷[0xe..0x10])
> [rdi+0x70] ← bswap16(载荷[0x10..0x12])
> [rdi+0x72] ← bswap16(载荷[0x12..0x14])    ← 记录数（`test ebx,ebx; jle` 就跳过记录循环）
> [rdi+0x7e] ← bswap16(载荷[0x14..0x16])
> [rdi+0x84] ← bswap16(载荷[0xe..0x10])     [rdi+0x86] ← 载荷[0x16..0x18]
> [rdi+0x88] ← 载荷[0x18..0x1a]
> 记录循环：表基址 = rdi+0x90，每条 0x20 字节，表项[0] = 1 + (记录 A == 0x2054 " T")
> ```
>
> 读者那边读的是 `ctx+0x66c + idx*32`（`idx = [ctx+0x96e]`）。两个结构差 **0x602**，所以
> 最可能是同一个对象的两个视角（`ctx = rdi + 0x602`）—— 也就是说 idx=0 的"轴数"就是
> `[rdi+0x6a]`。按这个把 `载荷[0xc..0xe]` 填成 3 试过（hello 头、能力块两条路都试），
> **仍是 rc=4**；下一格要查的是 `ctx+0x96e`（"当前路径"序号，决定读第几个 32 字节表项）
> 是谁写的 —— 很可能就是它偏了 1（单路径机床该是 0），于是读到的表项是
> `[rdi+0x8a] = 0`（记录循环把那格清成 0 了）。
>
> 2026-09 又查了三处（都没定案，但把杠杆找出来了）：
>
> - **全库反查 `[.. + 0x96e]` 也只有读、没有写** —— 和 `0x66c` 一样是整块拷贝进来的，
>   所以"当前路径序号"不是连接期逐字段写的。
> - **`fwlibe64.dll` 里有 `fwlib\semem.cfg`**（0x0f0dd0，被 0x18004a77d 引用）：它按
>   `<Fwlib 目录>\semem.cfg` 打开一个 **INI 风格**的"系列 → 能力"表（最多 0x20 条）；
>   **文件不在时用内置默认表**（7 条：`0x500000 / 0x510000 / 0x520000 / 0x530000 /
>   0x800000 / 0x832000 …` 各带一个 `0x10000`）。这是"给假机床补能力"的一个文件级杠杆
>   （还没试）。
> - 库里还引用注册表键 **`Software\FANUC\FwlibEth`**（0x0eb570）—— 本机**这个键不存在**
>   （`HKCU`/`HKLM` 都没有，只有 `HKCU\Software\FANUC\screen`），所以走的是默认。

### 反查"载荷第几字节是哪一格"（2026-09 新方法，先看这一节）

单轴那条闸门还没开（上面那段），但**"某个 item 的应答里第几字节是哪个字段"已经不用
等真机了** —— 这轮把它做成了自动反查，顺带抓出 client 里一处读错位置的真 bug。

```
python tools/site-probe/focas_sdk_layout.py cnc_rdcount 0            # 一条调用
python tools/site-probe/focas_sdk_layout.py --calls "cnc_rdlife:1,cnc_rdtofs:0+0+8"
python tools/site-probe/focas_sdk_layout.py --payload 01020304... cnc_rdtofsinfo
python tools/site-probe/focas_sdk_layout.py --len 12 cnc_rdmacro 0   # 长度查得严的要给
```

做法：给假机床铺一份**每个字都不一样**的载荷（第 i 个字 = `0x1000 + i*0x101`），跑一次
官方 SDK 的调用，把 SDK 填进出参的字节抠出来，再对出参每一格在载荷里**反查**它从哪儿来
（大端/小端 × 16/32 位各试一遍，唯一命中才报）。配套两支：

```
python tools/site-probe/fwlib_struct.py <Fwlib64.h> ODBTLIFE3 ODBALMMSG2  # 官方头里的结构体
python tools/site-probe/fwlib_proto.py  <SpecE 目录> cnc_rdtofsinfo       # 官方文档里的原型
```

- 结构体/原型从官方 SDK 包拿（`Fwlib64/30i/Fwlib64.h`、`Document/SpecE/**/*.xml`，每份
  XML 第一段就有 `<prottype>`）。**原型很重要**：`focas_sdk_probe.c` 里那些通用形状
  （`s1/s2/s3/s1_n/s2_n/s2_n_n/…`）就是照它配的，配错的话出参落在哪一格全是噪声 ——
  这轮顺手修了 `s1_n_s1_n`（`cnc_rdaxisdata` 的第二个 short 是**指针**，原来把整数当
  指针传了）、给 `s2_n` 的 `*num` 一个非零初值（给 0 会被回 `EW_LENGTH`），并补了
  `cnc_rddynamic2` / `cnc_loadtorq` / `cnc_rdgcode` 三个表项。
- 交叉印证：同一条 item 再对着**现场包里那份 Linux `libfwlib32.so`** 反汇编一遍
  （`python tools/site-probe/elf_dis.py <so> cnc_rdcount`；这轮给 `elf_dis.py` 补了
  x86 / x64，之前只认 ARM）。x86 那版在官方 SDK 包的 `Fwlib/Linux/x86/` 下。

反查出来的（都已写进 01 册 §2.4/§2.6）：

| item | Cb | 载荷 |
|---|---|---|
| `cnc_rdcount` | 0x8b d=e=0 | `datano`@2、件数@**20** ← **client 原来按 @0 读，是错的那一格** |
| `cnc_rdlife` | 0x8b d=e=1 | `datano`@2、寿命@**12**（和件数不是一个偏移） |
| `cnc_rdtofsinfo` | **0x0a** | `use_no`@2、`ofs_type`@4 |
| `cnc_rdmacroinfo` | **0x17** | 头两个 short 在 @2 / @6 |
| `cnc_rdexecprog` | **0x20** arg0=0x594 | 文本从 @4 起，**原样字节**（不是大端字） |
| `cnc_rdgcode` / `cnc_rdwkcdshft` / `cnc_loadtorq` | **0x96 / 0x63 / 0xfd** | 码新核出来 |
| `cnc_rdblkcount` | 0x35 | 就是**载荷 @0 的 BE32**（前一版写的"不是 @0"反了） |
| `cnc_rdparam` / `cnc_rdtofs` | 0x0e / 0x08 | `datano`@2、`type`@4、`ldata`@8 / `data`@0 |
| `cnc_rdprgnum`@2+@6、`cnc_rdseqnum`@0、`cnc_rdalarm2`@0、`cnc_rdngrp`@0、`cnc_rdtimer`@0+@4 | — | 复核：client 原来的读法**都对** |

假机床（`focas_machine.py`）跟着改了两处：`0x8b` 那块按 `ODBTLIFE3` 的真实位置铺
（`--count` 落到 @20、新增 `--life` 落到 @12）。

还差（接着磨就行，都不用真机）：`cnc_rdalmmsg2` 的块长/条数、`cnc_rdprogdir3` 与
`cnc_rdmacro` 的 `--len`（给 12 仍回 `EW_LENGTH`）、`cnc_rdsvmeter`/`cnc_rdspmeter`/
`cnc_rdposition` 那几条**一条请求带多个块**的逐块形状。

## 已经拿到什么

1. **FOCAS2 握手字节**（🟢 实测，`focas_run.sh`）。`cnc_allclibhndl3()` 对假机床
   依次发（两次 TCP 连接、四条消息）：

   ```
   a0 a0 a0 a0 00 01 01 01 00 02 00 01     # 12 字节，第一次连接
   a0 a0 a0 a0 00 01 01 01 00 02 00 02     # 12 字节，第二次连接（计数器 +1）
   a0 a0 a0 a0 00 01 21 01 00 00           # 10 字节
   a0 a0 a0 a0 00 01 02 01 00 00           # 10 字节
   ```

  没回对时 `cnc_allclibhndl3` 返回 **-16（EW_SOCKET）**，且会重试一次。
  **帧格式与校验判据已经反汇编出来了**（`focas_handshake_probe.sh` 能直接复跑）：

  ```
  [0..4)  a0 a0 a0 a0          魔数（Pdu::receive #23fbc 字节交换后与 0x24ac0 比）
  [4..6)  be16 类型            请求恒 0001；应答决定体长上限 1450/2910/3470
  [6]     功能码               Pdu::send 写调用方的 func；receive 要求 == 期望值（0=不查）
  [7]     方向                 请求恒 01；应答必须 1..4
  [8..10) be16 体长（字节）
  [10..)  体                   按 32 字节一块切，块内 [8..10) 非 0 就抛异常
  ```

  `Pdu::receive`（`0x23a94`）任一条不过就抛 `ErrObj(-17)`（= EW_PROTOCOL，
  所以 **-16 vs -17 正好能区分"没收到"和"收到了但字段不对"**）；体长的两条
  额外公式：`[6]==1 && [7]==2` 时体长必须 `n*8+16`（`n=be16(体[8..10))`），
  `[6]==0x21` 时体长必须 `> 1` 且 `be16(体[0..2)) != 0`。
  要往下拿数据调用（`cnc_statinfo` / `cnc_rdparam` …）得先把应答体的字段对上——
  这一步 **2026-09 第十轮已经做完，不用真机了**：

  **应答体 = 块个数 + 一串变长块**（`Pdu::getRbPos` `0x26ab0` / `Pdu::getRb` `0x26b78`）：

  ```
  体 [0..2)  = 块个数 N（BE16）；i 必须 < N，否则 getRbPos 抛 ErrObj
  体 [2..)   = N 个块首尾相接，每块：
                 [0..2)  本块字节数（BE16，getRbPos 靠它累加走到第 i 块）
                 [2..4)  ecode       [8..10) 返回码（非 0 → getRb 抛异常）
                 [10..12) / [12..14) detail1 / detail2
                 [14..16) 载荷字节数 [16..)  载荷
  ```

  **三条硬规则**：① **块个数 = 请求里 Cb 的个数**（请求体 `[0..2)` 就是它；
  无体的 10 字节请求按 1 个块回）；② 块长 ≥ 34（`system_info_v1` 要读块 `[16..34)`）；
  ③ 每块 `[8..10)` 必须是 0。

  `func 01` 的应答是另一套：16 字节头 + n 个 8 字节记录，体长必须是 `16 + 8n`
  （`n = be16(体[8..10))`）。`func == 2` 的应答 `[6]` 必须是 2（`SockPair::request`
  会轮询到 `[6]==2` 为止）。

  实测（`focas_handshake_probe2.sh` + `focas_data_probe.sh`）：`cnc_allclibhndl3`
  **rc=0**，`cnc_statinfo` / `cnc_rdcount` / `cnc_actf` / `cnc_acts` / `cnc_rdparam` /
  `cnc_rdtofs` / `cnc_exeprgname2` / `cnc_rdlife` 也都 **rc=0**。

  `mock.py` 为此加了 **`CBREP:<块长 hex>[:<载荷 hex>]`**：按请求的 Cb 个数自动生成
  同样多个应答块（块长 / 载荷可调），所以新数据项不用手搓应答：

  ```
  MAP:6:1:01=<16 字节体的应答>|21=CBREP:22|02=CBREP:22|<兜底>
  ```

  反汇编用 `focas_dis.sh`（`handshake|calls|full|sym|who|at|range|hs|lines`，
  `OUT=` 可写文件）——`libfwlib32.so.1` 是纯静态符号，直接 `objdump` 就行。

2. **插件的 C 入口点与签名**（🟢 `.dynsym` + 反汇编）。导出的四个 C 名字是
   `create` / `call` / `destroy` / `get_version`，真正转发到的是：

   ```c
   void *create(nlohmann::json *params, spdlog::logger *logger);
   int   call(void *instance, int op /*OperationType*/,
              const char *method,          /* 转发时转成 std::string */
              nlohmann::json *request, nlohmann::json *response,
              nlohmann::json *extra);
   void  destroy(void *instance);
   const char *get_version(void);
   ```

   `call` 的转发目标在 `libbase.so` 里，签名（🟢 修饰名）是
   `BaseMod::call(std::string, OperationType, json*, json*, json*)` ——
   `_ZN7BaseMod4callESs13OperationTypePN8nlohmann10basic_json…ES9_S9_`。
   也就是说 **C 侧的 4 个参数顺序是 `(实例, 操作类型, 方法名, 请求json, 响应json,
   附加json)`**，第一版 harness 按 `(实例, 方法名, 参数文本, out)` 传，直接把
   `const char*` 当成 `OperationType`、把参数文本当成 json 解引用，所以段错误。

3. **插件是"旧 string ABI + 自带第三方库"编的**（🟢 导入表/符号）：

   - 导入的是 `_ZNSsC1EPKcRKSaIcE`（旧 ABI 的 `std::string`），不是
     `_ZNSt7__cxx1112basic_string…` —— harness 必须加
     **`-D_GLIBCXX_USE_CXX11_ABI=0`**，否则字符串跨边界就把堆写坏
     （症状是 `free(): invalid pointer`）。
   - HTTP 用的是 **cpp-httplib**（`httplib::Client::set_connection_timeout` 等导入），
     日志用的是 **spdlog**（导入 `spdlog::logger::debug<const char*&, const char*&>(
     fmt::v8::basic_format_string<…>)`）。
   - json 是 **nlohmann**，但它的 map 比较器是 `std::less<std::string>`
     （修饰名里的 `St4lessISsE`），而 Debian 的 3.11.2 用 `std::less<void>` ——
     这就是目前过不去的那道坎（见下）。

4. **logger 可以造假**：插件只用它打日志，一个全零、虚表全是空函数的假对象就够
   （`plugin_drive.cpp` 里的 `fake_logger_make()`），构造因此能成功。

3. **依赖顺序**：`libstdc++.so.6` → `libbase64.so` → `libLogApi.so`（还要
   `libboost_thread.so`）→ `libbase.so` → 具体插件。现场库是 C++ 但 **NEEDED 里
   没有 libstdc++**，所以必须先在宿主里把 libstdc++ 拉起来（现场是
   `nclink-service` 干的）。

## 还差什么

`plugin_drive.cpp` 现在能：加载插件 → 造出实例（`ctor -> 0x…`）→ 调 `call`。
卡在最后一步：**`call` 要的 `nlohmann::json*` 必须是插件那一版的类型**
（`std::less<std::string>` 比较器，即 nlohmann < 3.11），我们的 3.11.2 是
`std::less<void>`，把我们的对象当它的对象解引用就会段错误。三条路：

1. 用**插件那一版的 nlohmann**（把版本试出来，或干脆用 3.9/3.10 的头文件）编
   harness，`json*` 就通了；
2. 或者直接跑现场宿主 `nclink-service`（它自己那版 json，天然对齐），把
   `driver_def` 的 `ipAddress` 指向假机床再触发一次读——见下面的进展；
3. 或者绕开 json：插件里凡是接受 JSON *文本* 的入口（如
   `GSKHTTP::GSKHTTP(spdlog::logger*, std::string)`）都能直接调，只是它不做业务。

任一条通了，GSK / KEDE / Mitsubishi-HTTP / 相机这几家的**设备侧请求形状**就能
在本地补齐，不用等现场（`protocal/docs/29-现场模型与驱动定义.md` §6）。

第 2 条（跑现场宿主）也已经试到中途（`ncl_service.sh`）：

- ✅ 宿主能在 armv7 容器里起来；
- ✅ spdlog 的 `Failed getting file size from fd: Value too large for defined data
  type` 是 32 位进程 `fstat()` 一个 Windows bind-mount 上的文件报 EOVERFLOW——
  把工作目录与日志都放到容器自己的文件系统（`/tmp`）就好了；
- ✅ `./nclink.cfg` 是**老的键值配置**（`-C nclink_cfg.json` 之外的另一份），
  键名从二进制里抄出来了（`server_ip` / `server_port` / `reconnect_sec` /
  `user_name` / `password` / `log_level` / `log_file` / `log_limit` / `model` /
  `device_id` / 各主题的 `_qos`），空文件会让它 **SIGBUS**（读一个 0 字节文件），
  写全键之后不再报"打开失败"；
- ❌ 但紧接着仍然是在 qemu 下的 SIGBUS（日志一个字都没写出来，崩在很早），
  这一条暂时走不通。

另外：`INCBOX200/log/` 里那几份**现场运行日志**能告诉我们运行期的形态
（每项一个 data_driver、重试节奏），但那份日志里 FOCAS 一次都没连上机床
（2909 次 `-16`），所以它不能当抓包用（29 册 §8）。
