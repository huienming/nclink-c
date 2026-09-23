# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 编译并运行 focas_sweep：拿**这一份 client**（不是官方 SDK）接一台真 FOCAS 服务端 /
# 模拟器，把 focas.h 里的公开接口挨个问一遍，逐条打 rc + last_error（表/数组连"拿到
# 几条"一起打）。盘"这套还有哪些没通"用 —— 01 册 §11.11.1 那张现状表就是它跑出来的。
#
#   .\focas_sweep.ps1 127.0.0.1                 # 本地模拟器（NAT 转发到 8193）
#   .\focas_sweep.ps1 192.168.110.192 -Port 8193
#
# 读法：**只看 rc != 0 的行**（last_error 是黏的，rc=0 的行里可能带着上一次的错话）。
# 要逐条看某个 item 的原始应答用 .\focas_item.ps1。
#
# 需要先把静态库编出来（仓库根目录：.\build.ps1）。
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Machine,
    [int]$Port = 8193,
    [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$root = Split-Path -Parent (Split-Path -Parent $here)
$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

$core = Join-Path $root "$BuildDir\Release\nclink_core.lib"
$clients = Join-Path $root "$BuildDir\clients\Release\nclink_clients.lib"
foreach ($lib in @($core, $clients)) {
    if (-not (Test-Path $lib)) {
        throw "静态库不在：$lib —— 先在仓库根目录跑 .\build.ps1"
    }
}

$work = Join-Path $env:TEMP "focas-sweep"
New-Item -ItemType Directory -Force -Path $work | Out-Null
$exe = Join-Path $work "focas_sweep.exe"

Write-Host "== 编译 focas_sweep"
$cmd = "call `"$vcvars`" >nul 2>&1 && cd /d `"$work`" && " +
       "cl /nologo /W4 /utf-8 /O2 /MD /I`"$root\include`" /I`"$root\clients\include`" " +
       "`"$here\focas_sweep.c`" /Fe:focas_sweep.exe " +
       "/link `"$clients`" `"$core`" ws2_32.lib"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "focas_sweep 编译失败" }

Write-Host "== 跑 $Machine`:$Port"
& $exe $Machine "$Port"
exit $LASTEXITCODE
