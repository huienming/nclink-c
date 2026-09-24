# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 编译并运行 focas_item：**裸读一个 item**，把请求的 d/e/arg2/arg3 直接指定，打印 rc、
# 应答长度与原始字节。用来核"这个 item 要发什么参数、机床回多少"（01 册 §11.11.1 里
# 几条口径就是这么核的：ACTF 只回一条记录、RDPARAM 的号要 d=e=号、宏变量 500/501 是
# vacant …）。
#
#   .\focas_item.ps1 127.0.0.1 RDPARAM 6711 6711
#   .\focas_item.ps1 127.0.0.1 ACTF 0 3
#   .\focas_item.ps1 192.168.110.192 RDMACRO 100 100
#
# 需要先把静态库编出来（仓库根目录：.\build.ps1）。
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Machine,
    [Parameter(Mandatory = $true, Position = 1)][string]$Item,
    [Parameter(Position = 2)][string]$D = "",
    [Parameter(Position = 3)][string]$E = "",
    [int]$Port = 8193,
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

$work = Join-Path $env:TEMP "focas-item"
New-Item -ItemType Directory -Force -Path $work | Out-Null
$exe = Join-Path $work "focas_item.exe"

Write-Host "== 编译 focas_item"
$cmd = "call `"$vcvars`" >nul 2>&1 && cd /d `"$work`" && " +
       "cl /nologo /W4 /utf-8 /O2 /MD /I`"$root\include`" /I`"$root\clients\include`" " +
       "`"$here\focas_item.c`" /Fe:focas_item.exe " +
       "/link `"$clients`" `"$core`" ws2_32.lib"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "focas_item 编译失败" }

$probeArgs = @($Machine, "$Port", $Item)
if ($D -ne "") { $probeArgs += $D }
if ($E -ne "") { $probeArgs += $E }

Write-Host "== 跑 $Machine`:$Port $Item"
& $exe @probeArgs
exit $LASTEXITCODE
