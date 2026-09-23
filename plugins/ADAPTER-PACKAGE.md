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
conf/fanuc.json               配置样例：装载 focas，机床 192.168.1.100:8193
conf/syntec.json              配置样例：装载 syntec，机床 192.168.1.100:8000
conf/mqtt.cfg                 MQTT broker 配置样例
docs/*-ADAPTER.md             各驱动自己的现场手册（点位、参数、注意事项）
run-once.ps1                  自检：轮询一遍全部点位，打印到屏幕并写日志
run.ps1                       常驻运行（Ctrl+C 退出）
list-plugins.ps1              列出装载到的驱动（排查"模块没装上"）
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
| `plugins` | **装载哪几个驱动模块** | 写驱动名（就是 `tools[].name` 那个名字）：`focas` → `plugins\ncl_driver_focas.dll`，`syntec` → `plugins\ncl_driver_syntec.dll`。装载器自己补前缀与后缀，也可以直接写文件名（`ncl_driver_focas.dll`）。写成对象时是 `plugins.load`（数组）+ `plugins.dir` + `plugins.auto`；**不写这一项 = 把 `plugins\` 里的模块全部装载**（本包两个驱动都会进来） |
| `tools[].name` | 这台设备用哪个驱动 | 名字必须是已装载模块声明的名字，否则启动直接报"工具 xxx 未装载：plugins 里没有 ncl_driver_xxx.dll"，不会悄悄跑起来 |
| `tools[].parameters` | 驱动自己的参数 | 各驱动支持哪些见它的现场手册：host / port / timeoutMs / connectTimeoutMs / retries … |
| `plugins.dir` | 模块目录 | 不写就是包里的 `plugins\`（`run.ps1` 会带 `-P <包根>\plugins`）；要放别处就写绝对路径 |

换机床只换配置：`.\run.ps1 -Config conf\syntec.json`。要加第三个厂商的驱动，把编译好的
`ncl_driver_<工具>.dll` 放进 `plugins\`、在 `plugins` 里加上它的名字即可，宿主不用换。

## 3. 跑起来

```powershell
.\list-plugins.ps1                                    # 驱动装载到了没（模块 + 声明的协议）
.\run-once.ps1 -Config conf\fanuc.json                # 自检：轮询一遍点位，不需要 broker
.\run-once.ps1 -Config conf\fanuc.json -Raw           # 带上每次请求的抓帧审计
.\run.ps1 -Config conf\fanuc.json -Broker tcp://10.0.0.9:1883
```

`run.ps1` / `run-once.ps1` 把参数原样转给 `ncl_server`：`-Config`（默认 `conf\fanuc.json`）、
`-Broker`、`-Interval`、`-RestPort`、`-PluginDir`、`-Raw`。完整选项见
`bin\ncl_server.exe --help`；自检的退出码 = 读不到的点位数，可以直接给现场脚本判"通没通"。

## 4. 校验与许可

```powershell
(Get-FileHash .\bin\ncl_server.exe -Algorithm SHA256).Hash   # 与 SHA256SUMS.txt 对照
```

包内每个文件的 SHA-256 在 `SHA256SUMS.txt`（Windows 用 `Get-FileHash`，Linux/msys 用
`sha256sum -c SHA256SUMS.txt`）。本包以 **MIT License** 授权，全文见 `LICENSE`。
