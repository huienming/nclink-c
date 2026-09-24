# 适配器包 · 现场说明

`ncl_server` 是唯一的设备程序（宿主）：一台 NC-Link 服务端 + REST + 轮询调度。**机床协议不在
程序里**，而是启动时从 `plugins\` 装载的驱动模块；**装哪个驱动由配置说**（第 2 节）。

```
机床 ──厂商协议──▶ plugins\ncl_driver_<驱动>.dll ──▶ ncl_server
                                                      ├── 采样上报（MQTT）
                                                      ├── 请求应答（MQTT：Query / Set / Method / Ping）
                                                      └── HTTP + Swagger（/swagger-ui、/api/*）
```

版本 **3.6.0**（配合 NC-Link 协议 3.0.0）。

---

## 1. 包内清单

```
bin/ncl_server.exe            设备程序（宿主）：一台 NC-Link 服务端 + REST + 轮询调度；
                              **文件工具（/CONTROLLER/FILE）也编在这里面**，它不是模块
plugins/ncl_driver_focas.dll  FANUC 驱动（FOCAS over TCP）
plugins/ncl_driver_syntec.dll 新代驱动（Syntec RemoteCNC over TCP）
plugins/ncl_driver_pseudo.dll 伪机床驱动（内置模拟器，不接硬件；点位模型照新代摆）
conf/fanuc.json               配置样例：装载 focas，机床 192.168.1.100:8193
conf/syntec.json              配置样例：装载 syntec，机床 192.168.1.100:8000
conf/pseudo.json              配置样例：装载 pseudo，不接任何机床也能跑通整条链路
conf/device.json              **不带 -c 时的默认配置**（= pseudo.json 的副本）：裸跑
                              bin\ncl_server.exe 或 .\run-once.ps1 用的就是它
conf/mqtt.cfg                 MQTT broker 配置样例
docs/*-ADAPTER.md             各驱动自己的现场手册（点位、参数、注意事项）
run-once.ps1                  自检：轮询一遍全部点位，打印到屏幕并写日志
run.ps1                       常驻运行（Ctrl+C 退出）
list-plugins.ps1              这次会装载哪个驱动（`-Config` 换一份配置，`-All` 列目录里全部）
README.md                     本文件
LICENSE                       MIT 许可全文
SHA256SUMS.txt                包内每个文件的 SHA-256
```

**点位表（模型路径 → 机床数据项）声明在驱动模块里**，配置只管机床地址、采样周期、broker 与
要装载哪个驱动。加/改一个点位＝改模块的那个 `.c` 文件并重编模块；采样周期与上报周期在生成的
模型文件里，现场可调不用编译（`bin\ncl_server.exe --model` 打印当前模型，存成文件后用配置里的
`"model"` 指过去）。

## 2. 装哪个驱动：配置里的 `plugins` 与 `tools`

```json
{
  "plugins": ["focas"],
  "tools": [
    {
      "name": "focas",
      "parameters": { "host": "192.168.1.100", "port": 8193,
                      "timeoutMs": 3000, "connectTimeoutMs": 3000,
                      "retries": 0, "negotiate": true }
    }
  ]
}
```

| 键 | 作用 | 说明 |
|----|------|------|
| `plugins` | **装载哪几个驱动模块** | 写驱动名（就是 `tools[].name` 那个名字）：`focas` → `plugins\ncl_driver_focas.dll`，`syntec` → `plugins\ncl_driver_syntec.dll`，`pseudo` → `plugins\ncl_driver_pseudo.dll`。装载器自己补前缀与后缀，也可以直接写文件名（`ncl_driver_focas.dll`）。写成对象时是 `plugins.load`（数组）+ `plugins.dir` + `plugins.auto`；**不写这一项 = 把 `plugins\` 里的模块全部装载**（本包三个驱动都会进来） |
| `tools[].name` | 这台设备用哪个驱动 | 名字必须是已装载模块声明的名字，否则启动直接报"工具 xxx 未装载：plugins 里没有 ncl_driver_xxx.dll"，不会悄悄跑起来 |
| `tools[].parameters` | 驱动自己的参数 | 各驱动支持哪些见它的现场手册：host / port / timeoutMs / connectTimeoutMs / retries … |
| `plugins.dir` | 模块目录 | 不写就是包里的 `plugins\`（`run.ps1` 会带 `-P <包根>\plugins`）；要放别处就写绝对路径 |

> **配置怎么找**：不带 `-c` 时读 `<root>\conf\device.json`；`<root>` 没给 `-r` 就自动挑一个
> —— 当前目录、它的上一级、上两级里**第一个带 `conf\` 或 `plugins\` 的目录**（包里双击
> `bin\ncl_server.exe` 也能定位到包根）。**包里已经放了这份默认配置**（= 伪机床那份，
> 所以开箱就能跑起来、不接机床也有数据）；装了真机之后改这一份就行，或者用 `-c` 指别的那几份
> 样例（`conf\fanuc.json` / `conf\syntec.json` / `conf\pseudo.json`）。指错了也不会只说一句
> "cannot read"：报错里会写清用了哪个根目录、`conf\` 里现成有哪些配置、以及可以直接抄的命令。
>
> 升级包时**别直接盖掉 `conf\device.json`** —— 那里面是你的站点配置；要么先把文件拷出来，
> 要么把站点配置放在包外、用 `run.ps1 -Config <路径>` 指过去。

换机床只换配置：`.\run.ps1 -Config conf\syntec.json`。要加第三个厂商的驱动，把编译好的
`ncl_driver_<工具>.dll` 放进 `plugins\`、在 `plugins` 里加上它的名字即可，宿主不用换。

### 怎么确认"现在装的是哪个驱动"

四处看得到，都是同一件事：

| 在哪看 | 看到什么 |
|--------|----------|
| 配置文件 | 你 `-c` 的那一份（不带就是 `conf\device.json`，包里默认 = 伪机床）：`"plugins": ["focas"]` 就是 focas |
| 屏幕 / `log\out.txt` | `已注册模块 …\plugins\ncl_driver_focas.dll（工具 "focas" 1.5.1，50 个点位 + 4 个方法…）`，紧接着 `设备 <SN> 已启动，工具 "focas" 提供 50 个点位（模型来自模块）` |
| `.\list-plugins.ps1` | 就是把上面那两行打给你看：不带参数按 `conf\device.json` 走，`-Config conf\syntec.json` 看另一份，`-All` 列 `plugins\` 里**全部**驱动（不看配置） |
| 上位机拿到的模型 | 模型里那条 `SAMPLE_CHANNEL` 的 `id` 就是驱动名（`focas` / `syntec`），`name` 里带协议描述；`bin\ncl_server.exe --model`、probe 拿到的模型里直接能搜到 |

一句话：**配置里的 `plugins` 选择装载哪个，日志里 `工具 "x"` 就是实际在服务的那一个**；
`--plugins` 只看"这个盒子能说什么"，`工具 "x"` 才是"这次真的在说哪个"。

## 3. 跑起来

```powershell
.\list-plugins.ps1                                    # 这次会装载哪个驱动（按 conf\device.json）
.\list-plugins.ps1 -Config conf\syntec.json           # 按另一份配置看
.\list-plugins.ps1 -All                               # 这个盒子里有哪些驱动（不看配置）
.\run-once.ps1                                        # 自检：轮询一遍点位（默认配置，不需要 broker）
.\run-once.ps1 -Config conf\fanuc.json                # 换一份样例（真机床那份）
.\run-once.ps1 -Config conf\fanuc.json -Raw           # 带上每次请求的抓帧审计
.\run.ps1 -Broker tcp://10.0.0.9:1883                 # 常驻跑；需要时再 -Config
```

`run.ps1` / `run-once.ps1` 把参数原样转给 `ncl_server`：`-Config`（默认 `conf\device.json`）、
`-Broker`、`-Interval`、`-RestPort`、`-PluginDir`、`-Raw`。完整选项见
`bin\ncl_server.exe --help`；自检的退出码 = 读不到的点位数，可以直接给现场脚本判"通没通"。

**手上还没有机床？**默认那份 `conf\device.json` 装的就是伪机床（pseudo）：自带模拟器、点位
模型照新代摆，不接任何硬件也能把整条链路跑通（自检 72/72）—— `.\run-once.ps1` 直接就是它。
它有哪些点位、怎么用 `pseudo/SESSION` 把这台假机床遥控到 RUN / HOLD / ALARM / 慢响应、怎么
注入失败，见 `docs\PSEUDO-ADAPTER.md`。

## 4. 校验与许可

```powershell
(Get-FileHash .\bin\ncl_server.exe -Algorithm SHA256).Hash   # 与 SHA256SUMS.txt 对照
```

包内每个文件的 SHA-256 在 `SHA256SUMS.txt`（Windows 用 `Get-FileHash`，Linux/msys 用
`sha256sum -c SHA256SUMS.txt`）。本包以 **MIT License** 授权，全文见 `LICENSE`。
