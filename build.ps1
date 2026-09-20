# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# Configure and build nclink-core-c.
#
#   .\build.ps1              # configure + build + run tests (MSVC x64)
#   .\build.ps1 -Clean       # wipe the build directory first
#
# 注：Ninja 靠 rules.ninja 里的 msvc_deps_prefix 认 cl /showIncludes 打印的
# "注意: 包含文件:" 行。CMake 只在 Ninja 生成器下探测这个前缀，而且是把 cl 的
# 输出按控制台代码页解码得到的；cl 也按控制台代码页输出。两边一旦不一致，存进
# rules.ninja 的就是乱码前缀，ninja 认不出任何一行包含信息，所有 .obj 都记成
# "#deps 0"——改了头文件却什么都不重编（症状就是莫名其妙的崩溃）。此时 -Clean
# 重配不一定救得回来（取决于当时的控制台代码页），所以脚本构建完会核对一次
# ninja -t deps，一个头依赖都没记下就把该目录换成 Visual Studio 生成器（它自己
# 跟踪包含关系，不受影响）。另外配置和构建时会把控制台代码页固定下来
# （默认 65001，-ConsoleCodePage 可改），从源头让 cl 和 CMake 的口径一致。
#   .\build.ps1 -Arch x86 -BuildDir build-x86   # 32-bit (Win32/x86) build
#   .\build.ps1 -Target test # build and run ctest only
#   .\build.ps1 -Tls         # also enable MQTT over ssl:// (needs OpenSSL)
#   .\build.ps1 -StaticMem -MemPoolBytes 65536  # no heap: one 64 KiB pool
#   .\build.ps1 -ConsoleCodePage 936   # pin another code page for cmake/cl
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
    [string]$OpenSslRoot = "",
    # "" = auto: reuse the generator of an existing build dir, else Ninja with an
    # automatic fallback to the Visual Studio generator. Needed because the CMake
    # bundled with VS 17.14 BuildTools (3.31.6-msvc6) currently cannot configure
    # with Ninja at all on new build trees: its try_compile scratch project never
    # gets CMakeFiles/rules.ninja, so CMake's own `ninja -t recompact` probe dies
    # with "loading 'CMakeFiles\rules.ninja': The system cannot find the file
    # specified" at "Detecting C compiler ABI info". Pass an explicit cmake -G
    # value to force one (e.g. -Generator 'Visual Studio 17 2022'). The same
    # fallback also fires when Ninja configures but then records no header
    # dependencies at all (see the note at the top of this script).
    [string]$Generator = "",
    # Console output code page pinned while cmake and cl run, so that cl's
    # localized messages and CMake's /showIncludes probe always agree (see
    # Get-CodePageSetup below). 65001 keeps those messages valid UTF-8.
    [int]$ConsoleCodePage = 65001
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$build = Join-Path $root $BuildDir

# NOTE: PowerShell variable names are case-insensitive, so the -Generator
# parameter and the working variable $generator below are one and the same
# variable. Keep a copy of what the caller asked for before $generator is
# derived from the build tree cache.
$generatorRequested = $Generator

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

# cl.exe prints its messages -- including the "注意: 包含文件:" lines ninja reads
# back under /showIncludes -- in the console output code page, and CMake's
# /showIncludes prefix probe decodes them with that same code page. Pin it while
# cmake and cl run so the two can never disagree, whatever terminal launched this
# script: a prefix CMake mis-decodes lands in rules.ninja as garbage, ninja then
# fails to recognize every include line and records no header dependencies at all
# (see the note at the top of this file). 65001 keeps those messages valid UTF-8;
# pass -ConsoleCodePage 936 for a terminal that renders UTF-8 as garbage.
function Get-CodePageSetup() {
    # The last word of `chcp`'s output is the number, which is locale-proof
    # ("Active code page: N" here, a translated sentence elsewhere). %% is how a
    # batch for-variable is escaped inside the generated .bat file.
    return "for /f `"tokens=*`" %%l in ('chcp') do for %%p in (%%l) do set `"NCL_OLDCP=%%p`"`r`n" +
        "chcp $ConsoleCodePage >nul 2>nul`r`n"
}
function Get-CodePageRestore() {
    # Put the caller's code page back, so a build does not leave the terminal
    # reconfigured. The command's own exit code is kept in NCL_RC meanwhile.
    return "if defined NCL_OLDCP chcp %NCL_OLDCP% >nul 2>nul`r`n"
}

