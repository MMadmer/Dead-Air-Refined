# Compiles the loose shader overrides the way the engine would, without the engine.
#
# The renderer only compiles a shader when a level first needs it, so a syntax error or a bad
# overload reaches you as a black surface or a hard fault in the middle of a level load, ten
# minutes after you wrote it. This runs fxc over the same include tree the game assembles - the
# packed archive first, its xtra and ours overlays on top, then gamedata/shaders/r3 - and does it
# for every quality permutation the preset ladder can produce.
#
#   tools\qa\Test-Shaders.ps1                       # every shader with a loose override
#   tools\qa\Test-Shaders.ps1 -Shader water_soft.ps # one of them, all permutations
#   tools\qa\Test-Shaders.ps1 -Verbose              # keep the warnings too
#
# Requires an unpacked copy of the shipped archive for the includes the overrides do not carry
# (shared\waterconfig.h and friends live only in the archive). -Unpack points at it.
[CmdletBinding()]
param(
    [string]$Unpack = "D:\Games\Dead Air\_work\da_unpack",
    [string]$Loose = "",
    [string]$Shader = "",
    [switch]$KeepTree
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
if (-not $Loose) { $Loose = Join-Path $repo "packaging\dead-air-x64\compatibility\gamedata\shaders\r3" }

$fxc = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Filter fxc.exe -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.DirectoryName -like "*\x64" } | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $fxc) { throw "fxc.exe not found - install the Windows SDK" }

# The include tree has to live somewhere without spaces in the path: fxc takes /I as one token and
# quoting it through the shells this repo uses is more trouble than a copy.
$tree = Join-Path $env:TEMP "dar-shader-check"
if (Test-Path -LiteralPath $tree) { Remove-Item -LiteralPath $tree -Recurse -Force }
New-Item -ItemType Directory -Force -Path $tree | Out-Null

foreach ($layer in @("configs\shaders\r3", "xtra\shaders\r3", "ours\shaders\r3")) {
    $src = Join-Path $Unpack $layer
    if (Test-Path -LiteralPath $src) { Copy-Item (Join-Path $src "*") $tree -Recurse -Force }
}
Copy-Item (Join-Path $Loose "*") $tree -Recurse -Force

# The permutations the preset ladder can actually produce. r4_shaders.cpp is the source of truth
# for the names; these are the ones any of the water, rain or combine shaders branch on.
# Options the engine sets for every shader it compiles, independent of the quality tier. Without
# them the shadow and gather shaders fail on things the game never sees: SMAP_size is the shadow
# map's resolution, and USE_HWSMAP decides whether a shadow vertex shader carries an explicit
# depth output at all.
$always = @("SM_5=1", "SMAP_size=2048", "USE_HWSMAP=1")

$perms = @(
    @{ Name = "Extreme"; Defines = @("USE_SOFT_WATER=1", "USE_REFLECTIONS=1", "SSR_QUALITY=4", "SSR_JITTER=1", "GBUFFER_OPTIMIZATION=1", "MSAA_SAMPLES=1") }
    @{ Name = "High";    Defines = @("USE_SOFT_WATER=1", "USE_REFLECTIONS=1", "SSR_QUALITY=3", "GBUFFER_OPTIMIZATION=1", "MSAA_SAMPLES=1") }
    @{ Name = "Default"; Defines = @("USE_SOFT_WATER=1", "USE_REFLECTIONS=1", "SSR_QUALITY=2", "GBUFFER_OPTIMIZATION=1", "MSAA_SAMPLES=1") }
    @{ Name = "Low";     Defines = @("USE_REFLECTIONS=1", "SSR_QUALITY=1", "GBUFFER_OPTIMIZATION=1", "MSAA_SAMPLES=1") }
    @{ Name = "Minimum"; Defines = @("MSAA_SAMPLES=1") }
    @{ Name = "NoGbufOpt"; Defines = @("USE_SOFT_WATER=1", "USE_REFLECTIONS=1", "SSR_QUALITY=3", "MSAA_SAMPLES=1") }
    @{ Name = "MSAA4";   Defines = @("USE_SOFT_WATER=1", "USE_REFLECTIONS=1", "SSR_QUALITY=3", "GBUFFER_OPTIMIZATION=1", "USE_MSAA=1", "MSAA_SAMPLES=4") }
)

# Only entry-point files are compiled. A .h is checked through whatever includes it, and a .s is
# lua for the resource manager, not HLSL.
$targets = if ($Shader) { @(Get-Item (Join-Path $tree $Shader)) }
           else { Get-ChildItem $Loose -File | Where-Object { $_.Extension -in ".ps", ".vs" } |
                  ForEach-Object { Get-Item (Join-Path $tree $_.Name) } }

# Permutations the engine never actually asks for, and which fail for that reason rather than
# because the shader is wrong. Each one is a shader whose C++ blender supplies state the harness
# does not model:
#   gather.ps       - the shadow gather declares s_smap through the blender, not through common.h,
#                     so it cannot be compiled standalone at all.
#   gtao_render.ps  - reads the depth target with SampleLevel, which is only valid while MSAA is
#                     off; the engine does not run GTAO with MSAA enabled.
# Anything NOT on this list that fails is a real failure.
$known = @(
    @{ Shader = "gather.ps";      Perm = "*" }
    @{ Shader = "gtao_render.ps"; Perm = "MSAA4" }
)

$fail = 0
$known_hit = 0
$run = 0
foreach ($t in $targets) {
    $profile = if ($t.Extension -eq ".vs") { "vs_5_0" } else { "ps_5_0" }
    foreach ($p in $perms) {
        $args = @("/nologo", "/I", $tree)
        foreach ($d in $always) { $args += @("/D", $d) }
        foreach ($d in $p.Defines) { $args += @("/D", $d) }
        # No /Fo on purpose: only the diagnostics matter, and a script that keeps writing a
        # freshly created binary into a temp folder is exactly the shape a heuristic antivirus
        # goes after. fxc still compiles and still reports every error without an output file.
        $args += @("/T", $profile, "/E", "main", $t.FullName)
        $out = & $fxc.FullName @args 2>&1
        $run++
        $errors = $out | Where-Object { $_ -match ": error " }
        if ($errors) {
            $expected = $known | Where-Object { $_.Shader -eq $t.Name -and ($_.Perm -eq "*" -or $_.Perm -eq $p.Name) }
            if ($expected) {
                $known_hit++
                if ($VerbosePreference -eq "Continue") { Write-Output "known $($t.Name) [$($p.Name)]" }
            }
            else {
                $fail++
                Write-Output "FAIL  $($t.Name) [$($p.Name)]"
                $errors | ForEach-Object { Write-Output "        $_" }
            }
        }
        elseif ($VerbosePreference -eq "Continue") {
            $warn = $out | Where-Object { $_ -match ": warning " }
            if ($warn) {
                Write-Output "warn  $($t.Name) [$($p.Name)]"
                $warn | ForEach-Object { Write-Output "        $_" }
            }
        }
    }
}

if (-not $KeepTree) { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
Write-Output ""
Write-Output "$run compile(s), $fail failure(s), $known_hit known-unbuildable permutation(s) skipped"
if ($fail) { exit 1 }
