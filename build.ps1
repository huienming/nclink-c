# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# Configure and build nclink-core-c.
#
#   .\build.ps1              # configure + build + run tests (MSVC)
#   .\build.ps1 -Clean       # wipe the build directory first
#   .\build.ps1 -Target test # build and run ctest only
#   .\build.ps1 -Tls         # also enable MQTT over ssl:// (needs OpenSSL)
#
[CmdletBinding()]
param(
    [string]$Configuration = "Release",
    [switch]$Clean,
    [string]$BuildDir = "build",
    [switch]$NoTest,
    [switch]$Tls,
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
$vcvars = Join-Path ${env:ProgramFiles(x86)} `
    "Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

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
Write-Host "config:    $Configuration"

$configure = "`"$cmake`" -S `"$root`" -B `"$build`" -G `"$generator`" -DCMAKE_BUILD_TYPE=$Configuration"
if ($Tls) {
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
Invoke-VsEnv $configure
Invoke-VsEnv "`"$cmake`" --build `"$build`""

if (-not $NoTest) {
    Invoke-VsEnv "`"$cmake`" --build `"$build`" --target test"
}

Write-Host "build complete: $build"
