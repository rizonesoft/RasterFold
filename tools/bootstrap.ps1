# Download the pinned local toolstack into tools\. Safe to run again.
# Usage: powershell -ExecutionPolicy Bypass -File tools\bootstrap.ps1 [-Force] [-Converter]
#
# The pins live in toolchain.json at the repository root (name, version, URL,
# SHA-256, size, install folder, probe file, and how to read the version).
# Components with "scope": "converter" and converter/vendor.json sources are
# staged only when -Converter is supplied.
param(
    [switch]$Force,
    [switch]$Converter
)
$ErrorActionPreference = "Stop"
$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $tools
if ($env:OS -ne 'Windows_NT') {
    $forward = @()
    if ($Converter) { $forward += '--converter' }
    & python3 (Join-Path $root 'tools/bootstrap.py') @forward
    exit $LASTEXITCODE
}
$manifestPath = Join-Path $root "toolchain.json"
if (-not (Test-Path $manifestPath)) { throw "bootstrap: toolchain.json not found at $manifestPath" }
$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
$cache = Join-Path $tools "downloads"
New-Item -ItemType Directory -Force -Path $cache | Out-Null
# Windows ships libarchive's bsdtar, which reads .zip, .tar.*, and .7z.
$bsdtar = Join-Path $env:SystemRoot "System32\tar.exe"

function Get-Sha256($path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $fs = [System.IO.File]::OpenRead($path)
    try { return ([BitConverter]::ToString($sha.ComputeHash($fs)) -replace '-', '').ToLowerInvariant() } finally { $fs.Dispose(); $sha.Dispose() }
}

function Write-Utf8NoBom($path, $text) {
    [System.IO.File]::WriteAllText($path, $text, [System.Text.UTF8Encoding]::new($false))
}

function Test-MovingRef($value) {
    if ($null -eq $value) { return $false }
    $low = ([string]$value).Trim().ToLowerInvariant()
    if ($low -in @('main', 'master', 'head', 'latest', 'trunk', 'dev', 'develop', 'nightly', 'edge')) { return $true }
    if ($low.StartsWith('refs/heads/') -or $low.Contains('/heads/')) { return $true }
    foreach ($marker in @('/archive/main.', '/archive/master.', '/archive/head.', '/tarball/main', '/tarball/master', '/zipball/main', '/zipball/master')) {
        if ($low.Contains($marker)) { return $true }
    }
    return $false
}

function Assert-SafeArchive($archive) {
    $entries = @(& $bsdtar -tvf $archive 2>$null)
    if ($LASTEXITCODE -ne 0) {
        $entries = @(& $bsdtar -tf $archive 2>$null)
        if ($LASTEXITCODE -ne 0) { throw "bootstrap: unable to inspect archive $archive" }
    }
    foreach ($line in $entries) {
        $trimmed = ([string]$line).Trim()
        if (-not $trimmed) { continue }
        # Strip bsdtar -tvf permissions/owner/date columns when present
        $memberPath = $trimmed
        $linkTarget = $null
        if ($trimmed -match '^[dlcbps-][rwxstST-]{9}\s+.*?\s+\S+\s+\S+\s+\S+\s+(.+)$') {
            $rest = $Matches[1]
            if ($rest -match '^(.+?)\s+->\s+(.+)$') {
                $memberPath = $Matches[1]
                $linkTarget = $Matches[2]
            } else {
                $memberPath = $rest
            }
        }
        $norm = $memberPath -replace '\\', '/'
        if ($norm.StartsWith('/') -or $norm -match '^[A-Za-z]:') {
            throw "bootstrap: archive member escapes extraction root: $memberPath"
        }
        if ($norm -split '/' -contains '..') {
            throw "bootstrap: archive member escapes extraction root: $memberPath"
        }
        if ($null -ne $linkTarget) {
            $tnorm = $linkTarget -replace '\\', '/'
            if ($tnorm.StartsWith('/') -or $tnorm -match '^[A-Za-z]:') {
                throw "bootstrap: archive symlink escapes extraction root: $memberPath -> $linkTarget"
            }
            $parentDir = Split-Path -Path ($norm.TrimEnd('/')) -Parent
            $combined = if ($parentDir) { "$parentDir/$tnorm" } else { $tnorm }
            $depth = 0
            foreach ($seg in ($combined -split '/')) {
                if (-not $seg -or $seg -eq '.') { continue }
                if ($seg -eq '..') {
                    $depth -= 1
                    if ($depth -lt 0) { throw "bootstrap: archive symlink escapes extraction root: $memberPath -> $linkTarget" }
                } else {
                    $depth += 1
                }
            }
        }
    }
}

