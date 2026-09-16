# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming
#
# Build the Java native library (nclink_jni.dll on Windows): the JNI glue plus
# the shared shim, linked against the core static library.
#
#   .\build-native.ps1                  # links build\nclink_core.lib from the repo root
#   .\build-native.ps1 -CoreLib <path>  # links another static core library
#   .\build-native.ps1 -JavaHome <dir>  # where jni.h lives (default: autodetect)
#
# Run the repo root build.ps1 first. Keep this file ASCII-only: Windows
# PowerShell 5.1 reads .ps1 files with the OEM code page.
[CmdletBinding()]
param(
    [string]$CoreLib = "",
    [string]$OutDir = "",
    [string]$JavaHome = ""
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$root = (Resolve-Path (Join-Path $here "..\..\..")).Path
$out = if ($OutDir -ne "") { $OutDir } else { Join-Path $here "bin" }
$lib = if ($CoreLib -ne "") { $CoreLib } else { Join-Path $root "build\nclink_core.lib" }

if (-not (Test-Path -LiteralPath $lib)) {
    throw "missing native core library: $lib (run .\build.ps1 in the repo root first)"
}

# jni.h / jni_md.h live in the JDK; find one the way javac would.
function Test-JniHeader([string]$dir) {
    if ([string]::IsNullOrEmpty($dir)) { return $false }
    return Test-Path -LiteralPath (Join-Path $dir "include\jni.h")
}

$jdk = $JavaHome
if (-not (Test-JniHeader $jdk)) { $jdk = $env:JAVA_HOME }
if (-not (Test-JniHeader $jdk)) {
    $found = @()
    foreach ($base in @($env:ProgramFiles, ${env:ProgramFiles(x86)},
                        "$env:LOCALAPPDATA\Programs")) {
        if ([string]::IsNullOrEmpty($base)) { continue }
        foreach ($pattern in @("*jdk*", "Java\jdk*", "Eclipse Adoptium\jdk*",
                               "Microsoft\jdk*")) {
            $found += Get-ChildItem -Path (Join-Path $base $pattern) -Directory `
                                    -ErrorAction SilentlyContinue
        }
    }
    foreach ($candidate in $found) {
        if (Test-JniHeader $candidate.FullName) {
            $jdk = $candidate.FullName
            break
        }
    }
}
if (-not (Test-JniHeader $jdk)) {
    throw "jni.h not found: install a JDK, or pass -JavaHome <jdk dir>"
}

New-Item -ItemType Directory -Force -Path $out | Out-Null

$vcvars = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat not found (Visual Studio 2022 Build Tools required)"
}

$jni = Join-Path $here "nclink_jni.c"
$native = Join-Path $root "bindings\native"
$shim = Join-Path $native "nclink_shim.c"
$dll = Join-Path $out "nclink_jni.dll"
# /MD must match how the core library was built (CMake Release uses -MD).
$cl = 'cl /nologo /LD /MD /O2 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS ' +
      ('/I "{0}\include" /I "{2}" /I "{1}\include" /I "{1}\include\win32" ' -f $root, $jdk, $native) +
      ('/Fe:"{0}" "{1}" "{2}" "{3}" ws2_32.lib ' -f $dll, $jni, $shim, $lib) +
      'iphlpapi.lib crypt32.lib msvcrt.lib'
$script = "@echo off`r`ncall `"$vcvars`" >nul`r`ncd /d `"$out`"`r`n$cl`r`n"

$bat = Join-Path $env:TEMP ("ncl-jni-" + [guid]::NewGuid().ToString() + ".bat")
Set-Content -LiteralPath $bat -Value $script -Encoding ASCII
try {
    & cmd /c $bat
    if ($LASTEXITCODE -ne 0) { throw "compiling nclink_jni failed ($LASTEXITCODE)" }
} finally {
    Remove-Item -LiteralPath $bat -Force -ErrorAction SilentlyContinue
}

Write-Host "java native library: $dll"
