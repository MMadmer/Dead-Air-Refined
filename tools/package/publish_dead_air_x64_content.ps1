<#
.SYNOPSIS
Publishes content bundles and deltas to the assets repository.

.DESCRIPTION
Every asset this uploads is permanent. A bundle's name carries the hash of its bytes, so a name
and its contents are one thing forever: replacing a published asset would leave every client
that already downloaded it holding a file the manifest still vouches for, and deleting one
breaks every installed version whose manifest names it. The script therefore refuses to replace
or delete anything, and re-uploading the same name is only allowed when the bytes are identical
- which is a no-op it can skip rather than a decision it has to make.

Ordering matters and is not negotiable: content assets are published BEFORE the game release
that names them. A game release whose manifest points at assets nobody can download yet is
broken for every installation made in that window, and the window is however long it takes
somebody to notice.
#>
[CmdletBinding()]
param(
    # The manifest produced by dead_air_x64_content_bundles.ps1. Every asset it names must be
    # present in the bundle cache, and nothing outside it is uploaded.
    [Parameter(Mandatory)][string]$Manifest,
    [Parameter(Mandatory)][string]$BundleCache,
    [string]$AssetsRepo = "MMadmer/Dead-Air-Refined_Assets",
    # Local clone of the assets repository. The ledger is written here; commits are the
    # operator's to make.
    [string]$AssetsClone = "D:\Games\Dead-Air-Refined_Assets",
    # Report what would happen and change nothing.
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Manifest -PathType Leaf)) {
    throw "The content manifest was not found: $Manifest"
}
if (-not (Test-Path -LiteralPath $BundleCache -PathType Container)) {
    throw "The bundle cache was not found: $BundleCache"
}
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw "The GitHub CLI (gh) is required to publish content assets."
}

