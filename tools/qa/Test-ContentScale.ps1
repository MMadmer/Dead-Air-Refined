# What the content system does at the size it is actually meant for.
#
# The shipped content is under twenty megabytes; the design targets five gigabytes. Almost
# nothing that goes wrong at five gigabytes goes wrong at twenty megabytes, and the failures
# that do are the expensive kind - a shard layout that piles everything into one bundle, a table
# that cannot grow without moving files that are already published, a manifest larger than the
# parser will accept.
#
# It synthesises a content tree of the right shape and runs the REAL builder over it in layout
# mode, so what is measured is the policy that ships rather than a copy of it here.
#
# It does not stand in for a real five-gigabyte end-to-end. Nothing here proves the downloader
# survives a four-hour transfer or that GitHub serves an asset of that size at a usable rate;
# those have to be measured against the real host when there is real content to measure.
[CmdletBinding()]
param(
    # Total synthetic content, in megabytes. The default is the projected size.
    [int]$TotalMB = 5120,
    # How many source directories to spread it across. Real content is lumpy - a few large
    # directories and a long tail of small ones - and a uniform split would flatter the policy.
    [int]$Directories = 60,
    [string]$WorkRoot,
    [string]$ConverterPath = "D:\Games\Dead Air\tools\AXRToolset\bin\converter.exe"
)

$ErrorActionPreference = "Stop"
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path

if (-not $WorkRoot) {
    $WorkRoot = Join-Path ([IO.Path]::GetTempPath()) "dar-content-scale"
}
$source = Join-Path $WorkRoot "source\gamedata"
$cache = Join-Path $WorkRoot "cache"
$groups = Join-Path $WorkRoot "groups.ltx"

if (Test-Path -LiteralPath $WorkRoot) { Remove-Item -LiteralPath $WorkRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $source, $cache | Out-Null

# A fresh table, so the policy is measured from nothing rather than from whatever the shipped
# one happens to contain today.
@"
[groups]
textures        = textures
meshes          = meshes
sounds          = sounds
anims           = anims

[shard]
target_mb       = 250
max_mb          = 1536

[assign]
"@ | Set-Content -LiteralPath $groups -Encoding utf8

# Lumpy on purpose. A Zipf-ish spread puts most of the bytes in a few directories, which is what
# a real texture tree looks like and what a placement policy actually has to cope with.
$roots = @("textures", "textures", "textures", "meshes", "sounds", "anims")
$weights = 1..$Directories | ForEach-Object { 1.0 / $_ }
$weightTotal = ($weights | Measure-Object -Sum).Sum
$totalBytes = [int64]$TotalMB * 1MB

Write-Host "synthesising $TotalMB MB across $Directories directories..."
$random = [Random]::new(20260831)
$buffer = New-Object byte[] (1MB)
$written = [int64]0

for ($index = 1; $index -le $Directories; $index++) {
    $root = $roots[($index - 1) % $roots.Count]
    $directory = Join-Path $source ("{0}\dir{1:D3}" -f $root, $index)
    New-Item -ItemType Directory -Force -Path $directory | Out-Null

    $share = [int64]($totalBytes * ($weights[$index - 1] / $weightTotal))
    if ($share -lt 1MB) { $share = 1MB }

    # Several files per directory rather than one, so the member lists are the size the real
    # ones will be - the manifest and the archive index both scale with that, not with bytes.
    $fileCount = [Math]::Max(1, [Math]::Min(40, [int]($share / 4MB)))
    for ($file = 0; $file -lt $fileCount; $file++) {
        $bytes = [int64]($share / $fileCount)
        $stream = [IO.File]::Create((Join-Path $directory ("asset{0:D3}.dds" -f $file)))
        try {
            $remaining = $bytes
            while ($remaining -gt 0) {
                $chunk = [int][Math]::Min($buffer.Length, $remaining)
                $random.NextBytes($buffer)
                $stream.Write($buffer, 0, $chunk)
                $remaining -= $chunk
            }
        }
        finally { $stream.Dispose() }
        $written += $bytes
    }
}
Write-Host "wrote $([math]::Round($written / 1GB, 2)) GB"

# ---- the real policy, in layout mode ---------------------------------------------------------
$builder = Join-Path $repositoryRoot "tools\package\dead_air_x64_content_bundles.ps1"
$layout = & $builder -SourceRoot $source -BundleCache $cache `
    -OutputManifest (Join-Path $WorkRoot "content-manifest.txt") `
    -PortVersion "9.9.9" -ReleaseTag "content-9.9.9" -GroupsFile $groups `
    -WorkRoot (Join-Path $WorkRoot "work") -ConverterPath $ConverterPath -LayoutOnly

Write-Host ""
$layout | Format-Table -AutoSize

$failures = [Collections.Generic.List[string]]::new()
$largest = ($layout | Measure-Object -Property Bytes -Maximum).Maximum
$ceiling = [int64]1536 * 1MB

foreach ($shard in $layout) {
    if ($shard.Bytes -gt $ceiling) {
        $failures.Add("$($shard.Group)/$($shard.Shard) is $($shard.MB) MB, above the 1536 MB ceiling")
    }
}

# A layout is only worth having if it spreads. One shard holding most of the content means every
# release that touches it re-publishes most of the content, which is the whole thing sharding
# exists to prevent.
if ($largest -gt 0.5 * $written) {
    $failures.Add("one shard holds $([math]::Round(100.0 * $largest / $written))% of all content")
}

# And it has to grow into more than one shard per group, or the append-only table has nowhere to
# put the next directory except on top of an existing one.
$perGroup = $layout | Group-Object Group
foreach ($group in $perGroup) {
    $groupBytes = ($group.Group | Measure-Object -Property Bytes -Sum).Sum
    if ($groupBytes -gt [int64]500 * 1MB -and $group.Count -lt 2) {
        $failures.Add("group $($group.Name) holds $([math]::Round($groupBytes / 1MB)) MB in a single shard")
    }
}

Write-Host "$($layout.Count) shard(s), largest $([math]::Round($largest / 1MB, 1)) MB"
Write-Host ""
if ($failures.Count -eq 0) {
    Write-Host "content scale checks passed" -ForegroundColor Green
}
else {
    Write-Host "FAILURES:" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}
