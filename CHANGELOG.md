# 变更记录

本文件记录 NC-Link C 实现（`nclink-core-c`）的版本变更。版本号跟随
NC-Link 规范版本：**3.0.0** 对应 GB/T 41970-2022 协议 3.0.0。

## 3.4.0

文件通道换成**显式握手**：字节仍然走 FTP、MQTT 只传 `/temp/<名字>` 令牌，但设备不再
从 `conf/mqtt.cfg` 猜对端。版本号推进到 **3.4.0**（`NCL_VERSION`、CMake 工程版本与
包名一致）。

### 变更（破坏性）

- **删掉"按 conf/mqtt.cfg 猜对端"那条隐式路径**。3.3.0 以前设备端文件工具默认按
  "broker 的主机名 + 2323 + admin/123456"去拨 FTP——只有对端恰好跑在 broker 那台机器
  上才成立。现在对端只有两个来源：
  - 上位机用 **`file/openFileChannel`** 握手把端点交过来（推荐）；
  - 设备自己用 `ncl_server_set_file_peer()` 钉一个静态对端（要求对端自己跑 FTP 服务端、
    目录布局是 `/<sn>/...`）。
  两者都没有时，设备端文件方法答新增的 **`NoFileChannelException`
  （`NCL_ERR_NO_CHANNEL`，-14）**，不再有 `127.0.0.1` 兜底。
- **客户端管理器不再在 `init` 时顺手起本机 FTP 端点**。要传文件就先
  `ncl_client_open_file_channel()`；它按需把端点拉起来（`ncl_client_holder_start_ftp*()`
  仍是显式起端点的入口，用于换端口/根目录/账号）。

### 新增
- **文件通道握手**（`file/openFileChannel` / `file/closeFileChannel`，`file` 工具的第
  6、7 个方法）：参数 `host`/`port`/`user` 必填，`password`、`channelId`（租约名）、
  `path`（远端前缀，默认设备 SN）、`force`（顶替已有通道）可选。租约名相同的重复握手是
  幂等 no-op（应答 `reused=true`），断线重连后重试不会打断正在传的传输；租约名不同又
  没给 `force` 则拒绝（`NG`，理由里带当前租约名）。设备侧查状态：
  `ncl_server_file_channel_is_open()` / `ncl_server_file_channel_id()`。
- **客户端侧 API**：`ncl_client_open_file_channel(client, options)`（`options` 可为
  `NULL`，结构体 `ncl_file_channel_options` 覆盖 host/port/user/password/channelId/
  path/force/timeout）、`ncl_client_close_file_channel()`（幂等）、
  `ncl_client_file_channel_is_open()`、`ncl_client_file_channel_id()`。默认路径下库给
  每条通道在端点上加**一个临时账号**（随机口令，只有设备知道），关闭通道时撤销它——
  撤一条通道不影响同一端点上别的对端。
- **配置文件 `conf/ftp.txt`（可省）**：`host` / `port` / `advertisePort` / `root` /
  `userName` / `password` / `path` / `force`，键全可选，缺文件/缺键/空值都退回默认，
  文件写坏只记一条 WARNING。每次开通道与 `start_ftp*()` 都重新读，改完不用重启；
  优先级 **函数参数 > conf/ftp.txt > 推导默认**。读写接口
  `ncl_file_channel_config_read/_write/_free()`。
- **地址推导**：默认交给设备的地址是"到 broker 的本机地址"
  （`ncl_socket_local_ip_toward()`：connected UDP 套接字问路由表，不发包）；broker 在
  本机时结果是回环地址，改取本机第一个非回环 IPv4，最后才 `127.0.0.1`。跨网段、端口
  映射、走 VPN 的部署用 `options.host` 或 `conf/ftp.txt` 显式指定。
- **FTP 服务端多账号**：`ncl_ftp_account` +
  `ncl_ftp_server_add_account()` / `ncl_ftp_server_remove_account()` /
  `ncl_ftp_server_account_count()`；每个账号可带自己的根目录与写权限，撤销账号会断开
  同名会话（文件通道的临时账号就建在这上面）。
- **`ncl_client_holder_server_uri()`**：返回进程级客户端实际建连用的 broker URL
  （`init_ex` 里存下来的那份，不一定是磁盘上的 `conf/mqtt.cfg`）。
- **托管绑定**：原生垫片新增 `nclshim_client_file_channel_open` / `..._open_ex` /
  `..._close` / `..._is_open` 与便利入口 `nclshim_client_ensure_file_channel`；
  C# / Java / Python 的 `DeviceClient` 加 `OpenFileChannel` / `CloseFileChannel` /
  `FileChannelIsOpen`，并且上传/下载/列目录/建目录/删文件/带文件参数的方法调用这些
  便利方法会**按需自动握一次手**（C API 保持显式，不做隐式网络动作）。
- **文件传输改成流式 + 可续传**（`ncl_ftp_client_upload()` /
  `ncl_ftp_client_download()`，设备端 file 工具直接用这两个）：
  - **流式**：上传按 256 KiB 从本地文件读着发（对端没有就 `STOR`）、下载按 64 KiB 收着写盘，
    内存不随文件大小增长 —— 512 MiB 的文件只需要 256 KiB + 64 KiB 两个缓冲，
    **20 MiB 静态池构建**也能传 64 MiB 文件（旧实现是整文件读进内存，那份构建跑不了）。
    服务端同样改成流式收写（`REST` 对 `STOR` 生效 = 从该偏移覆盖写，收完按实际长度截断）。
  - **续传**：上传先问对端 `SIZE`，已有前缀就 `APPE` 续；下载按本地已有大小 `REST` 续。
    单次调用内失败重试 3 次、每次从断点继续；测试里用
    `NCL_TEST_FTP_ABORT_AFTER` 把数据连接在第 3 MiB 掐断，断言传输仍成功、校验一致、
    线上字节数 ≥ 文件大小且 < 2×（即"续传"而不是"重传"）。
  - **计数**：`ncl_ftp_client_bytes_sent/received()`、
    `ncl_ftp_server_bytes_sent/received()`、`ncl_server_file_tool_bytes_sent/received()`
    （工具级，跨重连不归零）。
- **被动模式可按通道要求**：`ncl_file_channel_options.passive` / `conf/ftp.txt` 的
  `"passive": true` → 握手里带 `passive` 参数 → 设备改用 PASV 传输。设备在 NAT /
  容器 / 防火墙后面时必需（主动模式要 FTP 服务端反向连回设备）。
  设备侧也可直接 `ncl_server_file_tool_set_passive()`。
- **流式 SHA-256**（`ncl_sha256_new/update/finish/free()`）与流式文件复制
  （`ncl_file_open_read/read_chunk/close_read`、`ncl_file_open_write/write_chunk/close_write`、
  `ncl_path_same_file()`）；`ncl_file_checksum()` / `ncl_file_copy()` 内部也改成流式，
  于是"校验一个 512 MiB 文件"不再需要 512 MiB 内存。
