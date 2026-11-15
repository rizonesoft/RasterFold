# Download the pinned local toolstack into tools\. Safe to run again.
# Usage: powershell -ExecutionPolicy Bypass -File tools\bootstrap.ps1 [-Force] [-Converter]
#
# The pins live in toolchain.json at the repository root (name, version, URL,
# SHA-256, size, install folder, probe file, and how to read the version).
# Components with "scope": "converter" and converter/vendor.json sources are
# staged only when -Converter is supplied.
param(
    [switch]$Force,
    [switch]$Converter,
    [switch]$HelpersOnly
)
$ErrorActionPreference = "Stop"
$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $tools
if (-not $HelpersOnly -and $MyInvocation.InvocationName -ne '.' -and $env:OS -ne 'Windows_NT') {
    $forward = @()
    if ($Converter) { $forward += '--converter' }
    & python3 (Join-Path $root 'tools/bootstrap.py') @forward
    exit $LASTEXITCODE
}
$manifestPath = Join-Path $root "toolchain.json"
$cache = Join-Path $tools "downloads"
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

function Get-InstalledVersion($c, $probe) {
    try {
        $out = (& $probe @($c.versionArgs) 2>&1 | Out-String)
        if ($c.PSObject.Properties['versionMatch'] -and $c.versionMatch) {
            if ($out -match $c.versionMatch) { return $Matches[1] }
        } elseif ($out.Contains([string]$c.expectVersion)) {
            return [string]$c.expectVersion
        }
        return $null
    } catch {
        return "<exec error: $($_.Exception.Message)>"
    }
}

function Get-ComponentProbes($c) {
    if ($c.PSObject.Properties['probes'] -and $c.probes) {
        return @($c.probes | ForEach-Object { [string]$_ })
    }
    return @([string]$c.probe)
}

