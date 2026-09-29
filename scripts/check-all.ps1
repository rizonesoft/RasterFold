#Requires -Version 5.1
<#
.SYNOPSIS
  Runs every local gate and prints a PASS / FAIL / SKIP table.
.DESCRIPTION
  Gates, in order:
    build                  build.ps1 (the pinned Clang through Ninja); FAIL on any compiler warning
    correctness            tests\test_correctness.py, every case
    fuzz                   tests\fuzz_decode.py 200 1 (skipped by -Quick)
    todo-graph self-test   the plan tool's own fixtures
    todo-graph validate    FATAL, or a new WARN, fails
    plan --check           the derived plan projection is current
    milestones --check     todo/05-milestones and docs/research.md section 9 match research/milestones.py
    todo-claims            every claim under todo/ still measures true
    todo-claims self-test
    findings ledger        docs/reviews/findings.md is current (todo-findings.py --check)
    findings self-test
    run records            docs/reviews/run-records.md agrees with the review files (todo-runs.py --check)
    run records self-test
    review-prompt self-test
    panel slots            .conclave/panel.toml validates
    panel slots self-test
    campaign guard self-test   the Stop hook and guard lifecycle in a throwaway workspace
    provision              tools\provision.ps1 -Verify (toolchain, hooks, Python, Pillow)
    toolchain pins         ADVISORY: scripts\toolchain-latest.ps1; never fails the run
  Every gate runs even after an earlier one fails. Each gate's full output goes to
  build\logs\<gate>.log; a failing gate prints its last 25 lines.
  -SkipBuild reuses the last build (iteration only, never for a commit-time run).
  -Quick skips the fuzz gate (iteration only).
  Run: pwsh scripts/check-all.ps1 [-SkipBuild] [-Quick]
#>
[CmdletBinding()]
param([switch]$SkipBuild, [switch]$Quick)
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$LogDir = Join-Path $RepoRoot 'build\logs'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$results = [System.Collections.Generic.List[object]]::new()

$Python = $null
foreach ($name in @('python', 'python3', 'py')) {
  $cmd = Get-Command $name -ErrorAction SilentlyContinue
  if ($cmd) {
    $v = (& $cmd.Source --version 2>&1) -join ' '
    if ($v -match 'Python 3\.') { $Python = $cmd.Source; break }
  }
}
$PowerShell = (Get-Command powershell -ErrorAction SilentlyContinue).Source

function Invoke-Gate {
  param([string]$Name, [scriptblock]$Body, [switch]$Advisory, [scriptblock]$FailWhen)
  $log = Join-Path $LogDir (($Name -replace '[^A-Za-z0-9-]+', '-').Trim('-') + '.log')
  Write-Host "==> $Name" -ForegroundColor Cyan
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $status = 'PASS'
  $global:LASTEXITCODE = 0
  try {
    $prev = $ErrorActionPreference
    # Native tools write progress to stderr; under 'Stop' Windows PowerShell
    # would turn the first stderr line into a terminating error.
    $ErrorActionPreference = 'Continue'
    $out = & $Body 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev
    $out | Set-Content -Encoding UTF8 $log
    if ($code -ne 0) { $status = "FAIL (exit $code)" }
    elseif ($FailWhen -and (& $FailWhen $out)) { $status = 'FAIL (output)' }
  } catch {
    $ErrorActionPreference = 'Stop'
    "$($_.Exception.Message)" | Set-Content -Encoding UTF8 $log
    $status = "FAIL ($($_.Exception.Message))"
  }
  if ($status -ne 'PASS' -and $Advisory) { $status = "ADVISORY ($($status -replace '^FAIL \(|\)$', ''))" }
  if ($status -like 'FAIL*') {
    Write-Host "--- last lines of $log" -ForegroundColor Red
    Get-Content $log -Tail 25 | ForEach-Object { Write-Host "    $_" }
  }
  $results.Add([pscustomobject]@{ Gate = $Name; Status = $status; Seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1) })
}