- **`examples/ncl_file_bench.c`**：文件通道吞吐 / 跨主机测试台（`device` / `client`
  两个角色，可指定 host/port/passive），进度与结论见 **TRANSFER_PERF.md**
  （同机 16 MiB~512 MiB 上传 128~512 MiB/s、下载 106~140 MiB/s；跨容器
  上传 786~901 MiB/s、下载 136~155 MiB/s；续传行只传一半且逐字节一致）。

### 修复
- **`ncl_client_ll()` 以前把设备回的 NG 当成功**：空目录和被拒绝都返回 `NCL_OK`，
  调用方分不出来。现在应答项 `code != OK` 记日志并返回 `NCL_ERR`。
- **设备没取到文件时上传会报成功**：设备的 file 工具在拿不到文件时按协议回
  `code=OK` + `value=false`，客户端工具只看 code，于是"传输失败"被当成"上传成功"。
  现在客户端会检查那个布尔值并报失败。

### 文档
- 手册 5.10 重写成"握手 → 传输 → 收租约"三段，并补了 `conf/ftp.txt` 一节；5.9 补 FTP
  多账号；2.4.3/2.4.4/2.4.5（Java/Python/C#）与 3.2 客户端最小程序、7 章排错、附录 D
  目录布局同步；附录 A/B 由 `tools/gen_api_index.py` 重生成（错误码表补
  `NoFileChannelException` 文案）。四个绑定的 README、垫片头注释也跟上。

### 实测（本次发布前）

| 项 | 结果 |
|----|------|
| Windows x64（MSVC 14.44.35207，Release） | **25/25**；`file` 套件 **271 项断言**（新增：握手 6 个用例、`conf/ftp.txt` 4 个用例、**64 MiB 大文件**、**对端已有前半/本地已有前半**两种续传、**数据连接第 3 MiB 被掐断**后的续传重试） |
| Windows 其他变体 | x86、静态内存、TLS、静态内存+TLS、x86 静态内存各 **25/25** |
| Linux（gcc 13.4 / Debian bookworm，容器内） | 默认堆 / TLS / 静态内存 / 静态内存+TLS 各 **25/25** |
| mingw-w64（gcc 16.2.0，UCRT + posix threads） | 非 TLS 与 TLS 各 **25/25**（含 `test_cpp`） |
| 内存检查 | ASan + LeakSanitizer（6 个套件）**0 发现**；ThreadSanitizer **0 数据竞争** |
| 托管绑定自检 | C# 106 项、Java 107 项、Python 46 项，**0 失败**；对真 broker（EMQX）C# **135 项**、Java **15 项**、Python 46 项 0 失败（含文件通道握手全流程） |
| Go 绑定 | Linux 容器与 Windows（cgo + mingw）`go test ./...` 通过；`-tags nclink_tls` 两侧通过 |

## 3.3.0

### 新增
- **发布包同时提供两种构建**：`lib/<平台>/` 仍是默认（分配走 C 运行库堆）版，新增
  `lib/<平台>-staticmem/` 是**静态内存版**（库内分配全部走 `.bss` 里的固定池，本包按默认
  20 MiB 编译，不调用 `malloc`）；示例可执行文件同样给两份（`examples/bin/*-staticmem/`）。
  另有 **静态内存 + TLS** 的组合（`lib/*-staticmem-tls/`，池仍只覆盖库自身、OpenSSL 走系统堆）。
  打包脚本 `make_release.ps1` 一并收集，并新增产出 `dist/<包名>.zip.sha256`；版本号推进到
  **3.3.0**（`NCL_VERSION`、CMake 工程版本与包名一致）。

- **并发（多线程）压测套件 `mem_mt`（`tests/test_mem_mt.c`）**：多线程共享同一个池做随机
  流量（随机尺寸、随机分配/释放/重分配），每块带图案并在释放前校验；线程之间还通过环形
  队列**传递所有权**（A 分配写图案、B 收到校验再释放——正是"收包线程分配、调用方释放"的
  形状）；主线程在它们运行期间**不停调用 `ncl_mem_check()`**。为让自检可用于并发，
  `ncl_mem_check()` 现在还会**从块本身重算**账目恒等式（`已用 + 空闲 + 块头×块数 == 池大小`）
  并与计数器比对，全部在同一把锁下完成，因此每次观测都是一致快照、非零即真缺陷。
  收尾断言：池回到一整块空闲、`in_use 0`，且"申请最大空洞大小必须成功、比它大一个对齐
  单位必须被拒"。
  实测：单轮 64 KiB 池 4 线程 × 40000 次操作 = **160000 次操作 / 22838 次跨线程交接 /
  0 违规**；10 分钟并发长跑（8 线程 × 64 KiB/512 KiB/20 MiB）= **约 5.63 亿次操作、
  1.44 亿次分配、8030 万次跨线程交接、7034 轮、0 失败**；见手册 4.9.4。
- **ThreadSanitizer 门禁**：`./tools/asan-linux.sh --tsan --docker` 专压上述并发用例
  （TSan 与容器 ASLR 冲突，脚本自动把 `vm.mmap_rnd_bits` 降到 28，需要 `--privileged`）。
  6 线程 × 20000 次操作 **0 数据竞争**；`--docker` 的 ASan 套件也已把 `test_mem_mt` 纳入。
- **长跑（soak）工具 `tools/soak-linux.sh`**：为每个池尺寸各编一份库 + 压测程序，并行跑满
  指定时长（默认 1 小时），支持 `NCL_SOAK_MT=1` 切换到并发版；逐操作校验不变量、逐轮验证
  "排空后回到一整块空闲"，任一条不满足即非零退出。首次实测（18 逻辑核，7 个尺寸并行，
  3600 s，exit=0）：**260389 轮、约 52.1 亿次操作、22.0 亿次分配、0 失败**；形状拒绝约占
  拒绝的 60%（合成流量特征），最坏最大连续空洞约为池的 1/128～1/50。表见手册 4.9.3。

- **静态池的小对象尺寸类**：实测请求分布极度偏小（**98% 以上的请求 ≤128 字节**，33～64
  字节一档约占一半），所以池在首次使用时把自己切成"尺寸类区域 + 通用区"。尺寸类为
  {16, 32, 48, 64, 96, 128, 192, 256}（**步长不用 2 的幂**：C 类只在请求 > C−32 时划算，
  插 48/96 是为了压掉 65～96 字节那段亏损窗口），类内**无块头**、靠"指针落在哪个区域"
  判定类别，分配/释放 O(1) 且**类内结构上不可能外部碎片**；类用完**自动回落通用区**，
  因此配小了只损失速度、不产生新失败模式。开关 `NCLINK_MEM_CLASS_BYTES`（`-MemClassBytes`，
  `-1`＝池的 1/4，`0`＝关闭）。
  实测收益（64 KiB 池，24 个套件，**实际占用＝载荷+块头**的峰值，开/关对照）：
  server 36160→25584（**−29.2%**）、message 24064→16256（**−32.4%**）、
  client 20032→13920（−30.5%）、rest 25872→18000（−30.4%）、model 16304→10720（−34.2%）、
  event 25504→15344（−39.8%）、schema 7456→4032（−45.9%）、json 2800→1568（−44%）、
  mem_mc 50608→40736（−19.5%）；`ftp` 只 −1.9%（几乎全是 16 KiB 大块，本就不进类）。
  池大小边界随之改善：**32 KiB 池 22/25 → 23/25**（`message` 现在通过），64 KiB 仍 25/25。
  另新增两个统计口径：`footprint_bytes` / `peak_footprint_bytes`（载荷 + 块头）与每类的
  `class_size/class_bytes/class_live/class_free`——`in_use_bytes` 只算载荷、天然偏向通用区，
  比较"省没省"要看 footprint。

