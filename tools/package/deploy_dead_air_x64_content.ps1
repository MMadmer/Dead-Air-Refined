# Installs a locally built content set into a local installation, without a release.
#
# The shipped path to new content is: publish a content release, let the client fetch it. That
# is right for players and wrong for development, where the content changes several times a day
# and nothing is published yet. Without a local path the developer's own game is the one
# installation that can never satisfy its own manifest - it would sit in "incomplete" and its
# repair button would ask GitHub for assets that do not exist.
#
# So this does what ContentCommit does, against the bundle cache instead of a download cache.
# It is a development tool: it never runs on a player's machine and is not part of any release.
# The one rule it does NOT relax is the important one - nothing enters `database\` that has not
# matched the manifest's SHA-256 first.
[CmdletBinding()]
param(
    # The manifest the bundles were built with: it decides what the installation must contain.
    [Parameter(Mandatory)][string]$Manifest,
    # The -BundleCache that dead_air_x64_content_bundles.ps1 packed into.
    [Parameter(Mandatory)][string]$BundleCache,
    # One or more game roots. A QA rig whose `database` is a junction to the real one only
    # needs the real one listed - the junction carries the bundles across by itself.
    [Parameter(Mandatory)][string[]]$GameDir,
    # Print what would happen and touch nothing.
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Manifest -PathType Leaf)) {
    throw "Content manifest not found: $Manifest"
}
if (-not (Test-Path -LiteralPath $BundleCache -PathType Container)) {
    throw "Bundle cache not found: $BundleCache"
}

# ---- read the manifest ------------------------------------------------------------------------
# Deliberately as unforgiving as the engine's parser. A manifest this script accepts but the
# game rejects would produce an installation that looks deployed and refuses to start.
$lines = [IO.File]::ReadAllLines($Manifest)
if ($lines.Count -lt 1 -or $lines[0] -ne "schema=dead-air-refined.content/1") {
    throw "Not a content manifest (bad or missing schema line): $Manifest"
}
$version = ($lines | Where-Object { $_ -like "version=*" } | Select-Object -First 1) -replace '^version=', ''
$contentId = ($lines | Where-Object { $_ -like "content-id=*" } | Select-Object -First 1) -replace '^content-id=', ''

$bundles = [Collections.Generic.List[object]]::new()
$section = ""
foreach ($line in $lines) {
    if ($line -like "[[]*[]]") { $section = $line; continue }
    if ($section -ne "[bundles]" -or -not $line.Trim()) { continue }
    $f = $line -split "`t"
    if ($f.Count -lt 3) { throw "Malformed [bundles] row: $line" }
    $bundles.Add([pscustomobject]@{ Hash = $f[0]; Size = [int64]$f[1]; Name = $f[2] })
}
if ($bundles.Count -eq 0) {
    throw "The manifest declares no bundles: $Manifest"
}

Write-Host "$version, $($bundles.Count) bundle(s), content-id $($contentId.Substring(0, 16))..."

# The bundle name grammar is the delete authority: anything matching it in `database\` belongs
# to the content system and is moved out when the manifest stops naming it. Anything that does
# not match is somebody else's archive and is never touched.
$bundlePattern = '^xtra_dead_air_x64_content_[a-z0-9_]+_[0-9]{2}_[0-9a-f]{16}\.xdb0$'

