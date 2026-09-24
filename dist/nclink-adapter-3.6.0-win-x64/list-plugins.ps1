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
    # No -Config: the package default - the same file a bare exe picks up.
    if ($Config -eq "") {
        $Config = Join-Path $root "conf\device.json"
        if (-not (Test-Path -LiteralPath $Config)) { $Config = Join-Path $root "conf\fanuc.json" }
    }
    elseif (-not [System.IO.Path]::IsPathRooted($Config)) { $Config = Join-Path $root $Config }
    & $exe -r $root -c $Config --plugins
}
exit $LASTEXITCODE
