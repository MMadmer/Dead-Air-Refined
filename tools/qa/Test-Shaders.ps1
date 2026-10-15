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
#   tools\qa\Test-Shaders.ps1 -Dxc                  # the DirectX 12 module's DXIL mode (SM 6.6)
#   tools\qa\Test-Shaders.ps1 -Dxc -Sm 6_0          # the oldest shader model the engine accepts
#
# Entry points are the top-level .vs, .ps and .cs files (and .gs/.hs/.ds when an override has them);
# a .h is checked through whatever includes it, and a .s is lua for the resource manager, not HLSL.
# The rt\ and native\ folders are left out: those passes compile through da_dxc_compile_native
# (HLSL 2021, their own includes), not the engine shader path.
#
# -Dxc compiles what xrRender_R5 builds when its engine shaders are DXIL (dxc.md 3.3): FXC's
# preprocessor with the engine's option set plus the GRIZZLY_* macros, then dxc.exe with the engine's
# arguments at Shader Model -Sm; every permutation carries SMAP_ARRAY (the DirectX 12 module sets it),
# and the DXC-only .ms/.as entry points are compiled too (HLSL 2021).
#
# Requires an unpacked copy of the shipped archive for the includes the overrides do not carry
# (shared\waterconfig.h and friends live only in the archive). -Unpack points at it.
[CmdletBinding()]
param(
    [string]$Unpack = "D:\Games\Dead Air\_work\da_unpack",
    [string]$Loose = "",
    [string]$Shader = "",
    [switch]$Dxc,
    [string]$Sm = "6_6",
    # dxc.exe of the vendored release (Externals\dxc keeps only the runtime DLLs); $env:DXC_EXE overrides.
    [string]$DxcExe = "",
    [switch]$KeepTree
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
if (-not $Loose) { $Loose = Join-Path $repo "packaging\dead-air-x64\compatibility\gamedata\shaders\r3" }

$fxc = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Filter fxc.exe -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.DirectoryName -like "*\x64" } | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $fxc) { throw "fxc.exe not found - install the Windows SDK" }

$dxcPath = $null
if ($Dxc) {
    $candidates = @($DxcExe, $env:DXC_EXE, "D:\Games\Dead Air\_work\sdk-downloads\dxc-1.9.2607\extracted\bin\x64\dxc.exe") +
        @(Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Filter dxc.exe -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.DirectoryName -like "*\x64" } | Sort-Object FullName -Descending | ForEach-Object FullName)
    $dxcPath = $candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
    if (-not $dxcPath) { throw "dxc.exe not found - pass -DxcExe" }
    $version = (& $dxcPath --version 2>&1 | Select-Object -First 1)
    Write-Output "DXC mode: $dxcPath ($version), SM $($Sm.Replace('_', '.'))"
}

# The include tree has to live somewhere without spaces in the path: fxc takes /I as one token and
# quoting it through the shells this repo uses is more trouble than a copy. One per process: several
# checks may run at once.
$tree = Join-Path $env:TEMP "dar-shader-check-$PID"
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
# The DirectX 12 module adds SMAP_ARRAY to every shader it compiles, and DXC mode exists only there;
# the engine adds the GRIZZLY_* macros for DXC only (this device: 32 wave lanes, 16-bit min precision).
if ($Dxc) { $always += @("SMAP_ARRAY=1", "GRIZZLY_DXC=1", "GRIZZLY_SM=$($Sm.Replace('_', ''))", "GRIZZLY_WAVE_LANES=32", "GRIZZLY_MIN16=1") }

