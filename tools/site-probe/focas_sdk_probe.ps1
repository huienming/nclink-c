# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# 把 FANUC 官方 FOCAS SDK（Fwlib64.dll）接到假机床上，一条调用一条进程地跑，
# 打印"SDK 发了什么"和"SDK 解出了什么"。用途：把某个 cnc_* 调用的线上帧（Cb 码 +
# 应答块布局）和出参结构布局核出来，好写进 client 的 item 表。
#
#   .\focas_sdk_probe.ps1 -Dll "D:\Fwlib64" -Calls "cnc_statinfo","cnc_absolute 1 0"
#
# -Dll 指 Fwlib64.dll 所在目录（同目录还要有 fwlib30i64.dll 等按机型的实现，
# 否则 SDK 回 -15 EW_NODLL）。假机床见 focas_sdk_mock.py。
param(
    [Parameter(Mandatory = $true)][string]$Dll,
    [string[]]$Calls = @("cnc_statinfo", "cnc_absolute 1 0"),
    [int]$Port = 8193,
    [string]$BlockSize = "0x40",
    [string]$Payload = "",
    [string]$Work = "$env:TEMP\focas-probe",
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

New-Item -ItemType Directory -Force -Path $Work | Out-Null
Copy-Item -LiteralPath (Join-Path $here "focas_sdk_probe.c") -Destination $Work -Force
Copy-Item -LiteralPath (Join-Path $here "focas_sdk_mock.py") -Destination $Work -Force
Copy-Item -Path (Join-Path $Dll "*.dll") -Destination $Work -Force

if (-not $NoBuild -or -not (Test-Path (Join-Path $Work "focas_sdk_probe.exe"))) {
    Write-Host "== 编译探针"
    cmd /c "call `"$vcvars`" >nul 2>&1 && cd /d `"$Work`" && cl /nologo /W4 /utf-8 /O2 focas_sdk_probe.c /Fe:focas_sdk_probe.exe"
    if ($LASTEXITCODE -ne 0) { throw "探针编译失败" }
}

$extra = @()
if ($Payload) { $extra += @("--payload", $Payload) } else { $extra += @("--ramp") }

foreach ($call in $Calls) {
    $parts = $call -split "\s+"
    $name = $parts[0]
    $args = @($parts | Select-Object -Skip 1)
    $log = Join-Path $Work ("mock-" + $name + ".log")
    Write-Host ("== " + $call)

    $mock = Start-Process -FilePath "python" `
        -ArgumentList (@("-u", (Join-Path $Work "focas_sdk_mock.py"), "$Port",
                         "--size", $BlockSize) + $extra) `
        -WindowStyle Hidden -RedirectStandardOutput $log `
        -RedirectStandardError (Join-Path $Work "mock.err") -PassThru
    Start-Sleep -Milliseconds 1200
    try {
        & (Join-Path $Work "focas_sdk_probe.exe") 127.0.0.1 $Port $name @args
    } finally {
        Start-Sleep -Milliseconds 300
        Stop-Process -Id $mock.Id -ErrorAction SilentlyContinue
    }

    Write-Host "  ---- SDK 发了什么（Cb 码与参数）"
    Select-String -Path $log -Pattern "^    Cb " -Encoding UTF8 |
        ForEach-Object { "  " + $_.Line.Trim() }
    Write-Host "  ---- 请求帧"
    Get-Content $log -Encoding UTF8 |
        Select-String -Pattern "^ 0" -Context 0,0 | ForEach-Object { "  " + $_.Line }
}