- **静态内存（无堆）构建**：库内 471 处分配点全部改道 `ncl_mem_alloc()` /
  `ncl_mem_calloc()` / `ncl_mem_realloc()` / `ncl_mem_free()`（`src/core/ncl_mem.c`
  是唯一知道内存从哪来的文件；默认实现转发给 C 运行库，行为与开销不变）。
  `NCLINK_STATIC_MEM=ON` 时改由**静态数组里的一个固定池**供给：库不再调用
  `malloc`，池耗尽返回 `NULL` 并由上层翻成 `NCL_ERR_NOMEM`，绝不回退到堆。
  池默认 **20 MiB**（`NCLINK_MEM_POOL_BYTES` / `NCL_MEM_POOL_BYTES`，池在 `.bss`
  里，不占可执行文件体积），先跑通再按量到的峰值往下压。配套选项还有
  `NCLINK_MEM_SINGLE_THREAD`（去锁）、
  `NCLINK_MEM_STRICT`（外来指针释放即 abort）、`NCLINK_MEM_REPORT`（退出时打印峰值）；
  `build.ps1` 是 `-StaticMem / -MemPoolBytes / -MemReport`，`build-linux.sh` 是
  `NCL_STATIC_MEM=1 NCL_MEM_POOL_BYTES=...`。新增运行时统计
  `ncl_mem_get_stats()` / `ncl_mem_mode()` 与测试套件 `tests/test_mem.c`。
  **碎片**：池用最佳适配 + 释放时双向合并，`test_pool_fragmentation` 逐轮断言"空闲块
  数回到 1、最大连续空闲块逐字节等于轮次前"（长期块 + 6×300 次混合尺寸 churn）；
  拒绝时的空闲快照（`failure_free_bytes` / `failure_largest_free_bytes`）用来区分
  "池小了"和"空间被形状打散"。这一改有实测依据：64 KiB 池 + 首适配时 file 用例出现过
  一次真实的形状拒绝（总空闲 21152 / 最大连续 10592 / 请求 16384），换最佳适配后
  0 拒绝。实测（Windows x64 / MSVC）：**64 KiB 池全量测试 25/25 通过**，32 KiB 时
  22/25（message、ftp、file 是"总空闲都不够"的尺寸问题）；设备端常见组合的峰值在
  32 KiB 上下。细节见手册 4.9。
- **深挖碎片时修掉两个真缺陷**（都在静态池里，只有紧池 + 自检能稳定暴露）：
  ① `realloc` 原地扩张合并后没更新"后继块的前驱链接"，留下陈旧链接，之后释放会往错
  地址合并；② 切分块产生的空闲尾块没有与后面的空闲块合并，出现**两个空闲块永久并排**
  （空间是空的，却是碎的）。合并逻辑统一改成"向前合并到底"。
- 新增自检 `size_t ncl_mem_check(void)`：走一遍池，校验前驱链接、相邻空闲块、覆盖范围，
  返回问题条数（默认构建恒 0）。就是它把上面第 ② 条从"偶发崩溃"钉成了可复现的用例。
- 新增蒙特卡洛压测套件 `tests/test_mem_mc.c`（`mem_mc`）：随机尺寸跨三个数量级、随机
  分配/释放/重分配、每块带图案校验、每次操作后校验池不变量与账目恒等式、拒绝必须正当
  （把"总空闲够却失败"单独计为形状拒绝）、**同一种子跑两遍必须完全一致**（漂移检测）。
  它上线即抓到第三个缺陷：切分后若后继块是已分配的，`merge_forward` 提前返回，导致后继
  块的前驱链接陈旧（一个"任何尺寸变化都必须修后继链接"的不变量，现在由
  `pool_fix_follower()` 统一保证）。
  同序列对照（64 KiB，4 种子 × 20000 操作）：最佳适配 20 次拒绝（其中形状 7 次 / 35%），
  首适配 22 次（形状 13 次 / 59%），最坏最大连续空洞 5504 B vs 3920 B——最佳适配的收益
  第一次有了量化数字。新增开关 `NCLINK_MEM_FIRST_FIT`（`-MemFirstFit`）用于复现对照。
- `ncl_mem_stats` 追加 `meta_bytes`（块头总开销）与 `size_hist[16]`（请求尺寸直方图），
  `-MemReport` 退出时一并打印：实测 **98% 以上的请求 ≤ 128 字节**，而 `ncl_strbuf` /
  向量 / 模型 map 都是翻倍增长，单次最大块需求可达最终大小的 1.5 倍以上——这两条是
  "要不要上尺寸类"与"池该留多少余量"的直接依据。
- `ncl.hpp`、`examples/`、`tests/` 里释放库指针的地方统一改用 `ncl_free_safe()`，
  与静态池构建的所有权约定一致（此前直接用 libc `free()`）。

### 修复（Linux + ASan 复验发现）

- **MQTT 客户端重连会漏掉已结束的收包线程对象**：`ncl_mqtt_client_connect()` 在
  reader 因服务端 DISCONNECT/网络故障自行退出后再次被调用时，会直接覆盖
  `client->reader`，那个已结束但从未 join 的线程对象就漏了（24 字节；POSIX 上还会一
  并占着已结束线程的栈直到 join）。设备反复重连就是长期增长。现在重连前先 join 掉陈旧
  的 reader。由 LeakSanitizer 在 `test_mqtt_client` 抓到（MSVC ASan 无泄漏检测，
  所以此前看不见）。
- **`test_mqtt_client` 的假 broker 停机写法是 use-after-free**：`broker_stop()` 直接
  `ncl_socket_close(listener)` 去打断 `accept()`，而 close 会立即释放对象 —— 另一线程
  的 `accept()` 正在读它。库自己在 `http_server.c` 的注释里给出的正确写法是
  shutdown → join → close，`fake_nclink_server.c` 也是这么写的。已按同样形状修正。
- **`test_message` 的 `check_wire()` 泄漏解析结果**：`ncl_message_from_json()` 收的是
  `const ncl_json *`（借用），测试把 `ncl_json_parse_cstr()` 的结果直接传进去却没释放
  —— 每次调用漏一个 JSON 根对象，一次运行累计 269 个对象。已补 `ncl_json_free()`。
