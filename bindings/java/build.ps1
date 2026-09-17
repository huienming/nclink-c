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
# code page, and non-ASCII bytes in a script can break parsing.
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
# Java 8 bytecode: usable on older toolchains; the binding only uses java.lang /
# java.util.
& $javac.Source -encoding UTF-8 --release 8 -d $classes @sources
if ($LASTEXITCODE -ne 0) { throw "javac failed ($LASTEXITCODE)" }
Write-Host "classes: $classes"

if (-not $NoTest) {
    # -Dfile.encoding=UTF-8: JDK 17 and older print ASCII in a non-UTF-8 locale
    # (Windows console, POSIX locale in a container), turning Chinese into "?".
    & $java.Source "-Djava.library.path=$libdir" "-Dfile.encoding=UTF-8" -cp $classes com.nclink.SelfTest
    if ($LASTEXITCODE -ne 0) { throw "self-test failed ($LASTEXITCODE)" }

    # Opt-in: with NCLINK_TEST_BROKER=tcp://host:port the device and the client run
    # in one process and talk over a real broker (probe, bindings, sampling, events
    # and the whole file channel).
    if ($env:NCLINK_TEST_BROKER) {
        & $java.Source "-Djava.library.path=$libdir" "-Dfile.encoding=UTF-8" -cp $classes com.nclink.BrokerE2E
        if ($LASTEXITCODE -ne 0) { throw "broker e2e failed ($LASTEXITCODE)" }
    }
}