function Get-Sha256 {
    param([string]$Path)
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

foreach ($root in $GameDir) {
    Write-Host ""
    Write-Host "== $root"
    if (-not (Test-Path -LiteralPath (Join-Path $root "xrEngine.exe") -PathType Leaf)) {
        throw "Not a game installation (no xrEngine.exe): $root"
    }

    $database = Join-Path $root "database"
    $control = Join-Path $root ".dead-air-x64"
    $cache = Join-Path $control "content-cache"
    $latch = Join-Path $control "content-incomplete.txt"
    $state = Join-Path $control "content-state.txt"

    if (-not $DryRun) {
        New-Item -ItemType Directory -Force -Path $database, $control, $cache | Out-Null
        # Same discipline as the real commit: the installation declares itself incomplete before
        # the first change, so a deploy interrupted halfway cannot present as healthy.
        [IO.File]::WriteAllText($latch,
            "schema=dead-air-refined.content-incomplete/1`nversion=$version`nreason=install-commit`n",
            [Text.UTF8Encoding]::new($false))
    }

    # ---- phase 1: install everything the manifest names ---------------------------------------
    $installed = 0
    $already = 0
    foreach ($bundle in $bundles) {
        $target = Join-Path $database $bundle.Name
        if ((Test-Path -LiteralPath $target -PathType Leaf) -and
            (Get-Item -LiteralPath $target).Length -eq $bundle.Size -and
            (Get-Sha256 $target) -eq $bundle.Hash) {
            $already++
            continue
        }

        $source = Join-Path $BundleCache $bundle.Name
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "$($bundle.Name) is missing from the bundle cache. Rebuild it with dead_air_x64_content_bundles.ps1 -BundleCache $BundleCache"
        }
        if ((Get-Item -LiteralPath $source).Length -ne $bundle.Size -or (Get-Sha256 $source) -ne $bundle.Hash) {
            throw "$($bundle.Name) in the bundle cache does not match the manifest. The cache and the manifest are from different builds."
        }

        Write-Host "  install $($bundle.Name)"
        if (-not $DryRun) {
            # Through a temporary, so an interrupted copy cannot leave a short file under a
            # name that promises a hash.
            $staging = "$target.deploy-part"
            Copy-Item -LiteralPath $source -Destination $staging -Force
            Move-Item -LiteralPath $staging -Destination $target -Force
        }
        $installed++
    }

    # ---- phase 2: demote what the manifest no longer names ------------------------------------
    # Add before delete, always. An installation with one bundle too many is untidy; one with a
    # bundle too few is a broken game.
    $wanted = @{}
    foreach ($bundle in $bundles) { $wanted[$bundle.Name] = $true }

    $demoted = 0
    foreach ($file in Get-ChildItem -LiteralPath $database -File -Filter "xtra_dead_air_x64_content_*") {
        if ($wanted.ContainsKey($file.Name)) { continue }
        if ($file.Name -notmatch $bundlePattern) {
            Write-Host "  leave   $($file.Name) (not a bundle name - not ours to move)"
            continue
        }
        Write-Host "  demote  $($file.Name)"
        if (-not $DryRun) {
            # Into the cache under its own hash, the way a real commit retires one: a downgrade
            # later finds it there instead of re-downloading it.
            Move-Item -LiteralPath $file.FullName -Destination (Join-Path $cache (Get-Sha256 $file.FullName)) -Force
        }
        $demoted++
    }

    # ---- the manifest, then the latch ----------------------------------------------------------
    if (-not $DryRun) {
        Copy-Item -LiteralPath $Manifest -Destination (Join-Path $control "content-manifest.txt") -Force
        # The state cache records "this file was this hash at this mtime". Every mtime just
        # changed, so keeping it would only cost a rehash on the next start - and a wrong entry
        # would be worse than none. The game rebuilds it.
        Remove-Item -LiteralPath $state -Force -ErrorAction SilentlyContinue

        # Closing verification before the latch goes, on the files as they now sit in database\
        # rather than on what this script believes it wrote.
        foreach ($bundle in $bundles) {
            $target = Join-Path $database $bundle.Name
            if (-not (Test-Path -LiteralPath $target -PathType Leaf)) { throw "$($bundle.Name) is missing after the deploy." }
            if ((Get-Item -LiteralPath $target).Length -ne $bundle.Size) { throw "$($bundle.Name) is the wrong size after the deploy." }
            if ((Get-Sha256 $target) -ne $bundle.Hash) { throw "$($bundle.Name) does not match the manifest after the deploy." }
        }
        Remove-Item -LiteralPath $latch -Force -ErrorAction SilentlyContinue
    }

    "  {0} installed, {1} already current, {2} demoted{3}" -f $installed, $already, $demoted, $(if ($DryRun) { "  (dry run, nothing written)" } else { "" })
}

Write-Host ""
Write-Host "Restart the game: archives are mounted once at start, so bundles that arrived after it began are not picked up." -ForegroundColor Yellow
