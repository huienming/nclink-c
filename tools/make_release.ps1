# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# Assemble the release package: headers, both static libraries and the docs.
#
# 发布包默认**不含实现源码**：头文件 + 两个平台的库 + 文档 + 示例程序。
# 需要把仓库整棵树（src/tests/tools 与构建脚本）一起发出去时加 -WithSource。
#
# 打包完成后会做一遍内容审查：包内出现不该外发的字样即报错（关键词表以码点
# 转义书写，见下面的 $forbidden，本文件自身也满足该审查）。
#
#   .\tools\make_release.ps1                        # 头文件 + 库 + 文档 + 示例
#   .\tools\make_release.ps1 -Version 3.0.0
#   .\tools\make_release.ps1 -WithSource             # 额外含 src/tests/tools 与构建脚本
#
[CmdletBinding()]
param(
    [string]$Version = "3.0.0",
    [string]$Name = "",
    [switch]$NoZip,
    [switch]$WithSource
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ($Name -eq "") { $Name = "nclink-core-c-$Version" }
$pkg = Join-Path $root "dist\$Name"
$zip = Join-Path $root "dist\$Name.zip"

$msvcLib = Join-Path $root "build\nclink_core.lib"
$msvcTlsLib = Join-Path $root "build-tls\nclink_core.lib"
$gccLib = Join-Path $root "build-linux\libnclink_core.a"
$gccTlsLib = Join-Path $root "build-linux-tls\libnclink_core.a"

foreach ($required in @($msvcLib, $gccLib)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "missing library:  (build first)"
    }
}

if (Test-Path -LiteralPath $pkg) { Remove-Item -LiteralPath $pkg -Recurse -Force }
New-Item -ItemType Directory -Path $pkg -Force | Out-Null

function Copy-Tree([string]$from, [string]$to, [string[]]$include) {
    $dest = Join-Path $pkg $to
    New-Item -ItemType Directory -Path $dest -Force | Out-Null
    foreach ($pattern in $include) {
        Get-ChildItem -Path (Join-Path $root $from) -Filter $pattern -Recurse -File |
            ForEach-Object {
                $relative = $_.FullName.Substring((Join-Path $root $from).Length + 1)
                $target = Join-Path $dest $relative
                New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
                Copy-Item -LiteralPath $_.FullName -Destination $target -Force
            }
    }
}

Write-Host "assembling $pkg$(if ($WithSource) { ' (with source)' } else { ' (binaries + docs only)' })"

# headers
Copy-Tree "include" "include" @("*.h", "*.hpp")

# examples ship with every release: they are part of the documentation
Copy-Tree "examples" "examples" @("*.c", "*.txt")

# implementation source and tests: only when explicitly requested
if ($WithSource) {
    Copy-Tree "src" "src" @("*.c", "*.h")
    Copy-Tree "tests" "tests" @("*.c", "*.h", "*.json", "*.txt")
    Copy-Tree "tools" "tools" @("*.py", "*.mjs", "*.ps1")
}

# libraries
New-Item -ItemType Directory -Path (Join-Path $pkg "lib\windows-x64-msvc") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $pkg "lib\linux-x86_64-gcc") -Force | Out-Null
Copy-Item -LiteralPath $msvcLib -Destination (Join-Path $pkg "lib\windows-x64-msvc\nclink_core.lib") -Force

# Optional: the Windows library built with TLS support (needs the OpenSSL DLLs
# at run time, see RELEASE.md).
if (Test-Path -LiteralPath $msvcTlsLib) {
    New-Item -ItemType Directory -Path (Join-Path $pkg "lib\windows-x64-msvc-tls") -Force | Out-Null
    Copy-Item -LiteralPath $msvcTlsLib -Destination (Join-Path $pkg "lib\windows-x64-msvc-tls\nclink_core.lib") -Force
} else {
    Write-Host "  note: build-tls/nclink_core.lib not found, the Windows TLS variant is not packaged"
}
Copy-Item -LiteralPath $gccLib -Destination (Join-Path $pkg "lib\linux-x86_64-gcc\libnclink_core.a") -Force

