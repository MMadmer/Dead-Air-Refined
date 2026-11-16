<#
.SYNOPSIS
    What a module brings, kept out of the base game's save format (SAVE_COMPATIBILITY.md, "What a module
    brings"), in the real game.

.DESCRIPTION
    Starts the freshly built engine on a hidden desktop from a QA root of its own (under _work, with the game's
    archives joined by a junction) with two modules generated here, drives it by a loose script and the console,
    and judges by the log, by what the script reads back and by the save files read the way the base game reads
    them (scop_records.py). The installed game stays read-only.

    new  a new game; module A's items given to the actor, put into a stock box and carried by a stock NPC; a
         character of A with stock supplies; A's spawn-layer box; a stock medkit A declares its own; a stock
         pistol with A's ammo loaded and A's upgrade installed; two items for (e); B's knife, and a stock gauss
         whose class B changes; a squad of A with a stock member, a stock squad with A's character in it; a
         quest task of A in the actor's registry; the radio's blob. Saved as saves_qa_1.
    c    saves_qa_1.scop as the base game reads it: no record of a module's section, no record whose parent is
         not a record, a placeholder with its marker under every module object's id, the stock pistol with
         the base game's ammo and no module upgrade; the .scov with the modules' chunks.
    a    saves_qa_1 with the modules: every object back with its id, section and parent; the state of the pistol;
         the task, the radio's blob, xms.section_owner.
    b    saves_qa_1 with -no_xms: no unknown section, no script error, the modules' things not in the world and
         their placeholders in it, no task of A; played, saved as saves_qa_2. saves_qa_2.scop and .scoc copied to
         saves_qa_3 without a .scov - what the base game leaves after saving that world - and saves_qa_3 loaded
         with the modules: the lineage finds saves_qa_2.scov and everything is back.
    d    B switched off: saves_qa_1 loads, B's things kept, A's back, xms.object_owner names B for its
         placeholders; saved and loaded twice; B on again: its objects back where they were. The same with B's
         folder gone instead.
    e    A changed: an item's section gone, another's class changed - those dropped with their lines, the rest back.
    f    B off, xms_forget saves_qa_b, saved; B on: nothing of B comes back.
    i    no module: a new game's save holds no placeholder and no module chunk.
    o    an older save: an engine from before this (-OlderRuntime: a folder with its runtime files; the installed
         game's by default) saves a game with a module's spawn-layer box in .scop; loaded here
         without the module the box is kept and a placeholder holds its id; with the module it is back, once.
    h    the limits the log says: a new game in a module's own mode, with the module changing a stock box in place.
#>
[CmdletBinding()]
param(
    [string]$GameRoot = 'D:\Games\Dead Air',
    [string]$BuildRoot,
    [string]$QaRoot,
    [string]$ResultLabel = (Get-Date -Format 'yyyyMMdd-HHmmss'),
    [string]$VidMode = '1024x768',
    [int]$TimeoutSeconds = 600,
    [switch]$RebuildRoot,
    # (o): the folder the older game's runtime files are taken from - what it holds of them, the rest from the
    # installed game. Left out: the installed game, while that is still a game from before module saves.
    [string]$OlderRuntime,
    # (o) alone
    [switch]$OlderSaveOnly,
    [switch]$Keep
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $BuildRoot) { $BuildRoot = Join-Path $repositoryRoot 'bin\x64\Release' }
if (-not $QaRoot) { $QaRoot = Join-Path $repositoryRoot '_work\saves-qa\root' }
$GameRoot = (Resolve-Path -LiteralPath $GameRoot).Path
if (-not (Test-Path -LiteralPath (Join-Path $repositoryRoot 'tools\qa\saves\scop_records.py'))) {
    throw "Not the engine's repository: $repositoryRoot"
}
$qaRoot = [IO.Path]::GetFullPath($QaRoot)
if ($qaRoot.StartsWith($GameRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -and
    -not $qaRoot.StartsWith($repositoryRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "The QA root must not lie inside the installed game: $qaRoot"
}
$qaAppData = Join-Path $qaRoot 'appdata'
$qaSaves = Join-Path $qaAppData 'savedgames'
$qaGameData = Join-Path $qaRoot 'gamedata'
$qaModules = Join-Path $qaRoot 'modules'
$qaEngine = Join-Path $qaRoot 'xrEngine.exe'
$qaLog = Join-Path $qaAppData 'logs\grizzly_admin.log'
$qaScript = Join-Path $qaGameData 'scripts\saves_qa.script'
$compatGameData = Join-Path $repositoryRoot 'packaging\dead-air-x64\compatibility\gamedata'
$manifestPath = Join-Path $repositoryRoot 'packaging\dead-air-x64\installer\runtime-files.txt'
$helper = Join-Path $repositoryRoot 'tools\qa\Start-DetachedHiddenDesktopProcess.ps1'
$inspector = Join-Path $PSScriptRoot 'scop_records.py'
$layersKey = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers'
$resultRoot = Join-Path $repositoryRoot "_work\saves-qa\results\$ResultLabel"
$utf8 = [Text.UTF8Encoding]::new($false)
$cp1251 = [Text.Encoding]::GetEncoding(1251)
New-Item -ItemType Directory -Path $resultRoot -Force | Out-Null

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

function Update-Runtime {
    param([string]$From = $BuildRoot)
    $manifest = @(Get-Content -LiteralPath $manifestPath | Where-Object { $_.Trim() -ne '' })
    foreach ($file in $manifest) {
        $built = Join-Path $From $file
        $installed = Join-Path $GameRoot $file
        $source = if (Test-Path -LiteralPath $built) { $built } elseif (Test-Path -LiteralPath $installed) { $installed } else { throw "no runtime file: $file" }
        $target = Join-Path $qaRoot $file
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $target -Force
    }
    Get-ChildItem -LiteralPath $GameRoot -File -Filter '*.dll' | Where-Object { $manifest -notcontains $_.Name } |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $qaRoot $_.Name) -Force }
}

function New-Root {
    Write-Host "== building the QA root: $qaRoot"
    Remove-Root
    foreach ($d in @($qaRoot, $qaAppData, (Join-Path $qaAppData 'logs'), $qaSaves, $qaModules)) {
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
    foreach ($c in @('shaders_cache', 'shaders_cache_grizzly', 'shaders_cache_oxr', 'render-hardware.cache')) {
        $s = Join-Path $GameRoot "appdata\$c"
        if (Test-Path -LiteralPath $s) { Copy-Item -LiteralPath $s -Destination $qaAppData -Recurse -Force }
    }
    Copy-Tree (Join-Path $GameRoot 'gamedata') $qaGameData
    Copy-Tree $compatGameData $qaGameData
}

function Set-UserLtx {
    $source = Join-Path $GameRoot 'appdata\user.ltx'
    $lines = if (Test-Path -LiteralPath $source) { @([IO.File]::ReadAllLines($source, $cp1251)) } else { @() }
    $overrides = [ordered]@{ 'renderer' = 'renderer_r4'; 'vid_mode' = $VidMode; 'vid_window_mode' = 'st_opt_windowed'; 'rs_fullscreen' = 'off'
        'rs_v_sync' = 'off'; 'rs_fps_limit' = '60'; 'g_pause_in_background' = '0'; 'rs_always_active' = 'on'; 'snd_volume_music' = '0'; 'snd_volume_eff' = '0' }
    $lines = @($lines | Where-Object { $l = $_; -not (@($overrides.Keys | Where-Object { $l -match "^\s*$_\s" }).Count) })
    foreach ($k in $overrides.Keys) { $lines += "$k $($overrides[$k])" }
    [IO.File]::WriteAllLines((Join-Path $qaAppData 'user.ltx'), [string[]]$lines, $cp1251)
}

function Install-Module {
    param([string]$Id, [hashtable]$Files)
    $root = Join-Path $qaModules $Id
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $root 'mod.ltx'), "[module]`r`nid = $Id`r`nname = $Id`r`nversion = 1.0`r`napi = 1`r`ntarget = any`r`nmode = *`r`n", $utf8)
    foreach ($rel in $Files.Keys) {
        $path = Join-Path $root $rel
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        [IO.File]::WriteAllBytes($path, [byte[]]$utf8.GetBytes($Files[$rel]))
    }
}

