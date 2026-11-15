#Requires -Version 5.1
<#
.SYNOPSIS
  Verifies and repairs a RasterFold clone's developer setup.
.DESCRIPTION
  Legs:
    toolchain  every component pinned in toolchain.json is installed under tools\
               and reports its pinned version. Repair runs tools\bootstrap.ps1.
    hooks      git core.hooksPath = tools/githooks, and tools/githooks/pre-commit
               and commit-msg present. Repair sets core.hooksPath (never over a
               different value someone chose: that is reported and left alone).
    python     Python 3.11+ on PATH (tests, research tools, and the TODO gates).
    pillow     the Pillow package importable (tests and research tools read and
               write PNG and BMP through it). Repair: python -m pip install pillow.
    basedpyright
               the pinned Basedpyright from .venv (the Python language server).
               Repair: python -m venv .venv, then install requirements-dev.txt
               into it.
    ffmpeg     ADVISORY: ffmpeg on PATH with the libjxl encoder, needed only by
               tests\measure_final.py, tests\benchmark.py, and research\corpus.py.
               Reported, never repaired, never failing the run.
    compilation database
               the root compile_commands.json is present and current under
               scripts\gen-compdb.py --check. Repair runs scripts\gen-compdb.py.
  -Verify runs the checks only: exit 0 when every required leg is green, 1 otherwise.
  Without -Verify, failing legs are repaired, then verified again.
  Run: pwsh tools/provision.ps1 [-Verify]
