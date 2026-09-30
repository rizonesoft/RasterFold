# Download the pinned local toolstack into tools\. Safe to run again.
# Usage: powershell -ExecutionPolicy Bypass -File tools\bootstrap.ps1 [-Force]
#
# The pins live in toolchain.json at the repository root (name, version, URL,
# SHA-256, size, install folder, probe file, and how to read the version).
# A component whose probe file exists and reports the pinned version is left
# alone; anything else is downloaded (or taken from tools\downloads when the
# cached archive's hash matches), verified, and extracted.
param([switch]$Force)
$ErrorActionPreference = "Stop"
$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $tools
if ($env:OS -ne 'Windows_NT') {
  & python3 (Join-Path $root 'tools/bootstrap.py')
  exit $LASTEXITCODE
}
$manifestPath = Join-Path $root "toolchain.json"
if (-not (Test-Path $manifestPath)) { throw "bootstrap: toolchain.json not found at $manifestPath" }
$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
$cache = Join-Path $tools "downloads"
New-Item -ItemType Directory -Force -Path $cache | Out-Null
# Windows ships libarchive's bsdtar, which reads .zip and .7z. A GNU tar
# earlier on PATH (Git for Windows) cannot, so call this one by path.
$bsdtar = Join-Path $env:SystemRoot "System32\tar.exe"

# SHA-256 through .NET: Get-FileHash is missing when Windows PowerShell runs
# with a PowerShell 7 module path inherited from its parent shell.
function Get-Sha256($path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $fs = [System.IO.File]::OpenRead($path)
    try { return ([BitConverter]::ToString($sha.ComputeHash($fs)) -replace '-', '') } finally { $fs.Dispose(); $sha.Dispose() }
}

function Get-InstalledVersion($c, $probe) {
    try {
        $out = (& $probe @($c.versionArgs) 2>&1 | Out-String)
        if ($out -match $c.versionMatch) { return $Matches[1] }
    } catch { }
    return $null
}

foreach ($c in $manifest.components) {
    # The install folder must stay inside tools\: a manifest entry cannot
    # point the extraction (and its Remove-Item) anywhere else.
    if ($c.dir -notmatch '^[A-Za-z0-9._-]+$') { throw "bootstrap: $($c.name): dir '$($c.dir)' is not a plain folder name under tools\" }
    $dest = Join-Path $tools $c.dir
    $probe = Join-Path $dest ($c.probe -replace '/', '\')
    if ((Test-Path $probe) -and -not $Force) {
        $have = Get-InstalledVersion $c $probe
        if ($have -eq $c.expectVersion) { Write-Host "ok $($c.name) $have"; continue }
        Write-Host "$($c.name): found version '$have', pinned $($c.expectVersion); reinstalling"
    }
    $file = Join-Path $cache ([IO.Path]::GetFileName($c.url))
    $want = $c.sha256.ToUpper()
    if (-not (Test-Path $file) -or (Get-Sha256 $file) -ne $want) {
        Write-Host "download $($c.url)"
        $ProgressPreference = "SilentlyContinue"
        Invoke-WebRequest -Uri $c.url -OutFile $file -UseBasicParsing
    }
    $size = (Get-Item $file).Length
    if ($c.bytes -and $size -ne [int64]$c.bytes) { throw "bootstrap: $($c.name): $file is $size bytes, pinned $($c.bytes)" }
    $hash = (Get-Sha256 $file)
    if ($hash -ne $want) { throw "bootstrap: SHA-256 mismatch for $file`n  got  $hash`n  want $want" }

    $stage = Join-Path $cache "stage-$($c.dir)"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    & $bsdtar -xf $file -C $stage
    if ($LASTEXITCODE -ne 0) { throw "bootstrap: extract failed: $file" }
    # Use the single top-level folder when the archive has one.
    $top = @(Get-ChildItem $stage)
    $src = if ($top.Count -eq 1 -and $top[0].PSIsContainer) { $top[0].FullName } else { $stage }
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    Move-Item $src $dest
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    if (-not (Test-Path $probe)) { throw "bootstrap: $($c.name): $($c.probe) missing after extract" }
    $have = Get-InstalledVersion $c $probe
    if ($have -ne $c.expectVersion) { throw "bootstrap: $($c.name): installed version '$have', pinned $($c.expectVersion)" }
    Set-Content -Encoding Ascii (Join-Path $dest "PINNED.txt") "$($c.url)`nsha256 $($c.sha256)"
    Write-Host "installed $($c.name) $have"
}
