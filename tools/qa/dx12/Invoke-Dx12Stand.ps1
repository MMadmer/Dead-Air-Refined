# One stand run of the DirectX 12 feature work (PLAN.md 0.4). Stages the renderer binaries from
# bin\x64\Release under ninja.lock (or takes a snapshot with -Bin), then, under stand.lock, copies
# them and the gamedata overlay into the rig, records their SHA-256 in the run folder and runs
# tools\qa\Run-WaterQa.ps1.
#
#   tools\qa\dx12\Invoke-Dx12Stand.ps1 -Name vrs-v4-sunlight -Save qa_pedro -Tga
#   tools\qa\dx12\Invoke-Dx12Stand.ps1 -Name core-g0-pedro-r4-a -ExtraArgs '-always_active' -Tga `
#       -Bin <baseline>\bin -Overlay <baseline>\gamedata
#
# Runs land in D:\Games\Dead Air\_work\dx12-features-20260928\qa\<Name>. Every run re-copies the
# runtime DLLs, so a fallback test that renamed one in the rig is undone by the next run.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Name,
    [string]$Scenario = 'render_check',
    [string]$Save = 'qa_pedro',
    [string]$Weather = 'af3_bright_clear_4',
    # Ceiling for load + scenario; the run stops at the scenario's marker.
    [int]$Seconds = 150,
    [string]$ExtraArgs = '-r5 -always_active',
    # Console commands before the save loads; 'a;b' separates several.
    [string[]]$PreCommands = @(),
    [string]$Globals = '',
    # A binary snapshot to run instead of bin\x64\Release (no ninja.lock needed).
    [string]$Bin = '',
    # A gamedata overlay to run instead of the working tree's packaging overlay.
    [string]$Overlay = '',
    # The caller holds ninja.lock already (Invoke-Dx12Perf).
    [switch]$NinjaHeld,
    # The caller holds stand.lock already and keeps it until its own work on the rig engine is done
    # (a tool that attaches to the engine after the scenario, like the tree draw counter).
    [switch]$StandHeld,
    # Lossless parity shot: -ss_tga and qa_tga = true (render_check).
    [switch]$Tga,
    # Further module base names to deploy with the renderer, e.g. xrGame.
    [string[]]$ExtraModules = @(),
    [switch]$ClearShaderCache,
    # Grab the window on the scenario's "[qa] SHOT <n>" lines into <run>\window (UI scenarios).
    [switch]$WindowShots,
    [string]$Out = 'D:\Games\Dead Air\_work\dx12-features-20260928\qa',
    [string]$Rig = 'D:\Games\Dead Air\_qa\lightdiag'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Dx12Locks.ps1')
# 'a;b' is two commands: pwsh -File from another shell hands a string[] over as one string.
$PreCommands = @($PreCommands | ForEach-Object { $_ -split ';' } | ForEach-Object Trim | Where-Object { $_ })
$repo = $script:Dx12Repo

$modules = @('xrRender_R4', 'xrRender_R5', 'xrCore') + $ExtraModules
$runtime = @('dxcompiler.dll', 'dxil.dll', 'dstorage.dll', 'dstoragecore.dll')
$overlayDir = if ($Overlay) { $Overlay } else { Join-Path $repo 'packaging\dead-air-x64\compatibility\gamedata' }

function Get-BinaryList([string]$dir) {
    $files = foreach ($m in $modules) { foreach ($ext in '.dll', '.pdb') { Join-Path $dir "$m$ext" } }
    $files += $runtime | ForEach-Object { Join-Path $dir $_ }
    $files | Where-Object { Test-Path -LiteralPath $_ }
}

