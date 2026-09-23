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
$exe = Join-Path $root "bin\ncl_server.exe"
if ($Config -eq "") { $Config = Join-Path $root "conf\fanuc.json" }
$forward = @("-r", $root, "-c", $Config, "--once", "--stats", "-b", "-")
if ($Raw) { $forward += "--raw" }
& $exe @forward
exit $LASTEXITCODE
