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
