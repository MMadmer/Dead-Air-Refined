<#
.SYNOPSIS
    Loose particle effects with their action lists, the streamer mode and the radio's music of modules, in the
    real game (MODDING.md "Loose particle overrides", "Streamer mode", "Screens and music").

.DESCRIPTION
    Starts the freshly built engine on a hidden desktop from a QA root of its own under the system temp folder
    (the game's archives joined by a junction), with a module generated here, and judges by the log and by what
    the module's registrar (sound_qa_register.script) reads back. The installed game stays read-only: the runner
    resolves and prints its work root and refuses to start when it is not under the temp folder.

    A  the main menu, snd_streamer_mode 1: every .pe of a converted module with its [actions] and four malformed
       ones (the load summary and the CRC of the lists against the files); the game's list and a module's #file
       patch of it; described sounds marked copyrighted, with a replacement, without, with one that is
       copyrighted itself, with one that does not exist; xms.streamer_mode / sound_copyrighted /
       sound_replacement; xms.radio(); the menu music's copyrighted track plays its replacement and the one
       without is passed over; a track's copyright keys and a track with a file and a sound refused with a line
    B  a new game, the mode on: the loading screen's music; sounds started from a script, 2D, 3D and looped -
       refused, replaced (with the replacement's length while it plays, the sound's own after), free; the
       radio's mounted name; the mode switched off and on again stops what plays; spawn_ini of a radio whose
       custom_data names a file and of a radio of the level, and the radio script's binding
    C  no module, the mode off: a new game as before - no streamer line but the summary of the game's list
#>
[CmdletBinding()]
param(
    [string]$GameRoot = 'D:\Games\Dead Air',
    [string]$BuildRoot,
    # everything the run makes lives here; it must be under the system temp folder
    [string]$WorkRoot = (Join-Path ([IO.Path]::GetTempPath()) 'dar-sound-qa'),
    # a converted module's loose effects, every .pe with its [actions] (read only)
    [string]$ConvertedParticles = 'D:\XFinedProjects\WeaponsEvolution2026\gamedata\particles',
    # a few short .ogg files to make the module's sounds of (read only)
    [string]$SoundSource = 'D:\XFinedProjects\WeaponsEvolution\gamedata\sounds\blindside\kiparis',
    [string]$VidMode = '1024x768',
    [int]$TimeoutSeconds = 300,
    [switch]$SkipGame,
    [switch]$Keep
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not (Test-Path -LiteralPath (Join-Path $repositoryRoot 'tools\qa\sound\pe_actions.py'))) {
    throw "Not the engine's repository: $repositoryRoot"
}
if (-not $BuildRoot) { $BuildRoot = Join-Path $repositoryRoot 'bin\x64\Release' }
$GameRoot = (Resolve-Path -LiteralPath $GameRoot).Path
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
$work = [IO.Path]::GetFullPath($WorkRoot).TrimEnd('\')
Write-Host "== work root: $work"
if (-not ($work + '\').StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or $work.Length -le $tempRoot.Length) {
    throw "The work root must lie under the system temp folder ($tempRoot): $work"
}
if (($work + '\').StartsWith($GameRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "The work root must not lie inside the installed game: $work"
}
foreach ($source in @($ConvertedParticles, $SoundSource)) {
    if (-not (Test-Path -LiteralPath $source -PathType Container)) { throw "No such folder: $source" }
}

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
$limited = 'D:\Games\XFined-Editor\tools\rust\limited.py'
$utf8 = [Text.UTF8Encoding]::new($false)
$cp1251 = [Text.Encoding]::GetEncoding(1251)
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
    # the junction goes as a link: what it points at is the installed game's
    $link = Join-Path $qaRoot 'database'
    if (Test-Path -LiteralPath $link) { [IO.Directory]::Delete($link, $false) }
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
    param([string[]]$Commands)
    $source = Join-Path $GameRoot 'appdata\user.ltx'
    $lines = if (Test-Path -LiteralPath $source) { @([IO.File]::ReadAllLines($source, $cp1251)) } else { @() }
    $overrides = [ordered]@{ 'renderer' = 'renderer_r4'; 'vid_mode' = $VidMode; 'vid_window_mode' = 'st_opt_windowed'; 'rs_fullscreen' = 'off'
        'rs_v_sync' = 'off'; 'rs_fps_limit' = '60'; 'g_pause_in_background' = '0'; 'snd_volume_music' = '0'; 'snd_volume_eff' = '0' }
    $lines = @($lines | Where-Object { $l = $_; -not (@($overrides.Keys + @('snd_streamer_mode') | Where-Object { $l -match "^\s*$_\s" }).Count) })
    foreach ($k in $overrides.Keys) { $lines += "$k $($overrides[$k])" }
    [IO.File]::WriteAllLines((Join-Path $qaAppData 'user.ltx'), [string[]]($lines + $Commands), $cp1251)
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
    param([string]$Phase, [string[]]$Commands, [string[]]$WaitFor, [string]$Start = '')
    Get-Process xrEngine -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $qaEngine } | Stop-Process -Force
    if (Test-Path -LiteralPath $qaLog) { Remove-Item -LiteralPath $qaLog -Force }
    Set-UserLtx -Commands $Commands
    $desktop = "DeadAirSound$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
    $pidFile = Join-Path $resultRoot "$Phase-engine.pid"
    $launcherFile = Join-Path $resultRoot "$Phase-launcher.pid"
    Remove-Item -LiteralPath $pidFile, $launcherFile -Force -ErrorAction SilentlyContinue
    $arguments = '-i -fsltx fsgame.ltx -always_active -silent_error_mode -force_flushlog -r4 -nointro'
    if ($Start) { $arguments += " -start `"$Start`"" }
    Write-Host "== [$Phase] xrEngine.exe $arguments"
    $engine = $null
    try {
        & $helper -FilePath $qaEngine -Arguments $arguments -ProcessIdFile $pidFile -LauncherIdFile $launcherFile -DesktopName $desktop -TimeoutSeconds ($TimeoutSeconds + 60) | Out-Null
        $deadline = [DateTime]::UtcNow.AddSeconds(60)
        while (-not (Test-Path -LiteralPath $pidFile -PathType Leaf)) {
            if ([DateTime]::UtcNow -ge $deadline) { throw 'The hidden launcher did not publish the engine PID.' }
            Start-Sleep -Milliseconds 100
        }
        $engine = Get-Process -Id ([int](Get-Content -LiteralPath $pidFile -Raw)) -ErrorAction Stop
        $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        $pending = @($WaitFor)
        $said = [DateTime]::UtcNow
        while ($pending.Count -and [DateTime]::UtcNow -lt $deadline -and -not $engine.HasExited) {
            Start-Sleep -Milliseconds 500
            if (([DateTime]::UtcNow - $said).TotalSeconds -ge 60) { Write-Host "   ... still waiting for: $($pending -join ' | ')"; $said = [DateTime]::UtcNow }
            if (-not (Test-Path -LiteralPath $qaLog)) { continue }
            $pending = @($pending | Where-Object { -not (Select-String -LiteralPath $qaLog -Pattern $_ -Quiet -Encoding 1251 -ErrorAction SilentlyContinue) })
        }
        Assert-That "$Phase reached its log state" (-not $pending.Count) $(if ($pending.Count) { "missing: $($pending -join ' | ')" })
    }
    finally {
        if ($engine -and -not $engine.HasExited) { Stop-Process -Id $engine.Id -Force; $engine.WaitForExit(20000) | Out-Null }
        if (Test-Path -LiteralPath $launcherFile) {
            $launcher = Get-Process -Id ([int](Get-Content -LiteralPath $launcherFile -Raw)) -ErrorAction SilentlyContinue
            if ($launcher) { Stop-Process -Id $launcher.Id -Force -ErrorAction SilentlyContinue }
        }
        if (Test-Path -LiteralPath $qaLog) { Copy-Item -LiteralPath $qaLog -Destination (Join-Path $resultRoot "$Phase.log") -Force }
    }
    $log = if (Test-Path -LiteralPath $qaLog) { [IO.File]::ReadAllText($qaLog, $cp1251) } else { '' }
    Assert-That "$Phase finished without an engine fault" (-not ($log -match 'FATAL ERROR|stack trace:|Assertion failed|IReader overflow'))
    return $log
}

function Said {
    param([string]$Log, [string]$Name)
    $m = [regex]::Match($Log, "SOUNDQA $([regex]::Escape($Name)) = ([^\r\n]*)")
    if ($m.Success) { return $m.Groups[1].Value.Trim() }
    return '<not said>'
}

function Has {
    param([string]$Log, [string]$Text)
    return $Log.Contains($Text)
}

New-Root

# ---- the module ------------------------------------------------------------------------------------------------
$module = Join-Path $qaModules 'sound_qa'
New-Item -ItemType Directory -Path $module -Force | Out-Null
Write-ModuleFile $module 'mod.ltx' @'
[module]
id = sound_qa
name = sound_qa
version = 1.0
api = 1
target = any
mode = *

[screens]
loading_music = only
menu_music = only
loading_music_end = next
menu_music_end = next

[track.t_rep]
sound = sound_qa\cr_rep
play = loading, radio

[track.t_none]
sound = sound_qa\cr_none
play = loading, radio

[track.t_bad]
sound = sound_qa/CR_BAD.ogg
play = loading, radio

[track.t_file]
file = music\file_track.ogg
play = loading, radio
weight = 2
volume = 0.5
copyrighted = true
streamer = t_rep

[track.t_both]
file = music\file_track.ogg
sound = sound_qa\free
play = radio

[track.m_rep]
sound = sound_qa\cr_rep
play = menu

[track.m_none]
sound = sound_qa\cr_none
play = menu

[playlist.mix]
title = "Sound QA mix"
tracks = t_rep, t_file, nothere
order = list

[playlist.road]
tracks = t_none, t_both

[radio]
music = only
'@
$moduleScripts = Join-Path $module 'scripts'
New-Item -ItemType Directory -Path $moduleScripts -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'sound_qa_register.script') -Destination (Join-Path $moduleScripts 'register.script') -Force

# sounds: copyrighted with a replacement, without, with a copyrighted replacement, with a missing one; free ones
$sounds = Join-Path $module 'gamedata\sounds\sound_qa'
New-Item -ItemType Directory -Path $sounds -Force | Out-Null
foreach ($pair in @(@('cr_rep', 'kiparis_reload_empty'), @('cr_none', 'kiparis_holster'), @('cr_bad', 'kiparis_draw'),
        @('cr_missing', 'kiparis_reload'), @('free', 'kiparis_unjam'), @('listed', 'kiparis_holster'), @('mask_any', 'kiparis_draw'))) {
    Copy-Item -LiteralPath (Join-Path $SoundSource "$($pair[1]).ogg") -Destination (Join-Path $sounds "$($pair[0]).ogg") -Force
}
New-Item -ItemType Directory -Path (Join-Path $module 'music') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $SoundSource 'kiparis_reload_empty.ogg') -Destination (Join-Path $module 'music\file_track.ogg') -Force
Write-ModuleFile $sounds 'cr_rep.sndasset' 'return { snd = 1, copyrighted = true, streamer = [[sound_qa\free]] }'
Write-ModuleFile $sounds 'cr_none.sndasset' 'return { snd = 1, copyrighted = true }'
Write-ModuleFile $sounds 'cr_bad.sndasset' 'return { snd = 1, copyrighted = true, streamer = [[sound_qa\cr_none]] }'
Write-ModuleFile $sounds 'cr_missing.sndasset' 'return { snd = 1, copyrighted = true, streamer = "sound_qa/nothing_here.ogg" }'
Write-ModuleFile $sounds 'free.sndasset' 'return { snd = 1, volume = 0.9 }'

# the game's list, patched by the module
Write-ModuleFile $module 'patch\sound_qa.ltxp' @'
#file dead_air_x64_sound_rights.ltx
[copyrighted]
sound_qa\listed = sound_qa\free
sound_qa\mask_* =
'@

# a radio whose custom_data names a file, bound to the module's playlist
Write-ModuleFile $module 'gamedata\configs\xms\sound_qa.ltx' @'
[sound_qa_radio]:physic_object
visual      = dynamics\equipments\trade\radio.ogf
custom_data = scripts\sound_qa\radio.ltx
'@
Write-ModuleFile $module 'gamedata\configs\scripts\sound_qa\radio.ltx' @'
[logic]
active = ph_sound@play

[ph_sound@play]
snd      = common_radio
looped   = false
random   = true
min_idle = 2
max_idle = 4

[xms_radio]
playlist = sound_qa.mix
'@

# particles: a converted module's effects as they are, and four malformed ones
$particles = Join-Path $module 'gamedata\particles'
Copy-Tree $ConvertedParticles $particles
$bad = [ordered]@{
    'bad_size' = "size = 9`r`ndata_0000 = 0100000000"
    'bad_hex'  = "size = 4`r`ndata_0000 = 01zz0000"
    'bad_type' = "size = 8`r`ndata_0000 = 0100000063000000"
    'bad_cut'  = "size = 8`r`ndata_0000 = 010000000c000000"
}
foreach ($name in $bad.Keys) {
    Write-ModuleFile $particles "sound_qa\$name.pe" ("[_effect]`r`nversion = 1`r`nmax_particles = 1`r`nflags = 0`r`n`r`n[actions]`r`n" + $bad[$name] + "`r`n")
}
$peFiles = @(Get-ChildItem -LiteralPath $ConvertedParticles -Recurse -File -Filter '*.pe' | ForEach-Object { $_.FullName })
$listFile = Join-Path $resultRoot 'pe-files.txt'
[IO.File]::WriteAllLines($listFile, [string[]]$peFiles, $utf8)
$expected = (& python -I (Join-Path $PSScriptRoot 'pe_actions.py') sum "@$listFile" | Out-String).Trim()
Write-Host "== the converted effects carry: $expected (count bytes crc)"
$expectedParts = $expected -split ' '

# ---- A: the main menu ----------------------------------------------------------------------------------------
Write-Host '== A: the main menu, the mode on'
$log = Invoke-Engine -Phase 'menu' -Commands @('snd_streamer_mode 1') -WaitFor @('SOUNDQA done', '\[streamer\] sound_qa\\cr_rep: copyrighted - sound_qa\\free plays in its place')
$s = { param($name) Said $log $name }

Assert-That 'particles: the summary counts the converted effects and the malformed ones' ($log -match '\* Loose particles: \d+ effects replaced, \d+ added; \d+ groups replaced, \d+ added; 4 failed')
$m = [regex]::Match($log, '\* Loose particles: (\d+) effect\(s\) carry their action list, (\d+) bytes, crc ([0-9a-f]{8})')
Assert-That 'particles: every converted effect carries its action list, the bytes and their CRC as the files give them' ($m.Success -and
    $m.Groups[1].Value -eq $expectedParts[0] -and $m.Groups[2].Value -eq $expectedParts[1] -and $m.Groups[3].Value -eq $expectedParts[2]) $(if ($m.Success) { $m.Value } else { 'no summary' })
foreach ($pair in @(@('bad_size', 'ends before data_0001: 5 of its 9 bytes'), @('bad_hex', 'data_0000 is no even run of hex digits'),
        @('bad_type', 'action 1 of 1 is of no type the game knows \(99\)'), @('bad_cut', 'action 1 of 1 \(type 12\) is cut short: 0 of its 8 bytes'))) {
    Assert-That "particles: $($pair[0]) refused with its reason" ($log -match "! Loose particle effect 'sound_qa\\$($pair[0])': \[actions\] $($pair[1])")
}

Assert-That 'the game''s list and the module''s #file patch of it' ($log -match '\* \[streamer\] copyrighted: 8 line\(s\) of the game''s list, 4 sound\(s\) of modules by their descriptions')
Assert-That 'the script calls are there' ((& $s 'api') -eq '1' -and (& $s 'mode') -eq 'true')
foreach ($row in @(
        @('sound_qa\cr_rep', 'true', 'sound_qa\free'), @('sound_qa\cr_none', 'true', 'nil'), @('sound_qa\cr_bad', 'true', 'nil'),
        @('sound_qa\cr_missing', 'true', 'nil'), @('sound_qa\free', 'false', 'nil'), @('sound_qa\listed', 'true', 'sound_qa\free'),
        @('sound_qa\mask_any', 'true', 'nil'), @('SOUND_QA/CR_REP.ogg', 'true', 'sound_qa\free'), @('music\radio\music_1', 'true', 'nil'),
        @('radio\01\anything', 'true', 'nil'), @('xms_radio\sound_qa\t_file', 'false', 'nil'), @('xms_screens\sound_qa\t_file', 'false', 'nil'))) {
    Assert-That "xms.sound_copyrighted / sound_replacement: $($row[0])" ((& $s "copyrighted $($row[0])") -eq $row[1] -and (& $s "replacement $($row[0])") -eq $row[2]) "$(& $s "copyrighted $($row[0])") / $(& $s "replacement $($row[0])")"
}
Assert-That 'a replacement that is copyrighted itself is none, with its line' ($log -match '! \[streamer\] sound_qa\\cr_bad: its replacement sound_qa\\cr_none is copyrighted itself - nothing plays in its place')
Assert-That 'a replacement that does not exist is none, with its line' ($log -match '! \[streamer\] sound_qa\\cr_missing: its replacement sound_qa\\nothing_here is no sound of the game - nothing plays in its place')

Assert-That 'xms.radio(): only' ((& $s 'radio only') -eq 'sound_qa')
$tracks = @(
    'sound_qa|t_rep|sound_qa\cr_rep|true|sound_qa\free|1|1',
    'sound_qa|t_none|sound_qa\cr_none|true|nil|1|1',
    'sound_qa|t_bad|sound_qa\cr_bad|true|nil|1|1',
    'sound_qa|t_file|xms_radio\sound_qa\t_file|false|nil|0.5|2',
    'sound_qa|t_both|sound_qa\free|false|nil|1|1')
for ($i = 0; $i -lt $tracks.Count; $i++) {
    Assert-That "xms.radio(): track $($i + 1)" ((& $s "radio track $($i + 1)") -eq $tracks[$i]) (& $s "radio track $($i + 1)")
}
Assert-That 'xms.radio(): no other track' ((& $s 'radio track 6') -eq '<not said>')
Assert-That 'xms.radio(): the playlists' ((& $s 'radio playlist 1') -eq 'sound_qa|mix|Sound QA mix|list|sound_qa\cr_rep,xms_radio\sound_qa\t_file' -and
    (& $s 'radio playlist 2') -eq 'sound_qa|road|road|shuffle|sound_qa\cr_none,sound_qa\free') "$(& $s 'radio playlist 1') ; $(& $s 'radio playlist 2')"
Assert-That 'a playlist''s unknown track is left out with a line' ($log -match '! XMS: module \[sound_qa\] playlist \[mix\]: track \[nothere\] left out')
Assert-That 'a track''s copyright keys mean nothing, with a line' ($log -match '! XMS: module \[sound_qa\] track \[t_file\]: copyrighted and streamer are no keys of a track')
Assert-That 'a track with a file and a sound plays the sound, with a line' ($log -match '! XMS: module \[sound_qa\] track \[t_both\] names a file and a sound - the sound plays')
Assert-That 'xms_screens lists the radio' ($log -match '\*   playlist mix "Sound QA mix": list, 2 track\(s\): t_rep, t_file' -and
    $log -match '\*   track t_file: xms_screens\\sound_qa\\t_file, weight 2\.00, volume 0\.50, on the radio' -and
    $log -match '\* radio now: 5 track\(s\), 2 playlist\(s\), music only of sound_qa' -and
    $log -match '\*   streamer mode: sound_qa\\cr_rep plays sound_qa\\free instead')
Assert-That 'the menu music: the copyrighted track plays its replacement' ($log -match '~ \[streamer\] sound_qa\\cr_rep: copyrighted - sound_qa\\free plays in its place')
Assert-That 'the menu music: the one without a replacement is passed over, never started' (-not ($log -match '~ \[streamer\] sound_qa\\cr_none'))

if (-not $SkipGame) {
    Write-Host '== B: a new game, the mode on'
    Set-NewGame
    $log = Invoke-Engine -Phase 'game' -Commands @('snd_streamer_mode 1') -Start 'server(all/single/alife/new) client(localhost)' -WaitFor @('SOUNDQA game done')
    $s = { param($name) Said $log $name }
    Assert-That 'the loading music passes the copyrighted tracks without a replacement over' (
        (Has $log '* [screens] music: sound_qa\cr_none is copyrighted - passed over') -and (Has $log '* [screens] music: sound_qa\cr_bad is copyrighted - passed over'))
    Assert-That 'and plays the replacement and the file track' ((Has $log '* [screens] music: sound_qa\free.ogg') -or (Has $log '* [screens] music: xms_screens\sound_qa\t_file.ogg'))
    Assert-That 'and never the copyrighted ones' (-not (Has $log '* [screens] music: sound_qa\cr_rep.ogg') -and -not (Has $log '* [screens] music: sound_qa\cr_none.ogg') -and
        -not (Has $log '* [screens] music: sound_qa\cr_bad.ogg'))
    foreach ($tag in @('none 2d', 'none 3d', 'none looped', 'bad 2d', 'missing 3d', 'mask 3d', 'game radio 3d')) {
        Assert-That "refused from a script: $tag" ((& $s "play $tag") -eq 'false') (& $s "play $tag")
    }
    Assert-That 'the refused are said once each' ([regex]::Matches($log, '~ \[streamer\] sound_qa\\cr_none: copyrighted - not played').Count -eq 1 -and
        (Has $log '~ [streamer] sound_qa\cr_bad: copyrighted - not played') -and (Has $log '~ [streamer] sound_qa\mask_any: copyrighted - not played') -and
        (Has $log '~ [streamer] music\radio\music_1: copyrighted - not played'))
    Assert-That 'a listed sound with a replacement plays it' ((& $s 'play listed 2d') -eq 'true' -and (& $s 'length listed 2d') -eq (& $s 'free own length'))
    Assert-That 'a replacement plays in its place, 3D and looped, as long as the replacement' ((& $s 'play rep 3d') -eq 'true' -and (& $s 'play rep looped') -eq 'true' -and
        (& $s 'length rep 3d') -eq (& $s 'free own length') -and (& $s 'rep own length') -ne (& $s 'free own length')) "rep $(& $s 'rep own length') ms, free $(& $s 'free own length') ms, playing $(& $s 'length rep 3d') ms"
    Assert-That 'a free sound plays' ((& $s 'play free 2d') -eq 'true')
    Assert-That 'the radio''s mounted name plays, and a sound track''s own name' ((& $s 'play radio file') -eq 'true' -and (& $s 'play radio sound') -eq 'true')
    Assert-That 'the replacement ended, and the sound is itself again' ((& $s 'rep ended') -eq 'true' -and (& $s 'rep length after') -eq (& $s 'rep own length')) (& $s 'rep length after')
    Assert-That 'the mode off: a copyrighted sound plays' ((& $s 'mode now') -eq 'false' -and (& $s 'off plays') -eq 'true' -and (& $s 'off still plays') -eq 'true')
    Assert-That 'the mode switched on stops it' ((& $s 'switched on stops') -eq 'true' -and (& $s 'refused stays silent') -eq 'true')
    Assert-That 'spawn_ini of a radio whose custom_data names a file: the server object' ((& $s 'radio se ini') -eq 'sound_qa.mix') (& $s 'radio se ini')
    Assert-That 'and the game object' ((& $s 'radio online') -eq 'true' -and (& $s 'radio obj ini') -eq 'sound_qa.mix' -and (& $s 'radio obj logic') -eq 'ph_sound@play') "$(& $s 'radio obj ini') / $(& $s 'radio obj logic')"
    Assert-That 'the radio script reads its playlist' ((& $s 'radio binding') -eq 'sound_qa.mix|data|sound_qa.mix') (& $s 'radio binding')
    Assert-That 'spawn_ini of an object of the level: its own custom data (a radio where the level has one)' (
        (& $s 'stock custom data') -match '^(radio|logic)\|') "on $(& $s 'level'): $(& $s 'stock custom data')"

    Write-Host '== C: no module, the mode off'
    Get-ChildItem -LiteralPath $qaModules -Directory -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
    $plain = Join-Path $qaGameData 'scripts\sound_qa_plain.script'
    [IO.File]::WriteAllText($plain, @'
function on_game_start()
	RegisterScriptCallback("actor_on_first_update", function()
		if level.name() ~= "fake_start" then
			printf("SOUNDQA plain level %s on %s", tostring(xms ~= nil), level.name())
		end
	end)
end
'@, $utf8)
    # the game writes the options back after a new game: the start is named again
    Set-NewGame
    try {
        $log = Invoke-Engine -Phase 'plain' -Commands @() -Start 'server(all/single/alife/new) client(localhost)' -WaitFor @('SOUNDQA plain level')
    }
    finally { Remove-Item -LiteralPath $plain -Force -ErrorAction SilentlyContinue }
    Assert-That 'the game''s list is read, nothing of modules' ($log -match '\* \[streamer\] copyrighted: 6 line\(s\) of the game''s list, 0 sound\(s\) of modules')
    Assert-That 'no sound refused or replaced, no loose particle line' (-not ($log -match '~ \[streamer\]|passed over|Loose particle'))
    Assert-That 'no xms table without a module' ($log -match 'SOUNDQA plain level false')
}

if (-not $Keep) { Remove-Root }
Write-Host "== failures: $script:failed"
exit $script:failed
