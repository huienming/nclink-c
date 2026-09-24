# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# Assemble the adapter release package: the device program (the host), every
# adapter module built from plugins/, one configuration sample per driver, the
# site documents, the run scripts, checksums and a zip.
#
# The program is one NC-Link server; what makes it speak FOCAS (or Syntec, or
# any later driver) is a module in plugins/ that the configuration names. So
# the package is one folder - nclink-adapter-<version>-win-x64 - with several
# drivers in plugins/ and the site deciding which one it loads by editing its
# configuration (plugins/ADAPTER-PACKAGE.md is the package's README).
#
#   .\tools\make_adapter_release.ps1                    # dist\nclink-adapter-<core version>-win-x64
#   .\tools\make_adapter_release.ps1 -Version 3.6.0
#   .\tools\make_adapter_release.ps1 -NoZip
#   .\tools\make_adapter_release.ps1 -WithProtocolDocs  # internal notes too
#
# -WithProtocolDocs also ships plugins/README.md (how to write an adapter) and
# the protocol notes under protocal/docs/. They are engineering material, so
# they stay out of the default package: a package that goes to a machine tool
# builder carries the site documents only.
#
# Inputs (a missing one is a hard error, this package cannot be half assembled):
#   <build>\ncl_server.exe      the host (Ninja: <build>/, VS: <build>\Release\)
#   <build>\plugins\*.dll       one module per adapter (ncl_driver_<tool>)
#   plugins\ADAPTER-PACKAGE.md  the package README
#   conf\<driver>.json, conf\mqtt.cfg   configuration samples (which driver to load)
#   plugins\*-ADAPTER.md        the site manual of each driver, under docs/
#
# Note: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 files with
# the OEM code page, and a comment whose last byte pair is a multi-byte
# character can swallow the following line.
[CmdletBinding()]
param(
    [string]$Version = "",
    [string]$BuildDir = "builds/build",
    [string]$Name = "",
    [switch]$NoZip,
    [switch]$WithProtocolDocs
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

# The adapter ships the same host as the library package, so the version is read
# from the source unless it is given: a package whose name disagrees with the
# program inside it is a support call waiting to happen.
if ($Version -eq "") {
    $common = Join-Path $root "stack\include\nclink\ncl_common.h"
    $match = [regex]::Match((Get-Content -LiteralPath $common -Raw),
                            'NCL_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"')
    if (-not $match.Success) {
        throw "cannot read NCL_VERSION from $common"
    }
    $Version = $match.Groups[1].Value
}
if ($Name -eq "") { $Name = "nclink-adapter-$Version-win-x64" }
$pkg = Join-Path $root "dist\$Name"
$zip = Join-Path $root "dist\$Name.zip"

# Where the built files land depends on the generator: Ninja puts them
# straight under <build>/, Visual Studio adds a <config>/ level.
function Find-Built([string[]]$candidates) {
    foreach ($candidate in $candidates) {
        $path = Join-Path $root $candidate
        if (Test-Path -LiteralPath $path) { return $path }
    }
    return Join-Path $root $candidates[0]
}
$hostExe = Find-Built @("$BuildDir\Release\ncl_server.exe", "$BuildDir\ncl_server.exe")
$moduleDir = Join-Path $root "$BuildDir\plugins"
$readmeFile = Join-Path $root "plugins\ADAPTER-PACKAGE.md"
$license = Join-Path $root "LICENSE"
$configDir = Join-Path $root "conf"

foreach ($required in @($hostExe, $readmeFile, $license)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "missing input: $required (build first: .\build.ps1)"
    }
}

