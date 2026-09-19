# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# Configure and build nclink-core-c.
#
#   .\build.ps1              # configure + build + run tests (MSVC x64)
#   .\build.ps1 -Clean       # wipe the build directory first
#
# 注：Ninja 靠 rules.ninja 里的 msvc_deps_prefix 认 /showIncludes 的输出。老
# 生成目录里这个前缀可能被双重编码，导致"改了头文件却不重编"（症状就是莫名其妙的
# 崩溃），此时用 -Clean 重配一次即可。
#   .\build.ps1 -Arch x86 -BuildDir build-x86   # 32-bit (Win32/x86) build
#   .\build.ps1 -Target test # build and run ctest only
#   .\build.ps1 -Tls         # also enable MQTT over ssl:// (needs OpenSSL)
#   .\build.ps1 -StaticMem -MemPoolBytes 65536  # no heap: one 64 KiB pool
#
[CmdletBinding()]
param(
    [string]$Configuration = "Release",
    [ValidateSet("x64", "x86")]
    [string]$Arch = "x64",
    [switch]$Clean,
    [string]$BuildDir = "build",
    [switch]$NoTest,
    [switch]$Tls,
    [switch]$StaticMem,
    [string]$MemPoolBytes = "",
    [string]$MemClassBytes = "",
    [switch]$MemReport,
    [switch]$MemFirstFit,
    [string]$OpenSslRoot = ""
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$build = Join-Path $root $BuildDir

if ($Clean -and (Test-Path -LiteralPath $build)) {
    Write-Host "removing $build"
    Remove-Item -LiteralPath $build -Recurse -Force
}

$cmake = Join-Path ${env:ProgramFiles(x86)} `
    "Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path -LiteralPath $cmake)) {
    $found = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $found) { throw "cmake not found" }
    $cmake = $found.Source
}

$ninja = Join-Path ${env:ProgramFiles(x86)} `
    "Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
# vcvars32 gives the Win32 (x86) toolchain, vcvars64 the x64 one. With the Ninja
# generator the target architecture follows the compiler in the environment, so
# switching arch is just switching this batch file.
if ($Arch -eq "x86") {
    $vcvars = Join-Path ${env:ProgramFiles(x86)} `
        "Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
} else {
    $vcvars = Join-Path ${env:ProgramFiles(x86)} `
        "Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
}
if (-not (Test-Path -LiteralPath $vcvars)) { throw "vcvars not found for arch $Arch : $vcvars" }

function Invoke-VsEnv([string]$command) {
    $script = "@echo off`r`ncall `"$vcvars`" >nul`r`n$command"
    $tmp = Join-Path $env:TEMP ("ncl-build-" + [guid]::NewGuid().ToString() + ".bat")
    Set-Content -LiteralPath $tmp -Value $script -Encoding ASCII
    try {
        & cmd /c $tmp
        if ($LASTEXITCODE -ne 0) { throw "command failed ($LASTEXITCODE): $command" }
    } finally {
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    }
}

if (Test-Path -LiteralPath $ninja) {
    $generator = "Ninja"
    $env:PATH = (Split-Path -Parent $ninja) + ";" + $env:PATH
} else {
    $generator = "NMake Makefiles"
}

Write-Host "cmake:     $cmake"
Write-Host "generator: $generator"
Write-Host "arch:      $Arch"
Write-Host "config:    $Configuration"

$configure = "`"$cmake`" -S `"$root`" -B `"$build`" -G `"$generator`" -DCMAKE_BUILD_TYPE=$Configuration"
if ($Tls) {
    if ($Arch -eq "x86" -and $OpenSslRoot -eq "") {
        # A 32-bit OpenSSL static lib is uncommon (anaconda ships x64); pass
        # -OpenSslRoot <dir> explicitly when building TLS for x86.
        Write-Host "note: TLS + x86 needs a 32-bit OpenSSL static lib, pass -OpenSslRoot <dir>"
    }
    if ($OpenSslRoot -eq "") {
        foreach ($candidate in @($env:OPENSSL_ROOT_DIR,
                                "C:\ProgramData\anaconda3\Library",
                                "C:\OpenSSL-Win64", "C:\Program Files\OpenSSL-Win64",
                                "C:\vcpkg\installed\x64-windows")) {
            if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate "include\openssl\ssl.h"))) {
                $OpenSslRoot = $candidate
                break
            }
        }
    }
    if ($OpenSslRoot -eq "") {
        throw "OpenSSL not found: install it, set OPENSSL_ROOT_DIR, or pass -OpenSslRoot <dir>"
    }
    Write-Host "TLS enabled, OpenSSL root: $OpenSslRoot"
    $configure += " -DNCLINK_WITH_TLS=ON -DOPENSSL_ROOT_DIR=`"$OpenSslRoot`""
}
if ($StaticMem) {
    $poolBytes = if ($MemPoolBytes -ne "") { $MemPoolBytes } else { "20971520" }
    Write-Host "allocation: static pool, $poolBytes bytes"
    $configure += " -DNCLINK_STATIC_MEM=ON -DNCLINK_MEM_POOL_BYTES=$poolBytes"
    if ($MemReport) {
        $configure += " -DNCLINK_MEM_REPORT=ON"
    }
    if ($MemClassBytes -ne "") {
        $configure += " -DNCLINK_MEM_CLASS_BYTES=$MemClassBytes"
    }
    if ($MemFirstFit) {
        $configure += " -DNCLINK_MEM_FIRST_FIT=ON"
    }
}
Invoke-VsEnv $configure
Invoke-VsEnv "`"$cmake`" --build `"$build`""

if (-not $NoTest) {
    Invoke-VsEnv "`"$cmake`" --build `"$build`" --target test"
}

Write-Host "build complete: $build"
