# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

# Assemble the release package: the headers, the static libraries, the vendor
# protocol clients (their headers and their library), the example programs and
# the docs.
#
# The package carries no implementation source by default, and that includes
# the vendor clients: the core library speaks NC-Link only, so a recipient
# needs a library and a header set to talk to a machine tool, not the protocol
# implementation. src/, tests/, tools/ and the clients/plugins sources need
# -WithSource. The adapter modules and the device program live in their own
# package (tools/make_adapter_release.ps1).
# See the "vendor clients" section for the layout.
#
# After assembly a content guard runs over the package: a screened word fails
# the run (the keyword list is written as code point escapes, see $forbidden
# below, so this file itself passes that guard). A protocol spelling that
# happens to contain a screened word is exempted, see $protocolWords below.
#
# Note: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 files with
# the OEM code page, and a comment whose last byte pair is a multi-byte
# character can swallow the following line.
#
#   .\tools\make_release.ps1                        # headers + libs + docs + examples
#   .\tools\make_release.ps1 -Version 3.4.0
#   .\tools\make_release.ps1 -WithSource            # also ship src/tests/tools
#
# Libraries and example executables are collected from the locations below;
# a missing one is skipped with a note:
#   build/nclink_core.lib            lib/windows-x64-msvc/ + examples/bin/windows-x64-msvc/
#   build-x86/nclink_core.lib        lib/windows-x86-msvc/ + examples/bin/windows-x86-msvc/
#   build-tls/nclink_core.lib        lib/windows-x64-msvc-tls/
#   build-linux/libnclink_core.a     lib/linux-x86_64-gcc/ + examples/bin/linux-x86_64-gcc/
#   build-linux-tls/libnclink_core.a lib/linux-x86_64-gcc-tls/
#   build-mingw/libnclink_core.a     lib/windows-amd64-mingw/
#
# The static memory variant (-StaticMem) ships next to the default heap build,
# in a directory of its own so nothing that already links the default path
# changes meaning:
#   build-staticmem/nclink_core.lib          lib/windows-x64-msvc-staticmem/
#   build-x86-staticmem/nclink_core.lib      lib/windows-x86-msvc-staticmem/
#   build-linux-staticmem/libnclink_core.a   lib/linux-x86_64-gcc-staticmem/
#   build-staticmem-tls/nclink_core.lib      lib/windows-x64-msvc-staticmem-tls/
#   build-linux-staticmem-tls/libnclink_core.a  lib/linux-x86_64-gcc-staticmem-tls/
# plus the matching example binaries under examples/bin/<platform>-staticmem/.
# The static memory library carries a fixed pool inside its .bss (20 MiB with
# the default NCLINK_MEM_POOL_BYTES), so it is a separate artifact rather than a
# build option on the same file.
#
[CmdletBinding()]
param(
    # Empty means "read NCL_VERSION from include/nclink/ncl_common.h" - same
    # rule the adapter package uses, so the two can never disagree with the
    # header (a hardcoded default quietly names the package after an old
    # release).
    [string]$Version = "",
    [string]$Name = "",
    [switch]$NoZip,
    [switch]$WithSource
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ($Version -eq "") {
    $common = Join-Path $root "include\nclink\ncl_common.h"
    $match = [regex]::Match((Get-Content -LiteralPath $common -Raw),
                            'NCL_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"')
    if (-not $match.Success) {
        throw "cannot read NCL_VERSION from $common"
    }
    $Version = $match.Groups[1].Value
}
if ($Name -eq "") { $Name = "nclink-core-c-$Version" }
$pkg = Join-Path $root "dist\$Name"
$zip = Join-Path $root "dist\$Name.zip"

$msvcLib = Join-Path $root "build\nclink_core.lib"
$msvcTlsLib = Join-Path $root "build-tls\nclink_core.lib"
$gccLib = Join-Path $root "build-linux\libnclink_core.a"
$gccTlsLib = Join-Path $root "build-linux-tls\libnclink_core.a"
$mingwLib = Join-Path $root "build-mingw\libnclink_core.a"
$mingwTlsLib = Join-Path $root "build-mingw-tls\libnclink_core.a"
$msvcStaticLib = Join-Path $root "build-staticmem\nclink_core.lib"
$x86StaticLib = Join-Path $root "build-x86-staticmem\nclink_core.lib"
$gccStaticLib = Join-Path $root "build-linux-staticmem\libnclink_core.a"
$msvcStaticTlsLib = Join-Path $root "build-staticmem-tls\nclink_core.lib"
$gccStaticTlsLib = Join-Path $root "build-linux-staticmem-tls\libnclink_core.a"

# The two platforms the package must carry; the static memory variant ships as
# well because it cannot be produced from the heap library afterwards.
$requiredStatic = @($msvcStaticLib, $gccStaticLib)
foreach ($required in $requiredStatic) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "missing static memory library:  (build with -StaticMem first)"
    }
}

