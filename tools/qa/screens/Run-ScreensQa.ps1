<#
.SYNOPSIS
    The modules' screens and music in the real game: mod.ltx [screens], [screen.<id>], [track.<id>].

.DESCRIPTION
    Starts the freshly built engine on a hidden desktop from a QA root of its own, with modules generated here, and
    judges by the log and by what the console says (xms_screens). The installed game stays read-only.

    A  a module's menu screen: picked, the game's background gives way, the music list is the module's
    B  the loading screen: forced to a module's screen with layers of every kind; the presenter opens them
    C  the rules of who gives way (mix / only / limited to a level), asked of the console
    D  a module that ships nothing but descriptions: .npcasset characters and a [provides_mode]
    E  a new game with it: the NPC spawns as that character, a PDA layout restyles the map spots
#>
[CmdletBinding()]
param(
    [string]$GameRoot = 'D:\Games\Dead Air',
    [string]$BuildRoot,
    [string]$ResultLabel = (Get-Date -Format 'yyyyMMdd-HHmmss'),
    [string]$VidMode = '1024x768',
    [int]$TimeoutSeconds = 150,
    [switch]$RebuildRoot,
    # Screens: A and B. Descriptions: D and E, what the game makes of a module's assets and manifest.
    [ValidateSet('All', 'Screens', 'Descriptions')]
    [string]$Part = 'All',
    # D also reads these .npcasset files as a module of this id, for comparing what comes out
    [string]$ExtraCharacters,
    [string]$ExtraId = 'extra_characters'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $BuildRoot) { $BuildRoot = Join-Path $repositoryRoot 'bin\x64\Release' }
$GameRoot = (Resolve-Path -LiteralPath $GameRoot).Path
$qaRoot = Join-Path $GameRoot '_qa\screens'
$qaAppData = Join-Path $qaRoot 'appdata'
$qaGameData = Join-Path $qaRoot 'gamedata'
$qaModules = Join-Path $qaRoot 'modules'
$qaEngine = Join-Path $qaRoot 'xrEngine.exe'
$qaLog = Join-Path $qaAppData 'logs\grizzly_admin.log'
$compatGameData = Join-Path $repositoryRoot 'packaging\dead-air-x64\compatibility\gamedata'
$manifestPath = Join-Path $repositoryRoot 'packaging\dead-air-x64\installer\runtime-files.txt'
$helper = Join-Path $repositoryRoot 'tools\qa\Start-DetachedHiddenDesktopProcess.ps1'
$capture = Join-Path $repositoryRoot 'tools\qa\Capture-HiddenDesktopWindow.ps1'
$layersKey = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers'
$resultRoot = Join-Path $PSScriptRoot "results\$ResultLabel"
$utf8 = [Text.UTF8Encoding]::new($false)
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

function Update-Runtime {
    $manifest = @(Get-Content -LiteralPath $manifestPath | Where-Object { $_.Trim() -ne '' })
    foreach ($file in $manifest) {
        $built = Join-Path $BuildRoot $file
        $installed = Join-Path $GameRoot $file
        $source = if (Test-Path -LiteralPath $built) { $built } elseif (Test-Path -LiteralPath $installed) { $installed } else { throw "no runtime file: $file" }
        Copy-Item -LiteralPath $source -Destination (Join-Path $qaRoot $file) -Force
    }
    Get-ChildItem -LiteralPath $GameRoot -File -Filter '*.dll' | Where-Object { $manifest -notcontains $_.Name } |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $qaRoot $_.Name) -Force }
}

function New-Root {
    Write-Host "== building the QA root: $qaRoot"
    if (Test-Path -LiteralPath $qaRoot) {
        $link = Join-Path $qaRoot 'database'
        if (Test-Path -LiteralPath $link) { [IO.Directory]::Delete($link, $false) }
        Remove-Item -LiteralPath $qaRoot -Recurse -Force
    }
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
    foreach ($c in @('shaders_cache', 'shaders_cache_grizzly', 'shaders_cache_oxr', 'render-hardware.cache')) {
        $s = Join-Path $GameRoot "appdata\$c"
        if (Test-Path -LiteralPath $s) { Copy-Item -LiteralPath $s -Destination $qaAppData -Recurse -Force }
    }
    Copy-Tree (Join-Path $GameRoot 'gamedata') $qaGameData
    Copy-Tree $compatGameData $qaGameData
}

