# Configure and build nclink-core-c.
#
#   .\build.ps1              # configure + build + run tests (MSVC)
#   .\build.ps1 -Clean       # wipe the build directory first
#   .\build.ps1 -Target test # build and run ctest only
#
[CmdletBinding()]
param(
    [string]$Configuration = "Release",
    [switch]$Clean,
    [string]$BuildDir = "build",
    [switch]$NoTest
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
Invoke-VsEnv $configure
Invoke-VsEnv "`"$cmake`" --build `"$build`""

if (-not $NoTest) {
    Invoke-VsEnv "`"$cmake`" --build `"$build`" --target test"
}

Write-Host "build complete: $build"