$perms = @(
    @{ Name = "Extreme"; Defines = @("USE_SOFT_WATER=1", "USE_REFLECTIONS=1", "SSR_QUALITY=4", "SSR_JITTER=1", "GBUFFER_OPTIMIZATION=1", "MSAA_SAMPLES=1") }
    @{ Name = "High";    Defines = @("USE_SOFT_WATER=1", "USE_REFLECTIONS=1", "SSR_QUALITY=3", "GBUFFER_OPTIMIZATION=1", "MSAA_SAMPLES=1") }
    @{ Name = "Default"; Defines = @("USE_SOFT_WATER=1", "USE_REFLECTIONS=1", "SSR_QUALITY=2", "GBUFFER_OPTIMIZATION=1", "MSAA_SAMPLES=1") }
    @{ Name = "Low";     Defines = @("USE_REFLECTIONS=1", "SSR_QUALITY=1", "GBUFFER_OPTIMIZATION=1", "MSAA_SAMPLES=1") }
    @{ Name = "Minimum"; Defines = @("MSAA_SAMPLES=1") }
    @{ Name = "NoGbufOpt"; Defines = @("USE_SOFT_WATER=1", "USE_REFLECTIONS=1", "SSR_QUALITY=3", "MSAA_SAMPLES=1") }
    @{ Name = "MSAA4";   Defines = @("USE_SOFT_WATER=1", "USE_REFLECTIONS=1", "SSR_QUALITY=3", "GBUFFER_OPTIMIZATION=1", "USE_MSAA=1", "MSAA_SAMPLES=4") }
    # The DirectX 12 module (xrRender_R5) adds SMAP_ARRAY to every shader it compiles.
    @{ Name = "DX12";    Defines = @("USE_SOFT_WATER=1", "USE_REFLECTIONS=1", "SSR_QUALITY=3", "GBUFFER_OPTIMIZATION=1", "MSAA_SAMPLES=1", "SMAP_ARRAY=1") }
)
# Further permutations a DirectX 12 feature lane needs, @{ Name = ...; Defines = @(...) }.
# [dx12:async] begin perms
# [dx12:async] end perms
# [dx12:vrs] begin perms
# [dx12:vrs] end perms
# [dx12:gd] begin perms
# The GPU-driven trees' variant of the tree vertex shaders (gpudriven.md 6: DA_TREE_GD, a per-instance
# GDREF). Every other shader ignores the define, so a run aimed at one of them leaves the permutation out.
if (-not $Shader -or $Shader -match '^(deffer_tree_(flat|bump)|shadow_direct_tree)\.vs$') {
    $perms += @{ Name = "TreeGD"; Defines = @("USE_SOFT_WATER=1", "USE_REFLECTIONS=1", "SSR_QUALITY=3", "GBUFFER_OPTIMIZATION=1", "MSAA_SAMPLES=1", "SMAP_ARRAY=1", "DA_TREE_GD=1") }
    $perms += @{ Name = "TreeGD-aref"; Defines = @("GBUFFER_OPTIMIZATION=1", "MSAA_SAMPLES=1", "SMAP_ARRAY=1", "DA_TREE_GD=1", "USE_AREF=1") }
}
# [dx12:gd] end perms
# [dx12:mesh] begin perms
# The waving grass of the mesh path (deffer_detail_mesh.ms with DETAIL_WAVE: element 2 of the detail
# blender); a run aimed at a grass shader also gets the static-sun layout of the interpolants, wave
# and still. Every other shader ignores both defines.
if (-not $Shader -or $Shader -match '^deffer_detail_(mesh\.ms|cull\.as|[ws]_flat\.vs)$') {
    $perms += @{ Name = "GrassWave"; Defines = @("GBUFFER_OPTIMIZATION=1", "MSAA_SAMPLES=1", "SMAP_ARRAY=1", "DETAIL_WAVE=1") }
    # The shadow-pass variant (elements 4/5, DETAIL_DEPTH); its still twin only in a run aimed at it.
    $perms += @{ Name = "GrassWaveDepth"; Defines = @("GBUFFER_OPTIMIZATION=1", "MSAA_SAMPLES=1", "SMAP_ARRAY=1", "DETAIL_WAVE=1", "DETAIL_DEPTH=1") }
}
if ($Shader -match '^deffer_detail_(mesh\.ms|cull\.as|[ws]_flat\.vs)$') {
    $perms += @{ Name = "GrassWave-sun"; Defines = @("GBUFFER_OPTIMIZATION=1", "MSAA_SAMPLES=1", "SMAP_ARRAY=1", "DETAIL_WAVE=1", "USE_R2_STATIC_SUN=1") }
    $perms += @{ Name = "Grass-sun"; Defines = @("GBUFFER_OPTIMIZATION=1", "MSAA_SAMPLES=1", "SMAP_ARRAY=1", "USE_R2_STATIC_SUN=1") }
    $perms += @{ Name = "GrassDepth"; Defines = @("GBUFFER_OPTIMIZATION=1", "MSAA_SAMPLES=1", "SMAP_ARRAY=1", "DETAIL_DEPTH=1") }
}
# [dx12:mesh] end perms
# [dx12:rt] begin perms
# [dx12:rt] end perms