#>
[CmdletBinding()]
param(
  [switch]$Verify,
  [switch]$Converter
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
if ($env:OS -ne 'Windows_NT') {
  $forward = @()
  if ($Verify) { $forward += '--verify' }
  if ($Converter) { $forward += '--converter' }
  & python3 (Join-Path $Root 'tools/provision.py') @forward
  exit $LASTEXITCODE
}
$script:ConverterMode = [bool]$Converter
. (Join-Path $PSScriptRoot 'bootstrap.ps1') -HelpersOnly

function Get-Python {
  foreach ($name in @('python3', 'python', 'py')) {
    foreach ($cmd in @(Get-Command $name -All -ErrorAction SilentlyContinue)) {
      if (-not $cmd.Source) { continue }
      try {
        & $cmd.Source -c "import sys; sys.exit(sys.version_info < (3, 11))" 2>$null
        if ($LASTEXITCODE -eq 0) {
          $v = (& $cmd.Source --version 2>&1) -join ' '
          if ($v -match 'Python 3\.') { return @{ Path = $cmd.Source; Version = $v.Trim() } }
        }
      } catch { }
    }
  }
  return $null
}

function Test-ToolchainLeg {
  $res = @()
  $manifest = Get-Content (Join-Path $Root 'toolchain.json') -Raw | ConvertFrom-Json
  $toolsDir = Join-Path $Root 'tools'
  foreach ($c in $manifest.components) {
    if (-not $script:ConverterMode -and $c.PSObject.Properties['scope'] -and $c.scope -eq 'converter') { continue }
    $chk = Test-PinnedComponent $c $toolsDir
    $res += @{ Name = "toolchain $($c.name)"; Ok = $chk.Ok; Required = $true; Detail = $(if ($chk.Ok) { "$($chk.Version)" } else { $chk.Reason }) }
  }
  return $res
}

function Test-HooksLeg {
  $res = @()
  $cfg = ''
  try { $cfg = ((& git -C $Root config core.hooksPath 2>$null) -join "`n").Trim() } catch { }
  $res += @{ Name = 'hooks path'; Ok = ($cfg -eq 'tools/githooks'); Required = $true; Detail = $(if ($cfg) { "core.hooksPath = '$cfg'" } else { 'core.hooksPath unset' }) }
  foreach ($hook in @('pre-commit', 'commit-msg')) {
    $path = Join-Path $Root "tools/githooks/$hook"
    $res += @{ Name = "hooks $hook"; Ok = (Test-Path $path); Required = $true; Detail = $(if (Test-Path $path) { 'present' } else { "missing (restore: git checkout -- tools/githooks/$hook)" }) }
  }
  return $res
}

function Test-PythonLegs {
  $py = Get-Python
  if (-not $py) {
    return @(
      @{ Name = 'python'; Ok = $false; Required = $true; Detail = 'Python 3.11+ not on PATH (install from python.org or winget Python.Python.3.14)' },
      @{ Name = 'pillow'; Ok = $false; Required = $true; Detail = 'needs Python 3.11+ first' }
    )
  }
  $res = @(@{ Name = 'python'; Ok = $true; Required = $true; Detail = $py.Version })
  $pil = (& $py.Path -c "import PIL; print(PIL.__version__)" 2>$null) -join ''
  $res += @{ Name = 'pillow'; Ok = ($LASTEXITCODE -eq 0 -and $pil); Required = $true; Detail = $(if ($pil) { "Pillow $pil" } else { 'not importable (repair: python -m pip install pillow)' }) }
  return $res
}

function Test-FfmpegLeg {
  $cmd = Get-Command ffmpeg -ErrorAction SilentlyContinue
  if (-not $cmd) { return @{ Name = 'ffmpeg'; Ok = $false; Required = $false; Detail = 'not on PATH (benchmarks and the JPEG XL comparison cannot run)' } }
  $enc = (& $cmd.Source -hide_banner -h encoder=libjxl 2>&1 | Out-String)
  $ok = $enc -match 'libjxl'
  return @{ Name = 'ffmpeg'; Ok = $ok; Required = $false; Detail = $(if ($ok) { "$($cmd.Source) with libjxl" } else { "$($cmd.Source) has no libjxl encoder" }) }
}

function Test-BasedpyrightPin {
  param([string]$VersionOutput, [string]$Pin)
  $first = ($VersionOutput -split "`n")[0].Trim()
  if ($first -notmatch '^basedpyright\s+(\S+)$') { return $false }
  return ($Matches[1] -eq $Pin)
}

function Test-BasedpyrightLeg {
  $exe = Join-Path $Root '.venv\Scripts\basedpyright.exe'
  if (-not (Test-Path $exe)) {
    return @{ Name = 'basedpyright'; Ok = $false; Required = $true; Detail = 'missing .venv (repair: python -m venv .venv, then .venv\Scripts\python -m pip install -r requirements-dev.txt)' }
  }
  $ver = (& $exe --version 2>&1 | Out-String).Trim()
  $code = $LASTEXITCODE
  $pin = $null
  foreach ($line in Get-Content (Join-Path $Root 'requirements-dev.txt')) {
    if ($line -match '^basedpyright==(.+)$') { $pin = $Matches[1].Trim() }
  }
  $ok = ($code -eq 0 -and $pin -and (Test-BasedpyrightPin $ver $pin))
  return @{ Name = 'basedpyright'; Ok = $ok; Required = $true; Detail = $(if ($ok) { $ver } else { "installed '$ver' does not match the requirements-dev.txt pin $pin (repair installs requirements-dev.txt into .venv)" }) }
}

function Test-CompilationDatabaseLeg {
  $py = Get-Python
  if (-not $py) {
    return @{ Name = 'compilation database'; Ok = $false; Required = $true; Detail = 'needs Python 3.11+ first' }
  }
  $prevPref = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    $null = (& $py.Path (Join-Path $Root 'scripts/gen-compdb.py') --check 2>&1 | Out-String)
    $ok = ($LASTEXITCODE -eq 0)
  } finally {
    $ErrorActionPreference = $prevPref
  }
  return @{ Name = 'compilation database'; Ok = $ok; Required = $true; Detail = $(if ($ok) { 'current' } else { 'missing or stale (repair: python scripts/gen-compdb.py)' }) }
}