- 测试侧的分配器混用：`tests/` 里由 libc `malloc` 申请、却被 `ncl_free_safe()` 释放的
  缓冲（假服务器收包缓冲、TLS/broker 大载荷、ptrvec 元素）改回 libc `free()` —— 静态池
  构建下池会（按设计）拒收外来指针，于是这些缓冲全部泄漏（LeakSanitizer 报了 170 处）。
  `test_mem` 里两个"超大请求"探针只在池构建下运行（堆构建下那是 libc 的
  calloc 溢出/malloc 过大，ASan 直接 abort）。
- 新增 `tools/asan-linux.sh`（ASan + LeakSanitizer，`--docker` 可在 Windows 跑）。

- **`ncl_rest_attach()` 的上下文没人释放**（每次 attach 8 字节，`test_rest` 报 16 字节 /
  2 处）：REST 用同一个上下文注册 4 条路由，"谁负责回收"此前没有约定。新增
  `ncl_http_server_own_context(server, ctx, free_fn)`，**按上下文登记一次**（不是每个路由
  一个析构器——那个共用上下文会被释放 4 次），由 `ncl_http_server_free()` 在路由释放之后
  释放一次；同一指针重复登记返回 `NCL_ERR_EXISTS`，登记失败时调用方仍持有它。不登记
  上下文的既有写法（栈上/静态对象）行为逐位不变。`ncl_rest_attach()` 已改用它。
  `tests/test_http.c` 新增用例覆盖"恰好释放一次／重复登记被拒／参数校验"；验收标准是
  `tools/asan-linux.sh` 归零，现已达成：`asan-linux: 0 suite(s) with sanitizer findings`。

### 文档

- 手册补上"被别的程序集成"这一面：**2.5「集成到自己的工程」从三条扩到七条**——两个堆的
  释放边界、一个进程一份库（客户端 holder / 环境根 / logger / 线程池都是进程级全局）、
  安装根别靠 cwd（库里会建 `bin/sn.txt`、`conf/mqtt.cfg`、`log/out.txt`、`uploadFile/`）、
  池按量到的峰值开；顺带修掉 2.5 第 2 条里重复的 `ncl_net_ip_map_json()` 片段。
- **新增 4.9.5「集成约束」**：外来指针静默丢弃（默认）vs `NCL_MEM_STRICT` abort、
  库无法共用宿主的池（替换 `src/core/ncl_mem.c` 是唯一出路）、静态版仍走系统堆的路径
  （线程栈 / DNS / OpenSSL / CRT）、容量量法（`peak_footprint_bytes`、16 KiB 连续块、
  `failure_*` 快照、小池压 NOMEM）、一把全局锁与 `ncl_mem_check()` 的代价、进程级单例与
  退出顺序，末尾附一张"验收清单"表，可直接搬进宿主 CI。

### mingw 复验（Windows 目标的 GCC 16）

- **`build-linux.sh` 在 mingw 目标下会自动补 Windows 系统库**（`-lws2_32 -liphlpapi
  -lwinmm`）。此前按 `tools/stage-go-libs.sh` 里写的
  `CC=<mingw>/gcc AR=<mingw>/ar sh build-linux.sh build-mingw` 跑，库能出来，但示例与
  测试在链接期报一片 `undefined reference to __imp_WSAGetLastError` 之类——MSVC 靠源码
  里的 `#pragma comment(lib, ...)`，mingw 没有这个机制，于是那条"官方配方"实际是半截的。
  判据取 `$CC -dumpmachine`，不影响非 mingw 的构建。
- **三处测试缺 include 被 GCC 14+ 抓出来**（隐式声明在新版 GCC 里是 error，不再是
  warning）：`tests/fake_nclink_server.c` 少了 `<stdio.h>`（`snprintf`，连带
  `test_client` / `test_server` 编不过）、`tests/test_message.c` 少了
  `nclink/ncl_env.h`（`ncl_file_read_all`）、`tests/test_topic.c` 少了
  `nclink/ncl_common.h`（`ncl_free_safe`）。补齐后 mingw 侧 **25/25**。
- 复验结果：库 / 示例 / 测试在 **mingw-w64 gcc 16.2.0（UCRT、posix-threads）下全量
  25/25 通过**；**Go 绑定的 `go test ./...` 在 Windows 上用 cgo + mingw 也跑通**
  （此前只有 Linux gcc 那一遍的记录）。
- 发布包与文档跟上：`build-mingw/libnclink_core.a` 现在随包提供
  （`lib/windows-amd64-mingw/`）；RELEASE 里"未带 mingw 库"的两处改成实况；手册 2.2 补了
  mingw 这条已验证链路，2.4.2 的 Go 链接说明也点明系统库由脚本自动补。
- **mingw 的 TLS 变体也补齐了**：`build-mingw-tls/libnclink_core.a`（`NCL_WITH_TLS=1`，
  OpenSSL 3.6.1 来自 Strawberry Perl 的头文件与导入库）在 mingw 侧同样 **25/25**，
  Go 绑定的 `go test -tags nclink_tls ./...` 在 **Windows 上也跑通**。该变体与 Linux 的
  TLS 版口径一致：**动态依赖 OpenSSL**（链接要导入库、运行要 `libssl-3-x64*.dll` /
  `libcrypto-3-x64*.dll`），因为 MSVC 那份用的静态 OpenSSL 在 Windows 的 Go 工具链下
  用不了。包内 `lib/windows-amd64-mingw/` 现在同时放非 TLS 与 TLS 两份 `.a`，
  RELEASE 的限制条目改成"依赖 OpenSSL 3 导入库与 DLL"这条实况。

## 3.2.0

三种托管绑定补齐 HTTP/REST、设备端与文件通道，并加上 TLS 选项；顺带修掉
一批文件通道与连接路径上的库问题。

版本号从 3.1.0 起：`NCL_VERSION`、CMake 工程版本与发布包名都跟到 **3.2.0**。

### 修复

- **设备端释放时会踩到正在跑的采样任务（随机段错误）**：`ncl_server_free()` 先释放
  工具绑定表与模型、**最后**才停采样任务；采样线程的每一轮 collect 都要查绑定
  （`ncl_server_lookup()`），于是它可能读到刚释放的表 —— 命中就表现为
  `ncl_server_lookup` 里的 SIGSEGV。窗口只有微秒级，所以是"偶发"（Go 绑定的分配器
  复用得快，13/20 次复现；纯 C 基本靠运气）。现在 `ncl_server_free()` 先把采样任务
  停下并 join，再释放任何东西；`tests/test_server.c` 加了"带着在跑的通道直接 free"
  的用例守住这条顺序。

- **没编 TLS 时报的是笼统的 `NCL_ERR`**：`ssl://` 连不上时只看到 "连接失败"，看不出
  是"库没带 TLS 编"。现在客户端管理器在建连接前先判断 URL 与 `ncl_socket_tls_available()`，
  直接返回 `NCL_ERR_NOT_SUPPORTED`（-8）并记一条明确的日志。

