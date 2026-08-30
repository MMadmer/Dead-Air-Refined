<#
.SYNOPSIS
Builds the versioned content bundles and the content manifest.

.DESCRIPTION
Content lives outside the engine repository. This script turns an authored gamedata tree into
a set of .xdb0 bundles plus the manifest that pins them to a game version.

Two hashes, two jobs, and confusing them is the classic way to break this system:

  * the CONTENT DIGEST is computed over a bundle's members before packing. It is the build
    cache key and nothing else - it answers "do I already have a packed bundle for exactly
    these files", so unchanged content is never repacked. It never appears in a filename.
  * the FILE HASH is the SHA-256 of the finished .xdb0. It goes into the filename (first 16
    hex) and into the manifest (all 64). Name and bytes are 1:1 forever, which is what lets a
    client skip a download by looking at a filename, and what makes replacing an already
    published asset unrecoverable - see PROJECT_RULES.md section 11.

Reuse ladder: a cache hit on the content digest is reused verbatim, otherwise the bundle is
packed. Reuse is what keeps an unchanged bundle free across releases; without it a repack
could produce different bytes and force every player to re-download identical content.
#>
[CmdletBinding()]
param(
    # Authored content, laid out as a gamedata tree (textures\, meshes\, sounds\, anims\).
    [Parameter(Mandatory)][string]$SourceRoot,
    # Where finished bundles are kept between releases. Never delete this: it is what makes an
    # unchanged bundle free, and losing it while a release is unreachable forces a repack.
    [Parameter(Mandatory)][string]$BundleCache,
    [Parameter(Mandatory)][string]$OutputManifest,
    [Parameter(Mandatory)][string]$PortVersion,
    [Parameter(Mandatory)][string]$ReleaseTag,
    [string]$GroupsFile,
    [string]$ConverterPath = "D:\Games\Dead Air\tools\AXRToolset\bin\converter.exe",
    [string]$WorkRoot,
    [string]$AssetsRepo = "MMadmer/Dead-Air-Refined_Assets",
    # Archive whose members must not collide with content. Normally the compatibility archive.
    [string]$DisjointFrom,
    # Force packing even when the cache could satisfy the bundle. Diagnostic only.
    [switch]$AllowRepack
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "dead_air_x64_archive.ps1")

if ($PortVersion -notmatch '^\d+\.\d+\.\d+$') {
    throw "PortVersion must use numeric SemVer format, for example 1.4.0."
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not $GroupsFile) {
    $GroupsFile = Join-Path $repositoryRoot "packaging\dead-air-x64\content\groups.ltx"
}
if (-not $WorkRoot) {
    $WorkRoot = Join-Path $repositoryRoot "build\content"
}

function Read-GroupsFile {
    param([Parameter(Mandatory)][string]$Path)

    $groups = [ordered]@{}
    $assign = [ordered]@{}
    $shard = @{ target_mb = 250; max_mb = 1536 }
    $section = ""
    foreach ($rawLine in [IO.File]::ReadAllLines($Path)) {
        $line = $rawLine.Trim()
        if (-not $line -or $line.StartsWith(";")) { continue }
        if ($line -match '^\[(?<name>[a-z_]+)\]$') { $section = $Matches['name']; continue }
        if ($line -notmatch '^(?<key>[^=]+?)\s*=\s*(?<value>.+)$') { continue }
        $key = $Matches['key'].Trim()
        $value = $Matches['value'].Trim()
        switch ($section) {
            "groups" { $groups[$key] = $value }
            "assign" { $assign[$key.ToLowerInvariant()] = $value }
            "shard"  { $shard[$key] = [int]$value }
        }
    }
    if ($groups.Count -eq 0) { throw "No [groups] section in $Path" }
    return [pscustomobject]@{ Groups = $groups; Assign = $assign; Shard = $shard; Path = $Path }
}