function Test-PinnedComponent($c, $toolsDir) {
    if ($c.dir -notmatch '^[A-Za-z0-9._-]+$') {
        return @{ Ok = $false; Version = $null; Reason = "invalid dir '$($c.dir)'" }
    }
    $dest = Join-Path $toolsDir $c.dir
    $probe = Join-Path $dest ($c.probe -replace '/', '\')
    if (-not (Test-Path -LiteralPath $probe)) {
        return @{ Ok = $false; Version = $null; Reason = "missing $probe" }
    }
    $rcptPath = Join-Path $dest 'PINNED.json'
    if (-not (Test-Path -LiteralPath $rcptPath)) {
        return @{ Ok = $false; Version = $null; Reason = "missing PINNED.json in $dest" }
    }
    try {
        $rcpt = Get-Content -LiteralPath $rcptPath -Raw | ConvertFrom-Json
    } catch {
        return @{ Ok = $false; Version = $null; Reason = "unreadable PINNED.json in $dest" }
    }
    if ($rcpt.url -ne $c.url -or $rcpt.sha256 -ne $c.sha256) {
        return @{ Ok = $false; Version = $null; Reason = 'installation receipt disagrees with manifest' }
    }
    if (-not ($rcpt.PSObject.Properties['probes']) -or $null -eq $rcpt.probes) {
        return @{ Ok = $false; Version = $null; Reason = 'installation receipt omits verification probes' }
    }
    $wantProbes = @(Get-ComponentProbes $c)
    $haveProps = @($rcpt.probes.PSObject.Properties)
    if ($haveProps.Count -ne $wantProbes.Count) {
        return @{ Ok = $false; Version = $null; Reason = 'installation receipt omits or adds verification probes' }
    }
    foreach ($pName in $wantProbes) {
        $prop = $rcpt.probes.PSObject.Properties[$pName]
        if (-not $prop) {
            return @{ Ok = $false; Version = $null; Reason = "installation receipt omits probe: $pName" }
        }
        $pPath = Join-Path $dest ($pName -replace '/', '\')
        if (-not (Test-Path -LiteralPath $pPath)) {
            return @{ Ok = $false; Version = $null; Reason = "missing probe binary: $pName" }
        }
        if ((Get-Sha256 $pPath) -ne [string]$prop.Value) {
            return @{ Ok = $false; Version = $null; Reason = "installed probe changed: $pName" }
        }
    }
    $have = Get-InstalledVersion $c $probe
    if ($have -ne $c.expectVersion) {
        return @{ Ok = $false; Version = $have; Reason = "found '$have', pinned $($c.expectVersion)" }
    }
    return @{ Ok = $true; Version = $have; Reason = '' }
}

function Invoke-WithLocalArchive($archive, [scriptblock]$action) {
    $resolved = (Resolve-Path -LiteralPath $archive).ProviderPath
    if (-not $resolved.StartsWith('\\')) {
        return (& $action $resolved)
    }
    $tmpArc = Join-Path ([IO.Path]::GetTempPath()) ("rf_arc_" + [guid]::NewGuid().ToString("N") + "_" + [IO.Path]::GetFileName($resolved))
    [IO.File]::Copy($resolved, $tmpArc, $true)
    try {
        return (& $action $tmpArc)
    } finally {
        if (Test-Path -LiteralPath $tmpArc) { Remove-Item -Force -LiteralPath $tmpArc }
    }
}

function Assert-SafeArchive($archive) {
    $entries = Invoke-WithLocalArchive $archive {
        param($localArc)
        $out = @(& $bsdtar -tvf $localArc 2>$null)
        if ($LASTEXITCODE -ne 0) {
            $out = @(& $bsdtar -tf $localArc 2>$null)
            if ($LASTEXITCODE -ne 0) { throw "bootstrap: unable to inspect archive $archive" }
        }
        return ,$out
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

function Resolve-LexicalRelPath($baseRelPosix, $rawTarget) {
    $tnorm = $rawTarget.Replace('\', '/')
    if ([string]::IsNullOrEmpty($tnorm) -or $tnorm.StartsWith('/') -or ($tnorm -match '^[A-Za-z]:')) {
        return $null
    }
    $segs = [System.Collections.Generic.List[string]]::new()
    if ($baseRelPosix) {
        foreach ($part in ($baseRelPosix -split '/')) {
            if ($part -and $part -ne '.') { $segs.Add($part) }
        }
    }
    foreach ($part in ($tnorm -split '/')) {
        if (-not $part -or $part -eq '.') { continue }
        if ($part -eq '..') {
            if ($segs.Count -eq 0) { return $null }
            $segs.RemoveAt($segs.Count - 1)
        } else {
            $segs.Add($part)
        }
    }
    return ($segs -join '/')
}

function Assert-SafeSymlinkMap($rootRes, $linkRecords, [bool]$isWslShare) {
    $linkMap = @{}
    foreach ($rec in $linkRecords) {
        $linkMap[$rec.Rel] = $rec.RawTarget.Replace('\', '/')
    }
    foreach ($rec in $linkRecords) {
        $rel = $rec.Rel
        $rawTarget = $rec.RawTarget
        $tnorm = $linkMap[$rel]
        $lastSlash = $rel.LastIndexOf('/')
        $curParent = if ($lastSlash -gt 0) { $rel.Substring(0, $lastSlash) } else { '' }
        $resolved = Resolve-LexicalRelPath $curParent $tnorm
        if ($null -eq $resolved) {
            throw "bootstrap: vendor source symlink escapes root: $rel -> $rawTarget"
        }
        if (-not $isWslShare) {
            $fullResolved = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $rec.FullPath) ($tnorm -replace '/', '\')))
            if (-not $fullResolved.Equals($rootRes, [System.StringComparison]::OrdinalIgnoreCase) -and
                -not $fullResolved.StartsWith("$rootRes\", [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "bootstrap: vendor source symlink escapes root: $rel -> $rawTarget"
            }
        }
        $hops = 0
        $probeRel = $resolved
        while ($true) {
            $hops += 1
            if ($hops -gt 32) {
                throw "bootstrap: vendor source symlink loop or depth limit in tree: $rel"
            }
            if (-not $probeRel) { break }
            $parts = @($probeRel -split '/')
            $replaced = $false
            $prefix = ''
            for ($pi = 0; $pi -lt $parts.Count; $pi++) {
                $prefix = if ($prefix) { "$prefix/$($parts[$pi])" } else { $parts[$pi] }
                if ($linkMap.ContainsKey($prefix)) {
                    $pSlash = $prefix.LastIndexOf('/')
                    $pParent = if ($pSlash -gt 0) { $prefix.Substring(0, $pSlash) } else { '' }
                    $baseStep = Resolve-LexicalRelPath $pParent $linkMap[$prefix]
                    if ($null -eq $baseStep) {
                        throw "bootstrap: vendor source symlink escapes root: $rel -> $rawTarget"
                    }
                    $tail = if ($pi + 1 -lt $parts.Count) { ($parts[($pi + 1)..($parts.Count - 1)] -join '/') } else { '' }
                    if ($tail) {
                        $probeRel = Resolve-LexicalRelPath $baseStep $tail
                        if ($null -eq $probeRel) {
                            throw "bootstrap: vendor source symlink escapes root: $rel -> $rawTarget"
                        }
                    } else {
                        $probeRel = $baseStep
                    }
                    $replaced = $true
                    break
                }
            }
            if (-not $replaced) { break }
        }
    }
    return $linkMap
}

function Get-TreeSha256($treeRoot) {
    $resolvedRoot = (Resolve-Path -LiteralPath $treeRoot).ProviderPath
    $rootRes = $resolvedRoot.TrimEnd('\', '/')
    if (([IO.File]::GetAttributes($rootRes) -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "bootstrap: vendor source root must not be a symlink: $treeRoot"
    }
    $isWslShare = ($rootRes -match '^\\\\wsl(?:\.localhost|\$)\\([^\\]+)\\(.+)$')
    $wslDistro = if ($isWslShare) { $Matches[1] } else { $null }
    $wslLinuxRoot = if ($isWslShare) { '/' + ($Matches[2].Replace('\', '/')) } else { $null }

    $records = [System.Collections.Generic.List[object]]::new()
    $linkRecords = [System.Collections.Generic.List[object]]::new()
    $needWslReadlink = [System.Collections.Generic.List[object]]::new()
    $stack = [System.Collections.Generic.Stack[System.IO.DirectoryInfo]]::new()
    $stack.Push([System.IO.DirectoryInfo]::new($rootRes))

    while ($stack.Count -gt 0) {
        $dir = $stack.Pop()
        foreach ($sub in $dir.GetDirectories()) {
            $rel = $sub.FullName.Substring($rootRes.Length + 1).Replace('\', '/')
            if (($sub.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                $it = Get-Item -LiteralPath $sub.FullName -Force
                $rawTarget = if ($it.LinkType -eq 'SymbolicLink' -and $it.Target) { [string](@($it.Target)[0]) } else { $null }
                $rec = [pscustomobject]@{ Rel = $rel; Kind = 'L'; FullPath = $sub.FullName; File = $null; RawTarget = $rawTarget }
                if ($null -eq $rawTarget) {
                    if (-not $isWslShare) { throw "bootstrap: unsupported or unreadable reparse point in vendor tree: $rel" }
                    $needWslReadlink.Add($rec)
                }
                $linkRecords.Add($rec)
                $records.Add($rec)
            } else {
                $stack.Push($sub)
            }
        }
        foreach ($f in $dir.GetFiles()) {
            $rel = $f.FullName.Substring($rootRes.Length + 1).Replace('\', '/')
            $isRp = (($f.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
            if ($rel -eq 'PINNED.json') {
                if ($isRp) { throw 'bootstrap: PINNED.json must not be a symlink' }
                continue
            }
            if ($isRp) {
                $it = Get-Item -LiteralPath $f.FullName -Force
                $rawTarget = if ($it.LinkType -eq 'SymbolicLink' -and $it.Target) { [string](@($it.Target)[0]) } else { $null }
                $rec = [pscustomobject]@{ Rel = $rel; Kind = 'L'; FullPath = $f.FullName; File = $null; RawTarget = $rawTarget }
                if ($null -eq $rawTarget) {
                    if (-not $isWslShare) { throw "bootstrap: unsupported or unreadable reparse point in vendor tree: $rel" }
                    $needWslReadlink.Add($rec)
                }
                $linkRecords.Add($rec)
                $records.Add($rec)
            } else {
                $records.Add([pscustomobject]@{ Rel = $rel; Kind = 'F'; FullPath = $f.FullName; File = $f; RawTarget = $null })
            }
        }
    }

    if ($needWslReadlink.Count -gt 0) {
        $relArgs = @($needWslReadlink | ForEach-Object { $_.Rel })
        $pyCmd = 'import json,os,sys; root=sys.argv[1]; print(json.dumps({r:os.readlink(os.path.join(root,r)) for r in sys.argv[2:]}))'
        $rawJson = (& wsl.exe -d $wslDistro -- python3 -c $pyCmd $wslLinuxRoot @relArgs | Out-String).Trim()
        if ($LASTEXITCODE -ne 0 -or -not $rawJson) {
            throw "bootstrap: unable to read WSL symlink targets under $wslLinuxRoot"
        }
        $targetMap = $rawJson | ConvertFrom-Json
        foreach ($rec in $needWslReadlink) {
            $prop = $targetMap.PSObject.Properties[$rec.Rel]
            if (-not $prop -or [string]::IsNullOrEmpty([string]$prop.Value)) {
                throw "bootstrap: missing WSL symlink target for $($rec.Rel)"
            }
            $rec.RawTarget = [string]$prop.Value
        }
    }

    $linkMap = Assert-SafeSymlinkMap $rootRes $linkRecords ([bool]$isWslShare)
    $records.Sort([System.Comparison[object]]{ param($a, $b) [string]::CompareOrdinal($a.Rel, $b.Rel) })
    $shaAll = [System.Security.Cryptography.SHA256]::Create()
    try {
        $ms = [System.IO.MemoryStream]::new()
        $utf8 = [System.Text.UTF8Encoding]::new($false)
        foreach ($r in $records) {
            if ($r.Kind -eq 'L') {
                $lineBytes = $utf8.GetBytes("L`0$($r.Rel)`0$($linkMap[$r.Rel])`n")
            } else {
                $lineBytes = $utf8.GetBytes("F`0$($r.Rel)`0$($r.File.Length)`0$(Get-Sha256 $r.FullPath)`n")
            }
            $ms.Write($lineBytes, 0, $lineBytes.Length)
        }
        $ms.Position = 0
        return ([BitConverter]::ToString($shaAll.ComputeHash($ms)) -replace '-', '').ToLowerInvariant()
    } finally {
        $shaAll.Dispose()
    }
}

function Test-VendorReceipt($pkg, $dest) {
    $receiptPath = Join-Path $dest "PINNED.json"
    if (-not (Test-Path -LiteralPath $receiptPath)) { return $false }
    $rcpt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    if ($rcpt.id -ne $pkg.id -or
        $rcpt.version -ne $pkg.version -or
        $rcpt.commit -ne $pkg.commit -or
        $rcpt.url -ne $pkg.url -or
        $rcpt.sha256 -ne $pkg.sha256 -or
        [int64]$rcpt.bytes -ne [int64]$pkg.bytes) {
        return $false
    }
    $wantPatches = @()
    if ($pkg.PSObject.Properties['patches'] -and $null -ne $pkg.patches) { $wantPatches = @($pkg.patches) }
    $havePatches = @()
    if ($rcpt.PSObject.Properties['patches'] -and $null -ne $rcpt.patches) { $havePatches = @($rcpt.patches) }
    if ($wantPatches.Count -ne $havePatches.Count) { return $false }
    for ($pi = 0; $pi -lt $wantPatches.Count; $pi++) {
        if ($havePatches[$pi].name -ne $wantPatches[$pi].name -or $havePatches[$pi].sha256 -ne $wantPatches[$pi].sha256) {
            return $false
        }
    }
    return ((Get-TreeSha256 $dest) -eq $rcpt.tree_sha256)
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

if ($HelpersOnly -or $MyInvocation.InvocationName -eq '.') { return }

if (-not (Test-Path $manifestPath)) { throw "bootstrap: toolchain.json not found at $manifestPath" }
$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path $cache | Out-Null

function Move-StageToDest($srcDir, $destDir, $stageDir) {
    if (Test-Path -LiteralPath $destDir) { Remove-Item -Recurse -Force -LiteralPath $destDir }
    $resolvedDestParent = (Resolve-Path -LiteralPath (Split-Path -Parent $destDir)).ProviderPath
    $resolvedSrc = (Resolve-Path -LiteralPath $srcDir).ProviderPath
    if ($resolvedDestParent.StartsWith('\\') -or $resolvedSrc.StartsWith('\\')) {
        & robocopy.exe $resolvedSrc $destDir /E /NFL /NDL /NJH /NJS /NC /NS /NP /MT:8 | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "bootstrap: robocopy failed ($LASTEXITCODE) copying to $destDir" }
        $global:LASTEXITCODE = 0
    } else {
        Move-Item -LiteralPath $resolvedSrc -Destination $destDir
    }
    if (Test-Path -LiteralPath $stageDir) { Remove-Item -Recurse -Force -LiteralPath $stageDir }
}

foreach ($c in $manifest.components) {
    $isConverter = ($c.PSObject.Properties['scope'] -and $c.scope -eq 'converter')
    if ($isConverter -and -not $Converter) { continue }
    if ($c.dir -notmatch '^[A-Za-z0-9._-]+$') { throw "bootstrap: $($c.name): dir '$($c.dir)' is not a plain folder name under tools\" }
    $dest = Join-Path $tools $c.dir
    $probe = Join-Path $dest ($c.probe -replace '/', '\')
    if ((Test-Path -LiteralPath $dest) -and -not $Force) {
        $chk = Test-PinnedComponent $c $tools
        if ($chk.Ok) { Write-Host "ok $($c.name) $($chk.Version)"; continue }
        Write-Host "$($c.name): $($chk.Reason); reinstalling"
    }
    $file = Fetch-VerifiedFile $c $cache
    Assert-SafeArchive $file

    $resolvedCache = (Resolve-Path -LiteralPath $cache).ProviderPath
    $stage = if ($resolvedCache.StartsWith('\\')) {
        Join-Path ([IO.Path]::GetTempPath()) ("rf-stage-$($c.dir)-" + [guid]::NewGuid().ToString("N"))
    } else {
        Join-Path $cache "stage-$($c.dir)"
    }
    if (Test-Path -LiteralPath $stage) { Remove-Item -Recurse -Force -LiteralPath $stage }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Invoke-WithLocalArchive $file {
        param($localArc)
        & $bsdtar -xf $localArc -C $stage
        if ($LASTEXITCODE -ne 0) { throw "bootstrap: extract failed: $file" }
    }
    $top = @(Get-ChildItem $stage)
    $src = if ($top.Count -eq 1 -and $top[0].PSIsContainer) { $top[0].FullName } else { $stage }
    if (-not (Test-Path -LiteralPath (Join-Path $src ($c.probe -replace '/', '\')))) {
        throw "bootstrap: $($c.name): $($c.probe) missing after extract"
    }
    $probeMap = [ordered]@{}
    foreach ($pName in (Get-ComponentProbes $c)) {
        $pPath = Join-Path $src ($pName -replace '/', '\')
        if (-not (Test-Path -LiteralPath $pPath)) {
            throw "bootstrap: $($c.name): required probe '$pName' missing after extract"
        }
        $probeMap[$pName] = Get-Sha256 $pPath
    }
    $receiptJson = @{
        url = $c.url
        sha256 = $c.sha256
        probes = $probeMap
    } | ConvertTo-Json -Depth 4
    Write-Utf8NoBom (Join-Path $src "PINNED.json") ($receiptJson + "`n")
    Set-Content -Encoding Ascii (Join-Path $src "PINNED.txt") "$($c.url)`nsha256 $($c.sha256)"
    Move-StageToDest $src $dest $stage
    $post = Test-PinnedComponent $c $tools
    if (-not $post.Ok) { throw "bootstrap: $($c.name): verification failed after extract ($($post.Reason))" }
    Write-Host "installed $($c.name) $($post.Version)"
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
        $pkgDeps = @()
        if ($pkg.PSObject.Properties['deps'] -and $null -ne $pkg.deps) { $pkgDeps = @($pkg.deps) }
        foreach ($d in $pkgDeps) {
            if (-not $seenIds.ContainsKey($d)) { throw "bootstrap: vendor $($pkg.id) dep '$d' out of topological order" }
        }
        $seenIds[$pkg.id] = $true

        $dest = Join-Path $converterSrc $pkg.id
        $receiptPath = Join-Path $dest "PINNED.json"
        if ((Test-Path -LiteralPath $receiptPath) -and -not $Force) {
            if (Test-VendorReceipt $pkg $dest) {
                Write-Host "ok vendor:$($pkg.id) $($pkg.version)"
                continue
            }
            Write-Host "vendor:$($pkg.id): receipt or tree changed; restaging"
        }

        $file = Fetch-VerifiedFile $pkg $cache
        Assert-SafeArchive $file
        $patchRecords = @()
        $patchFiles = @()
        $pkgPatches = @()
        if ($pkg.PSObject.Properties['patches'] -and $null -ne $pkg.patches) { $pkgPatches = @($pkg.patches) }
        foreach ($p in $pkgPatches) {
            if ($p.sha256 -notmatch '^[0-9a-f]{64}$' -or (Test-MovingRef $p.url)) {
                throw "bootstrap: invalid patch metadata in $($pkg.id)"
            }
            $pf = Fetch-VerifiedFile $p $cache
            $patchFiles += @{ Spec = $p; Path = $pf }
            $patchRecords += @{ name = $p.name; sha256 = $p.sha256 }
        }

        $resolvedCache = (Resolve-Path -LiteralPath $cache).ProviderPath
        $stage = if ($resolvedCache.StartsWith('\\')) {
            Join-Path ([IO.Path]::GetTempPath()) ("rf-vendor-$($pkg.id)-" + [guid]::NewGuid().ToString("N"))
        } else {
            Join-Path $cache "stage-vendor-$($pkg.id)"
        }
        if (Test-Path -LiteralPath $stage) { Remove-Item -Recurse -Force -LiteralPath $stage }
        New-Item -ItemType Directory -Force -Path $stage | Out-Null
        Invoke-WithLocalArchive $file {
            param($localArc)
            & $bsdtar -xf $localArc -C $stage
            if ($LASTEXITCODE -ne 0) { throw "bootstrap: extract failed: $file" }
        }
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
        Move-StageToDest $src $dest $stage
        if (-not (Test-VendorReceipt $pkg $dest)) {
            throw "bootstrap: vendor $($pkg.id) receipt verification failed after staging"
        }
        Write-Host "staged vendor:$($pkg.id) $($pkg.version)"
    }
}
