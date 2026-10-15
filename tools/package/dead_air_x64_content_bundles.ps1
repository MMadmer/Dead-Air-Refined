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

Every bundle also gets its chunk map, <bundle>.chunks, cut by DarDelta and kept in the cache
beside it. Published with the bundle, it lets a client rebuild the bundle from whatever revision
of its slot it already holds and fetch only the rest by range - from any earlier release, with
nothing published per pair of releases.
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
    [switch]$AllowRepack,
    # The chunk map cutter, built by the ordinary engine build.
    [string]$DeltaTool = (Join-Path $PSScriptRoot "..\..\bin\x64\Release\DarDelta.exe"),
    # Report the shard layout and stop, without packing anything. A shard assignment is
    # permanent the moment it is published, so being able to look at one first is worth a
    # switch - and it lets the scale test measure the real policy instead of a copy of it.
    [switch]$LayoutOnly
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

# A chunk map as the client will read it: the whole file, not just its header. A map that does not
# parse is published once and then downloaded for nothing forever, so it is caught here.
function Test-ChunkMap {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Hash, [Parameter(Mandatory)][long]$Size)
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 72 -or [Text.Encoding]::ASCII.GetString($bytes, 0, 8) -ne "DARCHUNK") { return $false }
    if ([BitConverter]::ToUInt32($bytes, 8) -ne 1 -or [BitConverter]::ToUInt32($bytes, 12) -ne 8KB -or
        [BitConverter]::ToUInt32($bytes, 16) -ne 32KB -or [BitConverter]::ToUInt32($bytes, 20) -ne 128KB) { return $false }
    if ([Convert]::ToHexString($bytes, 24, 32).ToLowerInvariant() -ne $Hash -or [BitConverter]::ToUInt64($bytes, 56) -ne $Size) { return $false }
    $count = [BitConverter]::ToUInt64($bytes, 64)
    if ($bytes.Length -ne 72 + $count * 20) { return $false }
    $total = 0L
    for ($i = 0; $i -lt $count; $i++) { $total += [BitConverter]::ToUInt32($bytes, 72 + $i * 20) }
    return $total -eq $Size
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
# Two passes, because assignment needs sizes and sizes need the whole file list. The first pass
# measures every source directory; the second places the ones that have no shard yet.
#
# Placement is by accumulated BYTES, not by lowest shard index. That distinction is the whole
# difference between a layout that survives to five gigabytes and one that does not: filling the
# lowest index first piles everything into shard 00 until it passes the hard ceiling, and by then
# the table is append-only and there is no way out that does not move existing files - which
# renames their bundles and costs every installed player a full re-download.
$assignChanged = $false
$directories = [ordered]@{}
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

    # The assignment key is normally the first path component, which keeps a .dds and its .thm
    # in the same bundle for free. A directory that is already assigned at a deeper granularity
    # keeps it: the table is append-only, so the more specific line wins wherever one exists.
    $key = "$root/$($parts[1])".ToLowerInvariant()
    # The longest assigned prefix wins, at any depth (season sets live several levels down)
    for ($depth = $parts.Count - 1; $depth -ge 3; $depth--) {
        $deeper = ($parts[0..($depth - 1)] -join '/').ToLowerInvariant()
        if ($layout.Assign.Contains($deeper)) { $key = $deeper; break }
    }

    if (-not $directories.Contains($key)) {
        $directories[$key] = [pscustomobject]@{
            Key = $key; Root = $root; Group = $group
            Bytes = [int64]0; Entries = [Collections.Generic.List[object]]::new()
        }
    }
    $directories[$key].Bytes += $entry.Size
    $directories[$key].Entries.Add($entry)
}

$targetBytes = [int64]$layout.Shard.target_mb * 1MB
$shardBytes = @{}
foreach ($directory in $directories.Values) {
    if (-not $layout.Assign.Contains($directory.Key)) { continue }
    $bucketKey = "$($directory.Root)/$($layout.Assign[$directory.Key])"
    if (-not $shardBytes.ContainsKey($bucketKey)) { $shardBytes[$bucketKey] = [int64]0 }
    $shardBytes[$bucketKey] += $directory.Bytes
}

# Largest first: a big directory placed last would have to go somewhere already full, and one
# directory is never split, so it would produce one oversized bundle.
$unassigned = $directories.Values |
    Where-Object { -not $layout.Assign.Contains($_.Key) } |
    Sort-Object -Property Bytes -Descending

