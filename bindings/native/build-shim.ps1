# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming
#
# Build the shared native shim (nclink_shim.dll on Windows, libnclink_shim.so on
# Linux) used by the C#, Java and Python bindings.
#
#   .\build-shim.ps1                    # -> bindings/native/bin/nclink_shim.dll
#   .\build-shim.ps1 -OutDir <dir>      # e.g. next to your program
#   .\build-shim.ps1 -CoreLib <path>    # links another static core library
#   .\build-shim.ps1 -Arch x86          # 32-bit (vcvars32)
#
# Run the repo root build.ps1 first so nclink_core.lib exists; this script only
# turns the shim into a shared library. Keep this file ASCII-only: Windows
# PowerShell 5.1 reads .ps1 files with the OEM code page, and non-ASCII bytes in
# a script can break parsing.
[CmdletBinding()]
param(
    [string]$CoreLib = "",
    [string]$OutDir = "",
    [switch]$Tls,
    [string]$OpenSslRoot = "C:\ProgramData\anaconda3\Library",
    [ValidateSet("x64", "x86")]
    [string]$Arch = "x64"
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$root = (Resolve-Path (Join-Path $here "..\..")).Path
$out = if ($OutDir -ne "") { $OutDir }
       elseif ($Tls) { Join-Path $here "bin-tls" }
       else { Join-Path $here "bin" }
$lib = if ($CoreLib -ne "") { $CoreLib }
       elseif ($Tls) { Join-Path $root "build-tls\nclink_core.lib" }
       else { Join-Path $root "build\nclink_core.lib" }

if (-not (Test-Path -LiteralPath $lib)) {
    if ($Tls) {
        throw "missing TLS core library: $lib (run .\build.ps1 -Tls in the repo root first)"
    }
    throw "missing native core library: $lib (run .\build.ps1 in the repo root first)"
}

# ssl:// 需要 TLS 版的库 + OpenSSL 的导入库（运行期还要那两个 DLL，见下面拷贝）。
$tlsLibs = ""
if ($Tls) {
    $sslLib = Join-Path $OpenSslRoot "lib\libssl.lib"
    $cryptoLib = Join-Path $OpenSslRoot "lib\libcrypto.lib"
    if (-not (Test-Path -LiteralPath $sslLib) -or -not (Test-Path -LiteralPath $cryptoLib)) {
        throw "OpenSSL import libraries not found under $OpenSslRoot\lib (use -OpenSslRoot)"
    }
    $tlsLibs = '"{0}" "{1}" ' -f $sslLib, $cryptoLib
}
New-Item -ItemType Directory -Force -Path $out | Out-Null

$vcvars = Join-Path ${env:ProgramFiles(x86)} ("Microsoft Visual Studio\2022\BuildTools" +
    "\VC\Auxiliary\Build\vcvars{0}.bat" -f $(if ($Arch -eq "x86") { "32" } else { "64" }))
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars not found for arch $Arch (Visual Studio 2022 Build Tools required)"
}

$source = Join-Path $here "nclink_shim.c"
$dll = Join-Path $out "nclink_shim.dll"
# /MD must match how the core library was built (CMake Release uses -MD); mixing
# /MT and /MD makes the linker ask for both CRTs and fail on __imp_* symbols.
$cl = 'cl /nologo /LD /MD /O2 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS ' +
      ('/I "{0}\include" /Fe:"{1}" "{2}" "{3}" ws2_32.lib ' -f $root, $dll, $source, $lib) +
      $tlsLibs + 'iphlpapi.lib crypt32.lib msvcrt.lib'
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

if ($Tls) {
    # The import libraries need the OpenSSL DLLs at run time: put them next to
    # the shim so a test can load it without touching PATH.
    foreach ($name in @("libssl-3-x64.dll", "libcrypto-3-x64.dll")) {
        $from = Join-Path $OpenSslRoot ("bin\" + $name)
        if (Test-Path -LiteralPath $from) {
            Copy-Item -LiteralPath $from -Destination (Join-Path $out $name) -Force
        } else {
            Write-Host "  note: $name not found under $OpenSslRoot\bin (make sure it is on PATH)"
        }
    }
    Write-Host "TLS shim: $dll (needs the OpenSSL DLLs next to it or on PATH)"
}