# Optional: the Linux library built with TLS support (ssl:// over OpenSSL).
if (Test-Path -LiteralPath $gccTlsLib) {
    New-Item -ItemType Directory -Path (Join-Path $pkg "lib\linux-x86_64-gcc-tls") -Force | Out-Null
    Copy-Item -LiteralPath $gccTlsLib -Destination (Join-Path $pkg "lib\linux-x86_64-gcc-tls\libnclink_core.a") -Force
} else {
    Write-Host "  note: build-linux-tls/libnclink_core.a not found, the TLS variant is not packaged"
}

# docs
foreach ($doc in @("README.md", "MANUAL.md", "MANUAL.docx", "RELEASE.md",
                   "CHANGELOG.md", "LICENSE")) {
    Copy-Item -LiteralPath (Join-Path $root $doc) -Destination (Join-Path $pkg $doc) -Force
}
if ($WithSource) {
    foreach ($script in @("CMakeLists.txt", "build.ps1", "build-linux.sh")) {
        Copy-Item -LiteralPath (Join-Path $root $script) -Destination (Join-Path $pkg $script) -Force
    }
}

# Content guard: a release must not mention the implementation this library was
# derived from, nor any third party library it replaces. Every letter of the
# keyword list is written as a \uXXXX escape, so this file does not contain the
# words it screens for (and survives being read as ANSI). .NET regex decodes the
# escapes; the escapes around "(?i)" and "\b" style atoms stay intact.
$forbidden = '(?i)\b\u006a\u0061\u0076\u0061\b|\u0063\u006e\.\u006e\u0065\u0072\u0063|\.\u006a\u0061\u0076\u0061\b|\u0065\u0076\u0065\u0072\u0069\u0074|\u006a\u0061\u0063\u006b\u0073\u006f\u006e|\u0070\u0061\u0068\u006f|\u006f\u006b\u0068\u0074\u0074\u0070|\u0063\u006f\u006d\u006d\u006f\u006e\u0073-\u006e\u0065\u0074|\u006e\u0061\u006e\u006f\u0068\u0074\u0074\u0070\u0064|\u0073\u0077\u0061\u0067\u0067\u0065\u0072-\u0063\u006f\u0072\u0065|\u0063\u0061\u0066\u0066\u0065\u0069\u006e\u0065|\u006d\u0071\u0074\u0074\u00765|\b\u0061\u006e\u0064\u0072\u006f\u0069\u0064\b|\b\u004a\u004e\u0049\b|\b\u0067\u0072\u0061\u0064\u006c\u0065\b|\b\u006d\u0061\u0076\u0065\u006e\b|\u8fc1\u79fb|\u79fb\u690d|\u4e0a\u6e38|\u539f\u7248|\b\u0050\u004f\u0052\u0054\u0049\u004e\u0047\b|\u004d\u0065\u0073\u0073\u0061\u0067\u0065\u0055\u0074\u0069\u006c\u0073|\u0052\u0065\u0073\u0075\u006c\u0074\.\u0073\u0075\u0063\u0063\u0065\u0073\u0073|\u0052\u0065\u0073\u0075\u006c\u0074\.\u0066\u0061\u0069\u006c\u0065\u0064|\u0045\u006e\u0063\u006f\u0064\u0065\u0072\.\u0065\u006e\u0063\u006f\u0064\u0065|\u0044\u0065\u0063\u006f\u0064\u0065\u0072\.\u0064\u0065\u0063\u006f\u0064\u0065|\u0052\u006f\u006f\u0074\u004e\u006f\u0064\u0065|\u0044\u0061\u0074\u0061\u0049\u0074\u0065\u006d\u004e\u006f\u0064\u0065|\u0043\u006f\u006d\u0070\u006f\u006e\u0065\u006e\u0074\u004e\u006f\u0064\u0065|\u0044\u0065\u0076\u0069\u0063\u0065\u004e\u006f\u0064\u0065|\u0043\u006f\u006e\u0066\u0069\u0067\u004e\u006f\u0064\u0065|\u0042\u0061\u0073\u0065\u004e\u006f\u0064\u0065|\u0041\u0062\u0073\u0074\u0072\u0061\u0063\u0074\u0053\u0065\u0072\u0076\u0065\u0072|\u0041\u0062\u0073\u0074\u0072\u0061\u0063\u0074\u004d\u0065\u0073\u0073\u0061\u0067\u0065|\b\u0043\u006c\u0069\u0065\u006e\u0074\u0048\u006f\u006c\u0064\u0065\u0072|\u0053\u0065\u0072\u0076\u0065\u0072\u0046\u0069\u006c\u0065\u0054\u006f\u006f\u006c|\u0043\u006c\u0069\u0065\u006e\u0074\u0046\u0069\u006c\u0065\u0054\u006f\u006f\u006c|\u0044\u0065\u0066\u0061\u0075\u006c\u0074\u0046\u0069\u006c\u0065\u0054\u006f\u006f\u006c|\u004d\u0079\u0048\u0074\u0074\u0070\u0053\u0065\u0072\u0076\u0065\u0072|\u0054\u0068\u0072\u0065\u0061\u0064\u0053\u0065\u0072\u0076\u0069\u0063\u0065|\u004a\u0073\u006f\u006e\u0053\u0063\u0068\u0065\u006d\u0061\u0056\u0061\u006c\u0069\u0064\u0061\u0074\u006f\u0072|\u0043\u0068\u0065\u0063\u006b\u0055\u0074\u0069\u006c\u0073|\b\u0063\u006c\u0061\u007a\u007a\b|\u0045\u006e\u0075\u006d \u006c\u006f\u006f\u006b\u0075\u0070|\u0053\u0065\u0072\u0076\u0065\u0072\.\u0068\u0061\u006e\u0064\u006c\u0065'
$offenders = @()
Get-ChildItem -Path $pkg -Recurse -File |
    Where-Object { $_.Extension -in @(".md", ".h", ".c", ".txt", ".sh", ".ps1", ".json") } |
    ForEach-Object {
        $rel = $_.FullName.Substring($pkg.Length + 1)
        $no = 0
        foreach ($line in [System.IO.File]::ReadAllLines($_.FullName)) {
            $no++
            if ($line -match $forbidden) {
                $offenders += ("{0}:{1}: {2}" -f $rel, $no, $line.Trim())
            }
        }
    }
if ($offenders.Count -gt 0) {
    $offenders | Select-Object -First 20 | ForEach-Object { Write-Host "  ! $_" -ForegroundColor Red }
    throw ("release package contains {0} line(s) that must not ship" -f $offenders.Count)
}

# The Word manual is a zip: check its document text as well.
$docx = Join-Path $pkg "MANUAL.docx"
if (Test-Path -LiteralPath $docx) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($docx)
    try {
        $entry = $archive.GetEntry("word/document.xml")
        if ($entry -ne $null) {
            $reader = New-Object System.IO.StreamReader($entry.Open())
            try { $docxText = $reader.ReadToEnd() } finally { $reader.Dispose() }
            $docxText = [regex]::Replace($docxText, "<[^>]+>", "")
            if ($docxText -match $forbidden) {
                throw ("MANUAL.docx contains '{0}', which must not ship" -f $Matches[0])
            }
        }
    } finally {
        $archive.Dispose()
    }
}
Write-Host "  content guard: clean"

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
    Write-Host ("  zip {0} ({1:N1} KB)" -f $zip, ((Get-Item -LiteralPath $zip).Length / 1KB))
}