function Add-AssignLine {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Key,
        [Parameter(Mandatory)][string]$Shard
    )
    # Appended, never inserted, so the file reads as the history of what was assigned when.
    $lines = [Collections.Generic.List[string]][IO.File]::ReadAllLines($Path)
    $lines.Add(("{0,-24}= {1}" -f $Key, $Shard))
    [IO.File]::WriteAllText($Path, [string]::Join("`n", $lines) + "`n", [Text.UTF8Encoding]::new($false))
}

function Get-Sha256 {
    param([Parameter(Mandatory)][string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    try { return [Convert]::ToHexString([Security.Cryptography.SHA256]::Create().ComputeHash($stream)).ToLowerInvariant() }
    finally { $stream.Dispose() }
}

function Get-ArchiveMemberPaths {
    param(
        [Parameter(Mandatory)][string]$ArchivePath,
        [Parameter(Mandatory)][string]$WorkRoot,
        [Parameter(Mandatory)][string]$ConverterPath
    )
    $unpack = Join-Path $WorkRoot ("disjoint-" + [IO.Path]::GetFileNameWithoutExtension($ArchivePath))
    if (Test-Path -LiteralPath $unpack) { Remove-Item -LiteralPath $unpack -Recurse -Force }
    New-Item -ItemType Directory -Path $unpack -Force | Out-Null
    & $ConverterPath -unpack -xdb -dir $unpack $ArchivePath | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Could not read members of $ArchivePath" }
    $prefix = $unpack.Length + 1
    $paths = Get-ChildItem -LiteralPath $unpack -Recurse -File |
        ForEach-Object { $_.FullName.Substring($prefix).Replace('\', '/').ToLowerInvariant() }
    Remove-Item -LiteralPath $unpack -Recurse -Force
    return $paths
}

# ---- gather the source -------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $SourceRoot -PathType Container)) {
    throw "Content source root was not found: $SourceRoot"
}
$layout = Read-GroupsFile -Path $GroupsFile
New-Item -ItemType Directory -Path $BundleCache -Force | Out-Null
New-Item -ItemType Directory -Path $WorkRoot -Force | Out-Null

$sourcePrefix = (Resolve-Path $SourceRoot).Path.TrimEnd('\').Length + 1
$files = Get-ChildItem -LiteralPath $SourceRoot -Recurse -File
if ($files.Count -eq 0) { throw "Content source root is empty: $SourceRoot" }

Write-Host "hashing $($files.Count) source files..."
$hashed = $files | ForEach-Object -ThrottleLimit ([Environment]::ProcessorCount) -Parallel {
    $stream = [IO.File]::OpenRead($_.FullName)
    try { $hash = [Security.Cryptography.SHA256]::Create().ComputeHash($stream) }
    finally { $stream.Dispose() }
    [pscustomobject]@{
        Full = $_.FullName
        Rel  = $_.FullName.Substring($using:sourcePrefix).Replace('\', '/')
        Hash = [Convert]::ToHexString($hash).ToLowerInvariant()
        Size = $_.Length
    }
}

# ---- assign to group and shard -----------------------------------------------------------
$assignChanged = $false
$buckets = [ordered]@{}
foreach ($entry in $hashed) {
    $parts = $entry.Rel.Split('/')
    $root = $parts[0].ToLowerInvariant()
    $group = $null
    foreach ($name in $layout.Groups.Keys) {
        if ($layout.Groups[$name].ToLowerInvariant() -eq $root) { $group = $name; break }
    }
    if (-not $group) {
        throw "No [groups] entry covers '$root' (file $($entry.Rel)). Add the group before packing."
    }
    if ($parts.Count -lt 2) {
        throw "Content file must live in a subdirectory of its group root: $($entry.Rel)"
    }

    $key = "$root/$($parts[1])".ToLowerInvariant()
    if (-not $layout.Assign.Contains($key)) {
        # New directory: append it to the lowest existing shard of this group, or 00.
        $existing = @()
        foreach ($assigned in $layout.Assign.Keys) {
            if ($assigned.StartsWith("$root/")) { $existing += $layout.Assign[$assigned] }
        }
        $shardIndex = if ($existing.Count -eq 0) { "00" } else { ($existing | Sort-Object -Unique | Select-Object -First 1) }
        $layout.Assign[$key] = $shardIndex
        Add-AssignLine -Path $GroupsFile -Key $key -Shard $shardIndex
        $assignChanged = $true
        Write-Host "  assign: $key -> $group/$shardIndex (new directory, appended to groups.ltx)"
    }

    $bucketKey = "$group/$($layout.Assign[$key])"
    if (-not $buckets.Contains($bucketKey)) { $buckets[$bucketKey] = [Collections.Generic.List[object]]::new() }
    $buckets[$bucketKey].Add($entry)
}

# ---- build each bundle --------------------------------------------------------------------
$bundles = [Collections.Generic.List[object]]::new()
$packed = 0
$reused = 0

foreach ($bucketKey in $buckets.Keys) {
    $group, $shardIndex = $bucketKey.Split('/')
    $members = $buckets[$bucketKey] | Sort-Object Rel

    # Content digest: what is in the bundle, independent of how it was packed.
    $canonical = [Text.StringBuilder]::new()
    [void]$canonical.Append("epoch=1`n")
    foreach ($m in $members) {
        [void]$canonical.Append($m.Rel.ToLowerInvariant()).Append("`n").Append($m.Hash).Append("`n")
    }
    $contentDigest = [Convert]::ToHexString(
        [Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($canonical.ToString()))).ToLowerInvariant()

    $cacheRecord = Join-Path $BundleCache "$group-$shardIndex-$contentDigest.name"
    $bundleFile = $null
    if (-not $AllowRepack -and (Test-Path -LiteralPath $cacheRecord -PathType Leaf)) {
        $cachedName = ([IO.File]::ReadAllText($cacheRecord)).Trim()
        $cachedPath = Join-Path $BundleCache $cachedName
        if (Test-Path -LiteralPath $cachedPath -PathType Leaf) {
            $bundleFile = $cachedPath
            $reused++
            Write-Host "  reuse: $cachedName"
        }
    }

    if (-not $bundleFile) {
        $stage = Join-Path $WorkRoot "stage-$group-$shardIndex"
        if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
        foreach ($m in $members) {
            $destination = Join-Path $stage $m.Rel.Replace('/', '\')
            New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
            Copy-Item -LiteralPath $m.Full -Destination $destination -Force
        }

        # Per-bundle user data. level_ver carries the content digest so a bundle is
        # self-describing; the compatibility archive keeps its hand-maintained version.
        $userData = Join-Path $WorkRoot "userdata-$group-$shardIndex.ltx"
        # Built as a here-string, matching the compatibility archive's header byte for byte
        # (CRLF, single spaces). Concatenating this with + split the quotes and the $ markers
        # onto their own lines, which the archive loader answers with a startup assert long
        # before any log is written - so keep the literal form.
        $userDataText = @"
[header]
auto_load = true
creator = "Dead Air x64 Project"
entry_point = `$fs_root`$\gamedata\
level_name = single
level_ver = $contentDigest
"@ -replace "`r?`n", "`r`n"

        [IO.File]::WriteAllText($userData, $userDataText + "`r`n", [Text.UTF8Encoding]::new($false))

        $temporary = Join-Path $WorkRoot "pack-$group-$shardIndex.xdb0"
        New-XdbArchive -StageRoot $stage -OutputPath $temporary -UserDataPath $userData `
            -ConverterPath $ConverterPath -VerifyRoot (Join-Path $WorkRoot "verify-$group-$shardIndex")

        $fileHash = Get-Sha256 -Path $temporary
        $name = "xtra_dead_air_x64_content_" + $group + "_" + $shardIndex + "_" + $fileHash.Substring(0, 16) + ".xdb0"
        $bundleFile = Join-Path $BundleCache $name
        Move-Item -LiteralPath $temporary -Destination $bundleFile -Force
        [IO.File]::WriteAllText($cacheRecord, $name, [Text.UTF8Encoding]::new($false))
        Remove-Item -LiteralPath $stage -Recurse -Force
        $packed++
        Write-Host "  pack:  $name ($([math]::Round((Get-Item $bundleFile).Length / 1MB, 2)) MB, $($members.Count) files)"
    }

    $info = Get-Item -LiteralPath $bundleFile
    if ($info.Length -gt $layout.Shard.max_mb * 1MB) {
        throw "Bundle $($info.Name) is $([math]::Round($info.Length/1MB)) MB, above the $($layout.Shard.max_mb) MB ceiling. Split its directories across shards in groups.ltx."
    }
    # The engine's mount gate compares against _finddata_t::size, which is 32 bits wide on
    # Windows and truncates silently. A 4 GiB bundle would present a plausible wrong size and
    # be refused for a reason nobody could diagnose. The shard ceiling above is far lower, so
    # this can only fire if someone raises max_mb past all sense - which is exactly when it
    # needs to fire.
    if ($info.Length -ge 4GB) {
        throw "Bundle $($info.Name) is $([math]::Round($info.Length/1GB, 2)) GB. Bundles must stay below 4 GiB: the engine reads archive sizes through a 32-bit field."
    }
    $bundles.Add([pscustomobject]@{
        Name    = $info.Name
        Hash    = Get-Sha256 -Path $info.FullName
        Size    = $info.Length
        Tag     = $ReleaseTag
        Members = $members
    })
}

# ---- disjointness --------------------------------------------------------------------------
$seen = @{}
foreach ($bundle in $bundles) {
    foreach ($m in $bundle.Members) {
        $key = $m.Rel.ToLowerInvariant()
        if ($seen.ContainsKey($key)) {
            throw "Content path is in two bundles: $key ($($seen[$key]) and $($bundle.Name))"
        }
        $seen[$key] = $bundle.Name
    }
}
if ($DisjointFrom) {
    foreach ($member in (Get-ArchiveMemberPaths -ArchivePath $DisjointFrom -WorkRoot $WorkRoot -ConverterPath $ConverterPath)) {
        if ($seen.ContainsKey($member)) {
            throw "Content path also exists in $([IO.Path]::GetFileName($DisjointFrom)): $member. One of the two must drop it."
        }
    }
}

# ---- manifest --------------------------------------------------------------------------------
# Ordinal, not Sort-Object: the engine recomputes this content-id with std::string comparison and
# a culture-aware sort disagrees with that on underscores and digits. The two only have to differ
# once for every installed player to be told their manifest was tampered with.
$sorted = [Linq.Enumerable]::ToArray(
    [Linq.Enumerable]::OrderBy([object[]]$bundles, [Func[object, string]] { param($b) $b.Name },
        [StringComparer]::Ordinal))
$idInput = [Text.StringBuilder]::new()
foreach ($b in $sorted) { [void]$idInput.Append($b.Name).Append("`n").Append($b.Hash).Append("`n") }
$contentId = [Convert]::ToHexString(
    [Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($idInput.ToString()))).ToLowerInvariant()

$manifest = [Collections.Generic.List[string]]::new()
$manifest.Add("schema=dead-air-refined.content/1")
$manifest.Add("version=$PortVersion")
$manifest.Add("content-id=$contentId")
$manifest.Add("repo=$AssetsRepo")
$manifest.Add("[bundles]")
foreach ($b in $sorted) { $manifest.Add($b.Hash + "`t" + $b.Size + "`t" + $b.Name + "`t" + $b.Tag) }
$manifest.Add("[deltas]")

New-Item -ItemType Directory -Path (Split-Path -Parent $OutputManifest) -Force | Out-Null
[IO.File]::WriteAllText($OutputManifest, [string]::Join("`n", $manifest) + "`n", [Text.UTF8Encoding]::new($false))

$totalBytes = ($sorted | Measure-Object Size -Sum).Sum
[pscustomobject]@{
    Manifest          = $OutputManifest
    ContentId         = $contentId
    Bundles           = $sorted.Count
    Packed            = $packed
    Reused            = $reused
    SourceFiles       = $hashed.Count
    TotalMB           = [math]::Round($totalBytes / 1MB, 2)
    GroupsFileChanged = $assignChanged
}