# The modules: every adapter built from plugins/, minus the test fixtures that
# a debug build drops next to them (they exist to be rejected by the loader).
$modules = @()
if (Test-Path -LiteralPath $moduleDir) {
    $modules = @(Get-ChildItem -Path $moduleDir -File |
        Where-Object { $_.Name -like "*ncl_driver_*" -and $_.Name -notlike "*_test_*" `
            -and $_.Extension -in @(".dll", ".so") } |
        Sort-Object -Property Name)
}
if ($modules.Count -eq 0) {
    throw "no adapter module under $moduleDir (build first: .\build.ps1)"
}

# One configuration sample per driver (the site edits this to pick its driver)
# plus the broker file. The point map is in the module, not here.
$configs = @()
if (Test-Path -LiteralPath $configDir) {
    # *-model.json are model overrides a builder keeps next to the sources, not
    # driver samples: the model to publish is the one the modules declare
    # (bin\ncl_server.exe --model prints it).
    $configs = @(Get-ChildItem -Path $configDir -File -Filter "*.json" |
        Where-Object { $_.Name -notlike "*-model.json" } |
        Sort-Object -Property Name)
}
$mqttConfig = Join-Path $configDir "mqtt.cfg"

if (Test-Path -LiteralPath $pkg) {
    Get-ChildItem -LiteralPath $pkg -Force | ForEach-Object {
        if ($_.FullName -notlike "$pkg\*") {
            throw ("outside the package: {0}" -f $_.FullName)
        }
        if ($_.PSIsContainer) { [System.IO.Directory]::Delete($_.FullName, $true) }
        else { [System.IO.File]::Delete($_.FullName) }
    }
    # now empty, and about to be rebuilt from scratch
    [System.IO.Directory]::Delete($pkg, $false)
}
if (Test-Path -LiteralPath $pkg) { throw "could not clear the previous package: $pkg" }
foreach ($dir in @("bin", "plugins", "conf", "docs")) {
    New-Item -ItemType Directory -Path (Join-Path $pkg $dir) -Force | Out-Null
}

Write-Host "assembling $pkg"

Copy-Item -LiteralPath $hostExe -Destination (Join-Path $pkg "bin\ncl_server.exe") -Force
Write-Host "  + bin/ncl_server.exe"
foreach ($module in $modules) {
    Copy-Item -LiteralPath $module.FullName `
        -Destination (Join-Path $pkg ("plugins\{0}" -f $module.Name)) -Force
    Write-Host ("  + plugins/{0}" -f $module.Name)
}
foreach ($config in $configs) {
    Copy-Item -LiteralPath $config.FullName `
        -Destination (Join-Path $pkg ("conf\{0}" -f $config.Name)) -Force
    Write-Host ("  + conf/{0}" -f $config.Name)
}
if (Test-Path -LiteralPath $mqttConfig) {
    Copy-Item -LiteralPath $mqttConfig -Destination (Join-Path $pkg "conf\mqtt.cfg") -Force
    Write-Host "  + conf/mqtt.cfg"
} else {
    Write-Host "  note: conf/mqtt.cfg not found, the site writes its own broker file"
}

# The site manual of each driver (plugins/<PROTOCOL>-ADAPTER.md), next to the
# package README. FANUC has one; a driver without one is documented by its
# configuration sample and its header.
Get-ChildItem -Path (Join-Path $root "plugins") -File -Filter "*-ADAPTER.md" |
    Sort-Object -Property Name | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $pkg ("docs\{0}" -f $_.Name)) -Force
        Write-Host ("  + docs/{0}" -f $_.Name)
    }
Copy-Item -LiteralPath $readmeFile -Destination (Join-Path $pkg "README.md") -Force
Write-Host "  + README.md"
Copy-Item -LiteralPath $license -Destination (Join-Path $pkg "LICENSE") -Force

if ($WithProtocolDocs) {
    $notes = @(Get-ChildItem -Path (Join-Path $root "protocal\docs") -File -Filter "*.md" `
            -ErrorAction SilentlyContinue | Sort-Object -Property Name)
    if ($notes.Count -gt 0) {
        New-Item -ItemType Directory -Path (Join-Path $pkg "docs\protocol") -Force | Out-Null
        foreach ($note in $notes) {
            Copy-Item -LiteralPath $note.FullName `
                -Destination (Join-Path $pkg ("docs\protocol\{0}" -f $note.Name)) -Force
        }
        Write-Host ("  + docs/protocol/ ({0} notes)" -f $notes.Count)
    }
    $pluginsReadme = Join-Path $root "plugins\README.md"
    if (Test-Path -LiteralPath $pluginsReadme) {
        Copy-Item -LiteralPath $pluginsReadme -Destination (Join-Path $pkg "docs\plugins-README.md") -Force
        Write-Host "  + docs/plugins-README.md"
    }
}

# The three convenience scripts. ASCII only (same .ps1 reading rule as this
# file), and they pass everything through to the host, so the host's own usage
# text stays the single reference. The machine address and the driver to load
# live in the configuration, so the scripts do not carry them: -Config picks.
$runOnce = @'
# Self check: poll every configured point once, print it, exit. No broker.
#
#   .\run-once.ps1
#   .\run-once.ps1 -Config conf\syntec.json
#   .\run-once.ps1 -Raw            # audit with the frames
param(
    [string]$Config = "",
    [switch]$Raw
)
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root "bin\ncl_server.exe"
if ($Config -eq "") { $Config = Join-Path $root "conf\fanuc.json" }
# A relative -Config is relative to the package, not to the caller's directory
elseif (-not [System.IO.Path]::IsPathRooted($Config)) { $Config = Join-Path $root $Config }
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
#   .\run.ps1 -Config conf\syntec.json
#   .\run.ps1 -Config conf\fanuc.json -Broker tcp://10.0.0.9:1883 -Interval 500 -RestPort 8081
#   .\run.ps1 -Raw                 # audit every frame
param(
    [string]$Config = "",
    [string]$Broker = "",
    [string]$PluginDir = "",
    [int]$Interval = 1000,
    [int]$RestPort = 8080,
    [switch]$Raw
)
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root "bin\ncl_server.exe"
if ($Config -eq "") { $Config = Join-Path $root "conf\fanuc.json" }
# relative paths mean "inside the package", wherever the caller stands
elseif (-not [System.IO.Path]::IsPathRooted($Config)) { $Config = Join-Path $root $Config }
if ($PluginDir -eq "") { $PluginDir = Join-Path $root "plugins" }
elseif (-not [System.IO.Path]::IsPathRooted($PluginDir)) { $PluginDir = Join-Path $root $PluginDir }
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
# Which drivers does this box carry, and which one would a run load?
#
#   .\list-plugins.ps1                            # the configuration run.ps1 defaults to
#   .\list-plugins.ps1 -Config conf\syntec.json   # what that configuration selects
#   .\list-plugins.ps1 -All                       # every module in plugins\, config ignored
#
# The loader prints one "registered module" line per module it loaded: the tool
# name in it ("focas" / "syntec") is what the running device serves.
param(
    [string]$Config = "",
    [switch]$All
)
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root "bin\ncl_server.exe"
if ($All) {
    & $exe -r $root --plugins
} else {
    if ($Config -eq "") { $Config = Join-Path $root "conf\fanuc.json" }
    elseif (-not [System.IO.Path]::IsPathRooted($Config)) { $Config = Join-Path $root $Config }
    & $exe -r $root -c $Config --plugins
}
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
