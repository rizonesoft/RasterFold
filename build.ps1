# Native-host build entry point; every compiler lives under tools/.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root
if ($env:OS -eq 'Windows_NT') {
    if (-not (Test-Path 'tools/llvm-mingw/bin/clang.exe') -or -not (Test-Path 'tools/ninja/ninja.exe')) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File tools/bootstrap.ps1
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    & python scripts/build.py @args
} else {
    & python3 scripts/build.py @args
}
exit $LASTEXITCODE