- **主机名解析出多个地址时，第一个卡住的地址会吃光整个超时**：`ncl_socket_connect()`
  按 `getaddrinfo()` 的顺序逐个试，但每个都给完整超时。Windows 上 `localhost` 会先
 解析出 `::1`，而本机 IPv6 环回上没监听时那个 connect **不会立刻被拒**，于是白等
  一整个超时（默认 5 s）才轮到 `127.0.0.1`——表现就是"连 localhost 要 5 秒"，
  文件通道里更要命（设备侧 FTP 连接等 5 s，方法调用那头早就超时了）。现在非最后一个
  地址只试 300 ms（`NCL_SOCKET_STAGGER_MS`），最后一个地址拿剩下的全部预算；只有
  一个地址时行为不变。实测 `ncl_server_file_tool_detect("localhost", ...)` 由
  **5032 ms → 313 ms**。
- **方法名少了前导斜杠就不认**：文件头一直写着 `"/plc/getValue"` 与 `"plc/getValue"`
  都接受，但 `ncl_server_dispatch()` 只在**以 `/` 开头**时才拆 `<工具>/<方法>`，
  `"plc/getValue"` 被当成一个裸方法名 → "未找到方法"。现在两种写法都拆。
- **设备端文件工具的对端目录用了 `bin/sn.txt` 的 SN**：客户端把文件放在
  `<当前目录>/<它寻址的 SN>/` 下，而设备端却按 `bin/sn.txt` 里的 SN 去取 —— 只要
  `bin/sn.txt` 与设备端实际用的 SN 不同（显式指定 SN 的设备端就是这种情况），
  上传必然 550。现在一律用服务器自己的 SN（协议里的那个）。
- **`{"@file": ...}` 标记对象在 Windows 上废掉**：临时文件名取的是路径的 basename，
  而 `remote_basename()` 只认 `/`，Windows 本机路径（`C:\...`）整条被当成了文件名，
  复制必然失败（`fileKeys` 也不会出现在应答里）。现在两种分隔符都认。
- **工具返回的文件与文件工具的镜像目录对不上**：方法结果里的文件被复制到
  `<root>/temp/`，而文件工具的 `read`（客户端按 `/temp/<名字>` 来取）找的是
  `<root>/uploadFile/temp/`；于是"工具返回文件"只能命中"客户端本地已有一份"的
  特殊情况，真取字节就 404。现在落到 `<root>/uploadFile/temp/`，与镜像一致。
- **Java 设备端连不上 `ssl://`（连编译都过不了）**：README 与端到端用例都用
  `new Server(sn, model, broker, user, pass, sink, TlsOptions)`，但 `com.nclink.Server`
  只有 6 个参数的构造，`Native.serverCreateEx` 与 JNI 那侧的实现也缺 ——
  `javac` 直接失败。三处都补齐，与 C# / Python 的同一形状（设备端与客户端现在都能
  走 TLS）。
- **Java 绑定选不到带 TLS 的原生库**：`Native` 的加载顺序里"当前目录 →
  `bindings/java/native/bin/`"写死在 `-Djava.library.path` 前面，于是按 README 指到
  `bin-tls/` 也没用（永远先命中不带 TLS 的那份，`tlsAvailable()` 报 false）。现在
  显式给的 `-Djava.library.path` 优先于仓库里的默认位置。
- **托管绑定的客户端方法调用（`methodCall`）把应答文本当成了 JSON 句柄**：库返还的是
  **应答报文的 JSON 文本**（`char*`），C# 的 `NclDeviceClient.MethodCall` 与 Python 的
  `DeviceClient.method_call` 直接把它包成 JSON 对象 → 一读就炸
  （Python 报 `ValueError: ... is not a valid JsonType`，C# 读到野指针）。Java 那边
  是 `Json.parse(out[0])`，一直是对的。现在两边都用
  `NclJson.TakeText()` / `Json.parse(take_text(...))` 解析；离线自检覆盖不到这条路径，
  所以顺带加了"对真 broker"的可选端到端用例（见下）。
- **垫片 `nclshim_server_create()` 的"模型默认走内置模型"没实现**：文件头的注释一直
  这么写，但代码把 `model_json == NULL` 直接当"空模型"传给 `ncl_server_create()`，
  于是"不传模型"的设备端没有模型——采样通道、按路径应答都无从谈起（Java / Python
  绑定绕过了它：它们自己先把内置模型序列化出来再传）。现在垫片真的会把内置模型
  （`ncl_root_node_parse(NULL)`）序列化后传进去，C# / Java / Python 的行为一致。

### 构建与发布

- **TLS 选项贯通到三份托管绑定**（之前只有 C API 能设 CA / 双向证书 / 关校验 / SNI）：
  库新增 `ncl_client_holder_options` + `ncl_client_holder_init_ex()`，垫片新增
  `nclshim_open_ex()`、`nclshim_tls_available()`，以及设备端那条路
  `nclshim_server_create_ex()`（设备直连 `ssl://` broker）。托管侧：C#
  `Nclink.Init(uri, user, pass, NclTlsOptions)` / `Nclink.TlsAvailable`、
  Java `Nclink.init(..., TlsOptions)` / `Nclink.tlsAvailable()`、
  Python `nclink.init(..., tls=TlsOptions(...))` / `nclink.tls_available()`，
  设备端同理各带一个 TLS 参数。
- **文件通道的对端可配**（原来固定按 conf/mqtt.cfg 推"broker 主机 + 2323 + admin/123456"，
  对端不跟 broker 同机就用不了）：库新增 `ncl_server_set_file_peer()`（设备端指定对端
  FTP 的 host/port/账号）与 `ncl_client_holder_start_ftp_ex()`（本机 FTP 端点换端口 /
  根目录 / 账号）；垫片与三份绑定各包一层（C# `NclServer.SetFilePeer` +
  `Nclink.StartFileServer(port, root, user, pass)`、Java 与 Python 同名）。
- **垫片加 TLS 构建开关**：`bindings/native/build-shim.ps1 -Tls`（连 `build-tls` 的库、
  带上 OpenSSL 导入库，并把运行期要用的两个 DLL 拷到输出目录），README 里写了完整步骤。
- **C# 补上 `LoadModel` / `ClearModel`**（垫片里唯一没被 C# 包的一个函数）：
  客户端可以装上自己那份模型，路径 ↔ id 与采样补齐都靠它。

- **文件通道进了三份托管绑定**（`ncl_file` 早就在库里，垫片里新加一组
  `nclshim_*file*`）：客户端侧 `upload_file` / `download_file` / `list_files` /
  `make_directory` / `delete_file` / `method_call_file`（带文件参数的方法调用）+ 本地
  小工具（`need_compression` / `total_chunks` / `checksum` / `attribute` → `FileInfo`），
  C# 是 `NclDeviceClient.UploadLocalFile / DownloadTo / ListFiles / MethodCallFile`
  + `Nclink.StartFileServer()`，Java 与 Python 同一套形状；三个设备端示例都挂上了
  `register_file_tool()`（客户端传文件用得到）+ 容忍式 `start_ftp()`。
