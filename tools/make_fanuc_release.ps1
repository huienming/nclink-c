# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# Assemble the FANUC adapter release package: the assembled device program
# (the host plus the FOCAS adapter module), its configuration, the site manual,
# the two run scripts, checksums and a zip.
#
# The program is one NC-Link server; what makes it speak FANUC is the module in
# plugins/ and the point map in conf/fanuc.json, so the package is "a program
# that loads a module", not "a program with FANUC compiled in".
#
#   .\tools\make_fanuc_release.ps1                 # dist\nclink-fanuc-adapter-3.4.0-win-x64
#   .\tools\make_fanuc_release.ps1 -Version 3.4.0
#   .\tools\make_fanuc_release.ps1 -NoZip
#   .\tools\make_fanuc_release.ps1 -WithProtocolDocs   # internal notes too
#
# -WithProtocolDocs also ships docs/FANUC-CNC-FOCAS.md and docs/adapters-README.md.
# They are engineering notes (reverse engineering evidence, field box details),
# so they stay out of the default package: a package that goes to a machine
# tool builder carries the site manual only.
#
# Note: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 files with
# the OEM code page, and a comment whose last byte pair is a multi-byte
# character can swallow the following line.
#
[CmdletBinding()]
param(
    [string]$Version = "3.4.0",
    [string]$BuildDir = "build",
    [string]$Name = "",
    [switch]$NoZip,
    [switch]$WithProtocolDocs
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ($Name -eq "") { $Name = "nclink-fanuc-adapter-$Version-win-x64" }
$pkg = Join-Path $root "dist\$Name"
$zip = Join-Path $root "dist\$Name.zip"

$hostExe = Join-Path $root "$BuildDir\adapters\ncl_adapter.exe"
$module = Join-Path $root "$BuildDir\plugins\ncl_driver_focas.dll"
$siteManual = Join-Path $root "adapters\FANUC-ADAPTER.md"
$fanucConfig = Join-Path $root "conf\fanuc.json"
$mqttConfig = Join-Path $root "conf\mqtt.cfg"
$license = Join-Path $root "LICENSE"

foreach ($required in @($hostExe, $module, $siteManual, $fanucConfig, $license)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "missing input: $required (build first: .\build.ps1)"
    }
}

if (Test-Path -LiteralPath $pkg) { Remove-Item -LiteralPath $pkg -Recurse -Force }
if (Test-Path -LiteralPath $pkg) { throw "could not clear the previous package: $pkg" }
foreach ($dir in @("bin", "plugins", "conf")) {
    New-Item -ItemType Directory -Path (Join-Path $pkg $dir) -Force | Out-Null
}
if ($WithProtocolDocs) {
    New-Item -ItemType Directory -Path (Join-Path $pkg "docs") -Force | Out-Null
}

Write-Host "assembling $pkg"

Copy-Item -LiteralPath $hostExe -Destination (Join-Path $pkg "bin\ncl_adapter.exe") -Force
Write-Host "  + bin/ncl_adapter.exe"
Copy-Item -LiteralPath $module -Destination (Join-Path $pkg "plugins\ncl_driver_focas.dll") -Force
Write-Host "  + plugins/ncl_driver_focas.dll"
Copy-Item -LiteralPath $fanucConfig -Destination (Join-Path $pkg "conf\fanuc.json") -Force
Write-Host "  + conf/fanuc.json"
if (Test-Path -LiteralPath $mqttConfig) {
    Copy-Item -LiteralPath $mqttConfig -Destination (Join-Path $pkg "conf\mqtt.cfg") -Force
    Write-Host "  + conf/mqtt.cfg"
} else {
    Write-Host "  note: conf/mqtt.cfg not found, the site writes its own broker file"
}
Copy-Item -LiteralPath $siteManual -Destination (Join-Path $pkg "README.md") -Force
Write-Host "  + README.md"
Copy-Item -LiteralPath $license -Destination (Join-Path $pkg "LICENSE") -Force
if ($WithProtocolDocs) {
    foreach ($pair in @(
            @{ Src = "protocal\docs\01-FANUC-CNC-FOCAS.md"; Dst = "docs\FANUC-CNC-FOCAS.md" },
            @{ Src = "adapters\README.md"; Dst = "docs\adapters-README.md" })) {
        $src = Join-Path $root $pair.Src
        if (Test-Path -LiteralPath $src) {
            Copy-Item -LiteralPath $src -Destination (Join-Path $pkg $pair.Dst) -Force
            Write-Host ("  + {0}" -f $pair.Dst)
        } else {
            Write-Host ("  note: {0} not found, skipped" -f $pair.Src)
        }
    }
}