function Set-Disabled {
    param([string[]]$Ids)
    $list = Join-Path $qaModules 'disabled.ltx'
    if ($Ids.Count) { [IO.File]::WriteAllLines($list, [string[]]$Ids, $utf8) }
    elseif (Test-Path -LiteralPath $list) { Remove-Item -LiteralPath $list -Force }
}

# The loose script every phase runs: what to make, what to look at and what to save, as Lua data.
function Set-Plan {
    param([string]$Plan)
    $body = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'saves_qa.script'), $utf8)
    [IO.File]::WriteAllText($qaScript, "local plan = $Plan`r`n" + $body, $utf8)
}

function Invoke-Engine {
    param([string]$Phase, [string[]]$WaitFor, [string]$Start, [string]$Extra = '')
    Get-Process xrEngine -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $qaEngine } | Stop-Process -Force
    # the log of the last start may still be held for a moment (the game ending, a scanner reading it)
    for ($try = 0; (Test-Path -LiteralPath $qaLog) -and $try -lt 120; $try++) {
        try { Remove-Item -LiteralPath $qaLog -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 250 }
    }
    if (Test-Path -LiteralPath $qaLog) { throw "The log of the last start is still held: $qaLog" }
    Set-UserLtx
    New-Item -Path $layersKey -Force | Out-Null
    New-ItemProperty -Path $layersKey -Name $qaEngine -Value '~ HIGHDPIAWARE' -PropertyType String -Force | Out-Null
    $desktop = "DeadAirSaves$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
    $pidFile = Join-Path $resultRoot "$Phase-engine.pid"
    $launcherFile = Join-Path $resultRoot "$Phase-launcher.pid"
    Remove-Item -LiteralPath $pidFile, $launcherFile -Force -ErrorAction SilentlyContinue
    $arguments = "-i -fsltx fsgame.ltx -always_active -silent_error_mode -force_flushlog -r4 -nointro $Extra -start `"$Start`""
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
        Remove-ItemProperty -Path $layersKey -Name $qaEngine -ErrorAction SilentlyContinue
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
    Assert-That "$Phase finished without an engine fault" (-not ($log -match 'FATAL ERROR|stack trace:|Assertion failed|Expression\s*:'))
    Assert-That "$Phase has no script error" (-not ($log -match 'SCRIPT ERROR|LUA error|Lua error|stack traceback'))
    return $log
}

function Said {
    param([string]$Log, [string]$Name)
    $all = [regex]::Matches($Log, "SAVESQA $([regex]::Escape($Name)) = ([^\r\n]*)")
    if ($all.Count) { return $all[$all.Count - 1].Groups[1].Value.Trim() }
    return '<not said>'
}

function Read-Save {
    param([string]$Name)
    $scop = Join-Path $qaSaves "$Name.scop"
    $scov = Join-Path $qaSaves "$Name.scov"
    $arguments = @('-I', $inspector, $scop)
    if (Test-Path -LiteralPath $scov) { $arguments += $scov }
    $json = & python @arguments
    if ($LASTEXITCODE) { throw "scop_records.py failed on $Name" }
    return $json | ConvertFrom-Json
}

function Copy-Save {
    param([string]$From, [string]$To, [string[]]$Extensions = @('.scop', '.scoc', '.scov'))
    foreach ($e in $Extensions) {
        $s = Join-Path $qaSaves "$From$e"
        if (Test-Path -LiteralPath $s) { Copy-Item -LiteralPath $s -Destination (Join-Path $qaSaves "$To$e") -Force }
    }
}

# A new game leaves fake_start for the start [character_creation] names, the way the new-game screen asks.
function Set-NewGame {
    param([string]$Mode)
    $options = Join-Path $qaGameData 'configs\axr_options.ltx'
    $lines = if (Test-Path -LiteralPath $options) { @([IO.File]::ReadAllLines($options, $cp1251)) } else { @('[character_creation]') }
    $lines = @($lines | Where-Object { $_ -notmatch '^\s*new_game_(faction(_position)?|saves_qa_\w+_mode)\s*=' })
    $at = [array]::IndexOf($lines, ($lines | Where-Object { $_ -match '^\[character_creation\]' } | Select-Object -First 1))
    if ($at -lt 0) { $lines = @('[character_creation]') + $lines; $at = 0 }
    $start = @('new_game_faction = stalker', 'new_game_faction_position = _rookie_village')
    if ($Mode) { $start += "new_game_$($Mode)_mode = true" }
    $lines = $lines[0..$at] + $start + $(if ($at + 1 -lt $lines.Count) { $lines[($at + 1)..($lines.Count - 1)] } else { @() })
    [IO.File]::WriteAllLines($options, [string[]]$lines, $cp1251)
}
# ---- the modules -----------------------------------------------------------------------------------------

$moduleA = @{
    'items\pistol.itemasset' = 'return { item = 1, id = "pistol", base = "wpn_pm", name = "QA pistol" }'
    'items\ammo.itemasset' = 'return { item = 1, id = "ammo", base = "ammo_9x18_fmj", name = "QA ammo" }'
    'items\pm_ammo.itemasset' = 'return { item = 1, edit = "wpn_pm", add = { ammo_class = { "saves_qa_a_ammo" } } }'
    'items\gone.itemasset' = 'return { item = 1, id = "gone", base = "bandage", name = "QA gone" }'
    'items\morph.itemasset' = 'return { item = 1, id = "morph", base = "bread", name = "QA morph" }'
    'characters\guide.npcasset' = @'
return {
  npc = 1, id = "guide", name = "QA guide", community = "stalker",
  visual = [[actors\stalker_neutral\stalker_neutral_1]],
  supplies = { { section = "medkit", count = 1 }, "bread" },
}
'@
    'gamedata\configs\xms\saves_qa_a.ltx' = "[saves_qa_a_up]`r`nqa = 1`r`n"
    'patch\saves_qa_tasks.ltxp' = "#file misc\task_manager.ltx`r`n[saves_qa_a_task]`r`nqa = 1`r`n"
    'spawn\saves_qa.xspawn' = @'
[obj:crate]
op          = add
section     = inventory_box
level       = l01_escape
position    = -150.0, -20.0, -100.0
game_vertex = auto
'@
}
$moduleB = @{
    'items\knife.itemasset' = 'return { item = 1, id = "knife", base = "wpn_knife", name = "QA knife" }'
    'patch\saves_qa_b.ltxp' = "[wpn_gauss]`r`nclass = WP_SVU`r`n"
}

function Install-Modules {
    param([switch]$WithoutB, [switch]$ChangedA)
    $a = $moduleA.Clone()
    if ($ChangedA) {
        $a.Remove('items\gone.itemasset')
        $a['items\morph.itemasset'] = 'return { item = 1, id = "morph", base = "bread", name = "QA morph", set = { class = "II_MEDKI" } }'
    }
    Install-Module -Id 'saves_qa_a' -Files $a
    if ($WithoutB) { if (Test-Path -LiteralPath (Join-Path $qaModules 'saves_qa_b')) { Remove-Item -LiteralPath (Join-Path $qaModules 'saves_qa_b') -Recurse -Force } }
    else { Install-Module -Id 'saves_qa_b' -Files $moduleB }
}

# ---- the run -----------------------------------------------------------------------------------------------

if ($RebuildRoot -or -not (Test-Path -LiteralPath (Join-Path $qaRoot 'fsgame.ltx'))) { New-Root }
Update-Runtime
Copy-Tree $compatGameData $qaGameData
Get-ChildItem -LiteralPath $qaModules -Directory -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
Get-ChildItem -LiteralPath $qaSaves -File -Filter 'saves_qa*' -ErrorAction SilentlyContinue | Remove-Item -Force
Set-Disabled @()
Install-Modules

$names = @('pistol_actor', 'box', 'pistol_box', 'npc', 'pistol_npc', 'guide', 'declared', 'weapon', 'gone', 'morph', 'knife_b', 'gauss_b', 'crate', 'msquad', 'msquad_member', 'gsquad', 'gsquad_member')
$moduleOf = @{ pistol_actor = 'a'; pistol_box = 'a'; pistol_npc = 'a'; guide = 'a'; declared = 'a'; gone = 'a'; morph = 'a'; crate = 'a'; msquad = 'a'; msquad_member = 'a'; gsquad = 'a'; gsquad_member = 'a'; knife_b = 'b'; gauss_b = 'b'; box = ''; npc = ''; weapon = '' }

if (-not $OlderRuntime) { $OlderRuntime = $GameRoot }
$OlderRuntime = (Resolve-Path -LiteralPath $OlderRuntime).Path

if (-not $OlderSaveOnly) {
Write-Host '== new: a game with both modules, saved'
Set-NewGame
Set-Plan '{ phase = "new", save = "saves_qa_1" }'
$log = Invoke-Engine -Phase 'new' -Start 'server(all/single/alife/new) client(localhost)' -WaitFor @('Game saves_qa_1\S* is successfully saved')
$made = @{}
foreach ($n in $names) {
    $v = Said $log "made $n"
    if ($v -match '^(\d+) (\S+) (\d+)$') { $made[$n] = [pscustomobject]@{ id = [int]$Matches[1]; section = $Matches[2]; parent = [int]$Matches[3] } }
    Assert-That "made: $n" ($made.ContainsKey($n)) $v
}
$guideChildren = @([regex]::Matches($log, 'SAVESQA guide child = (\d+) (\S+)') | ForEach-Object { [pscustomobject]@{ id = [int]$_.Groups[1].Value; section = $_.Groups[2].Value } } | Sort-Object id -Unique)
Assert-That 'the character carries its stock supplies' ($guideChildren.Count -ge 1) ($guideChildren.section -join ', ')
foreach ($n in $names) {
    $want = $moduleOf[$n]
    $owner = Said $log "owner $n"
    $expected = if ($want) { "saves_qa_$want" } else { 'nil' }
    Assert-That "owner: $n is $expected" ($owner -eq $expected) $owner
}
$nsA = if ($log -match '\[1\] saves_qa_a 1\.0 \(ns=(\d+)') { [int]$Matches[1] } else { 1 }
Assert-That 'a quest task of A is given' ((Said $log 'task given') -eq 'true') (Said $log 'task given')
Assert-That 'the radio''s blob is taken' ((Said $log 'radio saved') -eq 'true')
Assert-That 'B''s class change of a stock section is noted' ($log -match '\* XMS: \[saves_qa_b\] changes the class of \[wpn_gauss\] \(WP_SVD -> WP_SVU\)')

if (-not $made.Count) { Write-Host '== nothing was made - stopping'; exit (++$script:failed) }

function Get-Expect {
    param([hashtable]$Presence)
    $rows = foreach ($n in $names) {
        if (-not $made.ContainsKey($n)) { continue }
        $m = $made[$n]
        $p = if ($Presence.ContainsKey($moduleOf[$n])) { $Presence[$moduleOf[$n]] } else { 'present' }
        "{ name = `"$n`", id = $($m.id), section = `"$($m.section)`", parent = $($m.parent), want = `"$p`" }"
    }
    $rows += foreach ($c in $guideChildren) {
        $p = if ($Presence.ContainsKey('a')) { $Presence['a'] } else { 'present' }
        "{ name = `"guide child $($c.id)`", id = $($c.id), section = `"$($c.section)`", parent = $($made['guide'].id), want = `"$p`" }"
    }
    return '{ ' + ($rows -join ', ') + ' }'
}

function Test-Checks {
    param([string]$Log, [string]$Label, [hashtable]$Presence, [hashtable]$Except = @{})
    foreach ($n in $names) {
        if (-not $made.ContainsKey($n)) { continue }
        $want = if ($Except.ContainsKey($n)) { $Except[$n] } elseif ($Presence.ContainsKey($moduleOf[$n])) { $Presence[$moduleOf[$n]] } else { 'present' }
        $got = Said $Log "check $n"
        Assert-That "${Label}: $n $want" ($got -eq $want) $got
    }
    foreach ($c in $guideChildren) {
        $want = if ($Presence.ContainsKey('a')) { $Presence['a'] } else { 'present' }
        $got = Said $Log "check guide child $($c.id)"
        Assert-That "${Label}: the character's $($c.section) $want" ($got -eq $want) $got
    }
}

$weaponId = $made['weapon'].id
Write-Host '== c: saves_qa_1 as the base game reads it'
$save1 = Read-Save 'saves_qa_1'
$records = @($save1.records)
$byId = @{}
foreach ($r in $records) { $byId[[int]$r.id] = $r }
$moduleSections = @('saves_qa_a_pistol', 'saves_qa_a_guide', 'saves_qa_a_gone', 'saves_qa_a_morph', 'saves_qa_a_ammo', 'saves_qa_b_knife', 'wpn_gauss')
Assert-That 'no record of a module''s section, nor of the section whose class B changed' (-not @($records | Where-Object { $moduleSections -contains $_.section }).Count) (@($records | Where-Object { $moduleSections -contains $_.section } | ForEach-Object { "$($_.id) $($_.section)" }) -join ', ')
$orphans = @($records | Where-Object { $_.parent -ne 65535 -and -not $byId.ContainsKey([int]$_.parent) })
Assert-That 'no record of a child whose parent is not a record' (-not $orphans.Count) (@($orphans | ForEach-Object { "$($_.id)<-$($_.parent)" }) -join ', ')
$placeholders = @($records | Where-Object { $_.section -eq 'space_restrictor' -and $_.name -like 'xms_ph_*' })
foreach ($n in $names) {
    if (-not $made.ContainsKey($n) -or -not $moduleOf[$n]) { continue }
    $r = $byId[$made[$n].id]
    $ok = $r -and $r.section -eq 'space_restrictor' -and $r.name -like 'xms_ph_*' -and $r.custom_data -match "module = saves_qa_$($moduleOf[$n])" -and
        $r.parent -eq 65535 -and $r.shapes -eq 0 -and -not ($r.flags -band 2) -and $r.spawn_id -eq 65535 -and $r.client_data -eq 0
    Assert-That "a placeholder holds the id of $n" $ok $(if ($r) { "$($r.section) $($r.name) flags=$($r.flags) shapes=$($r.shapes)" } else { 'no record' })
}
$lineages = @($placeholders | ForEach-Object { $_.name.Substring(7, 16) } | Sort-Object -Unique)
Assert-That 'every placeholder carries the lineage of the save''s own .scov' ($lineages.Count -eq 1 -and $lineages[0] -eq $save1.scov.save_id) "$($lineages -join ',') / $($save1.scov.save_id)"
$weaponRecord = $byId[$weaponId]
Assert-That 'the stock pistol stays in .scop' ($weaponRecord -and $weaponRecord.section -eq 'wpn_pm')
$types = @($save1.scov.chunks | ForEach-Object { $_.type })
Assert-That 'the .scov holds the modules'' objects, the pistol''s state and the manifest' ((@($types | Where-Object { $_ -like '584f*' }).Count -eq 2) -and
    (@($types | Where-Object { $_ -like '5845*' }).Count -ge 1) -and ($types -contains '31534d58') -and ($types -contains '584dff04')) ($types -join ' ')

Write-Host '== a: saves_qa_1 with the modules'
Set-Plan ("{ phase = `"check`", weapon = $weaponId, declared = $($made['declared'].id), expect = " + (Get-Expect @{}) + ' }')
$log = Invoke-Engine -Phase 'a' -Start 'server(saves_qa_1/single/alife/load) client(localhost)' -WaitFor @('SAVESQA done')
Test-Checks $log 'a' @{}
Assert-That 'a: the stock pistol has A''s ammo loaded again' ((Said $log 'weapon ammo') -eq '2') (Said $log 'weapon ammo')
Assert-That 'a: and A''s upgrade' ((Said $log 'weapon upgrade') -eq 'true')
Assert-That 'a: the declared medkit is A''s again' ((Said $log 'declared owner') -eq 'saves_qa_a')
Assert-That 'a: the counts' ($log -match '\* XMS: \[saves_qa_a\] (\d+) object\(s\) back' -and [int]$Matches[1] -ge 8) $(if ($log -match '\* XMS: \[saves_qa_a\] (\d+) object') { "A: $($Matches[1])" })
Assert-That 'a: A''s quest task is back in the actor''s registry' ((Said $log 'task') -eq 'true')
Assert-That 'a: the radio''s blob is back' ((Said $log 'radio') -eq 'true')
Assert-That 'a: section_owner of the system config' ((Said $log 'section pistol') -eq 'saves_qa_a' -and (Said $log 'section pm') -eq 'nil' -and (Said $log 'section gauss') -eq 'saves_qa_b') "$(Said $log 'section pistol') $(Said $log 'section pm') $(Said $log 'section gauss')"
Assert-That 'a: section_owner of a config read by name' ((Said $log 'section task') -eq 'saves_qa_a' -and (Said $log 'section stock task') -eq 'nil') "$(Said $log 'section task') $(Said $log 'section stock task')"
Assert-That 'a: no object dropped, no placeholder lost' (-not ($log -match 'dropped:|placeholder\(s\) without'))

