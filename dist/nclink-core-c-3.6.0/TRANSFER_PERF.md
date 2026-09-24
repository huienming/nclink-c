# 文件通道传输效率报告

本报告是 `nclink-core-c` 3.4.0 文件通道（MQTT 传令牌 + FTP 传字节）的实测数据，
复现方式、环境与口径都写在下面，**数字是实跑出来的，不是估算**。

## 1. 被测对象与口径

| 项 | 说明 |
|----|------|
| 传输实现 | `ncl_ftp_client_upload()` / `ncl_ftp_client_download()`：**流式**（256 KiB 一块读、64 KiB 一块写，内存不随文件大小增长）、**可续传**（上传按对端 SIZE 用 APPE 续、下载按本地大小用 REST 续），单次调用内自带 3 次重试，每次重试都从断点继续 |
| 校验 | 客户端工具在每次 write/read 前会先比一次 SHA-256（设备端属性 vs 本地镜像），一致就跳过传输；本报告的"校验"列是传输后用 SHA-256 逐字节核对 |
| 计时口径 | **`ncl_client_write()` / `ncl_client_read()` 的墙钟时间**，包含握手后的协议往返、设备端落盘、以及上面那次 SHA-256 比对；线上字节数取 FTP 端点的计数（`ncl_ftp_server_bytes_sent/received()`，实测与"字节数 × 1"完全相等，说明没有重复传输） |
| 测试台 | `examples/device/c/ncl_file_bench.c`（`device` / `client` 两个角色），载荷是按位置生成的确定性字节流 |
| "resume (half)" 行 | 先把接收侧的镜像写成前一半，再发起传输：线上字节数应恰为后一半——实测每行都精确等于一半 |

## 2. 环境

| 场景 | 拓扑 |
|------|------|
| 同机 | Windows 10 x64（MSVC 14.44.35207，Release）跑 `ncl_file_bench` 的设备端与客户端**两个进程**，broker = EMQX 5.x（Docker，127.0.0.1:1883），FTP 走回环（主动模式） |
| 跨主机 | **两个独立容器**（`gcc:13`，同一个 Docker 桥接网络 `nclbench`）：`ncl-client` 跑客户端（FTP 服务端）、`ncl-device` 跑设备端，broker = `emqx:1883`；设备按握手里给的 `ncl-client:2323` 回拨，**被动模式**（PASV）。两端的工作目录都在容器内 `/tmp`（tmpfs） |

## 3. 同机（Windows，回环，主动模式）

| 文件 | 上传（设备拉） | 线上 | 下载（设备推） | 线上 | 续传（半文件） | 线上 | 校验 |
|------|----------------|------|----------------|------|----------------|------|------|
| 16 MiB | 0.125 s / **128.0 MiB/s** | 16 MiB | 0.141 s / **113.5 MiB/s** | 16 MiB | 0.187 s / 42.8 MiB/s | 8 MiB | 逐字节一致 |
| 64 MiB | 0.125 s / **512.0 MiB/s** | 64 MiB | 0.562 s / **113.9 MiB/s** | 64 MiB | 0.640 s / 50.0 MiB/s | 32 MiB | 逐字节一致 |
| 256 MiB | 0.625 s / **409.6 MiB/s** | 256 MiB | 1.828 s / **140.0 MiB/s** | 256 MiB | 3.047 s / 42.0 MiB/s | 128 MiB | 逐字节一致 |
| 512 MiB | 1.297 s / **394.8 MiB/s** | 512 MiB | 4.828 s / **106.1 MiB/s** | 512 MiB | 5.219 s / 49.1 MiB/s | 256 MiB | 逐字节一致 |

## 4. 跨主机（客户端容器 ←→ 设备容器，Docker 桥接，被动模式）

| 文件 | 上传（设备拉） | 下载（设备推） | 续传（半文件） | 校验 |
|------|----------------|----------------|----------------|------|
| 16 MiB | 0.019 s / **842.1 MiB/s** | 0.103 s / **155.3 MiB/s** | 0.155 s / 51.6 MiB/s | 逐字节一致 |
| 64 MiB | 0.071 s / **901.4 MiB/s** | 0.452 s / **141.6 MiB/s** | 0.614 s / 52.1 MiB/s | 逐字节一致 |
| 256 MiB | 0.327 s / **782.9 MiB/s** | 1.819 s / **140.7 MiB/s** | 2.578 s / 49.7 MiB/s | 逐字节一致 |
| 512 MiB | 0.651 s / **786.5 MiB/s** | 3.760 s / **136.2 MiB/s** | 5.195 s / 49.3 MiB/s | 逐字节一致 |

