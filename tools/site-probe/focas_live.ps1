# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 编译并运行 focas_live：拿**这一份 client**（不是官方 SDK）去接一台真 FOCAS 服务端，
# 逐条把语义接口读一遍。假机床（clients/tests/test_focas.c 里那台）过了只说明自洽，
# 这个过得了才说明 01 册 §2.2/§2.3 是照真机抄的。
#
#   .\focas_live.ps1 192.168.110.192              # 全部条目
#   .\focas_live.ps1 192.168.110.192 -Raw         # 每条的收发报文一起打
#   .\focas_live.ps1 192.168.110.192 -SessionOnly # 只看握手
#
# 需要先把静态库编出来（仓库根目录：.\build.ps1）。
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Machine,
    [int]$Port = 8193,
    [switch]$Raw,
    [switch]$SessionOnly,
    [string]$BuildDir = "builds/build"
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

$work = Join-Path $env:TEMP "focas-live"
New-Item -ItemType Directory -Force -Path $work | Out-Null
$exe = Join-Path $work "focas_live.exe"

Write-Host "== 编译 focas_live"
# /MD 要跟静态库那边一致（cl 的默认是 /MT，混起来会在链接期报一堆 __imp_ 找不到）
$cmd = "call `"$vcvars`" >nul 2>&1 && cd /d `"$work`" && " +
       "cl /nologo /W4 /utf-8 /O2 /MD /I`"$root\include`" /I`"$root\clients\include`" " +
       "`"$here\focas_live.c`" /Fe:focas_live.exe " +
       "/link `"$clients`" `"$core`" ws2_32.lib"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "focas_live 编译失败" }

$probeArgs = @($Machine, "$Port")
if ($Raw) { $probeArgs += "--raw" }
if ($SessionOnly) { $probeArgs += "--session" }

Write-Host "== 跑 $Machine`:$Port"
& $exe @probeArgs
exit $LASTEXITCODE
