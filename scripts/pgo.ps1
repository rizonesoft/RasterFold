# Generate build/pgo/rasterfold.profdata from the instrumented Windows build.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root
if ($env:OS -eq 'Windows_NT') {
    & python scripts/pgo.py @args
} else {
    & python3 scripts/pgo.py @args
}
exit $LASTEXITCODE
