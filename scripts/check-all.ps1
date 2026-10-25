# Compatibility wrapper for the cross-platform gate runner.
[CmdletBinding()]
param([switch]$SkipBuild, [switch]$Quick, [switch]$AllTargets, [string]$Gate)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$forward = @()
if ($SkipBuild) { $forward += '--skip-build' }
if ($Quick) { $forward += '--quick' }
if ($AllTargets) { $forward += '--all-targets' }
if ($Gate) { $forward += @('--gate', $Gate) }
if ($env:OS -eq 'Windows_NT') { & python (Join-Path $root 'scripts/check-all.py') @forward }
else { & python3 (Join-Path $root 'scripts/check-all.py') @forward }
exit $LASTEXITCODE
