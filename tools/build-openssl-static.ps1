# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming
#
# 为 Windows 编一份静态 OpenSSL（no-shared no-asm），供 nclink-core-c 的
# TLS 版静态链接使用，产物无 OpenSSL DLL 依赖。
#
#   .\tools\build-openssl-static.ps1                        # 默认 3.0.20
#   .\tools\build-openssl-static.ps1 -Version 3.0.20 -Prefix D:\codex\openssl-static
#
# 依赖：`perl`（Strawberry/ActivePerl 均可，Git 自带的 Cygwin 精简版不行）、
# MSVC Build Tools（nmake 来自 vcvars64）、curl/tar（Windows 10+ 自带）。
# 装好 OpenSSL 后：
#   cmake -S . -B build-tls-static -DNCLINK_WITH_TLS=ON `
#         -DOPENSSL_USE_STATIC_LIBS=ON -DOPENSSL_ROOT_DIR=<Prefix>
#
[CmdletBinding()]
param(
    [string]$Version = "3.0.20",
    [string]$Prefix = "D:\codex\openssl-static",
    [string]$WorkDir = "D:\codex\openssl-src",
    [string]$PerlDir = "",
    [switch]$Reconfigure
)

$ErrorActionPreference = "Stop"
$vcvars = Join-Path ${env:ProgramFiles(x86)} `
    "Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
$src = Join-Path $WorkDir "openssl-$Version"
$tarball = Join-Path $WorkDir "openssl-$Version.tar.gz"

if (-not (Test-Path -LiteralPath $vcvars)) { throw "vcvars64.bat not found: $vcvars" }
if ($PerlDir -eq "") {
    foreach ($candidate in @("D:\codex\strawberry-perl\perl\bin",
                             "C:\Strawberry\perl\bin")) {
        if (Test-Path -LiteralPath (Join-Path $candidate "perl.exe")) {
            $PerlDir = $candidate
            break
        }
    }
}
if ($PerlDir -eq "") { throw "no full perl found: install Strawberry Perl or pass -PerlDir" }

New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
if (-not (Test-Path -LiteralPath $tarball)) {
    $url = "https://github.com/openssl/openssl/releases/download/openssl-$Version/openssl-$Version.tar.gz"
    Write-Host "downloading $url"
    & "$env:SystemRoot\System32\curl.exe" -sSL --proxy $env:HTTPS_PROXY -o $tarball $url
}
if (-not (Test-Path -LiteralPath $src)) {
    & "$env:SystemRoot\System32\tar.exe" -xzf $tarball -C $WorkDir
}

$flags = "VC-WIN64A no-shared no-asm no-tests --prefix=$Prefix --openssldir=$Prefix\ssl"
$log = Join-Path $WorkDir "build-$Version.log"
$bat = Join-Path $env:TEMP "ncl-openssl-static.bat"
$reconfigure = if ($Reconfigure -or -not (Test-Path (Join-Path $src "Makefile"))) { "1" } else { "0" }
@"
@echo off
set PATH=$PerlDir;%PATH%
call "$vcvars" >nul
cd /d "$src"
if "$reconfigure"=="1" perl Configure $flags || exit /b 1
nmake > "$log" 2>&1 || exit /b 1
nmake install_sw >> "$log" 2>&1 || exit /b 1
echo done
"@ | Set-Content -LiteralPath $bat -Encoding ASCII

& cmd.exe /c $bat
if ($LASTEXITCODE -ne 0) { throw "OpenSSL build failed, see $log" }
if (-not (Test-Path (Join-Path $Prefix "lib\libssl.lib"))) {
    throw "libssl.lib not found under $Prefix\lib"
}
Write-Host "static OpenSSL installed: $Prefix (log: $log)"