# The two convenience scripts. ASCII only (same .ps1 reading rule as this file),
# and they pass everything through to the host, so the host's own usage text
# stays the single reference. The machine address and the point map live in the
# configuration, so the scripts do not carry them.
$runOnce = @'
# Self check: poll every configured point once, print it, exit. No broker.
#
#   .\run-once.ps1
#   .\run-once.ps1 -Raw            # audit with the frames
#   .\run-once.ps1 -Config conf\other.json
param(
    [string]$Config = "",
    [switch]$Raw
)
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root "bin\ncl_adapter.exe"
if ($Config -eq "") { $Config = Join-Path $root "conf\fanuc.json" }
$forward = @("-r", $root, "-c", $Config, "--once", "--stats", "-b", "-")
if ($Raw) { $forward += "--raw" }
& $exe @forward
exit $LASTEXITCODE
'@
Set-Content -LiteralPath (Join-Path $pkg "run-once.ps1") -Value $runOnce -Encoding ASCII
Write-Host "  + run-once.ps1"

$run = @'
# Run for real: poll the machine, publish the samples, serve REST. Ctrl+C exits.
#
#   .\run.ps1
#   .\run.ps1 -Broker tcp://10.0.0.9:1883
#   .\run.ps1 -Broker tcp://10.0.0.9:1883 -Interval 500 -RestPort 8081 -Raw
#   .\list-plugins.ps1          # show the loaded adapter modules
param(
    [string]$Config = "",
    [string]$Broker = "",
    [string]$PluginDir = "",
    [int]$Interval = 1000,
    [int]$RestPort = 8080,
    [switch]$Raw
)
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root "bin\ncl_adapter.exe"
if ($Config -eq "") { $Config = Join-Path $root "conf\fanuc.json" }
if ($PluginDir -eq "") { $PluginDir = Join-Path $root "plugins" }
$forward = @("-r", $root, "-c", $Config, "-P", $PluginDir,
             "--interval", "$Interval", "--port", "$RestPort")
if ($Broker -ne "") { $forward += @("-b", $Broker) }
if ($Raw) { $forward += "--raw" }
& $exe @forward
exit $LASTEXITCODE
'@
Set-Content -LiteralPath (Join-Path $pkg "run.ps1") -Value $run -Encoding ASCII
Write-Host "  + run.ps1"

$listPlugins = @'
# What can this program talk to? Lists the adapter modules found in plugins/.
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $root "bin\ncl_adapter.exe") -r $root --plugins
exit $LASTEXITCODE
'@
Set-Content -LiteralPath (Join-Path $pkg "list-plugins.ps1") -Value $listPlugins -Encoding ASCII
Write-Host "  + list-plugins.ps1"

# Content guard: a package must not leak build machine paths or the private
# engineering tree it was assembled in. The docs are ours, so this is a leak
# check rather than a screening of third party names.
$leaks = @()
Get-ChildItem -Path $pkg -Recurse -File |
    Where-Object { $_.Extension -in @(".md", ".json", ".cfg", ".ps1", ".txt") } |
    ForEach-Object {
        $relative = $_.FullName.Substring($pkg.Length + 1)
        $no = 0
        foreach ($line in [System.IO.File]::ReadAllLines($_.FullName)) {
            $no++
            if ($line -match '(?i)\bcodex\b|D:\\codex|/d/codex') {
                $leaks += ("{0}:{1}: {2}" -f $relative, $no, $line.Trim())
            }
        }
    }
if ($leaks.Count -gt 0) {
    $leaks | Select-Object -First 20 | ForEach-Object { Write-Host "  ! $_" -ForegroundColor Red }
    throw ("the package mentions {0} line(s) of machine local paths" -f $leaks.Count)
}
Write-Host "  content guard: clean (no machine local paths)"

# checksums
$lines = @()
Get-ChildItem -Path $pkg -Recurse -File | Sort-Object FullName | ForEach-Object {
    $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower()
    $relative = $_.FullName.Substring($pkg.Length + 1).Replace("\", "/")
    $lines += "$hash  $relative"
}
Set-Content -LiteralPath (Join-Path $pkg "SHA256SUMS.txt") -Value $lines -Encoding ASCII

$files = (Get-ChildItem -Path $pkg -Recurse -File | Measure-Object).Count
$bytes = (Get-ChildItem -Path $pkg -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host ("  {0} files, {1:N1} KB" -f $files, ($bytes / 1KB))

if (-not $NoZip) {
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -Path $pkg -DestinationPath $zip -CompressionLevel Optimal
    $zipHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLower()
    Set-Content -LiteralPath ($zip + ".sha256") -Encoding ASCII `
        -Value ("{0}  {1}" -f $zipHash, (Split-Path -Leaf $zip))
    Write-Host ("  zip {0} ({1:N1} KB)" -f $zip, ((Get-Item -LiteralPath $zip).Length / 1KB))
    Write-Host ("  sha256 {0}" -f $zipHash)
}

# What the recipient sees, so a packaging run does not need a second look.
Write-Host ""
Get-ChildItem -Path $pkg -Recurse -File | Sort-Object FullName | ForEach-Object {
    $relative = $_.FullName.Substring($pkg.Length + 1).Replace("\", "/")
    Write-Host ("  {0,10:N1} KB  {1}" -f ($_.Length / 1KB), $relative)
}