function Set-UserLtx {
    param([string[]]$Commands)
    $cp1251 = [Text.Encoding]::GetEncoding(1251)
    $source = Join-Path $GameRoot 'appdata\user.ltx'
    $lines = if (Test-Path -LiteralPath $source) { @([IO.File]::ReadAllLines($source, $cp1251)) } else { @() }
    $overrides = [ordered]@{ 'renderer' = 'renderer_r4'; 'vid_mode' = $VidMode; 'vid_window_mode' = 'st_opt_windowed'; 'rs_fullscreen' = 'off'
        'rs_v_sync' = 'off'; 'rs_fps_limit' = '60'; 'g_pause_in_background' = '0'; 'snd_volume_music' = '0.5'; 'snd_volume_eff' = '0' }
    $lines = @($lines | Where-Object { $l = $_; -not (@($overrides.Keys | Where-Object { $l -match "^\s*$_\s" }).Count) })
    foreach ($k in $overrides.Keys) { $lines += "$k $($overrides[$k])" }
    [IO.File]::WriteAllLines((Join-Path $qaAppData 'user.ltx'), [string[]]($lines + $Commands), $cp1251)
}

# a 64 x 64 uncompressed DDS of one colour
function New-Dds {
    param([byte]$R, [byte]$G, [byte]$B, [byte]$A = 255)
    $w = 64; $h = 64
    $bytes = [byte[]]::new(128 + $w * $h * 4)
    [Text.Encoding]::ASCII.GetBytes('DDS ').CopyTo($bytes, 0)
    $put = { param($at, $v) [BitConverter]::GetBytes([uint32]$v).CopyTo($bytes, $at) }
    & $put 4 124; & $put 8 0x100F; & $put 12 $h; & $put 16 $w; & $put 20 ($w * 4)
    & $put 76 32; & $put 80 0x41; & $put 88 32; & $put 92 0x00FF0000; & $put 96 0x0000FF00; & $put 100 0x000000FF; & $put 104 4278190080
    & $put 108 0x1000
    for ($i = 128; $i -lt $bytes.Length; $i += 4) { $bytes[$i] = $B; $bytes[$i + 1] = $G; $bytes[$i + 2] = $R; $bytes[$i + 3] = $A }
    return , $bytes
}

# the smallest GIF of one frame
function New-Gif {
    return , [byte[]](0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 16, 0, 9, 0, 0x80, 0, 0, 0xFF, 0, 0, 0, 0, 0xFF, 0x2C, 0, 0, 0, 0, 16, 0, 9, 0, 0, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3B)
}

function Install-Module {
    param([string]$Id, [string]$Text, [hashtable]$Files = @{}, [string]$ModuleExtra = '')
    $root = Join-Path $qaModules $Id
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $root 'mod.ltx'), ("[module]`r`nid = $Id`r`nname = $Id`r`nversion = 1.0`r`napi = 1`r`ntarget = default`r`n$ModuleExtra`r`n`r`n" + $Text), $utf8)
    foreach ($rel in $Files.Keys) {
        $path = Join-Path $root $rel
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        [IO.File]::WriteAllBytes($path, [byte[]]$Files[$rel])
    }
}

