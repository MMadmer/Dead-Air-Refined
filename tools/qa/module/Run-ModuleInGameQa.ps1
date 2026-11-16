<#
.SYNOPSIS
    A built module in the real game: it mounts, its items exist, and every item with a first-person model can be
    held and every consumable used without the game stopping (MODDING.md, "A module's items").

.DESCRIPTION
    Starts the freshly built engine on a hidden desktop from a QA root of its own under the system temp folder
    (the game's archives and the module joined by junctions), with the module under test and a probe module made
    here (module_qa_register.script), and judges by the log and by what the probe says. The installed game and the
    module stay read-only: the runner resolves and prints its work root and refuses to start when it is not under
    the temp folder.

    One start, a new game (about two minutes): the module is mounted, the module system refuses nothing of it,
    every section its item assets make or change is there; then every motion (anm_*) of every first-person section
    those items have - an item's HUD section and its use scene's - is played once the way the game warms a scene
    up, each call guarded, so a motion or a model the game lacks is a line of game-fails.txt and not a stopped
    game; then a few of the items are really taken in the hands. -MenuOnly stops at the main menu.
#>
[CmdletBinding()]
param(
    # a built module's folder (the one that holds mod.ltx); read only
    [Parameter(Mandatory)] [string]$Module,
    [string]$GameRoot = 'D:\Games\Dead Air',
    [string]$BuildRoot,
    # everything the run makes lives here; it must be under the system temp folder
    [string]$WorkRoot = (Join-Path ([IO.Path]::GetTempPath()) 'dar-module-qa'),
    [string]$VidMode = '1024x768',
    # how many items are really taken in the hands, spread over all of them, and for how long (milliseconds)
    [int]$Hold = 6,
    [int]$HoldMs = 2000,
    # sections to hold before the sample
    [string[]]$HoldFirst = @(),
    # how many motions are played in one frame (1: a frame is drawn after each, to find the one a frame stops on)
    [int]$PerFrame = 60,
    # the game is taken to have stopped when its log is silent this long
    [int]$SilentSeconds = 90,
    [ValidateRange(60, 3500)] [int]$TimeoutSeconds = 420,
    [switch]$MenuOnly,
    # the QA root of the last run is used again as it is (a kept one): no copying, a run starts at once
    [switch]$Reuse,
    [switch]$Keep
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not (Test-Path -LiteralPath (Join-Path $repositoryRoot 'tools\qa\module\module_qa_register.script'))) {
    throw "Not the engine's repository: $repositoryRoot"
}
if (-not $BuildRoot) { $BuildRoot = Join-Path $repositoryRoot 'bin\x64\Release' }
$GameRoot = (Resolve-Path -LiteralPath $GameRoot).Path
$Module = (Resolve-Path -LiteralPath $Module).Path
if (-not (Test-Path -LiteralPath (Join-Path $Module 'mod.ltx') -PathType Leaf)) { throw "No mod.ltx in $Module" }
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
$work = [IO.Path]::GetFullPath($WorkRoot).TrimEnd('\')
Write-Host "== work root: $work"
if (-not ($work + '\').StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or $work.Length -le $tempRoot.Length) {
    throw "The work root must lie under the system temp folder ($tempRoot): $work"
}
if (($work + '\').StartsWith($GameRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "The work root must not lie inside the installed game: $work"
}

$cp1251 = [Text.Encoding]::GetEncoding(1251)
$utf8 = [Text.UTF8Encoding]::new($false)
$moduleId = ''
foreach ($line in [IO.File]::ReadAllLines((Join-Path $Module 'mod.ltx'), $cp1251)) {
    if ($line -match '^\s*id\s*=\s*([a-z0-9_]+)') { $moduleId = $Matches[1]; break }
}
if (-not $moduleId) { throw "mod.ltx of $Module names no id" }
Write-Host "== module: $moduleId ($Module)"

$qaRoot = Join-Path $work 'root'
$qaAppData = Join-Path $qaRoot 'appdata'
$qaGameData = Join-Path $qaRoot 'gamedata'
$qaModules = Join-Path $qaRoot 'modules'
$qaEngine = Join-Path $qaRoot 'xrEngine.exe'
$qaLog = Join-Path $qaAppData 'logs\grizzly_admin.log'
$resultRoot = Join-Path $work ("results\" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$compatGameData = Join-Path $repositoryRoot 'packaging\dead-air-x64\compatibility\gamedata'
$manifestPath = Join-Path $repositoryRoot 'packaging\dead-air-x64\installer\runtime-files.txt'
$helper = Join-Path $repositoryRoot 'tools\qa\Start-DetachedHiddenDesktopProcess.ps1'
New-Item -ItemType Directory -Path $resultRoot -Force | Out-Null
Write-Host "== results: $resultRoot"

$script:failed = 0
function Assert-That {
    param([string]$Name, [bool]$Condition, [string]$Detail = '')
    if (-not $Condition) { $script:failed++ }
    Write-Host ("   {0} {1}{2}" -f $(if ($Condition) { 'PASS' } else { 'FAIL' }), $Name, $(if ($Detail) { " - $Detail" } else { '' }))
}

function Copy-Tree {
    param([string]$Source, [string]$Destination)
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    & robocopy $Source $Destination /S /NFL /NDL /NJH /NJS /NP /R:2 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy '$Source' failed ($LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
}

function Remove-Root {
    if (-not (Test-Path -LiteralPath $qaRoot)) { return }
    # the junctions go as links: what they point at is the installed game's and the module's
    foreach ($link in @((Join-Path $qaRoot 'database'), (Join-Path $qaModules $moduleId))) {
        if (Test-Path -LiteralPath $link) { [IO.Directory]::Delete($link, $false) }
    }
    for ($try = 1; ; $try++) {
        try { Remove-Item -LiteralPath $qaRoot -Recurse -Force -ErrorAction Stop; return }
        catch { if ($try -ge 20) { throw } Start-Sleep -Seconds 1 }
    }
}

function New-Root {
    Write-Host "== building the QA root: $qaRoot"
    Remove-Root
    foreach ($d in @($qaRoot, $qaAppData, (Join-Path $qaAppData 'logs'), (Join-Path $qaAppData 'savedgames'), $qaModules)) {
        New-Item -ItemType Directory -Path $d -Force | Out-Null
    }
    New-Item -ItemType Junction -Path (Join-Path $qaRoot 'database') -Target (Join-Path $GameRoot 'database') | Out-Null
    New-Item -ItemType Junction -Path (Join-Path $qaModules $moduleId) -Target $Module | Out-Null
    Copy-Item -LiteralPath (Join-Path $GameRoot 'fsgame.ltx') -Destination (Join-Path $qaRoot 'fsgame.ltx') -Force
    $meta = Join-Path $qaRoot '.dead-air-x64'
    New-Item -ItemType Directory -Path $meta -Force | Out-Null
    foreach ($f in @('content-manifest.txt', 'content-state.txt')) {
        $s = Join-Path $GameRoot ".dead-air-x64\$f"
        if (Test-Path -LiteralPath $s) { Copy-Item -LiteralPath $s -Destination $meta -Force }
    }
    foreach ($c in @('shaders_cache', 'shaders_cache_grizzly', 'render-hardware.cache')) {
        $s = Join-Path $GameRoot "appdata\$c"
        if (Test-Path -LiteralPath $s) { Copy-Item -LiteralPath $s -Destination $qaAppData -Recurse -Force }
    }
    Copy-Tree (Join-Path $GameRoot 'gamedata') $qaGameData
    Copy-Tree $compatGameData $qaGameData
    $manifest = @(Get-Content -LiteralPath $manifestPath | Where-Object { $_.Trim() -ne '' })
    foreach ($file in $manifest) {
        $built = Join-Path $BuildRoot $file
        $installed = Join-Path $GameRoot $file
        $source = if (Test-Path -LiteralPath $built) { $built } elseif (Test-Path -LiteralPath $installed) { $installed } else { throw "no runtime file: $file" }
        $target = Join-Path $qaRoot $file
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $target -Force
    }
    Get-ChildItem -LiteralPath $GameRoot -File -Filter '*.dll' | Where-Object { $manifest -notcontains $_.Name } |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $qaRoot $_.Name) -Force }
}

function Set-UserLtx {
    $source = Join-Path $GameRoot 'appdata\user.ltx'
    $lines = if (Test-Path -LiteralPath $source) { @([IO.File]::ReadAllLines($source, $cp1251)) } else { @() }
    $overrides = [ordered]@{ 'renderer' = 'renderer_r4'; 'vid_mode' = $VidMode; 'vid_window_mode' = 'st_opt_windowed'; 'rs_fullscreen' = 'off'
        'rs_v_sync' = 'off'; 'rs_fps_limit' = '60'; 'g_pause_in_background' = '0'; 'snd_volume_music' = '0'; 'snd_volume_eff' = '0' }
    $lines = @($lines | Where-Object { $l = $_; -not (@($overrides.Keys | Where-Object { $l -match "^\s*$_\s" }).Count) })
    foreach ($k in $overrides.Keys) { $lines += "$k $($overrides[$k])" }
    [IO.File]::WriteAllLines((Join-Path $qaAppData 'user.ltx'), [string[]]$lines, $cp1251)
}

# A new game leaves fake_start for the start [character_creation] names, the way the new-game screen asks.
function Set-NewGame {
    $options = Join-Path $qaGameData 'configs\axr_options.ltx'
    $lines = if (Test-Path -LiteralPath $options) { @([IO.File]::ReadAllLines($options, $cp1251)) } else { @('[character_creation]') }
    $lines = @($lines | Where-Object { $_ -notmatch '^\s*new_game_faction(_position)?\s*=' })
    $at = [array]::IndexOf($lines, ($lines | Where-Object { $_ -match '^\[character_creation\]' } | Select-Object -First 1))
    if ($at -lt 0) { $lines = @('[character_creation]') + $lines; $at = 0 }
    $start = @('new_game_faction = stalker', 'new_game_faction_position = _rookie_village')
    $lines = $lines[0..$at] + $start + $(if ($at + 1 -lt $lines.Count) { $lines[($at + 1)..($lines.Count - 1)] } else { @() })
    [IO.File]::WriteAllLines($options, [string[]]$lines, $cp1251)
}

function Write-ModuleFile {
    param([string]$Root, [string]$Rel, [string]$Text)
    $path = Join-Path $Root $Rel
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
    [IO.File]::WriteAllBytes($path, [byte[]]$utf8.GetBytes($Text))
}

function Invoke-Engine {
    param([string]$Phase, [string[]]$WaitFor, [string]$Start = '', [int]$Seconds = $TimeoutSeconds)
    Get-Process xrEngine -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $qaEngine } | Stop-Process -Force
    # the log of the last start may still be held for a moment (the game ending, a scanner reading it)
    for ($try = 0; (Test-Path -LiteralPath $qaLog) -and $try -lt 120; $try++) {
        try { Remove-Item -LiteralPath $qaLog -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 250 }
    }
    if (Test-Path -LiteralPath $qaLog) { throw "The log of the last start is still held: $qaLog" }
    Set-UserLtx
    $desktop = "DeadAirModule$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
    $pidFile = Join-Path $resultRoot "$Phase-engine.pid"
    $launcherFile = Join-Path $resultRoot "$Phase-launcher.pid"
    Remove-Item -LiteralPath $pidFile, $launcherFile -Force -ErrorAction SilentlyContinue
    $arguments = '-i -fsltx fsgame.ltx -always_active -silent_error_mode -force_flushlog -r4 -nointro'
    if ($Start) { $arguments += " -start `"$Start`"" }
    Write-Host "== [$Phase] xrEngine.exe $arguments"
    $engine = $null
    try {
        & $helper -FilePath $qaEngine -Arguments $arguments -ProcessIdFile $pidFile -LauncherIdFile $launcherFile -DesktopName $desktop -TimeoutSeconds ([Math]::Min($Seconds + 60, 3600)) | Out-Null
        $deadline = [DateTime]::UtcNow.AddSeconds(60)
        while (-not (Test-Path -LiteralPath $pidFile -PathType Leaf)) {
            if ([DateTime]::UtcNow -ge $deadline) { throw 'The hidden launcher did not publish the engine PID.' }
            Start-Sleep -Milliseconds 100
        }
        $engine = Get-Process -Id ([int](Get-Content -LiteralPath $pidFile -Raw)) -ErrorAction Stop
        $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
        $pending = @($WaitFor)
        $said = [DateTime]::UtcNow
        $size, $grew = -1, [DateTime]::UtcNow
        while ($pending.Count -and [DateTime]::UtcNow -lt $deadline -and -not $engine.HasExited) {
            Start-Sleep -Milliseconds 500
            if (([DateTime]::UtcNow - $said).TotalSeconds -ge 120) {
                $last = if (Test-Path -LiteralPath $qaLog) { Select-String -LiteralPath $qaLog -Pattern 'MODQA at = ' -Encoding 1251 | Select-Object -Last 1 } else { $null }
                Write-Host "   ... $(if ($last) { $last.Line.Trim() } else { "still waiting for: $($pending -join ' | ')" })"
                $said = [DateTime]::UtcNow
            }
            if (-not (Test-Path -LiteralPath $qaLog)) { continue }
            # a game that stopped without a word (a fault it could not report) writes nothing more
            $now = (Get-Item -LiteralPath $qaLog).Length
            if ($now -ne $size) { $size = $now; $grew = [DateTime]::UtcNow }
            elseif (([DateTime]::UtcNow - $grew).TotalSeconds -ge $SilentSeconds) { Write-Host "   the log has been silent for $SilentSeconds s: the game is taken to have stopped"; break }
            $pending = @($pending | Where-Object { -not (Select-String -LiteralPath $qaLog -Pattern $_ -Quiet -Encoding 1251 -ErrorAction SilentlyContinue) })
        }
        Assert-That "$Phase reached its log state" (-not $pending.Count) $(if ($pending.Count) { "missing: $($pending -join ' | ')$(if ($engine.HasExited) { ' (the game ended)' })" })
    }
    finally {
        if ($engine -and -not $engine.HasExited) { Stop-Process -Id $engine.Id -Force; $engine.WaitForExit(20000) | Out-Null }
        if (Test-Path -LiteralPath $launcherFile) {
            $launcher = Get-Process -Id ([int](Get-Content -LiteralPath $launcherFile -Raw)) -ErrorAction SilentlyContinue
            if ($launcher) { Stop-Process -Id $launcher.Id -Force -ErrorAction SilentlyContinue }
        }
        if (Test-Path -LiteralPath $qaLog) { Copy-Item -LiteralPath $qaLog -Destination (Join-Path $resultRoot "$Phase.log") -Force -ErrorAction SilentlyContinue }
    }
    # the game may still hold its log as it ends: read it shared, and again when the read is refused
    $log = ''
    for ($try = 0; $try -lt 20 -and (Test-Path -LiteralPath $qaLog); $try++) {
        try {
            $stream = [IO.FileStream]::new($qaLog, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]'ReadWrite, Delete')
            try { $log = [IO.StreamReader]::new($stream, $cp1251).ReadToEnd() } finally { $stream.Dispose() }
            break
        } catch [IO.IOException] {
            Start-Sleep -Milliseconds 250
        }
    }
    $fault = [regex]::Match($log, '(?s)(FATAL ERROR.{0,1200})')
    Assert-That "$Phase finished without an engine fault" (-not ($log -match 'FATAL ERROR|stack trace:|Assertion failed')) $(
        if ($fault.Success) { ($fault.Groups[1].Value -replace '\s+', ' ').Substring(0, [Math]::Min(600, ($fault.Groups[1].Value -replace '\s+', ' ').Length)) })
    return $log
}

function Said {
    param([string]$Log, [string]$Name)
    $m = [regex]::Match($Log, "MODQA $([regex]::Escape($Name)) = ([^\r\n]*)")
    if ($m.Success) { return $m.Groups[1].Value.Trim() }
    return '<not said>'
}

# The lines of the module system and of the game's scripts that say something went wrong, without their repeats.
function Complaints {
    param([string]$Log, [string]$Pattern)
    return @($Log -split "`r?`n" | Where-Object { $_ -match $Pattern } | ForEach-Object { $_.Trim() } | Select-Object -Unique)
}

if ($Reuse -and (Test-Path -LiteralPath $qaEngine) -and (Test-Path -LiteralPath (Join-Path $qaModules $moduleId))) {
    Write-Host "== the QA root is used again: $qaRoot"
} else {
    New-Root
}

# ---- the probe module ---------------------------------------------------------------------------------------------
$probe = Join-Path $qaModules 'module_qa'
Write-ModuleFile $probe 'mod.ltx' @'
[module]
id = module_qa
name = module_qa
version = 1.0
api = 1
target = any
mode = *
'@
New-Item -ItemType Directory -Path (Join-Path $probe 'scripts') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'module_qa_register.script') -Destination (Join-Path $probe 'scripts\register.script') -Force
Write-ModuleFile $probe 'gamedata\configs\module_qa.ltx' "[module_qa]`r`nhold = $Hold`r`nhold_ms = $HoldMs`r`nper_frame = $PerFrame`r`n$(if ($HoldFirst.Count) { "hold_first = $($HoldFirst -join ',')`r`n" })"

try {
    if ($MenuOnly) {
        Write-Host '== menu: the module is mounted and its items are there'
        $log = Invoke-Engine -Phase 'menu' -WaitFor @('MODQA menu done') -Seconds 600
    } else {
        Write-Host '== a new game: the module is mounted, its items are there, every motion plays, a few items are held'
        Set-NewGame
        $log = Invoke-Engine -Phase 'game' -Start 'server(all/single/alife/new) client(localhost)' -WaitFor @('MODQA game done')
    }
    $phase = if ($MenuOnly) { 'menu' } else { 'game' }
    $s = { param($n) Said $log $n }
    $refused = @(Complaints $log "^! XMS")
    $refused | Set-Content -LiteralPath (Join-Path $resultRoot "$phase-refused.txt") -Encoding utf8
    Assert-That 'the module system refuses nothing' (-not $refused.Count) $(if ($refused.Count) { "$($refused.Count) line(s), first: $($refused[0])" })
    $other = @(Complaints $log "^! ")
    $other | Set-Content -LiteralPath (Join-Path $resultRoot "$phase-complaints.txt") -Encoding utf8
    Write-Host "   lines that start with '!': $($other.Count) ($phase-complaints.txt)"
    Assert-That 'the module is mounted' ($log -match [regex]::Escape("] $moduleId ")) ''
    Assert-That 'its item assets are read' ((& $s 'item assets') -match '^[1-9]' -and (& $s 'unread') -eq '0') "$(& $s 'item assets') assets, $(& $s 'unread') unread"
    Assert-That 'every section they make or change is there' ((& $s 'missing') -eq '0') "$(& $s 'missing') missing, first: $(& $s 'missing 1')"
    Write-Host "   motions of first-person sections: $(& $s 'motions'); items with a HUD: $(& $s 'to hold')"
    if (-not $MenuOnly) {
        $fails = @([regex]::Matches($log, 'MODQA fail = ([^\r\n]*)') | ForEach-Object { $_.Groups[1].Value.Trim() })
        $fails | Set-Content -LiteralPath (Join-Path $resultRoot 'game-fails.txt') -Encoding utf8
        $last = [regex]::Matches($log, 'MODQA at = ([^\r\n]*)') | Select-Object -Last 1
        Assert-That 'the level is reached' ((& $s 'level') -ne '<not said>') (& $s 'level')
        Assert-That 'every motion was played' ((& $s 'motions played') -eq (& $s 'motions')) "$(& $s 'motions played') of $(& $s 'motions')"
        Assert-That 'the items were held' ((& $s 'game done') -eq 'true') "held $(& $s 'held'); the last one: $(if ($last) { $last.Groups[1].Value.Trim() } else { 'none' })"
        Assert-That 'none failed' (-not $fails.Count) $(if ($fails.Count) { "$($fails.Count), first: $($fails[0])" })
    }
}
finally {
    if (-not $Keep) { Remove-Root }
}
Write-Host "== failures: $script:failed"
exit $script:failed