foreach ($required in @($msvcLib, $gccLib)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "missing library:  (build first)"
    }
}

if (Test-Path -LiteralPath $pkg) { Remove-Item -LiteralPath $pkg -Recurse -Force }
# An assembly that reuses a half deleted directory would silently ship whatever
# survived (a locked csharp obj/ made that happen once): make sure it is gone.
if (Test-Path -LiteralPath $pkg) { throw "could not clear the previous package: $pkg" }
New-Item -ItemType Directory -Path $pkg -Force | Out-Null

function Copy-Tree([string]$from, [string]$to, [string[]]$include,
                    [string]$SkipPattern = "") {
    $dest = Join-Path $pkg $to
    New-Item -ItemType Directory -Path $dest -Force | Out-Null
    foreach ($pattern in $include) {
        Get-ChildItem -Path (Join-Path $root $from) -Filter $pattern -Recurse -File |
            Where-Object { $SkipPattern -eq "" -or $_.FullName -notmatch $SkipPattern } |
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
# *.h: the device model header (device_model.h) travels with the examples, so
# that the packaged sources still compile.
Copy-Tree "examples" "examples" @("*.c", "*.cpp", "*.h", "*.txt")

# Prebuilt example executables: run them straight from the package.
#   build\examples\*.exe    -> examples/bin/windows-x64-msvc/  (same MSVC x64 Release as the lib)
#   build-linux\bin\ncl_*   -> examples/bin/linux-x86_64-gcc/  (gcc 13 + glibc, built in Docker)
$exeSources = @(
    @{ From = "build\examples";  Dst = "examples\bin\windows-x64-msvc"; Filter = "ncl_*.exe" },
    @{ From = "build-x86\examples"; Dst = "examples\bin\windows-x86-msvc"; Filter = "ncl_*.exe" },
    @{ From = "build-linux\bin"; Dst = "examples\bin\linux-x86_64-gcc"; Filter = "ncl_*" }
    @{ From = "build-staticmem\examples"; Dst = "examples\bin\windows-x64-msvc-staticmem"; Filter = "ncl_*.exe" },
    @{ From = "build-x86-staticmem\examples"; Dst = "examples\bin\windows-x86-msvc-staticmem"; Filter = "ncl_*.exe" },
    @{ From = "build-linux-staticmem\bin"; Dst = "examples\bin\linux-x86_64-gcc-staticmem"; Filter = "ncl_*" },
    @{ From = "build-staticmem-tls\examples"; Dst = "examples\bin\windows-x64-msvc-staticmem-tls"; Filter = "ncl_*.exe" },
    @{ From = "build-linux-staticmem-tls\bin"; Dst = "examples\bin\linux-x86_64-gcc-staticmem-tls"; Filter = "ncl_*" }
)
foreach ($exe in $exeSources) {
    $from = Join-Path $root $exe.From
    if (-not (Test-Path -LiteralPath $from)) {
        Write-Host ("  note: {0} not found, {1} stays empty" -f $exe.From, $exe.Dst)
        continue
    }
    $dest = Join-Path $pkg $exe.Dst
    New-Item -ItemType Directory -Path $dest -Force | Out-Null
    Get-ChildItem -Path $from -Filter $exe.Filter -File |
        # the device program is not an example program: it ships with the
        # adapter package (bin/ncl_server.exe), not in this one
        Where-Object { $_.Name -notin @("ncl_server", "ncl_server.exe") `
            -and $_.Name -notlike "ncl_test_*" } |
        ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $dest $_.Name) -Force
            Write-Host ("  + {0}/{1}" -f $exe.Dst, $_.Name)
        }
}

# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# vendor clients (every package: their headers and their static library)
#
# The core library speaks NC-Link and nothing else: the clients are what talks
# FOCAS / Syntec / Modbus / ..., so they travel as well - as a library and a
# header set, never as sources (their C files carry the protocol edge cases
# and stay with -WithSource, like src/):
#
#   include/nclink/clients/*.h       the clients' public headers, next to the
#                                    core ones: a consumer includes
#                                    <nclink/clients/focas.h>, the same root
#                                    the core uses itself
#   lib/<platform>/nclink_clients.*  the clients as a static library, one per
#                                    platform that has a core library
#
# <platform> names are the ones under lib/, so a build directory that is not
# present is skipped with a note: exactly like the core libraries above.
#
# The adapter modules and the device program are not in this package: the
# adapter's binary release is a folder of its own, with the host, every driver
# module and the configuration that picks one - see
# tools/make_adapter_release.ps1, which assembles
# dist/nclink-adapter-<version>-win-x64. This package ships the libraries both
# sides link against.
Copy-Tree "clients/include/nclink/clients" "include/nclink/clients" @("*.h")

$vendorPlatforms = @(
    @{ Build = "build";               Platform = "windows-x64-msvc" },
    @{ Build = "build-tls";           Platform = "windows-x64-msvc-tls" },
    @{ Build = "build-x86";           Platform = "windows-x86-msvc" },
    @{ Build = "build-staticmem";     Platform = "windows-x64-msvc-staticmem" },
    @{ Build = "build-staticmem-tls"; Platform = "windows-x64-msvc-staticmem-tls" },
    @{ Build = "build-x86-staticmem"; Platform = "windows-x86-msvc-staticmem" },
    @{ Build = "build-mingw";         Platform = "windows-amd64-mingw" },
    @{ Build = "build-linux";         Platform = "linux-x86_64-gcc" }
)
foreach ($vendor in $vendorPlatforms) {
    # The clients library: nclink_clients.lib from MSVC, libnclink_clients.a
    # from the others, exactly like the core library of that platform.
    # The CMake builds put the clients under <build>/clients/, the script
    # (build-linux.sh, also used for mingw) writes it at the root of <build>.
    $clientsLib = ""
    foreach ($candidate in @("$($vendor.Build)\clients\nclink_clients.lib",
                             "$($vendor.Build)\clients\libnclink_clients.a",
                             "$($vendor.Build)\libnclink_clients.a")) {
        if (Test-Path -LiteralPath (Join-Path $root $candidate)) {
            $clientsLib = $candidate
            break
        }
    }
    if ($clientsLib -ne "") {
        $dest = Join-Path $pkg ("lib\{0}\{1}" -f $vendor.Platform, (Split-Path -Leaf $clientsLib))
        New-Item -ItemType Directory -Path (Split-Path -Parent $dest) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $root $clientsLib) -Destination $dest -Force
        Write-Host ("  + lib/{0}/{1}" -f $vendor.Platform, (Split-Path -Leaf $clientsLib))
    } else {
        Write-Host ("  note: {0} has no clients library, the clients are not packaged for {1}" -f $vendor.Build, $vendor.Platform)
    }
}

# implementation source and tests: only when explicitly requested
if ($WithSource) {
    Copy-Tree "src" "src" @("*.c", "*.h")
    Copy-Tree "tests" "tests" @("*.c", "*.h", "*.json", "*.txt")
    Copy-Tree "tools" "tools" @("*.py", "*.mjs", "*.ps1")
    # the vendor side in full: the protocol clients and the adapter modules
    Copy-Tree "clients" "clients" @("*.c", "*.h", "*.md", "*.txt")
    Copy-Tree "plugins" "plugins" @("*.c", "*.h", "*.md", "*.txt")
}

# Language bindings: sources only, they link the packaged static libraries
# (the Go binding also carries nclink_thunks.c: cgo cannot hand a Go function
# pointer to C, so its callbacks live on the C side)
Copy-Tree "bindings/go" "bindings/go" @("*.go", "*.mod", "*.md", "*.c", "*.h")
# obj/ and bin/ are build output (.gitignore excludes them too), not shipped
Copy-Tree "bindings/csharp" "bindings/csharp" @("*.cs", "*.csproj", "*.md", "*.c", "*.h", "*.ps1", "*.config") "\\obj\\|\\bin\\"
# The shared native shim (C source + header + build scripts)
Copy-Tree "bindings/native" "bindings/native" @("*.c", "*.h", "*.ps1", "*.sh", "*.md") "\\bin\\"
Copy-Tree "bindings/java" "bindings/java" @("*.java", "*.c", "*.h", "*.md", "*.ps1", "*.sh") "\\bin\\|\\build\\"
Copy-Tree "bindings/python" "bindings/python" @("*.py", "*.md", "*.ps1", "*.sh") "\\bin\\|__pycache__\\"

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

# The static memory variant of both mandatory platforms. It goes to its own
# directory: the file name is identical, and a consumer has to opt in.
foreach ($pair in @(
        @{ Src = $msvcStaticLib; Dst = "lib\windows-x64-msvc-staticmem\nclink_core.lib" },
        @{ Src = $gccStaticLib; Dst = "lib\linux-x86_64-gcc-staticmem\libnclink_core.a" })) {
    $dest = Join-Path $pkg $pair.Dst
    New-Item -ItemType Directory -Path (Split-Path -Parent $dest) -Force | Out-Null
    Copy-Item -LiteralPath $pair.Src -Destination $dest -Force
    Write-Host ("  + {0}" -f $pair.Dst)
}
if (Test-Path -LiteralPath $x86StaticLib) {
    New-Item -ItemType Directory -Path (Join-Path $pkg "lib\windows-x86-msvc-staticmem") -Force | Out-Null
    Copy-Item -LiteralPath $x86StaticLib -Destination (Join-Path $pkg "lib\windows-x86-msvc-staticmem\nclink_core.lib") -Force
} else {
    Write-Host "  note: build-x86-staticmem not found, the 32-bit static memory library is not packaged"
}

# Static memory with TLS: the pool covers the library, OpenSSL keeps using the
# system heap, so this is a variant of its own rather than "the static one with
# a flag". Optional like the other TLS artifacts.
foreach ($pair in @(
        @{ Src = $msvcStaticTlsLib; Dst = "lib\windows-x64-msvc-staticmem-tls\nclink_core.lib" },
        @{ Src = $gccStaticTlsLib; Dst = "lib\linux-x86_64-gcc-staticmem-tls\libnclink_core.a" })) {
    if (Test-Path -LiteralPath $pair.Src) {
        $dest = Join-Path $pkg $pair.Dst
        New-Item -ItemType Directory -Path (Split-Path -Parent $dest) -Force | Out-Null
        Copy-Item -LiteralPath $pair.Src -Destination $dest -Force
        Write-Host ("  + {0}" -f $pair.Dst)
    } else {
        Write-Host ("  note: {0} not found, that static memory + TLS variant is not packaged" -f $pair.Src)
    }
}

# Optional: the Linux library built with TLS support (ssl:// over OpenSSL).
if (Test-Path -LiteralPath $gccTlsLib) {
    New-Item -ItemType Directory -Path (Join-Path $pkg "lib\linux-x86_64-gcc-tls") -Force | Out-Null
    Copy-Item -LiteralPath $gccTlsLib -Destination (Join-Path $pkg "lib\linux-x86_64-gcc-tls\libnclink_core.a") -Force
} else {
    Write-Host "  note: build-linux-tls/libnclink_core.a not found, the TLS variant is not packaged"
}

# Optional: the 32-bit (Win32/x86) MSVC build: .\build.ps1 -Arch x86 -BuildDir build-x86
$x86Lib = Join-Path $root "build-x86\nclink_core.lib"
if (Test-Path -LiteralPath $x86Lib) {
    New-Item -ItemType Directory -Path (Join-Path $pkg "lib\windows-x86-msvc") -Force | Out-Null
    Copy-Item -LiteralPath $x86Lib -Destination (Join-Path $pkg "lib\windows-x86-msvc\nclink_core.lib") -Force
} else {
    Write-Host "  note: build-x86/nclink_core.lib not found, the 32-bit library is not packaged"
}

# Optional: the mingw build of the Windows library, used by the Go bindings
# (cgo on Windows links with mingw, not MSVC).
if (Test-Path -LiteralPath $mingwLib) {
    New-Item -ItemType Directory -Path (Join-Path $pkg "lib\windows-amd64-mingw") -Force | Out-Null
    Copy-Item -LiteralPath $mingwLib -Destination (Join-Path $pkg "lib\windows-amd64-mingw\libnclink_core.a") -Force
} else {
    Write-Host "  note: build-mingw/libnclink_core.a not found, the Go binding needs it staged separately"
}
# The TLS flavour of the same library, for cgo -tags nclink_tls. Its OpenSSL
# dependency stays dynamic (import libraries + DLLs), exactly like the Linux
# TLS build - the static OpenSSL the MSVC variant links is not available for
# the Windows Go toolchain.
if (Test-Path -LiteralPath $mingwTlsLib) {
    Copy-Item -LiteralPath $mingwTlsLib -Destination (Join-Path $pkg "lib\windows-amd64-mingw\libnclink_core_tls.a") -Force
} else {
    Write-Host "  note: build-mingw-tls/libnclink_core.a not found, the Windows Go TLS variant is not packaged"
}

# docs
foreach ($doc in @("README.md", "MANUAL.md", "MANUAL.docx", "RELEASE.md",
                   "CHANGELOG.md", "TRANSFER_PERF.md", "LICENSE")) {
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
#
# The managed bindings (C# / JVM / Python) are shipped in the package, so their
# toolchain names are allowed: what stays screened are the "derived from"
# implementation and the third party libraries this library replaces.
$forbidden = '(?i)\u0063\u006e\.\u006e\u0065\u0072\u0063|\u0065\u0076\u0065\u0072\u0069\u0074|\u006a\u0061\u0063\u006b\u0073\u006f\u006e|\u0070\u0061\u0068\u006f|\u006f\u006b\u0068\u0074\u0074\u0070|\u0063\u006f\u006d\u006d\u006f\u006e\u0073-\u006e\u0065\u0074|\u006e\u0061\u006e\u006f\u0068\u0074\u0074\u0070\u0064|\u0073\u0077\u0061\u0067\u0067\u0065\u0072-\u0063\u006f\u0072\u0065|\u0063\u0061\u0066\u0066\u0065\u0069\u006e\u0065|\u006d\u0071\u0074\u0074\u00765|\b\u0061\u006e\u0064\u0072\u006f\u0069\u0064\b|\u8fc1\u79fb|\u79fb\u690d|\u4e0a\u6e38|\u539f\u7248|\b\u0050\u004f\u0052\u0054\u0049\u004e\u0047\b|\b\u004d\u0065\u0073\u0073\u0061\u0067\u0065\u0055\u0074\u0069\u006c\u0073\b|\b\u0052\u0065\u0073\u0075\u006c\u0074\.\u0073\u0075\u0063\u0063\u0065\u0073\u0073\b|\b\u0052\u0065\u0073\u0075\u006c\u0074\.\u0066\u0061\u0069\u006c\u0065\u0064\b|\b\u0045\u006e\u0063\u006f\u0064\u0065\u0072\.\u0065\u006e\u0063\u006f\u0064\u0065\b|\b\u0044\u0065\u0063\u006f\u0064\u0065\u0072\.\u0064\u0065\u0063\u006f\u0064\u0065\b|\b\u0052\u006f\u006f\u0074\u004e\u006f\u0064\u0065\b|\b\u0044\u0061\u0074\u0061\u0049\u0074\u0065\u006d\u004e\u006f\u0064\u0065\b|\b\u0043\u006f\u006d\u0070\u006f\u006e\u0065\u006e\u0074\u004e\u006f\u0064\u0065\b|\b\u0044\u0065\u0076\u0069\u0063\u0065\u004e\u006f\u0064\u0065\b|\b\u0043\u006f\u006e\u0066\u0069\u0067\u004e\u006f\u0064\u0065\b|\b\u0042\u0061\u0073\u0065\u004e\u006f\u0064\u0065\b|\b\u0041\u0062\u0073\u0074\u0072\u0061\u0063\u0074\u0053\u0065\u0072\u0076\u0065\u0072\b|\b\u0041\u0062\u0073\u0074\u0072\u0061\u0063\u0074\u004d\u0065\u0073\u0073\u0061\u0067\u0065\b|\b\u0043\u006c\u0069\u0065\u006e\u0074\u0048\u006f\u006c\u0064\u0065\u0072|\b\u0053\u0065\u0072\u0076\u0065\u0072\u0046\u0069\u006c\u0065\u0054\u006f\u006f\u006c\b|\b\u0043\u006c\u0069\u0065\u006e\u0074\u0046\u0069\u006c\u0065\u0054\u006f\u006f\u006c\b|\b\u0044\u0065\u0066\u0061\u0075\u006c\u0074\u0046\u0069\u006c\u0065\u0054\u006f\u006f\u006c\b|\b\u004d\u0079\u0048\u0074\u0074\u0070\u0053\u0065\u0072\u0076\u0065\u0072\b|\b\u0054\u0068\u0072\u0065\u0061\u0064\u0053\u0065\u0072\u0076\u0069\u0063\u0065\b|\b\u004a\u0073\u006f\u006e\u0053\u0063\u0068\u0065\u006d\u0061\u0056\u0061\u006c\u0069\u0064\u0061\u0074\u006f\u0072\b|\b\u0043\u0068\u0065\u0063\u006b\u0055\u0074\u0069\u006c\u0073\b|\b\u0063\u006c\u0061\u007a\u007a\b|\b\u0045\u006e\u0075\u006d \u006c\u006f\u006f\u006b\u0075\u0070\b|\b\u0053\u0065\u0072\u0076\u0065\u0072\.\u0068\u0061\u006e\u0064\u006c\u0065\b'
#
# One screened substring is a protocol's own spelling rather than a telltale:
# an MTConnect condition carries its seriousness in an XML attribute literally
# named "severity" (severity="FAULT"), and "severity" contains the screened
# "everit". The client that parses those documents, its header and its test
# fixtures therefore have to spell it that way. The word is cut out of every
# line before screening, so a hit for any other reason still fails the run.
$protocolWords = '(?i)\b\u0073\u0065\u0076\u0065\u0072\u0069\u0074\u0079\b'

$offenders = @()
Get-ChildItem -Path $pkg -Recurse -File |
    Where-Object { $_.Extension -in @(".md", ".h", ".c", ".txt", ".sh", ".ps1", ".json") } |
    ForEach-Object {
        $rel = $_.FullName.Substring($pkg.Length + 1)
        $no = 0
        foreach ($line in [System.IO.File]::ReadAllLines($_.FullName)) {
            $no++
            if (($line -replace $protocolWords, "") -match $forbidden) {
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
            if (($docxText -replace $protocolWords, "") -match $forbidden) {
                throw ("MANUAL.docx contains '{0}', which must not ship" -f $Matches[0])
            }
        }
    } finally {
        $archive.Dispose()
    }
}
Write-Host "  content guard: clean"

# Build output must never travel: the include filters already say what to copy,
# but -Filter matches short (8.3) names on Windows as well, so a stray
# obj/Debug or bin/Debug next to the sources can still ride along. Only the
# bindings are swept - examples/bin holds the packaged executables.
foreach ($junk in @("obj", "bin", "__pycache__")) {
    Get-ChildItem -Path (Join-Path $pkg "bindings") -Directory -Recurse -Filter $junk -ErrorAction SilentlyContinue |
        Sort-Object { $_.FullName.Length } -Descending |
        ForEach-Object {
            Write-Host ("  - pruned build output: {0}" -f $_.FullName.Substring($pkg.Length + 1))
            Remove-Item -LiteralPath $_.FullName -Recurse -Force
        }
}

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
    # The companion checksum file some pipelines expect next to the archive.
    $zipHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLower()
    Set-Content -LiteralPath ($zip + ".sha256") -Encoding ASCII `
        -Value ("{0}  {1}" -f $zipHash, (Split-Path -Leaf $zip))
    Write-Host ("  sha256 {0}" -f $zipHash)
}
