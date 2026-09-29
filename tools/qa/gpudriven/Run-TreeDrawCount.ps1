# One stand run with the tree draw counter attached (gpudriven.md 2.6, PLAN.md G0): Invoke-Dx12Stand
# runs render_check in parity mode (-Tga, SMAA, wind frozen, game time and weather clock pinned) with
# a long FPS window; once the lossless shot is taken the counter attaches to the rig engine, counts
# -Frames frames and detaches. tree_draws.json / tree_draws.txt land in the run folder.
#
#   tools\qa\gpudriven\Run-TreeDrawCount.ps1 -Name gd-g0-count-pedro -Save qa_pedro `
#       -Bin <baseline>\bin -Overlay <baseline>\gamedata -ExtraModules xrEngine
#   tools\qa\gpudriven\Run-TreeDrawCount.ps1 -Name gd-g4-count-pedro-off -PreCommands 'r__gpu_driven 0'
#
# -NoParity counts the free-running render_check instead (TAA as the rig is set, the clock running).
# The counter works on the rig engine outside the scenario's control, so this script holds stand.lock
# itself until the counter is done and the engine is gone (Invoke-Dx12Stand -StandHeld); without -Bin
# it stages bin\x64\Release under ninja.lock first (ninja before stand).
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Name,
    [string]$Save = 'qa_pedro',
    [int]$Frames = 12,
    [string]$ExtraArgs = '-r5 -always_active',
    [string[]]$PreCommands = @(),
    # Console commands of the parity recipe, run after load (render_check's qa_render_cmds).
    [string[]]$RenderCommands = @('r__aa st_aa_smaa', 'wind_freeze 1'),
    [switch]$NoParity,
    # The FPS window the counter attaches into; it has to outlast symbol loading and counting.
    [int]$WindowMs = 40000,
    [string]$Bin = '',
    [string]$Overlay = '',
    [string[]]$ExtraModules = @(),
    [string]$Out = 'D:\Games\Dead Air\_work\dx12-features-20260928\qa',
    [string]$Rig = 'D:\Games\Dead Air\_qa\lightdiag'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\dx12\Dx12Locks.ps1')
$repo = $script:Dx12Repo
$counter = Join-Path $PSScriptRoot 'count_tree_draws.py'
if (-not $env:DX12_LANE) { $env:DX12_LANE = 'gd' }

# The scenario copy in the rig carries this marker, so the engine found later is this run's.
$marker = "count_tree_draws $Name $([guid]::NewGuid().ToString('N').Substring(0, 8))"
$globals = @("qa_fps_ms = $WindowMs", "-- $marker")
if (-not $NoParity) {
    $globals = @('qa_render_cmds = { ' + (($RenderCommands | ForEach-Object { '"' + $_ + '"' }) -join ', ') + ' }') + $globals
}
$module = if ($ExtraArgs -match '(^|\s)-r5(\s|$)') { 'xrRender_R5' } else { 'xrRender_R4' }

$temp = Join-Path $env:TEMP "treedraws-$Name"
if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force }
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$json = Join-Path $temp 'tree_draws.json'
$text = Join-Path $temp 'tree_draws.txt'

if (-not $Bin) {
    # The same binaries Invoke-Dx12Stand would stage, copied while no build can write them.
    $Bin = Join-Path $temp 'bin'
    New-Item -ItemType Directory -Force -Path $Bin | Out-Null
    $release = Join-Path $repo 'bin\x64\Release'
    $files = foreach ($m in @('xrRender_R4', 'xrRender_R5', 'xrCore') + $ExtraModules) { "$m.dll", "$m.pdb" }
    $files += 'dxcompiler.dll', 'dxil.dll', 'dstorage.dll', 'dstoragecore.dll'
    $ninja = Enter-Lock 'ninja'
    try {
        $files | ForEach-Object { Join-Path $release $_ } | Where-Object { Test-Path -LiteralPath $_ } |
            ForEach-Object { Copy-Item -LiteralPath $_ -Destination $Bin -Force }
    } finally { Exit-Lock $ninja }
}
$stand = @{
    Name = $Name; Save = $Save; Scenario = 'render_check'; Seconds = 100 + [int]($WindowMs / 1000)
    ExtraArgs = $ExtraArgs; PreCommands = $PreCommands; Globals = ($globals -join "`n"); Bin = $Bin
    Tga = (-not $NoParity); ExtraModules = $ExtraModules; Out = $Out; Rig = $Rig; StandHeld = $true
}
if ($Overlay) { $stand.Overlay = $Overlay }