# Stage: copy the binaries out of bin while no build can write them.
$staging = $null
if ($Bin) {
    $source = (Resolve-Path -LiteralPath $Bin).Path
} else {
    $staging = Join-Path $env:TEMP ('dx12stand-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force -Path $staging | Out-Null
    $ninja = if ($NinjaHeld) { $null } else { Enter-Lock 'ninja' }
    try {
        Get-BinaryList (Join-Path $repo 'bin\x64\Release') | ForEach-Object { Copy-Item -LiteralPath $_ -Destination $staging -Force }
    } finally { Exit-Lock $ninja }
    $source = $staging
}

if ($Tga) {
    if ($ExtraArgs -notmatch '-ss_tga') { $ExtraArgs = "$ExtraArgs -ss_tga" }
    $Globals = "$Globals`nqa_tga = true"
    # The weather clock pin of the Gate-0 references (each save's ten minutes): the scenario would
    # otherwise take it from the level clock, which a slow load carries across a boundary.
    $pins = @{ qa_pedro = '17:10'; foliage = '9:30'; trees = '9:00'; fogqa = '9:50' }
    if ($pins.ContainsKey($Save) -and $Globals -notmatch 'qa_env_pin') { $Globals = "$Globals`nqa_env_pin = `"$($pins[$Save])`"" }
}

$stand = if ($StandHeld) { $null } else { Enter-Lock 'stand' }
try {
    # A rig engine still alive here belongs to the previous holder, which released the lock while it
    # was still shutting down: it gets 30 s to exit, then it is stopped (the stand is ours under the
    # lock), and the rig's log must be free before this run starts one.
    $rigEngines = { @(Get-CimInstance Win32_Process -Filter "Name='xrEngine.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.ExecutablePath -like "$Rig\*" }) }
    $deadline = (Get-Date).AddSeconds(30)
    while ((& $rigEngines).Count -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 2 }
    foreach ($engine in & $rigEngines) {
        Write-Host "stopping leftover rig engine $($engine.ProcessId)"
        Stop-Process -Id $engine.ProcessId -Force -ErrorAction SilentlyContinue
        Wait-Process -Id $engine.ProcessId -Timeout 15 -ErrorAction SilentlyContinue
    }
    $rigLog = Join-Path $Rig 'appdata\logs\grizzly_admin.log'
    for ($i = 0; $i -lt 30 -and (Test-Path -LiteralPath $rigLog); $i++) {
        try { [IO.File]::Open($rigLog, 'Open', 'ReadWrite', 'None').Dispose(); break } catch { Start-Sleep -Seconds 2 }
    }

    $hashes = foreach ($file in Get-BinaryList $source) {
        Copy-Item -LiteralPath $file -Destination $Rig -Force
        '{0}  {1}' -f (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash, (Split-Path $file -Leaf)
    }

    # The overlay: shaders, top-level configs, UI, strings, the option pages.
    $rigData = Join-Path $Rig 'gamedata'
    Copy-Item (Join-Path $overlayDir 'shaders\*') (Join-Path $rigData 'shaders\') -Recurse -Force
    Copy-Item (Join-Path $overlayDir 'configs\*.ltx') (Join-Path $rigData 'configs\') -Force
    Copy-Item (Join-Path $overlayDir 'configs\ui\*') (Join-Path $rigData 'configs\ui\') -Recurse -Force
    # The textures those ui configs name.
    Copy-Item (Join-Path $overlayDir 'textures\*') (Join-Path $rigData 'textures\') -Recurse -Force
    foreach ($lang in 'rus', 'eng') {
        Copy-Item (Join-Path $overlayDir "configs\text\$lang\*") (Join-Path $rigData "configs\text\$lang\") -Recurse -Force
    }
    Get-ChildItem (Join-Path $overlayDir 'scripts') -File | Where-Object {
        $_.Name -like 'ui_mm_opt*.script' -or $_.Name -in 'dead_air_x64_option_list.script', 'atmosfear_options.script'
    } | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $rigData 'scripts\') -Force }

    $dest = Join-Path $Out $Name
    $watch = $null
    if ($WindowShots) {
        $window = Join-Path $dest 'window'
        if (Test-Path -LiteralPath $window) { Remove-Item -LiteralPath $window -Recurse -Force }
        $watch = Start-Job -ScriptBlock { param($script, $o, $s, $r) & $script -Out $o -Seconds $s -Rig $r } `
            -ArgumentList (Join-Path $repo 'tools\qa\Watch-QaShots.ps1'), $window, ($Seconds + 150), $Rig
    }
    $started = Get-Date
    & (Join-Path $repo 'tools\qa\Run-WaterQa.ps1') -Name $Name -Scenario $Scenario -Save $Save -Weather $Weather `
        -Seconds $Seconds -KeepShaderCache:(-not $ClearShaderCache) -ExtraArgs $ExtraArgs -PreCommands $PreCommands `
        -Globals $Globals -Overlay $overlayDir -Out $Out -Rig $Rig

    if ($watch) {
        Wait-Job $watch -Timeout 90 | Out-Null
        Receive-Job $watch -ErrorAction SilentlyContinue | ForEach-Object { "  window: $_" }
        Remove-Job $watch -Force
    }
    @("run: $Name", "started: $(Get-Date $started -Format s)", "seconds: $([int]((Get-Date) - $started).TotalSeconds)",
        "scenario: $Scenario", "save: $Save", "weather: $Weather", "args: $ExtraArgs",
        "pre: $($PreCommands -join ' | ')", "globals: $($Globals -replace "`n", ' ; ')",
        "bin: $(if ($Bin) { $Bin } else { 'bin\x64\Release (staged)' })", "overlay: $overlayDir", '') + $hashes |
        Set-Content -LiteralPath (Join-Path $dest 'run.txt') -Encoding UTF8
} finally {
    Remove-Item -LiteralPath (Join-Path $Rig 'gamedata\scripts\qa_water.script') -Force -ErrorAction SilentlyContinue
    Exit-Lock $stand
    if ($staging) { Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue }
}
