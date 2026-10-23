# Copyright (c) 2026 Grizzly
# Released under the MIT licence, see License.txt
#
# The release's compiled shaders (r4_shaders.cpp, shader_pack_path): the QA rig runs every quality preset with each
# anti-aliasing mode on every renderer from an empty cache (tools\qa\water\shader_sweep.lua) and what it compiled is
# staged in artifacts\shader_pack; New-DeadAirCompatibilityArchive packs it as gamedata\shaders_cache_pack. The engine
# reads a file there where the player's own cache has none valid, so the first start after an update compiles only
# what the sweep did not reach. Bytecode does not depend on the GPU, but the DXIL and SPIR-V folders are named after
# the device's shader model, wave size and 16-bit support: the rig's card fills the NVIDIA ones.
#
#   pwsh -File tools\package\build_shader_pack.ps1 [-Renderers r4,r5,r6] [-Save qa_meadow] [-Seconds 900]
[CmdletBinding()]
param(
    [string[]]$Renderers = @("r4", "r5", "r6"),
    [string]$Save = "qa_meadow",
    [string]$Rig = "D:\Games\Dead Air\_qa\lightdiag",
    [int]$Seconds = 900
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
$cache = Join-Path $Rig "appdata\shaders_cache_grizzly"
$pack = Join-Path $repo "artifacts\shader_pack"

# From an empty cache: a file the sweep did not write is not in the pack, and none of an older build's stays.
if (Test-Path -LiteralPath $cache) { Remove-Item -LiteralPath $cache -Recurse -Force }
foreach ($renderer in $Renderers) {
    Write-Host "== sweep on $renderer"
    & (Join-Path $repo "tools\qa\Run-WaterQa.ps1") -Name "shader-pack-$renderer" -Scenario shader_sweep -Save $Save `
        -Seconds $Seconds -KeepShaderCache -ExtraArgs "-$renderer" | Out-Null
    $log = Join-Path $Rig "appdata\logs\grizzly_admin.log"
    if (-not (Select-String -LiteralPath $log -Pattern "\[qa\] sweep DONE" -Quiet)) {
        throw "the sweep on $renderer did not finish (see $log)"
    }
}

if (Test-Path -LiteralPath $pack) { Remove-Item -LiteralPath $pack -Recurse -Force }
New-Item -ItemType Directory -Path $pack -Force | Out-Null
# The pipeline libraries are the driver's, not bytecode: they stay out.
Get-ChildItem -LiteralPath $cache -Directory | Copy-Item -Destination $pack -Recurse
Get-ChildItem -LiteralPath $pack -Recurse -File -Include "pipelines.bin", "*.vkcache", "*.tmp" | Remove-Item -Force
$files = Get-ChildItem -LiteralPath $pack -Recurse -File
$bytes = ($files | Measure-Object -Property Length -Sum).Sum
Write-Host ("shader pack: {0} files, {1:N1} MB in {2}" -f $files.Count, ($bytes / 1MB), $pack)
foreach ($dir in Get-ChildItem -LiteralPath $pack -Directory) {
    $n = @(Get-ChildItem -LiteralPath $dir.FullName -Recurse -File).Count
    Write-Host ("  {0}: {1} files" -f $dir.Name, $n)
}
