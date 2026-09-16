# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming
#
# Configure-free Java binding build: native library (JNI + shim) and javac.
#
#   .\build.ps1                # native + classes + self-test
#   .\build.ps1 -SkipNative    # javac only (native library already built)
#   .\build.ps1 -NoTest        # do not run the self-test
#   .\build.ps1 -CoreLib <path>
#
# Keep this file ASCII-only: Windows PowerShell 5.1 reads .ps1 files with the OEM
# code page.
[CmdletBinding()]
param(
    [string]$CoreLib = "",
    [switch]$SkipNative,
    [switch]$NoTest,
    [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$classes = Join-Path $here "$BuildDir\classes"
$libdir = Join-Path $here "native\bin"

if (-not $SkipNative) {
    $nativeArgs = @{ }
    if ($CoreLib -ne "") { $nativeArgs["CoreLib"] = $CoreLib }
    & (Join-Path $here "native\build-native.ps1") @nativeArgs
}

$javac = (Get-Command javac -ErrorAction SilentlyContinue)
$java = (Get-Command java -ErrorAction SilentlyContinue)
if (-not $javac -or -not $java) { throw "javac / java not found: install a JDK (17+ recommended)" }

$sources = @()
foreach ($tree in @("src", "demo", "tests")) {
    $path = Join-Path $here $tree
    if (Test-Path -LiteralPath $path) {
        $sources += Get-ChildItem -Path $path -Recurse -File -Filter *.java |
            Select-Object -ExpandProperty FullName
    }
}
if ($sources.Count -eq 0) { throw "no .java sources found under $here" }

New-Item -ItemType Directory -Force -Path $classes | Out-Null
# Java 8 bytecode: ???/???????????????? java.lang / java.util??
& $javac.Source -encoding UTF-8 --release 8 -d $classes @sources
if ($LASTEXITCODE -ne 0) { throw "javac failed ($LASTEXITCODE)" }
Write-Host "classes: $classes"

if (-not $NoTest) {
    # -Dfile.encoding=UTF-8：JDK 17 及更早在非 UTF-8 区域（Windows 控制台 / Linux
    # 容器里的 POSIX locale）默认按 ASCII 输出，中文会变成 "?"。
    & $java.Source "-Djava.library.path=$libdir" "-Dfile.encoding=UTF-8" `
        -cp $classes com.nclink.SelfTest
    if ($LASTEXITCODE -ne 0) { throw "self-test failed ($LASTEXITCODE)" }
}
