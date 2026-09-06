# A small synthetic content set for the content-flow and installer tests: a real build by the
# shipping bundle builder (not layout mode), so the manifest, the names and the bundles are
# exactly what a release produces - only smaller (about 21 MB over four bundles). Prints the
# asset root and the manifest path.
[CmdletBinding()]
param(
    [string]$WorkRoot = (Join-Path $env:TEMP "dar-synth-content"),
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
    [int]$FilesPerDir = 6,
    [int]$FileKB = 700
)
$ErrorActionPreference = "Stop"
$source = Join-Path $WorkRoot "source\gamedata"
$cache = Join-Path $WorkRoot "cache"
$groups = Join-Path $WorkRoot "groups.ltx"
if (Test-Path -LiteralPath $WorkRoot) { Remove-Item -LiteralPath $WorkRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $source, $cache | Out-Null
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

# A fixed seed: the same set every time, so a bundle name in a failure report means something.
$random = [Random]::new(20260905)
$buffer = New-Object byte[] ($FileKB * 1KB)
foreach ($dir in @("textures\synth_a", "textures\synth_b", "meshes\synth", "sounds\synth", "anims\synth")) {
    $path = Join-Path $source $dir
    New-Item -ItemType Directory -Force -Path $path | Out-Null
    for ($i = 0; $i -lt $FilesPerDir; $i++) {
        $random.NextBytes($buffer)
        [IO.File]::WriteAllBytes((Join-Path $path ("asset{0:D2}.dds" -f $i)), $buffer)
    }
}

$builder = Join-Path $RepositoryRoot "tools\package\dead_air_x64_content_bundles.ps1"
$manifest = Join-Path $WorkRoot "content-manifest.txt"
& $builder -SourceRoot $source -BundleCache $cache -OutputManifest $manifest `
    -PortVersion "9.9.9" -ReleaseTag "content-9.9.9" -GroupsFile $groups `
    -WorkRoot (Join-Path $WorkRoot "work") | Out-Host
"ASSET_ROOT=$cache"
"MANIFEST=$manifest"
Get-Content -LiteralPath $manifest | Select-Object -First 12