function Invoke-Engine {
    param([string]$Phase, [string[]]$Commands, [string[]]$WaitFor, [string]$Start = '')
    Get-Process xrEngine -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $qaEngine } | Stop-Process -Force
    if (Test-Path -LiteralPath $qaLog) { Remove-Item -LiteralPath $qaLog -Force }
    Set-UserLtx -Commands $Commands
    New-Item -Path $layersKey -Force | Out-Null
    New-ItemProperty -Path $layersKey -Name $qaEngine -Value '~ HIGHDPIAWARE' -PropertyType String -Force | Out-Null
    $desktop = "DeadAirScreens$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
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
        while ($pending.Count -and [DateTime]::UtcNow -lt $deadline -and -not $engine.HasExited) {
            Start-Sleep -Milliseconds 500
            if (-not (Test-Path -LiteralPath $qaLog)) { continue }
            $pending = @($pending | Where-Object { -not (Select-String -LiteralPath $qaLog -Pattern $_ -Quiet -ErrorAction SilentlyContinue) })
        }
        Assert-That "$Phase reached its log state" (-not $pending.Count) $(if ($pending.Count) { "missing: $($pending -join ' | ')" })
        Start-Sleep -Seconds 4
        try { & $capture -DesktopName $desktop -ProcessId $engine.Id -OutputPath (Join-Path $resultRoot "$Phase.png") | Out-Null } catch { Write-Warning "capture failed: $_" }
    }
    finally {
        if ($engine -and -not $engine.HasExited) { Stop-Process -Id $engine.Id -Force; $engine.WaitForExit(20000) | Out-Null }
        if (Test-Path -LiteralPath $launcherFile) {
            $launcher = Get-Process -Id ([int](Get-Content -LiteralPath $launcherFile -Raw)) -ErrorAction SilentlyContinue
            if ($launcher) { Stop-Process -Id $launcher.Id -Force -ErrorAction SilentlyContinue }
        }
        if (Test-Path -LiteralPath $qaLog) { Copy-Item -LiteralPath $qaLog -Destination (Join-Path $resultRoot "$Phase.log") -Force }
    }
    $log = if (Test-Path -LiteralPath $qaLog) { Get-Content -LiteralPath $qaLog -Raw -Encoding 1251 } else { '' }
    Assert-That "$Phase finished without an engine fault" (-not ($log -match 'FATAL ERROR|stack trace:|Assertion failed'))
    return $log
}

if ($RebuildRoot -or -not (Test-Path -LiteralPath (Join-Path $qaRoot 'fsgame.ltx'))) { New-Root }
Update-Runtime
Get-ChildItem -LiteralPath $qaModules -Directory -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force

if ($Part -ne 'Descriptions') {
$shaderS = @'
function normal(shader, t_base, t_second, t_detail)
	shader:begin("stub_notransform_t", "screens_qa_fog")
		: fog(false)
		: zb(false, false)
		: blend(true, blend.srcalpha, blend.invsrcalpha)
end
'@
$shaderPs = @'
#include "common.h"
uniform float4 xms_ui_time;
float4 main(v2p_TL I) : SV_Target
{
	return float4(0.6, 0.7, 0.8, 0.25 + 0.2 * sin(xms_ui_time.x + I.HPos.x * 0.01));
}
'@
$page = '<html><body style="margin:0"><div style="position:absolute;left:80px;top:80px;color:#ffffff;font-size:64px">Screens QA</div><img src="logo.png"></body></html>'

$ogg = Join-Path $resultRoot 'tone.ogg'
if (-not (Test-Path -LiteralPath $ogg)) {
    & ffmpeg -hide_banner -loglevel error -y -f lavfi -i 'sine=frequency=220:duration=2' -c:a libvorbis $ogg
    if ($LASTEXITCODE) { throw 'ffmpeg could not make the test tone' }
}
Install-Module -Id 'screens_a' -Text @'
[screens]
menu_screens = only
menu_music   = only
loading_music_end = next

[screen.night]
show  = loading, menu, pause
music = rain
hide  = tip
layer = art\back.dds
layer = art\spark.gif
opacity = 0.5
rect = 10 10 30 30
fit = contain
layer = shaders_src\screens_qa_fog.s
layer = art\title.html

[screen.only_garbage]
show   = loading
levels = l02_garbage
layer  = art\red.dds

[track.rain]
file = music\rain.ogg

[track.wind]
file = music\wind.ogg
play = loading, menu
weight = 3
volume = 0.5
'@ -Files @{
    'art\back.dds' = (New-Dds 20 40 90)
    'art\red.dds' = (New-Dds 200 10 10)
    'art\spark.gif' = (New-Gif)
    'art\title.html' = $utf8.GetBytes($page)
    'shaders_src\screens_qa_fog.s' = $utf8.GetBytes($shaderS)
    'shaders_src\screens_qa_fog.ps' = $utf8.GetBytes($shaderPs)
    'music\rain.ogg' = [IO.File]::ReadAllBytes($ogg)
    'music\wind.ogg' = [IO.File]::ReadAllBytes($ogg)
}

