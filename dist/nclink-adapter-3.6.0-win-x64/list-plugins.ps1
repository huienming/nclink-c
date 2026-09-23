# What can this program talk to? Lists the adapter modules found in plugins/.
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $root "bin\ncl_server.exe") -r $root --plugins
exit $LASTEXITCODE