function Get-Sha256 {
    param([Parameter(Mandatory)][string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    try { return [Convert]::ToHexString([Security.Cryptography.SHA256]::Create().ComputeHash($stream)).ToLowerInvariant() }
    finally { $stream.Dispose() }
}

# ---- read the manifest ---------------------------------------------------------------------
$version = ""
$contentId = ""
$assets = [Collections.Generic.List[object]]::new()
$section = ""
foreach ($line in [IO.File]::ReadAllLines($Manifest)) {
    if ($line.StartsWith("version=")) { $version = $line.Substring(8); continue }
    if ($line.StartsWith("content-id=")) { $contentId = $line.Substring(11); continue }
    if ($line -eq "[bundles]") { $section = "bundles"; continue }
    if ($line -eq "[deltas]") { $section = "deltas"; continue }
    if (-not $line.Trim()) { continue }

    $f = $line -split "`t"
    if ($section -eq "bundles" -and $f.Count -eq 4) {
        $assets.Add([pscustomobject]@{ Hash = $f[0]; Size = [int64]$f[1]; Name = $f[2]; Tag = $f[3]; Kind = "bundle" })
    }
    elseif ($section -eq "deltas" -and $f.Count -eq 8) {
        $assets.Add([pscustomobject]@{ Hash = $f[0]; Size = [int64]$f[1]; Name = $f[2]; Tag = $f[3]; Kind = "delta" })
    }
}
if (-not $version -or -not $contentId) { throw "The manifest has no version or content-id." }
if ($assets.Count -eq 0) { throw "The manifest names no assets." }

# The digest the engine recomputes at every launch. Verifying it here means a manifest that
# would be rejected as tampered is caught before its assets are uploaded, not after.
$sorted = [Linq.Enumerable]::ToArray(
    [Linq.Enumerable]::OrderBy([object[]]($assets | Where-Object Kind -eq "bundle"),
        [Func[object, string]] { param($b) $b.Name }, [StringComparer]::Ordinal))
$idInput = [Text.StringBuilder]::new()
foreach ($b in $sorted) { [void]$idInput.Append($b.Name).Append("`n").Append($b.Hash).Append("`n") }
$computed = [Convert]::ToHexString(
    [Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($idInput.ToString()))).ToLowerInvariant()
if ($computed -ne $contentId) {
    throw "The manifest's content-id does not match its bundles. Rebuild it rather than editing it."
}

Write-Host "content $version, id $contentId"
Write-Host "$($sorted.Count) bundle(s), $(($assets | Where-Object Kind -eq 'delta').Count) delta(s)"

# ---- verify every asset locally before touching the network --------------------------------
foreach ($asset in $assets) {
    $path = Join-Path $BundleCache $asset.Name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "$($asset.Name) is named by the manifest but is not in the bundle cache."
    }
    $info = Get-Item -LiteralPath $path
    if ($info.Length -ne $asset.Size) {
        throw "$($asset.Name) is $($info.Length) bytes, the manifest says $($asset.Size)."
    }
    $actual = Get-Sha256 -Path $path
    if ($actual -ne $asset.Hash) {
        throw "$($asset.Name) does not match its hash in the manifest."
    }
}
Write-Host "all $($assets.Count) asset(s) verified against the manifest"

# ---- what is already published --------------------------------------------------------------
$tags = $assets | ForEach-Object Tag | Sort-Object -Unique
$published = @{}
foreach ($tag in $tags) {
    $existing = & gh release view $tag --repo $AssetsRepo --json assets 2>$null
    if ($LASTEXITCODE -eq 0 -and $existing) {
        foreach ($item in ($existing | ConvertFrom-Json).assets) {
            $published["$tag/$($item.name)"] = $item.size
        }
    }
    elseif (-not $DryRun) {
        Write-Host "creating release $tag"
        & gh release create $tag --repo $AssetsRepo --title $tag --notes "Content assets for Dead Air: Refined $version." | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Could not create release $tag in $AssetsRepo." }
    }
}

$upload = [Collections.Generic.List[object]]::new()
foreach ($asset in $assets) {
    $key = "$($asset.Tag)/$($asset.Name)"
    if (-not $published.ContainsKey($key)) { $upload.Add($asset); continue }

    # A name that is already published must carry the same bytes. Because the name contains the
    # hash of those bytes, a size mismatch under the same name means the two disagree, and that
    # is not something to resolve by overwriting - it means something upstream produced a
    # different file under a name that promised otherwise.
    if ($published[$key] -ne $asset.Size) {
        throw "$($asset.Name) is already published in $($asset.Tag) at $($published[$key]) bytes but is $($asset.Size) bytes here. A published asset is never replaced."
    }
    Write-Host "  already published: $($asset.Name)"
}

if ($upload.Count -eq 0) {
    Write-Host "nothing to upload"
}

foreach ($asset in $upload) {
    $path = Join-Path $BundleCache $asset.Name
    $mb = [math]::Round($asset.Size / 1MB, 2)
    if ($DryRun) {
        Write-Host "  would upload: $($asset.Name) ($mb MB) to $($asset.Tag)"
        continue
    }
    Write-Host "  uploading: $($asset.Name) ($mb MB)"
    & gh release upload $asset.Tag $path --repo $AssetsRepo | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Could not upload $($asset.Name) to $($asset.Tag)." }
}

# ---- the ledger ------------------------------------------------------------------------------
# Append-only, and it records what a release actually consisted of - including the shard table
# that decided it. groups.ltx lives in the engine repository and is edited there; this copy is
# audit, never a second editable original.
if (-not $DryRun -and (Test-Path -LiteralPath $AssetsClone -PathType Container)) {
    $indexDirectory = Join-Path $AssetsClone "index"
    New-Item -ItemType Directory -Path $indexDirectory -Force | Out-Null
    $ledger = Join-Path $indexDirectory "content-$version.txt"
    if (Test-Path -LiteralPath $ledger -PathType Leaf) {
        throw "The ledger for $version already exists: $ledger. A published release is never rewritten."
    }

    $groupsFile = Join-Path (Split-Path -Parent $PSScriptRoot) "..\packaging\dead-air-x64\content\groups.ltx"
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add("version=$version")
    $lines.Add("content-id=$contentId")
    $lines.Add("published=" + [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ"))
    $lines.Add("[assets]")
    foreach ($asset in $assets) {
        $lines.Add($asset.Kind + "`t" + $asset.Tag + "`t" + $asset.Name + "`t" + $asset.Size + "`t" + $asset.Hash)
    }
    $lines.Add("[groups.ltx]")
    if (Test-Path -LiteralPath $groupsFile -PathType Leaf) {
        foreach ($line in [IO.File]::ReadAllLines($groupsFile)) { $lines.Add($line) }
    }
    [IO.File]::WriteAllText($ledger, [string]::Join("`n", $lines) + "`n", [Text.UTF8Encoding]::new($false))
    Write-Host "ledger written: $ledger"
    Write-Host "commit it in $AssetsClone yourself - this script does not commit."
}

[pscustomobject]@{
    Version   = $version
    ContentId = $contentId
    Assets    = $assets.Count
    Uploaded  = if ($DryRun) { 0 } else { $upload.Count }
    DryRun    = [bool]$DryRun
}