foreach ($directory in $unassigned) {
    $root = $directory.Root

    # The emptiest shard of this group that still has room. Failing that, a new one - which is
    # exactly the growth the append-only table is designed to absorb.
    $best = $null
    $bestBytes = [int64]0
    foreach ($bucketKey in $shardBytes.Keys) {
        if (-not $bucketKey.StartsWith("$root/")) { continue }
        $bytes = $shardBytes[$bucketKey]
        if ($bytes + $directory.Bytes -gt $targetBytes) { continue }
        if (-not $best -or $bytes -lt $bestBytes) { $best = $bucketKey; $bestBytes = $bytes }
    }

    if ($best) {
        $shardIndex = $best.Split('/')[1]
    }
    else {
        $used = @($shardBytes.Keys | Where-Object { $_.StartsWith("$root/") } |
            ForEach-Object { [int]$_.Split('/')[1] })
        # Cast, because Measure-Object hands back a Double and the D format specifier only
        # accepts integrals - which surfaces as an unhelpful "format specifier was invalid"
        # from somewhere else entirely.
        $next = [int]$(if ($used.Count -eq 0) { 0 } else { ($used | Measure-Object -Maximum).Maximum + 1 })
        if ($next -gt 99) {
            throw "Group '$($directory.Group)' needs more than 100 shards. Split it into two groups."
        }
        $shardIndex = "{0:D2}" -f $next
    }

    $bucketKey = "$root/$shardIndex"
    if (-not $shardBytes.ContainsKey($bucketKey)) { $shardBytes[$bucketKey] = [int64]0 }
    $shardBytes[$bucketKey] += $directory.Bytes

    $layout.Assign[$directory.Key] = $shardIndex
    Add-AssignLine -Path $GroupsFile -Key $directory.Key -Shard $shardIndex
    $assignChanged = $true
    Write-Host ("  assign: {0} -> {1}/{2} ({3} MB, new directory, appended to groups.ltx)" -f
        $directory.Key, $directory.Group, $shardIndex, [math]::Round($directory.Bytes / 1MB, 1))

    if ($directory.Bytes -gt 1.5 * $targetBytes) {
        $note = "  note:   {0} alone is {1} MB, above 1.5x the {2} MB target." -f
            $directory.Key, [math]::Round($directory.Bytes / 1MB, 1), $layout.Shard.target_mb
        Write-Host $note
        Write-Host ("          Its bundle will be oversized until it is subdivided by hand. Add deeper " +
            "keys for its subdirectories BEFORE it is first published: moving it afterwards renames " +
            "its bundle and costs every player a re-download.")
    }
}

$buckets = [ordered]@{}
foreach ($directory in $directories.Values) {
    $bucketKey = "$($directory.Group)/$($layout.Assign[$directory.Key])"
    if (-not $buckets.Contains($bucketKey)) { $buckets[$bucketKey] = [Collections.Generic.List[object]]::new() }
    foreach ($entry in $directory.Entries) { $buckets[$bucketKey].Add($entry) }
}

if ($LayoutOnly) {
    $report = [Collections.Generic.List[object]]::new()
    foreach ($bucketKey in $buckets.Keys) {
        $group, $shardIndex = $bucketKey.Split('/')
        $bytes = ($buckets[$bucketKey] | Measure-Object -Property Size -Sum).Sum
        $report.Add([pscustomobject]@{
            Group = $group
            Shard = $shardIndex
            Files = $buckets[$bucketKey].Count
            MB    = [math]::Round($bytes / 1MB, 1)
            Bytes = [int64]$bytes
        })
    }
    return ($report | Sort-Object Group, Shard)
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

# ---- chunk maps ----------------------------------------------------------------------------
# Cut once per bundle and kept beside it: a bundle's bytes never change under its name, so neither
# does its map.
if (-not (Test-Path -LiteralPath $DeltaTool -PathType Leaf)) {
    throw "DarDelta.exe was not found: $DeltaTool. Build the x64 Release tools first."
}
$chunkMaps = 0
foreach ($bundle in $bundles) {
    $mapPath = Join-Path $BundleCache ($bundle.Name + ".chunks")
    if (-not (Test-Path -LiteralPath $mapPath -PathType Leaf) -or -not (Test-ChunkMap -Path $mapPath -Hash $bundle.Hash -Size $bundle.Size)) {
        & $DeltaTool --chunk-map (Join-Path $BundleCache $bundle.Name) $mapPath | Out-Null
        if ($LASTEXITCODE -ne 0 -or -not (Test-ChunkMap -Path $mapPath -Hash $bundle.Hash -Size $bundle.Size)) {
            throw "DarDelta could not cut the chunk map of $($bundle.Name)"
        }
        $chunkMaps++
    }
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
# The schema keeps the section of the published deltas that chunk maps replaced; it stays empty.
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
    ChunkMapsCut      = $chunkMaps
    SourceFiles       = $hashed.Count
    TotalMB           = [math]::Round($totalBytes / 1MB, 2)
    GroupsFileChanged = $assignChanged
}