Install-Module -Id 'screens_b' -Text @'
[screen.other]
show  = loading, menu
layer = art\green.dds
'@ -Files @{ 'art\green.dds' = (New-Dds 10 160 40) } -ModuleExtra 'disable_game_updates = true'

Write-Host '== A: the menu and the rules'
$log = Invoke-Engine -Phase 'menu' -Commands @('xms_screens') -WaitFor @('\[screens\] menu:')
Assert-That 'the game read both modules' ($log -match '\[screens_a\] loading: screens mix, music mix; menu: screens only, music only')
Assert-That 'a screen lists its layers' ($log -match 'screen night: 4 layer\(s\), shows on loading menu pause')
Assert-That 'a track of a screen only joins no shuffle' ($log -match 'track rain: xms_screens\\screens_a\\rain')
Assert-That '"only" takes the menu from the game and the other module' ($log -match '\[screens\] menu: screens_a\.night, 1 track')
Assert-That 'a module that says so turns the game''s updates off' ($log -match "\[ModOptOut\] auto-update and bug reports disabled by mod 'screens_b'")
Assert-That 'nothing the module declared was refused' (-not ($log -match '! XMS.*screens_a|! \[screens\]'))
Assert-That 'mixing leaves other modules a chance on the loading screen' ($log -match 'loading now: (game\.own|screens_a\.|screens_b\.)')