- **HTTP / REST 端点进了三份托管绑定**（`ncl_rest_attach` 早就在库里）：
  `start_http(port, with_config)` + 自定义路由（`method` 支持 `"*"`、`/api/` 开头是
  前缀匹配）+ `request_count` / `set_cors`；C# 是 `NclServer.StartHttp()` /
  `NclHttpEndpoint.Route()`，Java 是 `Server.startHttp()` / `HttpEndpoint.route()`，
  Python 是 `Server.start_http()` / `HttpEndpoint.route()`。端点内容全在库里：
  `GET /api/schema`（OpenAPI 3.0）、`GET /swagger-ui`、`POST /api/<工具>/<方法>`
  （等价于 methodCall，走 Result 信封），配置端点（SN / 模型 / 驱动 / mqtt.cfg /
  服务器列表）。
- **C# 绑定补齐设备端**：`NclServer`（工具注册 / 路径绑定 / 采样通道 / 事件推送 /
  离线 dispatch / 自研传输）+ `NclHttpEndpoint` + `Nclink.Parse()` / `NclMessage`
  （报文解码，`AsSample()` / `AsEvent()` 拿快照），`NclOperation` / `NclToolBinding` /
  `NclToolHandler` 与 Java / Python 一套语义；新增设备端示例
  `samples/Nclink.Demo.Device`（`broker` 写 `-` 就是离线：出站报文走自研传输打到
  控制台，REST 端点照常可用）与一键构建 `bindings/csharp/build.ps1`。
- **C# 自检工程** `tests/Nclink.SelfTest`（106 项，不需要 broker）：JSON / 模型 /
  报文解析 / 设备端（离线 dispatch、工具注册、采样通道、事件、自研传输、关闭语义）/
  HTTP（REST、配置端点、swagger-ui、自定义路由、错误路径、幂等关闭）；net472 与
  net8.0 两个目标都跑通。Java 自检补上 HTTP 与文件小工具用例（75 → 107 项），
  Python 32 → 46 项。
- **可选的真 broker 端到端用例**：`NCLINK_TEST_BROKER=tcp://host:port` 一设，Python
  （`tests/test_broker_e2e.py`）与 C#（自检里的 `Broker` 一节）就把设备端与客户端
  放进**同一个进程**、报文真的过一遍 MQTT：probe、路径绑定取值/写值、`methodCall`、
  采样上报、事件推送，以及整条**文件通道**（上传 / 列目录 / 下载 / 建目录 / 删文件 /
  带文件参数的方法调用 + 工具返回文件）。Java 也补了 `com.nclink.BrokerE2E`
  （`build.ps1` 里设了环境变量就会跑）。C# 132 项 / Java 12 项 / Python 46 项，实测
  Mosquitto 2 与 EMQX 5.8.9 各跑一遍都 0 失败（methodCall 的应答解析、文件通道的
  几个 bug 都是这么抓到的）。
- **可选的真 TLS broker 端到端用例**：再有 `NCLINK_TEST_TLS_BROKER=ssl://host:port`
  与 `NCLINK_TEST_TLS_CA=<pem>`，三份绑定就多跑一段"设备端与客户端都过 `ssl://`"的
  用例（C# 134 项 / Java 13 项 / Python 46 项，对 Mosquitto 的 8883/18832 TLS 监听
  与 EMQX 都 0 失败）：正例之外还有"不给 CA 必须被证书校验挡下"的反例；库没编 TLS 时
  明确的 `NCL_ERR_NOT_SUPPORTED` 由离线用例守着。
- **Go 绑定补齐设备端**：新增 `bindings/go/server.go`（`NewServer` / `RegisterTool` +
  `Binding` / `RegisterBuiltinTool` / `RegisterFileTool` / `Subscribe` / `InitSamples` /
  `AddSample` / `RemoveSample` / `PushEvent` / 离线 `Dispatch` 与 `Invoke*` /
  `StartHTTP` + `Route`）与 `nclink_thunks.c`（cgo 不能把 Go 函数指针交给 C，而 C API
  是**用函数指针认工具方法**的，所以每种方法一个 C 跳板；回调句柄另起一次分配，因为
  cgo 只允许把"不含 Go 指针的 Go 内存"交给 C），示例 `example/device` 既能离线跑
  （出站报文打到控制台）也能过 broker；离线自检不需要 broker。
- **Go 绑定的 `-tags nclink_tls` 之前等于没开**：那个 tag 只加了 `-lssl -lcrypto`，
  链的还是非 TLS 的 `libnclink_core.a`，`ssl://` 永远返回 `NCL_ERR_NOT_SUPPORTED`。
  现在该 tag 链 `libnclink_core_tls.a`（`tools/stage-go-libs.sh` 已经会暂存这份），
  并补了 `nclink.TLSAvailable()` 供调用方先问一句。
- **借用视图的 `Dispose` 改成空操作**（C#）：`NclJson` 的下标/成员视图、设备端的
  `NclServer.Model` 这类借用对象以前 `Dispose` 会把句柄置空、之后再用就抛
  `ObjectDisposedException`；现在与 Java / Python 一致——借用的东西不归你管，
  `Dispose` 什么都不做。
- Python 设备端示例的收尾计数：关闭后再读 `sample_upload_count` / `event_count` 会抛
  `ClosedException`，现在先读计数再关。

### 采样与报文

- `ncl_message_sample_value_at()`：**按行**取某列的值。行数 = 数据最多的那一列的
  点数（最细的那根时间轴），更粗的列在覆盖该行的段里反复取第一个点 —— 1 ms 的功率
  列与 0.25 ms 的振动列可以放在同一张表里逐行读，消费端不用自己判断谁粗谁细。
- `ncl_message_sample_point_count()` 的语义明确为"行数 = 最多那列的点数"（此前要求
  各列点数相同，现在不同也能读）。
- **完整性口径放宽到只看外层**：表头项数 == 列数、各列槽位数一致即可，内层（每槽
  装几个点）各列自便，每槽点数抖动也照常上报。整列 `[]` 仍按"本周期无数据"处理，
  由 `ncl_message_sample_normalise()` 按列换成 `null`（不再要求"换完能回到统一
  标量形状"）。
- 设备端/客户端示例按新口径消费：逐列打印编码与点数，再按行打前 8 行。

### 设备端示例（ncl_device_demo）

- **默认一直运行**：省略运行秒数（或写 0）就一直跑到 Ctrl+C / SIGTERM；给正数则跑完
  自动退出（脚本、冒烟）。Ctrl+C 走正常清理路径（停采样、停 FTP/HTTP、断开 MQTT），
  退出码 0；SIGINT/SIGTERM 的处理函数只置标志，主循环 100 ms 一跳，响应不迟。
- 默认模型的采样整理成**两个通道**：
  - 通道 0 `sample_channel0`（1 s / 1 s，机床运行状态八项）：加工计件、进给倍率、
    当前加工程序名、当前刀号、主轴转速、设备状态、加工模式、报警号；
  - 通道 1 `EdgeSersors`（`sampleInterval` 1 ms / `uploadInterval` 100 ms，十二项）：
    5 轴的功率与振动（振动每槽 4 点 = 0.25 ms）；主轴 S 上挂**两路**传感器，用数据项的
    `number` 区分。一条报文 100 个槽位（功率列 100 点、振动列 400 点）。
