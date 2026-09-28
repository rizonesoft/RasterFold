# Build rasterfold.exe with Microsoft ml64 and link.
# Usage: powershell -ExecutionPolicy Bypass -File build.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe was not found. Install Visual Studio with the MSVC x64 toolset."
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "No Visual Studio installation with the MSVC x64 toolset was found." }
$ml = Get-ChildItem -Path (Join-Path $vs "VC\Tools\MSVC") -Filter ml64.exe -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match "\\Hostx64\\x64\\ml64.exe$" } |
    Sort-Object FullName -Descending |
    Select-Object -First 1
if (-not $ml) { throw "ml64.exe (Hostx64\\x64) was not found under $vs" }
$link = Join-Path $ml.DirectoryName "link.exe"

$kits = "C:\Program Files (x86)\Windows Kits\10\Lib"
$sdk = Get-ChildItem $kits -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (-not $sdk) { throw "Windows 10/11 SDK libraries were not found." }
$lib = Join-Path $sdk.FullName "um\x64"

$outDir = Join-Path $root "build"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$sources = @("util", "crc32", "packbits", "fileio", "bmp", "rice", "rans", "codec", "main")
$objects = @()
foreach ($name in $sources) {
    $src = Join-Path $root "src\$name.asm"
    $obj = Join-Path $outDir "$name.obj"
    & $ml.FullName /nologo /c /Zi /Fo $obj /I (Join-Path $root "src") $src
    if ($LASTEXITCODE -ne 0) { throw "ml64 failed on $name.asm ($LASTEXITCODE)" }
    $objects += $obj
}
$exe = Join-Path $outDir "rasterfold.exe"
& $link /nologo /NODEFAULTLIB /SUBSYSTEM:CONSOLE /ENTRY:main /MACHINE:X64 `
    /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /DEBUG /OUT:$exe `
    @objects /LIBPATH:$lib kernel32.lib shell32.lib
if ($LASTEXITCODE -ne 0) { throw "link failed ($LASTEXITCODE)" }

$mlVer = cmd /c "`"$($ml.FullName)`" 2>&1" | Select-Object -First 3
@(
    "ml64: $($ml.FullName)"
    ($mlVer -join "`n")
    "link: $link"
    "sdk: $($sdk.Name)"
) | Set-Content -Encoding Ascii (Join-Path $outDir "toolchain.txt")
Write-Host "built $exe"