> 跨主机这一列**不是回环**：两端在不同的网络命名空间里，控制面真的过 EMQX，
> 字节真的走 TCP。容器里两端都是 tmpfs，所以绝对速率偏高；换成真机 + 磁盘会降到
> 磁盘和网卡的带宽。

## 5. 结论与解释

1. **方向不对称，约 3~6 倍**：上传（设备**拉**）明显快于下载（设备**推**）。
   拉的方向上接收端只做顺序写；推的方向上还要走"设备读本地 → 发 → 上位机 FTP
   服务端写盘并在结束时按实际长度截断"这条链，且 `STOR` 的完成应答要等对端把数据
   收完，所以单次往返更"重"。跨主机上这个差距更大（786 vs 136 MiB/s）。
2. **续传行的速率最低（42~52 MiB/s），但代价是"半份"**：续传本身只传剩余的
   一半（线上字节数实测正好是一半），慢的部分是那次 SHA-256 比对——`write/read`
   开头就比一次全文件校验和，续传场景下两端各算一遍。想更快可以在应用层用
   `ncl_client_method_call_file` 跳过比对，或接受"少传一半、多算一次哈希"。
3. **内存与文件大小无关**：512 MiB 的往返全程只有 256 KiB 读缓冲 + 64 KiB 写缓冲；
   64 MiB 的文件在 **20 MiB 静态池**（`-StaticMem`，库内不调用 `malloc`）的构建里
   同样 25/25 通过。旧实现是"整文件读进内存再传"，那份构建跑不了这个尺寸。
4. **断点续传是真的**：`file` 套件里有一条用例把数据连接在第 3 MiB 处掐断
   （`NCL_TEST_FTP_ABORT_AFTER`），断言传输仍然成功、校验一致、线上字节数
   ≥ 文件大小且 < 2×（重试从断点继续，而不是重传）；另两条用例覆盖"对端已有前半"
   与"本地已有前半"两种续传起点。
5. **跨主机要被动模式**：设备在容器/NAT 后面时，主动模式（PORT）要求 FTP 服务端
   反向连回设备，通常不可达；握手里加 `passive: true`（C API
   `ncl_file_channel_options.passive`、`conf/ftp.txt` 的 `"passive": true`）后设备改
   用 PASV 主动外连，本报告的跨主机数据就是这么跑出来的。

## 6. 复现

```bash
# 设备端（另一台机器 / 容器；被动模式下它只需能连到 broker 与客户端的 2323）
./ncl_file_bench device tcp://broker:1883 V2BENCH0001 300

# 客户端（上位机；host/port 是"设备要拨回来的地址"，跨网段就填对端能访问到的地址）
./ncl_file_bench client tcp://broker:1883 V2BENCH0001 512 10.0.0.7 2323 passive
```

同机复现就是把两边都跑在本机（`host` 留空即可）。容器版（本报告第 4 节）：

```bash
docker network create nclbench && docker network connect nclbench emqx
docker run -d --name ncl-device --network nclbench -v <repo>:/work:ro -w /tmp gcc:13 \
    sh -c "mkdir -p /tmp/dev && cd /tmp/dev && NCL_DEVICE_ROOT=/tmp/dev \
           /work/builds/build-linux/bin/ncl_file_bench device tcp://emqx:1883 V2BENCH0002 600"
docker run --rm --name ncl-client --hostname ncl-client --network nclbench \
    -v <repo>:/work:ro -w /tmp gcc:13 \
    sh -c "mkdir -p /tmp/cli && cd /tmp/cli && /work/builds/build-linux/bin/ncl_file_bench \
           client tcp://emqx:1883 V2BENCH0002 512 ncl-client 2323 passive"
```

## 7. 没测到的

- **真实广域网**：只有回环与容器桥接两个口径；带丢包/高 RTT 的链路没测（续传逻辑
  正是为它写的，但需要 tc/netem 或真链路才能给数）。
- **磁盘 I/O 受限**：两端都是内存（回环页缓存 / tmpfs），所以数字是"协议 + 拷贝"
  的上限，不是磁盘带宽。
- **FTPS**：本库 FTP 不带 TLS，字节在网线上是明文。
- **多通道并发**：报告是单通道串行口径；同一进程多设备并发时的相互影响没测。
