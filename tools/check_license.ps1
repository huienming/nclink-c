# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming
#
# 检查每个源文件是否带 SPDX 许可头（缺了就报错退出 1），-Fix 会自动补上。
# 测试套件里的 test_license 做的是同一件事（两个平台都会跑），本脚本用于
# 手工检查与批量修复。
#
#   .\tools\check_license.ps1                 # 只检查
#   .\tools\check_license.ps1 -Fix            # 补齐缺失的头
#   .\tools\check_license.ps1 -Root <目录>    # 检查别的目录树
#
[CmdletBinding()]
param(
    [switch]$Fix,
    [string]$Root = "",
    [string]$Holder = "Copyright (c) 2026 huienming"
)

$ErrorActionPreference = "Stop"
if ($Root -eq "") { $Root = Split-Path -Parent $PSScriptRoot }

$needle = "SPDX-License-Identifier: MIT"
$extensions = @(".c", ".h", ".py", ".mjs", ".ps1", ".sh")
$skipDirs = @("build", "build-linux", "build-asan", "dist", ".git", "__pycache__")

function Get-Header([string]$extension) {
    if ($extension -in @(".c", ".h")) {
        return "/* SPDX-License-Identifier: MIT */`n/* $Holder */`n`n"
    }
    return "# SPDX-License-Identifier: MIT`n# $Holder`n`n"
}

$files = Get-ChildItem -LiteralPath $Root -Recurse -File | Where-Object {
    $extensions -contains $_.Extension -and
    -not ($_.FullName -split '[\\/]' | Where-Object { $skipDirs -contains $_ })
}

$missing = @()
foreach ($file in $files) {
    $head = [System.IO.File]::ReadAllLines($file.FullName) | Select-Object -First 3
    if (-not ($head -match [regex]::Escape($needle))) { $missing += $file }
}

Write-Host ("checked {0} files, {1} without a licence header" -f $files.Count, $missing.Count)

if ($missing.Count -eq 0) { exit 0 }

if (-not $Fix) {
    $missing | ForEach-Object { Write-Host ("  missing: {0}" -f $_.FullName.Substring($Root.Length + 1)) }
    Write-Host "run with -Fix to insert the header"
    exit 1
}

foreach ($file in $missing) {
    $text = [System.IO.File]::ReadAllText($file.FullName)
    $header = Get-Header $file.Extension
    if ($text.StartsWith("#!")) {
        $split = $text.IndexOf("`n")
        $text = $text.Substring(0, $split + 1) + $header + $text.Substring($split + 1)
    } else {
        $text = $header + $text
    }
    [System.IO.File]::WriteAllText($file.FullName, $text)
    Write-Host ("  fixed: {0}" -f $file.FullName.Substring($Root.Length + 1))
}
exit 0