- **数据项支持 `number`**：一个部件挂多路同类传感器时就是"同 `type`、不同 `number`、
  不同 id"的几个数据项，路径变成 `/<父路径>/<type>@<number>`（`/AXIS@S/POWER@1`、
  `/AXIS@S/POWER@2`），工具绑定、采样表头、按路径查询都按这条完整路径走；没有
  `number` 的项就是单路，路径不带后缀。`tests/test_model.c` 增补了路径、按路径反查、
  同一个通道两路传感器各成一列，以及 `number` 的序列化字段顺序（在 `dataType` 之前）。
- 主轴转速按"轴 + 物理量"写在主轴 S 轴上：`/AXIS@S/SPEED`（`SPEED` 数据项），不再
  是设备级自成一类的 `SPINDLE_SPEED`；示例补上对应工具绑定与模拟值。
- 三个示例（设备端、C 客户端、C++ 客户端）都会打印轴上的量（`路径 含义`），
  `SPEED` 读作"转速"：`/AXIS@S/SPEED 主轴转速`。
- 模拟产件数与主循环计数改成 64 位，长时间运行（现场是"一直跑"）不会溢出。
- 日志同时写 `<root>/log/out.txt`（UTF-8，10 MB 轮转）与控制台（stderr，真控制台
  走 `WriteConsoleW`，中文在任何代码页下都对）。

### 修复

- **`ncl_mqtt_client_disconnect()` 断开前先把套接字里在途的字节读干净**。
  之前是"发完 DISCONNECT 直接 shutdown(SD_BOTH) + closesocket"：Windows 上若关闭时
  还有没读走的接收数据（例如刚到的 SUBACK），close 会走 **RST** 而不是 FIN，对端收到
  RST 时会把它**还没读**的数据一起丢掉 —— 刚发过去的 DISCONNECT 就这样消失，broker
  只看到"连接被重置"（表现为测试里 `disconnect_count` 一直是 0，约 10~25% 偶发；
  真机上就是 broker 日志里的"客户端非正常断开"）。现在断开路径先把在途字节读掉
  （最多 8 KB、每次 recv 1 ms），再 FIN 收尾，broker 能正常读到干净的 DISCONNECT。
  实测：`mqtt_client` 套件由 40 次里 10 次失败 → **40/40 通过**，全套 22/22 连跑 5 轮通过。

### 平台与定时精度

- **短等待不再被 Windows 的时钟粒度拖住**（默认 ~15.6 ms）。`ncl_cond_wait_timeout()`
  与 `ncl_sleep_millis()` 对 ≤ 100 ms 的等待改走高精度计时器：优先 Win10 1803+ 的
  `CreateWaitableTimerEx`（`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`，精度 ~0.5 ms，
  计时器按线程持有并用 `WaitForMultipleObjects` 与"被唤醒"事件一起等），老系统上
  回退到 `timeBeginPeriod(1)`（~1.7 ms）。Windows 侧条件变量随之改成"计数信号量 +
  等人数"实现，语义不变（没有等待者时信号同样丢弃）。
  本机实测：`ncl_sleep_millis(1)` 由 14.1 ms → **1.56 ms**，
  `ncl_cond_wait_timeout(_, _, 1)` 由 15.5 ms → **1.56 ms**，`ncl_sleep_millis(100)`
  不变；示例的 `EdgeSersors`（1 ms 槽位 / 100 ms 上报）从 ~1.4 s 一条变成 ~222 ms
  一条（剩下的开销是每槽 12 次完整 Query）。Windows 链接多一个 `winmm`
  （MSVC 由源码里的 `#pragma comment` 自动带上，MinGW 需 `-lwinmm`；Go 绑定的 cgo
  LDFLAGS 已加）。

### 构建与发布

- `build.ps1 -Arch x86 -BuildDir build-x86`：新增 **32 位（Win32/x86）** 构建
  （库、示例、测试都是 x86，同一个 Ninja 工程换 `vcvars32` 即可）。
- 发布包新增 `lib/windows-x86-msvc/`、`examples/bin/{windows-x64-msvc,windows-x86-msvc,
  linux-x86_64-gcc}/`：**编好的示例可执行文件**随包交付，拿到即可跑（Windows 版是
  `/MD`，需要 VC++ 2015-2022 x64/x86 运行库；Linux 版需要 glibc 2.31+）。
- 包内同时带上语言绑定源码（`bindings/go`、`bindings/csharp`，都只放源码）。

### 文档

- 手册（`MANUAL.md` / `MANUAL.docx`）跟改：两个采样通道与各自的实测输出、按行消费、
  亚毫秒采样、设备端"一直运行 + Ctrl+C"、日志与编码说明。
- `RELEASE.md` 的包内清单、ABI 表与验证状态按本次实测更新。
## 3.0.0

首个 C 版本，零第三方依赖（仅可选的 zlib），Windows（MSVC）与 Linux（gcc）
双平台编译并跑通全部测试。

以 **MIT License** 授权（见 `LICENSE`），所有源文件带
`SPDX-License-Identifier: MIT` 头。

### MQTT 修复与互操作验证

- 新增 `tests/test_broker.c` 与 `tools/interop.sh`：对真实 broker（EMQX /
  Mosquitto，Docker 一键起）验证 QoS 0/1/2、通配订阅、40 KB 报文、退订、空闲
  保活、会话被顶替与重连后的订阅恢复。首次运行即发现并修掉下面两个问题。
- **修复：会话被顶替（0x8E）后不再自动重连**。此前两端都会立刻重连，导致同一
  clientId 的两个连接互相顶替、无限循环；现在把 0x8E 如实上报给回调并停止
  重连，由应用显式决定是否抢回身份（`ncl_mqtt_client_connect()`）。
- **修复：重连后的订阅恢复不再阻塞接收线程**。此前恢复订阅调用的是同步
  `subscribe()`，而 SUBACK 只能由接收线程处理，于是每恢复一个订阅就白等一个
  超时（10 s × 订阅数），期间报文不收、保活不发（会被 broker 以 0x8D 断开）。
  现在恢复订阅改为只发不等的异步路径，并在每次 connect 后恢复（显式重连不再
  静默丢订阅）。
- **修复：对端关闭连接立刻感知**。此前把"套接字被对端关闭"与"空闲超时"当成同
  一种情况，只能等保活看门狗（最多 2 × keepAlive）才发现掉线；现在 EOF/部分
  报文都会立即结束会话，不会在死连接上滞留，也不会因半截报文而错帧。
- **修复：服务器 DISCONNECT 立即结束会话**。协议规定服务器发完 DISCONNECT 就
  关连接，客户端不再继续读，直接进入断开/重连流程（0x8E 则按上面所述不重连）。