function Test-ConverterSourcesLeg {
  $vendorPath = Join-Path $Root 'converter\vendor.json'
  if (-not (Test-Path $vendorPath)) {
    return @(@{ Name = 'converter sources'; Ok = $false; Required = $true; Detail = "missing $vendorPath" })
  }
  $vendor = Get-Content $vendorPath -Raw | ConvertFrom-Json
  $srcRoot = Join-Path $Root 'tools\converter-src'
  $missing = @()
  $drifted = @()
  foreach ($pkg in $vendor.packages) {
    $pkgDir = Join-Path $srcRoot $pkg.id
    $rcptPath = Join-Path $pkgDir 'PINNED.json'
    if (-not (Test-Path -LiteralPath $rcptPath)) {
      $missing += $pkg.id
      continue
    }
    try {
      if (-not (Test-VendorReceipt $pkg $pkgDir)) { $drifted += $pkg.id }
    } catch {
      $drifted += "$($pkg.id) ($($_.Exception.Message))"
    }
  }
  if ($missing.Count -gt 0 -or $drifted.Count -gt 0) {
    $parts = @()
    if ($missing.Count -gt 0) { $parts += "missing: $($missing -join ', ')" }
    if ($drifted.Count -gt 0) { $parts += "mismatch: $($drifted -join '; ')" }
    return @(@{ Name = 'converter sources'; Ok = $false; Required = $true; Detail = ($parts -join '; ') })
  }
  return @(@{ Name = 'converter sources'; Ok = $true; Required = $true; Detail = "$($vendor.packages.Count) pinned source trees" })
}

function Test-All {
  $r = @()
  $r += Test-ToolchainLeg
  if ($script:ConverterMode) { $r += Test-ConverterSourcesLeg }
  $r += Test-HooksLeg
  $r += Test-PythonLegs
  $r += Test-BasedpyrightLeg
  $r += Test-CompilationDatabaseLeg
  $r += Test-FfmpegLeg
  return $r
}

function Repair-Hooks {
  $cfg = ''
  try { $cfg = ((& git -C $Root config core.hooksPath 2>$null) -join "`n").Trim() } catch { }
  if ($cfg -and $cfg -ne 'tools/githooks') {
    Write-Warning "provision: core.hooksPath points at '$cfg'; left alone (unset it to let provision re-wire)"
    return
  }
  & git -C $Root config core.hooksPath 'tools/githooks'
  Write-Host 'provision: core.hooksPath = tools/githooks'
}

function Show-Results($Results) {
  foreach ($r in $Results) {
    $verdict = if ($r.Ok) { 'OK' } elseif ($r.Required) { 'FAULT' } else { 'ADVISORY' }
    Write-Host "provision: $($r.Name): $verdict ($($r.Detail))"
  }
  $required = @($Results | Where-Object { $_.Required })
  $ok = @($required | Where-Object { $_.Ok }).Count
  Write-Host "provision: $ok/$($required.Count) required legs green"
  return ($ok -eq $required.Count)
}

$results = @(Test-All)
if (-not $Verify) {
  $faults = @($results | Where-Object { -not $_.Ok -and $_.Required } | ForEach-Object { $_.Name })
  if ((@($faults | Where-Object { $_ -like 'toolchain *' }).Count -gt 0) -or ($faults -contains 'converter sources')) {
    $bsArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'bootstrap.ps1'))
    if ($script:ConverterMode) { $bsArgs += '-Converter' }
    & powershell @bsArgs
    if ($LASTEXITCODE -ne 0) { Write-Warning 'provision: tools\bootstrap.ps1 failed' }
  }
  if ($faults -contains 'hooks path') { Repair-Hooks }
  if ($faults -contains 'pillow' -and -not ($faults -contains 'python')) {
    $py = Get-Python
    & $py.Path -m pip install --quiet pillow
  }
  if ($faults -contains 'basedpyright' -and -not ($faults -contains 'python')) {
    $py = Get-Python
    if (-not (Test-Path (Join-Path $Root '.venv\Scripts\python.exe'))) {
      & $py.Path -m venv (Join-Path $Root '.venv')
    }
    & (Join-Path $Root '.venv\Scripts\python.exe') -m pip install --quiet -r (Join-Path $Root 'requirements-dev.txt')
  }
  if ($faults -contains 'compilation database' -and -not ($faults -contains 'python')) {
    $py = Get-Python
    & $py.Path (Join-Path $Root 'scripts/gen-compdb.py')
  }
  if ($faults.Count -gt 0) { $results = @(Test-All) }
}
if (Show-Results $results) { exit 0 } else { exit 1 }