function Invoke-VsEnv([string]$command) {
    $script = "@echo off`r`ncall `"$vcvars`" >nul`r`n" + (Get-CodePageSetup) +
        "$command`r`nset `"NCL_RC=%ERRORLEVEL%`"`r`n" + (Get-CodePageRestore) + "exit /b %NCL_RC%"
    $tmp = Join-Path $env:TEMP ("ncl-build-" + [guid]::NewGuid().ToString() + ".bat")
    Set-Content -LiteralPath $tmp -Value $script -Encoding ASCII
    try {
        & cmd /c $tmp
        if ($LASTEXITCODE -ne 0) { throw "command failed ($LASTEXITCODE): $command" }
    } finally {
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    }
}

# Same as Invoke-VsEnv but returns the exit code instead of throwing, so a failed
# configure can be retried with another generator. The batch file records its exit
# code in a file: piping it through the PowerShell pipeline would turn cmake's
# stderr into a terminating NativeCommandError ($ErrorActionPreference = "Stop").
function Invoke-VsEnvCode([string]$command) {
    $codeFile = Join-Path $env:TEMP ("ncl-code-" + [guid]::NewGuid().ToString() + ".txt")
    $script = "@echo off`r`ncall `"$vcvars`" >nul`r`n" + (Get-CodePageSetup) +
        "$command`r`nset `"NCL_RC=%ERRORLEVEL%`"`r`n" + (Get-CodePageRestore) +
        "echo %NCL_RC% > `"$codeFile`""
    $tmp = Join-Path $env:TEMP ("ncl-build-" + [guid]::NewGuid().ToString() + ".bat")
    Set-Content -LiteralPath $tmp -Value $script -Encoding ASCII
    try {
        & cmd /c $tmp | Out-Host
        return [int]((Get-Content -LiteralPath $codeFile -Raw).Trim())
    } finally {
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $codeFile -Force -ErrorAction SilentlyContinue
    }
}

$ninjaAvailable = $false
if (Test-Path -LiteralPath $ninja) {
    $ninjaAvailable = $true
    $env:PATH = (Split-Path -Parent $ninja) + ";" + $env:PATH
}

# Reuse the generator an existing build tree was configured with, so a fallback
# run does not re-probe and rebuild from scratch on every invocation.
$cachedGenerator = ""
$cacheFile = Join-Path $build "CMakeCache.txt"
if (Test-Path -LiteralPath $cacheFile) {
    $m = Select-String -LiteralPath $cacheFile -Pattern '^CMAKE_GENERATOR:INTERNAL=(.+)$' -ErrorAction SilentlyContinue
    if ($m) { $cachedGenerator = $m.Matches[0].Groups[1].Value.Trim() }
}

if ($Generator -ne "") {
    $generator = $Generator
} elseif ($cachedGenerator -ne "") {
    $generator = $cachedGenerator
} elseif ($ninjaAvailable) {
    $generator = "Ninja"
} else {
    $generator = "NMake Makefiles"
}

Write-Host "cmake:     $cmake"
Write-Host "generator: $generator"
Write-Host "arch:      $Arch"
Write-Host "config:    $Configuration"

$configureFlags = "-DCMAKE_BUILD_TYPE=$Configuration"
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
    $configureFlags += " -DNCLINK_WITH_TLS=ON -DOPENSSL_ROOT_DIR=`"$OpenSslRoot`""
}
if ($StaticMem) {
    $poolBytes = if ($MemPoolBytes -ne "") { $MemPoolBytes } else { "20971520" }
    Write-Host "allocation: static pool, $poolBytes bytes"
    $configureFlags += " -DNCLINK_STATIC_MEM=ON -DNCLINK_MEM_POOL_BYTES=$poolBytes"
    if ($MemReport) {
        $configureFlags += " -DNCLINK_MEM_REPORT=ON"
    }
    if ($MemClassBytes -ne "") {
        $configureFlags += " -DNCLINK_MEM_CLASS_BYTES=$MemClassBytes"
    }
    if ($MemFirstFit) {
        $configureFlags += " -DNCLINK_MEM_FIRST_FIT=ON"
    }
}