$fxcProfiles = @{ ".vs" = "vs_5_0"; ".ps" = "ps_5_0"; ".cs" = "cs_5_0"; ".gs" = "gs_5_0"; ".hs" = "hs_5_0"; ".ds" = "ds_5_0" }
# Mesh and amplification shaders exist only in DXC mode (new-only extensions, HLSL 2021).
$dxcOnly = @(".ms", ".as")
# Below Shader Model 6.5 the engine has no mesh path.
$meshSm = $Dxc -and ([version]($Sm.Replace('_', '.')) -ge [version]'6.5')
$extensions = @($fxcProfiles.Keys) + $(if ($meshSm) { $dxcOnly } else { @() })

$targets = if ($Shader) {
        if ($Shader -match '^(rt|native)[\\/]') {
            Write-Output "$Shader is a native pass (rt\ and native\ compile through da_dxc_compile_native): not checked here"
            exit 0
        }
        @(Get-Item (Join-Path $tree $Shader))
    }
    else { Get-ChildItem $Loose -File | Where-Object { $_.Extension -in $extensions } |
           ForEach-Object { Get-Item (Join-Path $tree $_.Name) } }

# Permutations the engine never actually asks for, and which fail for that reason rather than
# because the shader is wrong. Each one is a shader whose C++ blender supplies state the harness
# does not model:
#   gather.ps       - the shadow gather declares s_smap through the blender, not through common.h,
#                     so it cannot be compiled standalone at all.
#   gtao_render.ps  - reads the depth target with SampleLevel, which is only valid while MSAA is
#                     off; the engine does not run GTAO with MSAA enabled.
#   ssao_hbao.ps    - no main(): an include of combine_1/ssao_calc under USE_HBAO (console-only
#                     r2_ssao_mode), checked through them; it is in the overlay for a DXC fix only.
# Anything NOT on this list that fails is a real failure.
$known = @(
    @{ Shader = "gather.ps";      Perm = "*" }
    @{ Shader = "gtao_render.ps"; Perm = "MSAA4" }
    @{ Shader = "ssao_hbao.ps";   Perm = "*" }
)
# Known-unbuildable permutations of a DirectX 12 feature lane's shaders, @{ Shader = ...; Perm = ... }.
# [dx12:async] begin known
# The compute GTAO render pass, like its pixel pass, reads the single-sampled G-buffer: GTAO is
# never created under MSAA.
$known += @{ Shader = "gtao_render.cs"; Perm = "MSAA4" }
# [dx12:async] end known
# [dx12:vrs] begin known
# [dx12:vrs] end known
# [dx12:gd] begin known
# [dx12:gd] end known
# [dx12:mesh] begin known
# [dx12:mesh] end known
# [dx12:rt] begin known
# [dx12:rt] end known