- `tests/test_mqtt_client.c` 的假 broker 改为可接受多次连接，并新增两个回归用例：
  「断开后自动重连并恢复订阅、且恢复过程不阻塞接收线程」与「服务器 0x8E 停止
  自动重连」——不装 Docker 也能挡住这两个问题。

### TLS（可选）

- 新增可选的 TLS 传输：`ncl_socket_connect_tls()` / `ncl_socket_tls_available()`
  与 MQTT 客户端上的 `ssl://`、`tls_ca_file`、`tls_verify_peer`、
  `tls_server_name`、`tls_client_cert/key`。默认构建仍然零依赖，
  用 `-DNCLINK_WITH_TLS=ON`（CMake）或 `NCL_WITH_TLS=1 ./build-linux.sh` 打开，
  链接 `-lssl -lcrypto`。校验链与主机名默认开启（IP 与 DNS 名都支持），
  可显式关掉用于自签调试。
- 新增 `tests/test_tls.c`（第 21 个套件，未启用 TLS 时自动跳过）：内置 TLS
  服务端，覆盖握手、CONNECT/SUBSCRIBE、8 KB 报文跨记录、服务端推送、陌生 CA
  与错误主机名必须失败、`verify_peer=false` 必须成功。
- `tools/interop.sh` 增加 Mosquitto 的 TLS 监听（18832），互操作套件再对
  `ssl://` 跑一遍（本机实测 44 检查全通过）。
- Windows 也可用：`.\build.ps1 -Tls`（或 `-DNCLINK_WITH_TLS=ON -DOPENSSL_ROOT_DIR=…`）
  自动定位 OpenSSL 3 并编译，两个平台的 TLS 构建均 21/21 通过；发布包附带
  `lib/windows-x64-msvc-tls/` 与 `lib/linux-x86_64-gcc-tls/`，Windows 版运行时
  需要 OpenSSL 3 的 DLL（`libssl-3-x64.dll`、`libcrypto-3-x64.dll`）。

### 协议与基础

- JSON DOM（有序对象、空值省略、忽略未知字段、数字保留原文）、
  字符串/容器、平台抽象（线程/互斥/条件变量/时间/熵）、日志、
  运行环境与路径（`conf/`、`bin/`、`log/`）、线程池与 TTL 缓存。
- NC-Link 常量与校验、全部主题构造与 `sn` 提取、18 种消息与 4 种消息项
  （字段顺序按规范固定）、完整数据模型（节点树、路径规则、采样通道绑定、
  映射表）、hex 与可选 zlib 编解码。
- MQTT 5.0：报文编解码逐字节对齐 OASIS 规范；传输层含 QoS 0/1/2、保活、
  自动重连与订阅恢复；TCP 套接字层（Winsock/BSD 双实现）。
- 日志的控制台镜像在 Windows 上按"真控制台 / 管道"分别解码（`WriteConsoleW`
  与本地 ANSI 代码页），中文不再乱码；`NCL_CONSOLE_ENCODING=utf8` 可强制按
  UTF-8 写（`ncl_console_write()`）。日志文件始终是 UTF-8。

### 客户端与服务端

- `ncl_client`：请求/响应关联、5 分钟响应缓存、getValue/getLength/setValue
  （含索引与区间）/probe/methodCall；`ncl_client_holder`：按 SN 的客户端表、
  30 分钟空闲过期、MQTT 连接与 FTP 端点生命周期。
- `ncl_server`：工具注册与 `<operation>#<path>` 绑定、Query/Set/MethodCall/Probe/Ping
  分发、线程池异步处理、采样通道（定时采集 + 聚合上报）、事件推送、
  参数 JSON Schema 校验（`check` 语义）、无 broker 的发布钩子。
- HTTP/REST：HTTP/1.1 基础层（解析、路由、应答、CORS，含前缀兜底路由）、
  统一应答封装、OpenAPI 3.0 生成、`/api/schema`、`/swagger-ui`、
  12 个配置接口，以及 `POST /api/<工具>/<方法>` 工具入口。
- 配置：SN、模型、驱动、服务器列表、`conf/mqtt.cfg`、`ipConf.json` 的读写。

### 文件传输

- 自带 FTP 服务端与客户端（RFC 959/2389 子集：主动/被动数据连接、
  STOR/RETR/LIST/MKD/RMD/DELE/RNFR/SIZE/MDTM 等），登录根隔离。
- 文件属性与工具函数（SHA-256 校验和、分片数、压缩判定）、
  FTP 文件工具两端、`file` 工具（`/CONTROLLER/FILE`）、
  methodCall 的 `@file` 标记与 `fileKeys` 替换；设备端与客户端两个 FTP 端点。

### 校验与事件

- JSON Schema 校验器（draft-07 子集，含自带正则引擎），用于 methodCall 的
  参数校验与 `check` 快速失败。
- 事件：`ncl_server_push_event()` 发布 + `ncl_client_subscribe_events()` /
  `ncl_client_set_event_handler()` 接收（主题 `Event/<sn>`）。
- 采样：客户端订阅 `Sample/<sn>/#` 与回调；**亚毫秒采样**支持"值本身是数组"
  （外层 1 ms 槽位、内层批次），报文不加字段；发布前校验**内外层都要对齐**，
  不完整即丢弃。
- 采样补齐：设备端省掉表头 `paths`、或用 `[]` 给"本周期该项没有数据"占位时，
  客户端先用设备模型补回规范形状再交给回调（`ncl_message_sample_normalise()`，
  见 4.5）；通道查不到、项数对不上、形状统一不了就原样交过去，绝不猜对应关系。

### 文档与工程

- `MANUAL.md` / `MANUAL.docx` 使用手册（构建、核心概念、逐模块 API、
  典型任务、排错表、API 索引）、`README.md` 工程说明、`RELEASE.md` 发布包说明。
- `examples/`：设备端与客户端两个可运行示例（模型、工具与 schema、采样、
  事件、文件、参数校验全覆盖）。
- 设备端示例可以指向一个**空目录**首次启动：目录不存在就建，缺什么按出厂默认值
  补齐 —— 随机 9 位 SN（`bin/sn.txt`）、默认机床模型（`conf/model/nclink.json`）、
  本机 broker 配置（`conf/mqtt.cfg`，`tcp://127.0.0.1:1883` 匿名）；已存在的
  文件一律不动，换模型/换 broker 直接改文件即可（见手册 3.4）。
- `tests/`：19 个测试套件（含假 MQTT broker 与许可头检查），Windows ctest 与
  Linux `build-linux.sh` 均 19/19；ASan 构建全绿。
- `tools/`：markdown → docx 转换与校验、发布打包脚本。

### 未实现部分

HTTP multipart、驱动层 Modbus/串口、`Edge/*` 主题暂未提供，调用时会返回明确错误码。
（`ssl://` 在本版本已提供，见 TLS 一节：需要按 `NCLINK_WITH_TLS=ON` 构建；
没带 TLS 编译时用 `ssl://` 会返回 `NCL_ERR_NOT_SUPPORTED`。）