function Get-ConfigureCommand([string]$gen) {
    return "`"$cmake`" -S `"$root`" -B `"$build`" -G `"$gen`" $configureFlags"
}

$code = Invoke-VsEnvCode (Get-ConfigureCommand $generator)
if ($code -ne 0 -and $generatorRequested -eq "" -and $generator -eq "Ninja") {
    # See the -Generator parameter comment: this CMake's Ninja path cannot
    # generate the try_compile scratch project, so fall back and keep building.
    Write-Warning "Ninja configure failed; retrying with the Visual Studio generator."
    Write-Warning "Cause: this CMake never writes CMakeFiles/rules.ninja for its try_compile"
    Write-Warning "scratch project, so its own 'ninja -t recompact' probe fails. Install a CMake"
    Write-Warning "from cmake.org instead of the VS-bundled one to get Ninja back."
    if (Test-Path -LiteralPath $build) { Remove-Item -LiteralPath $build -Recurse -Force }
    $generator = "Visual Studio 17 2022"
    $code = Invoke-VsEnvCode (Get-ConfigureCommand $generator)
}
if ($code -ne 0) { throw "configure failed ($code): $(Get-ConfigureCommand $generator)" }

# Multi-config generators (Visual Studio) need an explicit configuration when
# building or running the test target, and their ctest target is RUN_TESTS
# (`test` is a Makefile/Ninja-only target name).
function Get-BuildFlags([string]$gen) {
    if ($gen -like "Visual Studio*") { " --config $Configuration" } else { "" }
}
function Get-TestTarget([string]$gen) {
    if ($gen -like "Visual Studio*") { "RUN_TESTS" } else { "test" }
}

Invoke-VsEnv "`"$cmake`" --build `"$build`"$(Get-BuildFlags $generator)"

# See the note at the top of this script: a Ninja tree whose msvc_deps_prefix
# does not match what cl.exe actually prints records every object with "#deps 0"
# and then ignores header edits entirely. The build above is enough to tell the
# two cases apart, so check the recorded dependencies and, when the tree is that
# broken, redo it with the Visual Studio generator once (its .tlog-based include
# tracking is unaffected, and the generator is then reused on later runs because
# it is cached in the build directory).
if ($generator -eq "Ninja") {
    $ninjaExe = if ($ninjaAvailable) { $ninja } else { (Get-Command ninja -ErrorAction SilentlyContinue).Source }
    # Like Invoke-VsEnvCode: let cmd absorb stderr, because a native command
    # writing to stderr would otherwise become a terminating error under
    # $ErrorActionPreference = "Stop".
    $deps = if ($ninjaExe) { cmd /c "`"$ninjaExe`" -C `"$build`" -t deps 2>nul" } else { @() }
    $objects = @($deps | Where-Object { $_ -match ': #deps ' })
    $withDeps = @($objects | Where-Object { $_ -match '#deps [1-9]' })
    if ($objects.Count -gt 0 -and $withDeps.Count -eq 0) {
        if ($generatorRequested -ne "") {
            Write-Warning "Ninja recorded no header dependencies for the $($objects.Count) objects in $build,"
            Write-Warning "so editing a header will not rebuild anything. Re-run without -Generator (or pass"
            Write-Warning "'Visual Studio 17 2022') to get a tree that tracks includes."
        } else {
            Write-Warning "Ninja recorded no header dependencies for the $($objects.Count) objects in ${build}:"
            Write-Warning "rules.ninja's msvc_deps_prefix does not match what this cl.exe prints, so header"
            Write-Warning "edits would silently rebuild nothing."
            Write-Warning "Switching the tree to the Visual Studio generator, which tracks includes itself."
            if (Test-Path -LiteralPath $build) { Remove-Item -LiteralPath $build -Recurse -Force }
            $generator = "Visual Studio 17 2022"
            $code = Invoke-VsEnvCode (Get-ConfigureCommand $generator)
            if ($code -ne 0) { throw "configure failed ($code): $(Get-ConfigureCommand $generator)" }
            Invoke-VsEnv "`"$cmake`" --build `"$build`"$(Get-BuildFlags $generator)"
        }
    }
}

if (-not $NoTest) {
    Invoke-VsEnv "`"$cmake`" --build `"$build`" --target $(Get-TestTarget $generator)$(Get-BuildFlags $generator)"
}

Write-Host "build complete: $build"
Write-Host "generator:     $generator"
