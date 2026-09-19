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

docker run --rm --platform linux/arm/v7 -v <现场包>/app1/hp2x:/hp2x:ro \
  -v $PWD:/work ncl-arm-probe sh /work/arm_analyze.sh '(*S7).Mode'
```

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
  要往下拿数据调用（`cnc_statinfo` / `cnc_rdparam` …）得先把应答体的字段对上；
  那一步需要一台真机抓一次，或按 Fwlib32 的 PDU 结构补全。

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