Write-Host '== B: a real loading with every kind of layer'
$log = Invoke-Engine -Phase 'loading' -Commands @('xms_screens_force loading screens_a.night', 'loading_screen_capture 12') -Start 'server(all/single/alife/new) client(localhost)' `
    -WaitFor @('\[screens\] loading: screens_a\.night', '\[loading\] its own thread drew')
Assert-That 'the screen tied to a track plays it' ($log -match '\[screens\] music: xms_screens\\screens_a\\rain\.ogg')
Assert-That 'no layer was refused' (-not ($log -match '! \[screens\]|! \[loading\]'))
$frames = @(Get-ChildItem -LiteralPath (Join-Path $qaAppData 'screenshots') -Directory -Filter 'loading_*' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1 |
    ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -File })
Assert-That 'the loading screen''s own thread drew it' ($frames.Count -gt 0) "$($frames.Count) frame(s)"
if ($frames.Count) { Copy-Item -LiteralPath $frames[[int]($frames.Count / 2)].FullName -Destination $resultRoot -Force }

}

if ($Part -ne 'Screens') {
Write-Host '== D: what a module only describes'
$register = @'
local function say(name, value) xms.log("CHARQA " .. name .. " = " .. tostring(value)) end

local ini = system_ini()
say("section", ini:section_exist("chars_a_guard"))
say("template", ini:section_exist("chars_a_base"))
say("squad", ini:section_exist("chars_a_guard_squad"))
say("no_squad", ini:section_exist("chars_a_loner_squad"))
say("profiles", ini:r_string("profiles", "files"))
say("characters", ini:r_string("profiles", "specific_characters_files"))
local cfg = ini:r_string("chars_a_guard", "xms_trade")
local t = ini_file(cfg)
say("trade", t:section_exist("trader") and t:r_string("trader", "buy_supplies"))
say("name", game.translate_string("chars_a_guard_name"))
local known = xms.known_modes()
say("mode", known[1] and (known[1].id .. "/" .. known[1].caption))
say("listed", dead_air_x64_mode_select.modes_count())

local spawned
local function poll()
	local obj = spawned and level.object_by_id(spawned)
	if not obj then return end
	UnregisterScriptCallback("actor_on_update", poll)
	say("pda", xms.pda.describe())
	-- asked of the server: an NPC's items come online some frames after it, and the game's intro pauses them
	local knives, medkits, breads = 0, 0, 0
	for i = 1, 65534 do
		local item = alife():object(i)
		if item and item.parent_id == spawned then
			if item:section_name() == "wpn_knife" then knives = knives + 1 end
			if item:section_name() == "medkit" then medkits = medkits + 1 end
			if item:section_name() == "bread" then breads = breads + 1 end
		end
	end
	say("npc_name", obj:character_name())
	say("npc_profile", obj:profile_name())
	say("npc_section", obj:section())
	say("npc_community", obj:character_community())
	say("npc_kit", knives .. "/" .. medkits .. "/" .. breads)
	say("npc_trade", xms_characters.own_trade(obj))
end
RegisterScriptCallback("actor_on_first_update", function()
	local a = db.actor
	local se = alife():create("chars_a_guard", a:position(), a:level_vertex_id(), a:game_vertex_id())
	spawned = se and se.id
	say("spawned", spawned ~= nil)
	RegisterScriptCallback("actor_on_update", poll)
end)
'@
$fixture = Join-Path $repositoryRoot 'tools\nq\fixtures\pda\courier.pdaasset'
$layout = [IO.File]::ReadAllText($fixture, $utf8).Replace('preview = {', 'map_spots = { green_spot = { icon = "ui_xms_icon_cross", color = "#FF00FFFF", size = 32, inner = { icon = "ui_xms_icon_cross", size = 12 }, arrows = "ui_mini_sn_spot" } }, preview = {')
Install-Module -Id 'spots_a' -Text "[pda]`r`nlayout = ui\qa.pdaasset`r`n" -Files @{ 'ui\qa.pdaasset' = $utf8.GetBytes($layout) } -ModuleExtra 'mode = *'
Install-Module -Id 'chars_a' -Text @'
[provides_mode]
id = chars_mode
title = Режим проверки
'@ -Files @{
    'scripts\register.script' = $utf8.GetBytes($register)
    'characters\base.npcasset' = $utf8.GetBytes(@'
return {
	npc = 1, id = "base", abstract = true, community = "stalker", icon = "ui_inGame2_neutral_2", rank = 300,
	visual = [[actors\stalker_neutral\stalker_neutral_1]],
	supplies = { { section = "wpn_knife" }, { section = "bread", count = 2 }, "conserva" },
}
'@)
    'some\deep\folder\guard.npcasset' = $utf8.GetBytes(@'
-- the file name is its id
return {
	npc = 1, base = "base",
	name = { rus = "Проводник", eng = "Guide" },
	bio = "Знает дорогу <домой>",
	supplies = { { section = "bread", count = 0 }, { section = "medkit", count = 3 } },
	trade = { sells = { "food" }, buys = {}, markup = 1.5, pays = 0.2, restock = 6,
		stock = { { section = "bread", count = 5, chance = 0.5 } } },
}
'@)
    'characters\loner.npcasset' = $utf8.GetBytes('return { npc = 1, id = "loner", name = "Одиночка", visual = "actors\\stalker_neutral\\stalker_neutral_1", squad = false }')
    'characters\bad.npcasset' = $utf8.GetBytes('return { npc = 1, id = "bad", name = "a" .. "b", visual = "x" }')
    'characters\noname.npcasset' = $utf8.GetBytes('return { npc = 1, id = "noname", visual = "x" }')
}
if ($ExtraCharacters) {
    $files = @{}
    Get-ChildItem -LiteralPath $ExtraCharacters -Filter '*.npcasset' -Recurse | ForEach-Object { $files["characters\$($_.Name)"] = [IO.File]::ReadAllBytes($_.FullName) }
    Install-Module -Id $ExtraId -Text '' -Files $files
}

$published = Join-Path $qaAppData 'xms_generated'
$stale = Join-Path $published 'configs\gameplay\character_desc_gone.xml'
New-Item -ItemType Directory -Path (Split-Path -Parent $stale) -Force | Out-Null
[IO.File]::WriteAllText($stale, '<xml/>')

$log = Invoke-Engine -Phase 'describe' -Commands @() -WaitFor @('CHARQA listed')
Assert-That 'the module''s characters are read' ($log -match '\[chars_a\] 2 character\(s\) read from the module')
Assert-That 'one written as an expression is left out, with its line' ($log -match 'bad\.npcasset: left out - line 1: only plain values')
Assert-That 'one without a name is left out' ($log -match 'noname\.npcasset: left out - it has no name')
foreach ($pair in @(@('section', 'true'), @('template', 'false'), @('squad', 'true'), @('no_squad', 'false'))) {
    Assert-That "section check: $($pair[0])" ($log -match "CHARQA $($pair[0]) = $($pair[1])")
}
Assert-That 'the profile list grew, nothing of the game''s lost' ($log -match 'CHARQA profiles = [^\r\n]*,npc_profile_chars_a')
Assert-That 'the character list grew' ($log -match 'CHARQA characters = [^\r\n]*,character_desc_chars_a')
Assert-That 'the trader''s config opens through the scripts' ($log -match 'CHARQA trade = chars_a_guard_stock')
Assert-That 'the name is in the string table, in cp1251' ($log -match 'CHARQA name = Проводник')
Assert-That 'the mode the manifest declares is known by its title' ($log -match 'CHARQA mode = chars_mode/Режим проверки')
Assert-That 'and the new-game list offers it' (($log -match 'module mode\(s\) offered on the new-game screen: 1') -and ($log -match 'CHARQA listed = 1'))
Assert-That 'no file of the module was made for any of it' (-not (Test-Path -LiteralPath (Join-Path $qaModules 'chars_a\gamedata')))
Assert-That 'what the last start published and this one did not is gone' (-not (Test-Path -LiteralPath $stale))
$desc = [IO.File]::ReadAllText((Join-Path $published 'configs\gameplay\character_desc_chars_a.xml'), [Text.Encoding]::GetEncoding(1251))
Assert-That 'a template is no character' (-not $desc.Contains('chars_a_base"'))
$strings = [IO.File]::ReadAllText((Join-Path $published 'configs\text\rus\chars_a_npc.xml'), [Text.Encoding]::GetEncoding(1251))
Assert-That 'a text is escaped and converted' ($strings.Contains('Знает дорогу &lt;домой&gt;'))
Assert-That 'an English install gets the English name' ([IO.File]::ReadAllText((Join-Path $published 'configs\text\eng\chars_a_npc.xml')).Contains('Guide'))
$stamp = (Get-Item -LiteralPath (Join-Path $published 'configs\text\rus\chars_a_npc.xml')).LastWriteTimeUtc

Write-Host '== E: a new game: the character spawns as itself'
$log = Invoke-Engine -Phase 'describe-game' -Commands @() -Start 'server(all/single/alife/new) client(localhost)' -WaitFor @('CHARQA npc_trade')
Assert-That 'a later start leaves a file that did not change alone' ((Get-Item -LiteralPath (Join-Path $published 'configs\text\rus\chars_a_npc.xml')).LastWriteTimeUtc -eq $stamp)
Assert-That 'the NPC spawned from the module''s section' ($log -match 'CHARQA spawned = true')
Assert-That 'it is the character, by name' ($log -match 'CHARQA npc_name = Проводник')
Assert-That 'by profile' ($log -match 'CHARQA npc_profile = chars_a_guard')
Assert-That 'by section and community' (($log -match 'CHARQA npc_section = chars_a_guard') -and ($log -match 'CHARQA npc_community = stalker'))
Assert-That 'the kit is the base''s and its own, merged' ($log -match 'CHARQA npc_kit = [1-9]/[1-9]/0')
Assert-That 'the shop is its own' ($log -match 'CHARQA npc_trade = misc\\trade\\chars_a\\guard\.ltx')
Assert-That 'the PDA layout restyled the map spots' ($log -match 'XMS PDA: \[spots_a\] the layout restyles the map spots')
Assert-That 'and the patch fitted the game''s file' (-not ($log -match 'bad generated patch|xmlp \[spots_a\] replace failed'))
}

Write-Host "== failures: $script:failed"
exit $script:failed