Write-Host '== b: saves_qa_1 with -no_xms, then through a save the base game makes'
Set-Plan ("{ phase = `"check`", weapon = $weaponId, save = `"saves_qa_2`", wait = 5, expect = " + (Get-Expect @{ a = 'placeholder'; b = 'placeholder' }) + ' }')
$log = Invoke-Engine -Phase 'b-noxms' -Start 'server(saves_qa_1/single/alife/load) client(localhost)' -Extra '-no_xms' -WaitFor @('Game saves_qa_2\S* is successfully saved')
Test-Checks $log 'b' @{ a = 'placeholder'; b = 'placeholder' }
Assert-That 'b: no unknown section' (-not ($log -match 'unknown section'))
Assert-That 'b: the stock pistol has the base game''s ammo' ((Said $log 'weapon ammo') -eq '0') (Said $log 'weapon ammo')
Assert-That 'b: and no module upgrade' ((Said $log 'weapon upgrade') -eq 'false')
Assert-That 'b: no quest task of A in the actor''s registry' ((Said $log 'task') -eq 'false')
Assert-That 'b: both modules kept' ($log -match 'module \[saves_qa_a\] is absent' -and $log -match 'module \[saves_qa_b\] is absent')
Remove-Item -LiteralPath (Join-Path $qaSaves 'saves_qa_3.*') -Force -ErrorAction SilentlyContinue
Copy-Save 'saves_qa_2' 'saves_qa_3' @('.scop', '.scoc')
Set-Plan ("{ phase = `"check`", weapon = $weaponId, declared = $($made['declared'].id), expect = " + (Get-Expect @{}) + ' }')
$log = Invoke-Engine -Phase 'b-lineage' -Start 'server(saves_qa_3/single/alife/load) client(localhost)' -WaitFor @('SAVESQA done')
Assert-That 'b: the lineage finds saves_qa_2.scov' ($log -match 'the module content of this save was found in \[[^\]]*saves_qa_2\.scov\]')
Test-Checks $log 'b-lineage' @{}
Assert-That 'b-lineage: the pistol''s ammo and upgrade back' ((Said $log 'weapon ammo') -eq '2' -and (Said $log 'weapon upgrade') -eq 'true')
Assert-That 'b-lineage: the declared medkit is A''s' ((Said $log 'declared owner') -eq 'saves_qa_a')
Assert-That 'b-lineage: A''s quest task back' ((Said $log 'task') -eq 'true')
$save2 = Read-Save 'saves_qa_2'
$lineage2 = @($save2.records | Where-Object { $_.section -eq 'space_restrictor' -and $_.name -like 'xms_ph_*' } | ForEach-Object { $_.name.Substring(7, 16) } | Sort-Object -Unique)
Assert-That 'b: a save without the modules carries its own lineage on the placeholders it keeps' ($lineage2.Count -eq 1 -and $lineage2[0] -eq $save2.scov.save_id) "$($lineage2 -join ',')"

Write-Host '== d: B switched off, saved and loaded twice, back'
Set-Disabled @('saves_qa_b')
Set-Plan ("{ phase = `"check`", weapon = $weaponId, save = `"saves_qa_d1`", owners = { $($made['knife_b'].id) }, expect = " + (Get-Expect @{ b = 'placeholder' }) + ' }')
$log = Invoke-Engine -Phase 'd-off-1' -Start 'server(saves_qa_1/single/alife/load) client(localhost)' -WaitFor @('Game saves_qa_d1\S* is successfully saved')
Test-Checks $log 'd-off-1' @{ b = 'placeholder' }
Assert-That 'd: object_owner answers the absent module for its placeholder' ((Said $log "owner of $($made['knife_b'].id)") -eq 'saves_qa_b') (Said $log "owner of $($made['knife_b'].id)")
Assert-That 'd: one line for the absent module' ($log -match 'module \[saves_qa_b\] is absent - this game keeps \d+ object\(s\)') ([regex]::Match($log, 'module \[saves_qa_b\][^\r\n]*').Value)
Set-Plan ("{ phase = `"check`", weapon = $weaponId, save = `"saves_qa_d2`", expect = " + (Get-Expect @{ b = 'placeholder' }) + ' }')
$log = Invoke-Engine -Phase 'd-off-2' -Start 'server(saves_qa_d1/single/alife/load) client(localhost)' -WaitFor @('Game saves_qa_d2\S* is successfully saved')
Test-Checks $log 'd-off-2' @{ b = 'placeholder' }
$d1 = Read-Save 'saves_qa_d1'
$d2 = Read-Save 'saves_qa_d2'
$bType = @($save1.scov.chunks | Where-Object { $_.type -like '584f*' -and $_.type -ne ('584f{0:x4}' -f $nsA) } | ForEach-Object { $_.type })
Assert-That 'd: B''s chunks stay in the .scov through the saves made without it' ($bType.Count -eq 1 -and (@($d1.scov.chunks | Where-Object { $bType -contains $_.type }).Count -eq 1) -and (@($d2.scov.chunks | Where-Object { $bType -contains $_.type }).Count -eq 1) -and ($d2.scov.chunks.type -contains '314b4d58')) ($bType -join ',')
Set-Disabled @()
Set-Plan ("{ phase = `"check`", weapon = $weaponId, declared = $($made['declared'].id), expect = " + (Get-Expect @{}) + ' }')
$log = Invoke-Engine -Phase 'd-back' -Start 'server(saves_qa_d2/single/alife/load) client(localhost)' -WaitFor @('SAVESQA done')
Test-Checks $log 'd-back' @{}
Assert-That 'd-back: B''s objects back' ($log -match '\* XMS: \[saves_qa_b\] [1-9]\d* object\(s\) back')

Write-Host '== d: B''s folder gone instead'
Install-Modules -WithoutB
Set-Plan ("{ phase = `"check`", weapon = $weaponId, save = `"saves_qa_d3`", expect = " + (Get-Expect @{ b = 'placeholder' }) + ' }')
$log = Invoke-Engine -Phase 'd-removed' -Start 'server(saves_qa_1/single/alife/load) client(localhost)' -WaitFor @('Game saves_qa_d3\S* is successfully saved')
Test-Checks $log 'd-removed' @{ b = 'placeholder' }
Install-Modules
Set-Plan ("{ phase = `"check`", weapon = $weaponId, expect = " + (Get-Expect @{}) + ' }')
$log = Invoke-Engine -Phase 'd-restored' -Start 'server(saves_qa_d3/single/alife/load) client(localhost)' -WaitFor @('SAVESQA done')
Test-Checks $log 'd-restored' @{}

Write-Host '== e: A changed - a section gone, a class changed'
Install-Modules -ChangedA
Set-Plan ("{ phase = `"check`", weapon = $weaponId, expect = " + (Get-Expect @{}) + ' }')
$log = Invoke-Engine -Phase 'e' -Start 'server(saves_qa_1/single/alife/load) client(localhost)' -WaitFor @('SAVESQA done')
Test-Checks $log 'e' @{} @{ gone = 'absent'; morph = 'absent' }
Assert-That 'e: the gone section''s object dropped with its line' ($log -match "object $($made['gone'].id) \[saves_qa_a_gone\] dropped: its section is gone from the module")
Assert-That 'e: the changed class''s object dropped with its line' ($log -match "object $($made['morph'].id) \[saves_qa_a_morph\] dropped: its section's class is not the one it was written for")
Install-Modules

Write-Host '== f: xms_forget'
Set-Disabled @('saves_qa_b')
Set-Plan ("{ phase = `"check`", weapon = $weaponId, forget = `"saves_qa_b`", save = `"saves_qa_f`", expect = " + (Get-Expect @{ b = 'placeholder' }) + ' }')
$log = Invoke-Engine -Phase 'f-forget' -Start 'server(saves_qa_d2/single/alife/load) client(localhost)' -WaitFor @('Game saves_qa_f\S* is successfully saved')
Assert-That 'f: xms_forget answers' ($log -match '\* XMS: forgot module \[saves_qa_b\]: [1-9]\d* object\(s\)')
Assert-That 'f: the scripts hear of it' ((Said $log 'forgotten') -eq 'saves_qa_b' -and (Said $log 'forget heard') -eq 'saves_qa_b')
$f = Read-Save 'saves_qa_f'
Assert-That 'f: no chunk of B in the save' (-not @($f.scov.chunks | Where-Object { $bType -contains $_.type }).Count)
Assert-That 'f: no placeholder of B in the save' (-not @($f.records | Where-Object { $_.name -like 'xms_ph_*' -and $_.custom_data -match 'saves_qa_b' }).Count)
Set-Disabled @()
Set-Plan ("{ phase = `"check`", weapon = $weaponId, expect = " + (Get-Expect @{ b = 'absent' }) + ' }')
$log = Invoke-Engine -Phase 'f-back' -Start 'server(saves_qa_f/single/alife/load) client(localhost)' -WaitFor @('SAVESQA done')
Test-Checks $log 'f-back' @{ b = 'absent' }

Write-Host '== i: no module - nothing of this in the save'
Get-ChildItem -LiteralPath $qaModules -Directory | Remove-Item -Recurse -Force
Set-NewGame
Set-Plan '{ phase = "plain", save = "saves_qa_i" }'
$log = Invoke-Engine -Phase 'i' -Start 'server(all/single/alife/new) client(localhost)' -WaitFor @('Game saves_qa_i\S* is successfully saved')
$i = Read-Save 'saves_qa_i'
Assert-That 'i: no placeholder' (-not @($i.records | Where-Object { $_.section -eq 'space_restrictor' -and $_.name -like 'xms_ph_*' }).Count)
Assert-That 'i: no module chunk' (-not @($i.scov.chunks | Where-Object { $_.type -match '^58(4d|4c|4f|45)' -or $_.type -in @('31534d58', '314b4d58') }).Count) (($i.scov.chunks.type) -join ' ')

}

Write-Host "== o: an older save - a module's spawn-layer object inside .scop, written by the engine of $OlderRuntime"
Get-ChildItem -LiteralPath $qaModules -Directory | Remove-Item -Recurse -Force
Install-Module -Id 'saves_qa_o' -Files @{ 'spawn\saves_qa.xspawn' = $moduleA['spawn\saves_qa.xspawn'] }
Update-Runtime -From $OlderRuntime
try {
    Set-NewGame
    Set-Plan '{ phase = "plain", save = "saves_qa_o1" }'
    $log = Invoke-Engine -Phase 'o-old' -Start 'server(all/single/alife/new) client(localhost)' -WaitFor @('Game saves_qa_o1\S* is successfully saved')
}
finally { Update-Runtime }
$o1 = Read-Save 'saves_qa_o1'
$crates = @($o1.records | Where-Object { $_.section -eq 'inventory_box' -and $_.spawn_id -ge 32768 -and $_.spawn_id -ne 65535 })
Assert-That 'o: the older save holds the spawn-layer box in .scop' ($crates.Count -eq 1) "$($crates.Count)"
if ($crates.Count -eq 1) {
    $crateId = [int]$crates[0].id
    $expect = "{ { name = `"old crate`", id = $crateId, section = `"inventory_box`", parent = 65535, want = `"WANT`" } }"
    Remove-Item -LiteralPath (Join-Path $qaModules 'saves_qa_o') -Recurse -Force
    Set-Plan ("{ phase = `"check`", save = `"saves_qa_o2`", expect = " + $expect.Replace('WANT', 'placeholder') + ' }')
    $log = Invoke-Engine -Phase 'o-absent' -Start 'server(saves_qa_o1/single/alife/load) client(localhost)' -WaitFor @('Game saves_qa_o2\S* is successfully saved')
    Assert-That 'o: the absent module''s object is taken out of the world and kept' ($log -match 'module \[saves_qa_o\] is absent - 1 object\(s\) of its spawn layer this older save kept in \.scop are kept') ([regex]::Match($log, '[^\r\n]*saves_qa_o\][^\r\n]*').Value)
    Assert-That 'o: a placeholder holds its id' ((Said $log 'check old crate') -eq 'placeholder') (Said $log 'check old crate')
    Install-Module -Id 'saves_qa_o' -Files @{ 'spawn\saves_qa.xspawn' = $moduleA['spawn\saves_qa.xspawn'] }
    Set-Plan ("{ phase = `"check`", expect = " + $expect.Replace('WANT', 'present') + ' }')
    $log = Invoke-Engine -Phase 'o-back' -Start 'server(saves_qa_o2/single/alife/load) client(localhost)' -WaitFor @('SAVESQA done')
    Assert-That 'o: back with its id, not placed twice' ((Said $log 'check old crate') -eq 'present' -and -not ($log -match 'late spawn: \+')) (Said $log 'check old crate')
}

if (-not $OlderSaveOnly) {
Write-Host '== h: the limits said in the log - a game in a module''s mode, a module changing the game''s own objects'
Get-ChildItem -LiteralPath $qaModules -Directory | Remove-Item -Recurse -Force
Install-Module -Id 'saves_qa_h' -Files @{ 'spawn\saves_qa.xspawn' = "[obj:base:l01_escape/esc_inventory_box_0000]`r`nop = modify`r`ndisabled = false`r`n" }
[IO.File]::AppendAllText((Join-Path $qaModules 'saves_qa_h\mod.ltx'), "`r`n[provides_mode]`r`nid = saves_qa_h`r`n", $utf8)
Set-NewGame -Mode 'saves_qa_h'
Set-Plan '{ phase = "plain", save = "saves_qa_h" }'
$log = Invoke-Engine -Phase 'h' -Start 'server(all/single/alife/new) client(localhost)' -WaitFor @('Game saves_qa_h\S* is successfully saved')
Assert-That 'h: the changes to the game''s own objects are said' ($log -match '! XMS: \[saves_qa_h\] changes 1 and removes 0 of the game''s own objects in place') ([regex]::Match($log, '[^\r\n]*in place[^\r\n]*').Value)
Assert-That 'h: the module''s mode is said' ($log -match '! XMS: this game runs mode \[saves_qa_h\] of module \[saves_qa_h\]') ([regex]::Match($log, '[^\r\n]*this game runs[^\r\n]*').Value)
Set-NewGame
}

Remove-Item -LiteralPath $qaScript -Force -ErrorAction SilentlyContinue
if (-not $Keep) { Remove-Root }
Write-Host "== failures: $script:failed"
exit $script:failed
