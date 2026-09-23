# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming
#
# C# binding build: native shim (optional) + managed assemblies + self-test.
#
#   .\build.ps1                # native shim + core + samples + self-test
#   .\build.ps1 -SkipNative    # managed only (shim already built)
#   .\build.ps1 -NoTest        # do not run the self-test
#   .\build.ps1 -CoreLib <path>
#   .\build.ps1 -Framework net8.0
#
# On Linux / macOS run the same dotnet commands by hand (the native shim there is
# sh bindings/native/build-shim.sh):
#
#   dotnet build bindings/csharp/src/Nclink.Core/Nclink.Core.csproj -c Release
#   dotnet run --project bindings/csharp/tests/Nclink.SelfTest -c Release
#
# Keep this file ASCII-only: Windows PowerShell 5.1 reads .ps1 files with the OEM
# code page, and non-ASCII bytes in a script can break parsing.
[CmdletBinding()]
param(
    [string]$CoreLib = "",
    [switch]$SkipNative,
    [switch]$NoTest,
    [string]$Framework = ""
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$root = (Resolve-Path (Join-Path $here "..\..")).Path

if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    throw "dotnet not found: install the .NET SDK (8.0+ recommended)"
}

if (-not $SkipNative) {
    if ($env:OS -eq "Windows_NT") {
        $nativeArgs = @{ }
        if ($CoreLib -ne "") { $nativeArgs["CoreLib"] = $CoreLib }
        & (Join-Path $here "..\native\build-shim.ps1") @nativeArgs
        if ($LASTEXITCODE -ne 0) { throw "native shim build failed ($LASTEXITCODE)" }
    } else {
        Write-Host "skipping the native shim (run: sh bindings/native/build-shim.sh)"
    }
}

$buildArgs = @("build", "-c", "Release")
$runArgs = @("-c", "Release")
if ($Framework -ne "") {
    $buildArgs += @("-f", $Framework)
    $runArgs += @("-f", $Framework)
}

foreach ($project in @(
        "src\Nclink.Core\Nclink.Core.csproj",
        "samples\Nclink.Demo.Cli\Nclink.Demo.Cli.csproj",
        "samples\Nclink.Demo.Device\Nclink.Demo.Device.csproj",
        "tests\Nclink.SelfTest\Nclink.SelfTest.csproj")) {
    $path = Join-Path $here $project
    Write-Host "build $project"
    & dotnet @buildArgs $path
    if ($LASTEXITCODE -ne 0) { throw "dotnet build failed: $project ($LASTEXITCODE)" }
}

if (-not $NoTest) {
    $selfTest = Join-Path $here "tests\Nclink.SelfTest\Nclink.SelfTest.csproj"
    if ($Framework -ne "") {
        & dotnet run --project $selfTest --no-build @runArgs
        if ($LASTEXITCODE -ne 0) { throw "self-test failed ($LASTEXITCODE)" }
    } else {
        # 两个目标都跑一遍（net472 的可执行文件只在 Windows 上能跑）
        & dotnet run --project $selfTest --no-build -c Release -f net8.0
        if ($LASTEXITCODE -ne 0) { throw "self-test failed on net8.0 ($LASTEXITCODE)" }
        $legacy = Join-Path $here "tests\Nclink.SelfTest\bin\Release\net472\Nclink.SelfTest.exe"
        if ($env:OS -eq "Windows_NT" -and (Test-Path -LiteralPath $legacy)) {
            & $legacy
            if ($LASTEXITCODE -ne 0) { throw "self-test failed on net472 ($LASTEXITCODE)" }
        }
    }
}