$scenario = Join-Path $Rig 'gamedata\scripts\qa_water.script'
$pidFile = Join-Path ([IO.Path]::GetTempPath()) 'dar-probe-engine.pid'
$shots = Join-Path $Rig 'appdata\screenshots'
# render_check starts its 2 s r__capture right after the lossless shot (parity) or the settle
# (free-running); 2 s later its FPS window opens. The capture folder is the cue, so the engine needs
# no log flushing and the run's frame stays a normal parity frame.
$delay = 5
$markedAt = $null; $engine = $null; $seenAt = $null; $counted = $false

$lock = Enter-Lock 'stand'
# From here on a failing step must not skip the copy of what was counted nor the lock release.
$ErrorActionPreference = 'Continue'
try {
    # The previous holder's engine may still be shutting down (its lock released after a 10 s wait):
    # let it exit and let go of the rig's binaries before the helper copies over them.
    Get-Process xrEngine -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Rig\*" } |
        ForEach-Object { $_.WaitForExit(30000) | Out-Null }
    $deadline = (Get-Date).AddSeconds(60)
    foreach ($file in 'xrRender_R4.dll', 'xrRender_R5.dll', 'xrCore.dll') {
        while ((Get-Date) -lt $deadline) {
            try { [IO.File]::Open((Join-Path $Rig $file), 'Open', 'ReadWrite', 'None').Dispose(); break }
            catch [IO.FileNotFoundException] { break }
            catch { Start-Sleep -Seconds 1 }
        }
    }
    $job = Start-ThreadJob -ScriptBlock {
        param($script, $params)
        & $script @params
    } -ArgumentList (Join-Path $repo 'tools\qa\dx12\Invoke-Dx12Stand.ps1'), $stand

    while ($job.State -in 'NotStarted', 'Running') {
        Start-Sleep -Milliseconds 500
        if (-not $markedAt) {
            if ((Test-Path -LiteralPath $scenario) -and (Select-String -LiteralPath $scenario -SimpleMatch $marker -Quiet)) {
                $markedAt = (Get-Item -LiteralPath $scenario).LastWriteTime
            }
            continue
        }
        if (-not $engine) {
            if ((Test-Path -LiteralPath $pidFile) -and (Get-Item -LiteralPath $pidFile).LastWriteTime -ge $markedAt) {
                $raw = (Get-Content -LiteralPath $pidFile -Raw -ErrorAction SilentlyContinue)
                if ($raw -and $raw.Trim()) {
                    $engine = Get-Process -Id ([int]$raw.Trim()) -ErrorAction SilentlyContinue |
                        Where-Object { $_.Path -like "$Rig\*" -and $_.StartTime -ge $markedAt }
                }
            }
            continue
        }
        if (-not $seenAt) {
            if (Get-ChildItem -LiteralPath $shots -Directory -Filter 'capture_*' -ErrorAction SilentlyContinue |
                    Where-Object CreationTime -ge $engine.StartTime) {
                $seenAt = Get-Date
            }
            continue
        }
        if (((Get-Date) - $seenAt).TotalSeconds -lt $delay) { continue }
        # The counter must be done well inside the FPS window.
        $budget = [Math]::Max(10, [int]($WindowMs / 1000) - $delay - 5)
        & python $counter $engine.Id --frames $Frames --module $module --timeout $budget --json $json 2>&1 |
            Tee-Object -FilePath $text
        $counted = $true
        break
    }
    Receive-Job $job -Wait -AutoRemoveJob
} finally {
    # The run's engine is ours: it must be gone before the next run copies binaries into the rig.
    if ($engine -and -not $engine.HasExited -and -not $engine.WaitForExit(30000)) {
        Write-Warning "$Name : stopping the run's engine $($engine.Id)"
        Stop-Process -Id $engine.Id -Force -ErrorAction SilentlyContinue
        $engine.WaitForExit(15000) | Out-Null
    }
    Exit-Lock $lock
}

$dest = Join-Path $Out $Name
if ((Test-Path -LiteralPath $dest) -and (Test-Path -LiteralPath $text)) {
    Copy-Item -LiteralPath $text -Destination $dest -Force
    if (Test-Path -LiteralPath $json) { Copy-Item -LiteralPath $json -Destination $dest -Force }
}
if (-not $counted) { Write-Warning "$Name : the counter never attached (engine or capture folder not seen)" }
elseif (-not (Test-Path -LiteralPath $json)) { Write-Warning "$Name : the counter produced no result" }
Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
