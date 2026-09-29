# Build Bin\rasterfold.exe with the pinned local toolstack.
# Usage: powershell -ExecutionPolicy Bypass -File build.ps1 [ninja arguments]
# The first run downloads Clang (llvm-mingw), Ninja and the libjxl tools into
# tools\ (tools\bootstrap.ps1). No Visual Studio installation is needed.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root
$ninja = Join-Path $root "tools\ninja\ninja.exe"
$clang = Join-Path $root "tools\llvm-mingw\bin\clang.exe"
if (-not (Test-Path $ninja) -or -not (Test-Path $clang)) {
    & powershell -ExecutionPolicy Bypass -File (Join-Path $root "tools\bootstrap.ps1")
    if ($LASTEXITCODE -ne 0) { throw "tools\bootstrap.ps1 failed" }
}
& $ninja @args
if ($LASTEXITCODE -ne 0) { throw "ninja failed ($LASTEXITCODE)" }
$version = (& $clang --version | Select-Object -First 1)
Set-Content -Encoding Ascii (Join-Path $root "build\toolchain.txt") "$version`nninja $(& $ninja --version)"
Write-Host "built $(Join-Path $root 'Bin\rasterfold.exe')"