function Fetch-VerifiedFile($entry, $cacheDir) {
    $archiveName = if ($entry.PSObject.Properties['archive'] -and $entry.archive) { [string]$entry.archive } else { [IO.Path]::GetFileName($entry.url) }
    if (-not $archiveName -or $archiveName.Contains('/') -or $archiveName.Contains('\') -or $archiveName.Contains('..')) {
        throw "bootstrap: invalid archive name: $archiveName"
    }
    $file = Join-Path $cacheDir $archiveName
    $want = ([string]$entry.sha256).ToLowerInvariant()
    if (-not (Test-Path $file) -or (Get-Sha256 $file) -ne $want) {
        Write-Host "download $($entry.url)"
        $ProgressPreference = "SilentlyContinue"
        $partial = "$file.partial"
        Invoke-WebRequest -Uri $entry.url -OutFile $partial -UseBasicParsing
        Move-Item -Force $partial $file
    }
    $size = (Get-Item $file).Length
    if ($entry.bytes -and $size -ne [int64]$entry.bytes) { throw "bootstrap: $($entry.url): $file is $size bytes, pinned $($entry.bytes)" }
    $hash = Get-Sha256 $file
    if ($hash -ne $want) { throw "bootstrap: SHA-256 mismatch for $file`n  got  $hash`n  want $want" }
    return $file
}

function Get-TreeSha256($treeRoot) {
    $rootRes = [IO.Path]::GetFullPath($treeRoot).TrimEnd('\', '/')
    $files = Get-ChildItem -Path $treeRoot -Recurse -File -Force | Where-Object {
        $_.Name -ne 'PINNED.json' -and -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint)
    }
    $records = [System.Collections.Generic.List[object]]::new()
    foreach ($f in $files) {
        $full = [IO.Path]::GetFullPath($f.FullName)
        $rel = $full.Substring($rootRes.Length + 1).Replace('\', '/')
        $records.Add([pscustomobject]@{ Rel = $rel; File = $f })
    }
    $records.Sort([System.Comparison[object]]{ param($a, $b) [string]::CompareOrdinal($a.Rel, $b.Rel) })
    $shaAll = [System.Security.Cryptography.SHA256]::Create()
    try {
        $ms = [System.IO.MemoryStream]::new()
        $utf8 = [System.Text.UTF8Encoding]::new($false)
        foreach ($r in $records) {
            $lineBytes = $utf8.GetBytes("$($r.Rel)`0$($r.File.Length)`0$(Get-Sha256 $r.File.FullName)`n")
            $ms.Write($lineBytes, 0, $lineBytes.Length)
        }
        $ms.Position = 0
        return ([BitConverter]::ToString($shaAll.ComputeHash($ms)) -replace '-', '').ToLowerInvariant()
    } finally {
        $shaAll.Dispose()
    }
}

function Read-GzipUtf8($gzPath) {
    $fs = [System.IO.File]::OpenRead($gzPath)
    try {
        $gz = [System.IO.Compression.GZipStream]::new($fs, [System.IO.Compression.CompressionMode]::Decompress)
        try {
            $sr = [System.IO.StreamReader]::new($gz, [System.Text.UTF8Encoding]::new($false))
            return $sr.ReadToEnd()
        } finally { $gz.Dispose() }
    } finally { $fs.Dispose() }
}

function Apply-UnifiedPatch($srcRoot, $patchText, [int]$strip = 1) {
    $rawLines = $patchText -split "`r?`n"
    if ($patchText.EndsWith("`n") -and $rawLines.Count -gt 0 -and $rawLines[$rawLines.Count - 1] -eq '') {
        $lines = $rawLines[0..($rawLines.Count - 2)]
    } else {
        $lines = $rawLines
    }
    $i = 0
    $applied = 0
    $rootRes = [IO.Path]::GetFullPath($srcRoot).TrimEnd('\', '/')
    while ($i -lt $lines.Count) {
        $line = $lines[$i]
        if (-not $line.StartsWith('--- ')) { $i += 1; continue }
        $oldRaw = ($line.Substring(4) -split "`t", 2)[0]
        if ($i + 1 -ge $lines.Count -or -not $lines[$i + 1].StartsWith('+++ ')) {
            throw "bootstrap: malformed patch header near line $($i + 1)"
        }
        $newRaw = ($lines[$i + 1].Substring(4) -split "`t", 2)[0]
        $i += 2
        if ($newRaw -eq '/dev/null') { throw "bootstrap: patch file deletion unsupported" }
        $parts = @($newRaw -split '/')
        if ($parts.Count -le $strip) { throw "bootstrap: patch path shorter than strip=${strip}: $newRaw" }
        $rel = ($parts[$strip..($parts.Count - 1)] -join '/')
        if (-not $rel -or $rel.Contains('\') -or ($parts -contains '..') -or $rel.StartsWith('/')) {
            throw "bootstrap: patch path escapes source root: $rel"
        }
        $target = [IO.Path]::GetFullPath((Join-Path $rootRes ($rel -replace '/', '\')))
        if (-not $target.StartsWith("$rootRes\", [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "bootstrap: patch path escapes source root: $rel"
        }
        if ($oldRaw -eq '/dev/null') {
            $srcLines = @()
            $hadTrailing = $true
        } else {
            if (-not (Test-Path $target)) { throw "bootstrap: patch target missing: $rel" }
            $rawTarget = [System.IO.File]::ReadAllText($target, [System.Text.UTF8Encoding]::new($false))
            $hadTrailing = $rawTarget.EndsWith("`n")
            $splitTarget = $rawTarget -split "`r?`n"
            $srcLines = if ($hadTrailing -and $splitTarget.Count -gt 0 -and $splitTarget[$splitTarget.Count - 1] -eq '') {
                @($splitTarget[0..($splitTarget.Count - 2)])
            } else { @($splitTarget) }
        }
        $outLines = [System.Collections.Generic.List[string]]::new()
        $cursor = 0
        while ($i -lt $lines.Count -and $lines[$i].StartsWith('@@ ')) {
            if ($lines[$i] -notmatch '^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@') {
                throw "bootstrap: malformed patch hunk: $($lines[$i])"
            }
            $oldStart = [int]$Matches[1]
            $oldLen = if ($Matches[2]) { [int]$Matches[2] } else { 1 }
            $i += 1
            $targetIdx = if ($oldStart -eq 0 -and $oldLen -eq 0) { 0 } else { $oldStart - 1 }
            if ($targetIdx -lt $cursor -or $targetIdx -gt $srcLines.Count) {
                throw "bootstrap: patch hunk out of bounds in $rel at $oldStart"
            }
            while ($cursor -lt $targetIdx) { $outLines.Add($srcLines[$cursor]); $cursor += 1 }
            $seenOld = 0
            while ($i -lt $lines.Count -and -not ($lines[$i].StartsWith('@@ ') -or $lines[$i].StartsWith('--- ') -or $lines[$i].StartsWith('diff '))) {
                $hl = $lines[$i]
                if ($hl.StartsWith(' ')) {
                    if ($cursor -ge $srcLines.Count -or $srcLines[$cursor] -ne $hl.Substring(1)) {
                        throw "bootstrap: patch context mismatch in $rel at line $($cursor + 1)"
                    }
                    $outLines.Add($srcLines[$cursor]); $cursor += 1; $seenOld += 1
                } elseif ($hl.StartsWith('-')) {
                    if ($cursor -ge $srcLines.Count -or $srcLines[$cursor] -ne $hl.Substring(1)) {
                        throw "bootstrap: patch deletion mismatch in $rel at line $($cursor + 1)"
                    }
                    $cursor += 1; $seenOld += 1
                } elseif ($hl.StartsWith('+')) {
                    $outLines.Add($hl.Substring(1))
                } elseif ($hl.StartsWith('\')) {
                    $hadTrailing = $false
                } else {
                    throw "bootstrap: unexpected patch line in ${rel}: $hl"
                }
                $i += 1
            }
            if ($seenOld -ne $oldLen) {
                throw "bootstrap: patch old line count mismatch in ${rel}: expected $oldLen, got $seenOld"
            }
        }
        while ($cursor -lt $srcLines.Count) { $outLines.Add($srcLines[$cursor]); $cursor += 1 }
        $parent = Split-Path -Path $target -Parent
        if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
        $joined = ($outLines -join "`n") + $(if ($hadTrailing) { "`n" } else { "" })
        Write-Utf8NoBom $target $joined
        $applied += 1
    }
    if ($applied -eq 0) { throw "bootstrap: patch contained zero file hunks" }
}

function Get-InstalledVersion($c, $probe) {
    try {
        $out = (& $probe @($c.versionArgs) 2>&1 | Out-String)
        if ($out -match $c.versionMatch) { return $Matches[1] }
    } catch { }
    return $null
}

foreach ($c in $manifest.components) {
    $isConverter = ($c.PSObject.Properties['scope'] -and $c.scope -eq 'converter')
    if ($isConverter -and -not $Converter) { continue }
    if ($c.dir -notmatch '^[A-Za-z0-9._-]+$') { throw "bootstrap: $($c.name): dir '$($c.dir)' is not a plain folder name under tools\" }
    $dest = Join-Path $tools $c.dir
    $probe = Join-Path $dest ($c.probe -replace '/', '\')
    if ((Test-Path $probe) -and -not $Force) {
        $have = Get-InstalledVersion $c $probe
        if ($have -eq $c.expectVersion) { Write-Host "ok $($c.name) $have"; continue }
        Write-Host "$($c.name): found version '$have', pinned $($c.expectVersion); reinstalling"
    }
    $file = Fetch-VerifiedFile $c $cache
    Assert-SafeArchive $file

    $stage = Join-Path $cache "stage-$($c.dir)"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    & $bsdtar -xf $file -C $stage
    if ($LASTEXITCODE -ne 0) { throw "bootstrap: extract failed: $file" }
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

if ($Converter) {
    $vendorPath = Join-Path $root "converter\vendor.json"
    if (-not (Test-Path $vendorPath)) { throw "bootstrap: converter/vendor.json not found at $vendorPath" }
    $vendor = Get-Content $vendorPath -Raw | ConvertFrom-Json
    if ($vendor.schema -ne 1) { throw "bootstrap: converter/vendor.json schema must be 1" }
    foreach ($sys in @($vendor.system_codecs)) {
        if ($sys.bundled -or $sys.hevc_bundled -or $sys.hevc_source_or_binary_present) {
            throw "bootstrap: system codecs must not bundle HEVC"
        }
    }
    $converterSrc = Join-Path $tools "converter-src"
    New-Item -ItemType Directory -Force -Path $converterSrc | Out-Null
    $seenIds = @{}
    foreach ($pkg in $vendor.packages) {
        if (-not $pkg.bundled) { throw "bootstrap: vendor $($pkg.id) cannot be unbundled" }
        if ($pkg.PSObject.Properties['branch'] -and $pkg.branch) { throw "bootstrap: vendor $($pkg.id) forbids moving branch ref" }
        if ($pkg.commit -notmatch '^[0-9a-f]{40}$') { throw "bootstrap: vendor $($pkg.id) requires 40-char commit hex" }
        if ($null -ne $pkg.tag -and (Test-MovingRef $pkg.tag)) { throw "bootstrap: vendor $($pkg.id) refuses moving tag: $($pkg.tag)" }
        if ((Test-MovingRef $pkg.url) -or (Test-MovingRef $pkg.repo)) { throw "bootstrap: vendor $($pkg.id) URL uses moving ref" }
        if ($pkg.sha256 -notmatch '^[0-9a-f]{64}$') { throw "bootstrap: vendor $($pkg.id) invalid sha256" }
        foreach ($d in @($pkg.deps)) {
            if (-not $seenIds.ContainsKey($d)) { throw "bootstrap: vendor $($pkg.id) dep '$d' out of topological order" }
        }
        $seenIds[$pkg.id] = $true

        $dest = Join-Path $converterSrc $pkg.id
        $receiptPath = Join-Path $dest "PINNED.json"
        if ((Test-Path $receiptPath) -and -not $Force) {
            $rcpt = Get-Content $receiptPath -Raw | ConvertFrom-Json
            if ($rcpt.id -eq $pkg.id -and $rcpt.version -eq $pkg.version -and $rcpt.commit -eq $pkg.commit -and $rcpt.sha256 -eq $pkg.sha256 -and (Get-TreeSha256 $dest) -eq $rcpt.tree_sha256) {
                Write-Host "ok vendor:$($pkg.id) $($pkg.version)"
                continue
            }
            Write-Host "vendor:$($pkg.id): receipt or tree changed; restaging"
        }

        $file = Fetch-VerifiedFile $pkg $cache
        Assert-SafeArchive $file
        $patchRecords = @()
        $patchFiles = @()
        foreach ($p in @($pkg.patches)) {
            if ($p.sha256 -notmatch '^[0-9a-f]{64}$' -or (Test-MovingRef $p.url)) {
                throw "bootstrap: invalid patch metadata in $($pkg.id)"
            }
            $pf = Fetch-VerifiedFile $p $cache
            $patchFiles += @{ Spec = $p; Path = $pf }
            $patchRecords += @{ name = $p.name; sha256 = $p.sha256 }
        }

        $stage = Join-Path $cache "stage-vendor-$($pkg.id)"
        if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
        New-Item -ItemType Directory -Force -Path $stage | Out-Null
        & $bsdtar -xf $file -C $stage
        if ($LASTEXITCODE -ne 0) { throw "bootstrap: extract failed: $file" }
        $top = @(Get-ChildItem $stage)
        $src = if ($top.Count -eq 1 -and $top[0].PSIsContainer) { $top[0].FullName } else { $stage }
        foreach ($pf in $patchFiles) {
            $ptext = if ($pf.Path.EndsWith('.gz')) { Read-GzipUtf8 $pf.Path } else { [System.IO.File]::ReadAllText($pf.Path, [System.Text.UTF8Encoding]::new($false)) }
            $strip = if ($pf.Spec.PSObject.Properties['strip']) { [int]$pf.Spec.strip } else { 1 }
            Apply-UnifiedPatch $src $ptext $strip
        }
        $treeHash = Get-TreeSha256 $src
        $receiptJson = @{
            id = $pkg.id
            version = $pkg.version
            commit = $pkg.commit
            url = $pkg.url
            sha256 = $pkg.sha256
            bytes = $pkg.bytes
            patches = $patchRecords
            tree_sha256 = $treeHash
        } | ConvertTo-Json -Depth 4
        Write-Utf8NoBom (Join-Path $src "PINNED.json") ($receiptJson + "`n")
        if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
        Move-Item $src $dest
        if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
        Write-Host "staged vendor:$($pkg.id) $($pkg.version)"
    }
}