function Invoke-PythonGate([string]$Name, [string]$Script, [string[]]$Arguments) {
  $path = Join-Path $RepoRoot $Script
  if (-not $Python) {
    $results.Add([pscustomobject]@{ Gate = $Name; Status = 'SKIP (no Python 3 on PATH)'; Seconds = 0 }); return
  }
  if (-not (Test-Path $path)) {
    $results.Add([pscustomobject]@{ Gate = $Name; Status = "SKIP ($Script absent)"; Seconds = 0 }); return
  }
  $py = $Python
  Invoke-Gate $Name ({ & $py $path @Arguments }.GetNewClosure())
}

Push-Location $RepoRoot
try {
  if (-not $SkipBuild) {
    $ps = $PowerShell
    # A warning is a failed build: the code compiles clean today and stays that way.
    Invoke-Gate 'build' ({ & $ps -NoProfile -ExecutionPolicy Bypass -File (Join-Path $RepoRoot 'build.ps1') }.GetNewClosure()) `
      -FailWhen { param($o) @($o | Where-Object { $_ -match ':\d+:\d+: warning:' }).Count -gt 0 }
  }
  Invoke-PythonGate 'correctness' 'tests\test_correctness.py' @()
  if (-not $Quick) { Invoke-PythonGate 'fuzz' 'tests\fuzz_decode.py' @('200', '1') }
  Invoke-PythonGate 'todo-graph self-test' 'scripts\todo-graph.py' @('self-test')
  Invoke-PythonGate 'todo-graph validate' 'scripts\todo-graph.py' @('validate')
  Invoke-PythonGate 'plan --check' 'scripts\todo-graph.py' @('plan', '--check')
  Invoke-PythonGate 'milestones --check' 'research\milestones.py' @('--check')
  Invoke-PythonGate 'todo-claims' 'scripts\todo-claims.py' @()
  Invoke-PythonGate 'todo-claims self-test' 'scripts\todo-claims.py' @('--self-test')
  Invoke-PythonGate 'findings ledger' 'scripts\todo-findings.py' @('--check')
  Invoke-PythonGate 'findings self-test' 'scripts\todo-findings.py' @('--self-test')
  Invoke-PythonGate 'run records' 'scripts\todo-runs.py' @('--check')
  Invoke-PythonGate 'run records self-test' 'scripts\todo-runs.py' @('--self-test')
  Invoke-PythonGate 'review-prompt self-test' 'scripts\review_prompt.py' @('--self-test')
  Invoke-PythonGate 'panel slots' 'scripts\panel_slots.py' @('validate')
  Invoke-PythonGate 'panel slots self-test' 'scripts\panel_slots.py' @('--self-test')
  Invoke-PythonGate 'campaign guard self-test' 'scripts\campaign_guard.py' @('--self-test')
  $ps = $PowerShell
  Invoke-Gate 'provision' ({ & $ps -NoProfile -ExecutionPolicy Bypass -File (Join-Path $RepoRoot 'tools\provision.ps1') -Verify }.GetNewClosure())
  Invoke-Gate 'toolchain pins' ({ & $ps -NoProfile -ExecutionPolicy Bypass -File (Join-Path $RepoRoot 'scripts\toolchain-latest.ps1') }.GetNewClosure()) -Advisory
} finally {
  Pop-Location
}

$results | Format-Table -AutoSize | Out-String | Write-Host
$failed = @($results | Where-Object { $_.Status -like 'FAIL*' })
$skipped = @($results | Where-Object { $_.Status -like 'SKIP*' })
if ($failed.Count -gt 0) {
  Write-Host "check-all: $($failed.Count) of $($results.Count) gate(s) FAILED (logs in build\logs)" -ForegroundColor Red
  exit 1
}
if ($skipped.Count -gt 0) {
  Write-Host "check-all: $($skipped.Count) gate(s) SKIPPED; a skip is not green" -ForegroundColor Yellow
  exit 1
}
Write-Host "check-all: all $($results.Count) gates passed" -ForegroundColor Green
exit 0
