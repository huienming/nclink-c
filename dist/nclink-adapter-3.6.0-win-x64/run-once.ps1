# Self check: poll every configured point once, print it, exit. No broker.
#
#   .\run-once.ps1                 # conf\device.json (the package's default)
#   .\run-once.ps1 -Config conf\syntec.json
#   .\run-once.ps1 -Raw            # audit with the frames
param(
    [string]$Config = "",
    [switch]$Raw
)
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root "bin\ncl_server.exe"
# No -Config: the package default - the same file a bare exe picks up.
if ($Config -eq "") {
    $Config = Join-Path $root "conf\device.json"
    if (-not (Test-Path -LiteralPath $Config)) { $Config = Join-Path $root "conf\fanuc.json" }
}
# A relative -Config is relative to the package, not to the caller's directory
elseif (-not [System.IO.Path]::IsPathRooted($Config)) { $Config = Join-Path $root $Config }
$forward = @("-r", $root, "-c", $Config, "--once", "--stats", "-b", "-")
if ($Raw) { $forward += "--raw" }
& $exe @forward
exit $LASTEXITCODE