# dxc.md 3.3 step 4 (release build); the FXC-preprocessed text is self-contained.
$dxcArgs = @("-Zpr", "-O3", "-flegacy-resource-reservation", "-Qstrip_debug", "-Qstrip_reflect",
    "-Wno-conversion", "-Wno-for-redefinition", "-Wno-effects-syntax")
$preprocessed = Join-Path $tree "_dxc_input.hlsl"

$fail = 0
$known_hit = 0
$run = 0
foreach ($t in $targets) {
    # A model vertex shader has one main() per skinning mode and the engine compiles it once per
    # mode (r4_shaders.cpp: SKIN_NONE for everything else), so each mode is a permutation too.
    $skins = if ($t.Extension -eq ".vs" -and (Select-String -LiteralPath $t.FullName -Pattern "SKIN_0|skin\.h|deffer_model_" -Quiet)) {
        @("SKIN_NONE", "SKIN_0", "SKIN_1", "SKIN_2", "SKIN_3", "SKIN_4")
    } else { @("") }
    foreach ($p in $perms) {
      foreach ($skin in $skins) {
        $name = if ($skin) { "$($p.Name) $skin" } else { $p.Name }
        $defs = @()
        foreach ($d in $always) { $defs += @("/D", $d) }
        foreach ($d in $p.Defines) { $defs += @("/D", $d) }
        if ($skin) { $defs += @("/D", "$skin=1") }
        if ($Dxc) {
            # FXC's preprocessor first, as the engine does, then DXC on the self-contained text.
            $out = & $fxc.FullName /nologo /I $tree @defs /P $preprocessed $t.FullName 2>&1
            if ($LASTEXITCODE -eq 0) {
                $stage = $t.Extension.Substring(1)
                $hv = if ($t.Extension -in $dxcOnly) { "2021" } else { "2018" }
                $out = & $dxcPath -T "$($stage)_$Sm" -E main -HV $hv @dxcArgs -Fo NUL $preprocessed 2>&1
            }
            $errors = $out | Where-Object { $_ -match "(: error|error:|error X)" }
            if (-not $errors -and $LASTEXITCODE -ne 0) { $errors = @($out | Select-Object -First 3) + "exit code $LASTEXITCODE" }
            $warnPattern = "warning"
        } else {
            if ($t.Extension -in $dxcOnly) { continue }
            # No /Fo on purpose: only the diagnostics matter, and a script that keeps writing a
            # freshly created binary into a temp folder is exactly the shape a heuristic antivirus
            # goes after. fxc still compiles and still reports every error without an output file.
            $out = & $fxc.FullName /nologo /I $tree @defs /T $fxcProfiles[$t.Extension] /E main $t.FullName 2>&1
            # Some errors carry no file position ("error X3501: 'main': entrypoint not found").
            $errors = $out | Where-Object { $_ -match ": error |^error X" }
            $warnPattern = ": warning "
        }
        $run++
        if ($errors) {
            $expected = $known | Where-Object { $_.Shader -eq $t.Name -and ($_.Perm -eq "*" -or $_.Perm -eq $p.Name) }
            if ($expected) {
                $known_hit++
                if ($VerbosePreference -eq "Continue") { Write-Output "known $($t.Name) [$name]" }
            }
            else {
                $fail++
                Write-Output "FAIL  $($t.Name) [$name]"
                $errors | ForEach-Object { Write-Output "        $_" }
            }
        }
        elseif ($VerbosePreference -eq "Continue") {
            $warn = $out | Where-Object { $_ -match $warnPattern }
            if ($warn) {
                Write-Output "warn  $($t.Name) [$name]"
                $warn | ForEach-Object { Write-Output "        $_" }
            }
        }
      }
    }
}

if (-not $KeepTree) { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
Write-Output ""
Write-Output "$run compile(s)$(if ($Dxc) { ' with DXC' }), $fail failure(s), $known_hit known-unbuildable permutation(s) skipped"
if ($fail) { exit 1 }
