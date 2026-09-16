# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming
#
# Build the C# native shim (nclink_shim.dll on Windows, libnclink_shim.so on Linux).
#
#   .\build-native.ps1                  # links build/nclink_core.lib from the repo root
#   .\build-native.ps1 -CoreLib <path>  # links another static core library
#
# Run the repo root build.ps1 first so nclink_core.lib exists; this script only
# turns the shim into a shared library. Keep this file ASCII-only: Windows
# PowerShell 5.1 reads .ps1 files with the OEM code page, and non-ASCII bytes in
# a script can break parsing.
[CmdletBinding()]
param(
    [string]$CoreLib = "",
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$root = (Resolve-Path (Join-Path $here "..\..\..")).Path
$out = if ($OutDir -ne "") { $OutDir } else { Join-Path $here "bin" }
$lib = if ($CoreLib -ne "") { $CoreLib } else { Join-Path $root "build\nclink_core.lib" }

if (-not (Test-Path -LiteralPath $lib)) {
    throw "missing native core library: $lib (run .\build.ps1 in the repo root first)"
}
New-Item -ItemType Directory -Force -Path $out | Out-Null

$vcvars = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat not found (Visual Studio 2022 Build Tools required)"
}

$source = Join-Path $here "nclink_shim.c"
$dll = Join-Path $out "nclink_shim.dll"
# /MD must match how the core library was built (CMake Release uses -MD); mixing
# /MT and /MD makes the linker ask for both CRTs and fail on __imp_* symbols.
$cl = 'cl /nologo /LD /MD /O2 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS ' +
      ('/I "{0}\include" /Fe:"{1}" "{2}" "{3}" ws2_32.lib ' -f $root, $dll, $source, $lib) +
      'iphlpapi.lib crypt32.lib msvcrt.lib'
# cd into the output directory so the object files land next to the DLL.
$script = "@echo off`r`ncall `"$vcvars`" >nul`r`ncd /d `"$out`"`r`n$cl`r`n"

$bat = Join-Path $env:TEMP ("ncl-shim-" + [guid]::NewGuid().ToString() + ".bat")
Set-Content -LiteralPath $bat -Value $script -Encoding ASCII
try {
    & cmd /c $bat
    if ($LASTEXITCODE -ne 0) { throw "compiling the shim failed ($LASTEXITCODE)" }
} finally {
    Remove-Item -LiteralPath $bat -Force -ErrorAction SilentlyContinue
}

Write-Host "native shim: $dll"
