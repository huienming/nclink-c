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

> 还没钉死的一格：官方 SDK 对 `ODBAXIS` 那一族（`cnc_srvdelay` / `cnc_absolute`）的
> 单轴调用**在本地**就回 `EW_ATTRIB`（轴号越界）——它从不把 `data[]` 填出来，说明驱动
> 眼里的"受控轴数"还是 0。已排除：握手 `func 01` 的 16 字节头、握手记录（A/B/C/D 与
> 每条的 `0x18` 详情）、能力块 `0x0e/0x26f0` 的载荷（照 NCGuide 的 `ODBSYS` 铺过）。
> 剩下最可能是 `0x18` 载荷里"记录类型/轴号"那格的取值；桥里开关都留好了，试出来就能把
> "8 字节还是 4 字节"一次定死。

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
