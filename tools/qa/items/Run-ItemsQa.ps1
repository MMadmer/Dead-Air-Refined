<#
.SYNOPSIS
    A module's items, its traders, loot and recipes, and patches of any config, in the real game: *.itemasset,
    *.tradeasset, *.lootasset, *.recipeasset, `#file` in .ltxp, and a change that reaches the sections inheriting it.

.DESCRIPTION
    Starts the freshly built engine on a hidden desktop from a QA root of its own (under _work, with the game's
    archives joined by a junction), with a module generated here, and judges by the log and by what the
    module's registrar reads back. The installed game stays read-only.

    A  the main menu: the items' sections, strings, icon, hud; a stock weapon changed with `add`, reaching its
       scope variants; a trader's stock, the death loot and the recipes of the module's own files and an item
       scene, each read by the script that reads it; a key of another file's in an item named and never read;
       a module patch of a base overlay's section; a config the game writes back read as written; the refusals
    B  a new game: the new items spawn into the actor's inventory under their names
    C  no module at all (-Identity): a new game started by the installed engine and by the new build, each
       dumping the composed system config, game.ltx and four configs scripts read; the dumps must be equal
#>
[CmdletBinding()]
param(
    [string]$GameRoot = 'D:\Games\Dead Air',
    [string]$BuildRoot,
    [string]$QaRoot,
    [string]$ResultLabel = (Get-Date -Format 'yyyyMMdd-HHmmss'),
    [string]$VidMode = '1024x768',
    [int]$TimeoutSeconds = 180,
    [switch]$RebuildRoot,
    [switch]$SkipGame,
    # C: what the configs are without modules, the installed engine against the new build
    [switch]$Identity,
    # keep the QA root for a look afterwards
    [switch]$Keep
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $BuildRoot) { $BuildRoot = Join-Path $repositoryRoot 'bin\x64\Release' }
if (-not $QaRoot) { $QaRoot = Join-Path $repositoryRoot '_work\items-qa\root' }
$GameRoot = (Resolve-Path -LiteralPath $GameRoot).Path
$qaRoot = [IO.Path]::GetFullPath($QaRoot)
if ($qaRoot.StartsWith($GameRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -and
    -not $qaRoot.StartsWith($repositoryRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "The QA root must not lie inside the installed game: $qaRoot"
}
$qaAppData = Join-Path $qaRoot 'appdata'
$qaGameData = Join-Path $qaRoot 'gamedata'
$qaModules = Join-Path $qaRoot 'modules'
$qaEngine = Join-Path $qaRoot 'xrEngine.exe'
$qaLog = Join-Path $qaAppData 'logs\grizzly_admin.log'
$compatGameData = Join-Path $repositoryRoot 'packaging\dead-air-x64\compatibility\gamedata'
$manifestPath = Join-Path $repositoryRoot 'packaging\dead-air-x64\installer\runtime-files.txt'
$helper = Join-Path $repositoryRoot 'tools\qa\Start-DetachedHiddenDesktopProcess.ps1'
$layersKey = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers'
$resultRoot = Join-Path $repositoryRoot "_work\items-qa\results\$ResultLabel"
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
    # an engine that has just exited may hold its folder for a moment
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
    $source = Join-Path $GameRoot 'appdata\user.ltx'
    $lines = if (Test-Path -LiteralPath $source) { @([IO.File]::ReadAllLines($source, $cp1251)) } else { @() }
    $overrides = [ordered]@{ 'renderer' = 'renderer_r4'; 'vid_mode' = $VidMode; 'vid_window_mode' = 'st_opt_windowed'; 'rs_fullscreen' = 'off'
        'rs_v_sync' = 'off'; 'rs_fps_limit' = '60'; 'g_pause_in_background' = '0'; 'snd_volume_music' = '0'; 'snd_volume_eff' = '0' }
    $lines = @($lines | Where-Object { $l = $_; -not (@($overrides.Keys | Where-Object { $l -match "^\s*$_\s" }).Count) })
    foreach ($k in $overrides.Keys) { $lines += "$k $($overrides[$k])" }
    [IO.File]::WriteAllLines((Join-Path $qaAppData 'user.ltx'), [string[]]($lines + $Commands), $cp1251)
}

function Install-Module {
    param([string]$Id, [string]$Text, [hashtable]$Files = @{})
    $root = Join-Path $qaModules $Id
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $root 'mod.ltx'), ("[module]`r`nid = $Id`r`nname = $Id`r`nversion = 1.0`r`napi = 1`r`ntarget = any`r`nmode = *`r`n`r`n" + $Text), $utf8)
    foreach ($rel in $Files.Keys) {
        $path = Join-Path $root $rel
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        [IO.File]::WriteAllBytes($path, [byte[]]$utf8.GetBytes($Files[$rel]))
    }
}

function Invoke-Engine {
    param([string]$Phase, [string[]]$Commands, [string[]]$WaitFor, [string]$Start = '')
    Get-Process xrEngine -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $qaEngine } | Stop-Process -Force
    # the log of the last start may still be held for a moment (the game ending, a scanner reading it)
    for ($try = 0; (Test-Path -LiteralPath $qaLog) -and $try -lt 120; $try++) {
        try { Remove-Item -LiteralPath $qaLog -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 250 }
    }
    if (Test-Path -LiteralPath $qaLog) { throw "The log of the last start is still held: $qaLog" }
    Set-UserLtx -Commands $Commands
    New-Item -Path $layersKey -Force | Out-Null
    New-ItemProperty -Path $layersKey -Name $qaEngine -Value '~ HIGHDPIAWARE' -PropertyType String -Force | Out-Null
    $desktop = "DeadAirItems$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
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
    Assert-That "$Phase finished without an engine fault" (-not ($log -match 'FATAL ERROR|stack trace:|Assertion failed'))
    return $log
}

function Said {
    param([string]$Log, [string]$Name)
    $m = [regex]::Match($Log, "ITEMSQA $([regex]::Escape($Name)) = ([^\r\n]*)")
    if ($m.Success) { return $m.Groups[1].Value.Trim() }
    return '<not said>'
}

if ($RebuildRoot -or -not (Test-Path -LiteralPath (Join-Path $qaRoot 'fsgame.ltx'))) { New-Root }
Update-Runtime
Get-ChildItem -LiteralPath $qaModules -Directory -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force

$register = @'
local function say(name, value) xms.log("ITEMSQA " .. name .. " = " .. tostring(value)) end
local function r(ini, sec, key)
	if ini:section_exist(sec) and ini:line_exist(sec, key) then return ini:r_string(sec, key) or "<bare>" end
	return "<none>"
end
local ini = system_ini()

say("pm_section", ini:section_exist("items_qa_pm_gold"))
say("pm_cost", r(ini, "items_qa_pm_gold", "cost"))
say("pm_weight", r(ini, "items_qa_pm_gold", "inv_weight"))
say("pm_inherited", r(ini, "items_qa_pm_gold", "fire_modes"))
say("pm_name_id", r(ini, "items_qa_pm_gold", "inv_name"))
say("pm_short_id", r(ini, "items_qa_pm_gold", "inv_name_short"))
say("pm_name", game.translate_string(r(ini, "items_qa_pm_gold", "inv_name")))
say("pm_descr", game.translate_string(r(ini, "items_qa_pm_gold", "description")))
say("pm_icon", r(ini, "items_qa_pm_gold", "icons_texture") .. "|" .. r(ini, "items_qa_pm_gold", "inv_grid_x") .. "," ..
	r(ini, "items_qa_pm_gold", "inv_grid_y") .. "," .. r(ini, "items_qa_pm_gold", "inv_grid_width") .. "," ..
	r(ini, "items_qa_pm_gold", "inv_grid_height"))
say("pm_hud", r(ini, "items_qa_pm_gold", "hud"))
say("pm_hud_visual", r(ini, "items_qa_pm_gold_hud", "item_visual"))
say("pm_hud_marker", r(ini, "items_qa_pm_gold_hud", "qa_marker"))
say("pm_extra", r(ini, "items_qa_pm_gold_extra", "cost"))
say("child_cost", r(ini, "items_qa_child", "cost"))
say("child_parent_hud", r(ini, "items_qa_child", "hud"))
say("template", ini:section_exist("items_qa_mixin") and r(ini, "items_qa_mixin", "inv_name"))
say("pm_unchanged_icon", r(ini, "wpn_pm", "inv_grid_x"))
local recipes = {}
if ini:section_exist("xms_craft") then
	for i = 0, ini:line_count("xms_craft") - 1 do
		local _, k, v = ini:r_line("xms_craft", i, "", "")
		recipes[#recipes + 1] = k .. "=" .. v
	end
end
say("craft", table.concat(recipes, " | "))
say("craft_overlay", r(ini, "xms_craft", "items_qa_own"))
say("variant_weight", r(ini, "items_qa_pm_gold_variant", "inv_weight"))
say("variant_of_variant", r(ini, "items_qa_pm_gold_variant2", "qa_variant"))
say("order_marker", r(ini, "wpn_pm", "qa_order"))
say("icon_parent", r(ini, "medkit_script", "icons_texture"))
say("icon_variant", r(ini, "bandage_script", "icons_texture"))
say("long_parent", ini:section_exist("items_qa_long_parent"))
local big = r(ini, "items_qa_list", "big")
say("big_list", #big .. "|" .. string.sub(big, -2))

say("ak74_ammo", r(ini, "wpn_ak74", "ammo_class"))
say("kobra_ammo", r(ini, "wpn_ak74_kobra", "ammo_class"))
say("ps01_ammo", r(ini, "wpn_ak74_ps01", "ammo_class"))
say("ak74_cost", r(ini, "wpn_ak74", "cost"))
say("kobra_cost", r(ini, "wpn_ak74_kobra", "cost"))
say("kobra_weight", r(ini, "wpn_ak74_kobra", "inv_weight"))
say("ak74_hud_marker", r(ini, "wpn_ak74_hud", "qa_marker"))
say("kobra_hud_marker", r(ini, "wpn_ak74_kobra_hud", "qa_marker"))
say("base_overlay", r(ini, "da_hud_animations", "wpn_hand_01"))
say("base_overlay_child", r(ini, "items_qa_pda_child", "hud"))
say("system_list", r(ini, "items_qa_list", "k"))
say("system_patch", r(ini, "items_qa_system", "k"))
say("foreign_set", r(ini, "items_qa_foreign", "qa_foreign"))

say("drink_hud", ini:section_exist("item_ea_items_qa_drink_hud"))
say("drink_hud_visual", r(ini, "item_ea_items_qa_drink_hud", "item_visual"))
say("drink_hud_show", r(ini, "item_ea_items_qa_drink_hud", "anm_ea_show"))
local scenes = ini_file("items\\items\\animations_settings.ltx")
say("scene", scenes:section_exist("items_qa_drink"))
say("scene_snd", r(scenes, "items_qa_drink", "snd"))
say("scene_tm", r(scenes, "items_qa_drink", "tm"))
say("scene_anm", r(scenes, "items_qa_drink", "anm"))
say("scene_helm", r(scenes, "items_qa_drink", "helm"))
say("scene_fit", r(scenes, "items_qa_drink", "fit"))
say("scene_own", r(scenes, "items_qa_drink", "items_qa_scene"))

local death = ini_file("misc\\death_generic.ltx")
say("loot_included", death:section_exist("stalker_novice") and death:line_count("base"))
say("loot_count", r(death, "item_count", "items_qa_pm_gold"))
say("loot_base", r(death, "base", "items_qa_pm_gold"))
say("loot_stalker", r(death, "stalker", "items_qa_pm_gold"))
say("loot_bandit", r(death, "bandit", "items_qa_pm_gold"))
say("loot_keep", r(death, "keep_items", "items_qa_marker"))
say("foreign_loot", r(death, "item_count", "items_qa_foreign"))

local trade = ini_file("misc\\trade\\trade_military.ltx")
say("sold_1", r(trade, "supplies_1", "items_qa_pm_gold"))
say("sold_2", r(trade, "supplies_2", "items_qa_pm_gold"))
say("sold_gone", r(trade, "supplies_1", "flask_empty"))
say("foreign_sold", r(trade, "supplies_1", "items_qa_foreign"))
say("preset_merchant", r(trade, "merchant", "items_qa_marker"))
say("preset_inherited", r(trade, "trade_generic_sell", "items_qa_marker"))
local other_trade = ini_file("misc\\trade\\trade_barman.ltx")
say("preset_other_trader", r(other_trade, "merchant", "items_qa_marker"))

local itms = ini_file("plugins\\itms_manager.ltx")
say("stackable", r(itms, "stackable", "items_qa_drink"))

-- an ini_file saves at its end once it is writable: what it writes must be the file, not the patch
local written = ini_file("misc\\death_generic.ltx")
written:set_readonly(false)
written:save_at_end(false)
say("writable", r(written, "item_count", "items_qa_pm_gold"))
say("writable_stock", r(written, "item_count", "wpn_knife"))
written:save_as(getFS():update_path("$app_data_root$", "items_qa_written.ltx"))
say("done", true)

local spawned = {}
local wanted = { "items_qa_pm_gold", "items_qa_drink", "wpn_ak74_kobra" }
local function poll()
	local a = db.actor
	for _, sec in ipairs(wanted) do
		if not spawned[sec] then
			local obj = a:object(sec)
			if obj then
				spawned[sec] = true
				say("held " .. sec, obj:section() .. " / " .. game.translate_string(r(ini, sec, "inv_name")))
			end
		end
	end
	for _, sec in ipairs(wanted) do
		if not spawned[sec] then return end
	end
	UnregisterScriptCallback("actor_on_update", poll)
	say("game done", true)
end
RegisterScriptCallback("actor_on_first_update", function()
	local a = db.actor
	for _, sec in ipairs(wanted) do
		local se = alife():create(sec, a:position(), a:level_vertex_id(), a:game_vertex_id(), a:id())
		say("spawned " .. sec, se ~= nil)
	end
	RegisterScriptCallback("actor_on_update", poll)
end)
'@

Install-Module -Id 'items_qa' -Text '' -Files @{
    'scripts\register.script' = $register
    # sorts before the item it starts from: it waits for it
    'items\a_child.itemasset' = @'
return { item = 1, id = "child", base = "items_qa_pm_gold", set = { cost = 1 } }
'@
    'items\pm_gold.itemasset' = @'
-- a new item, with everything an item can say
return {
  item = 1,
  id = "pm_gold",
  base = "wpn_pm",
  name = { rus = "Золотой ПМ", eng = "Golden PM" },
  description = "Блестит <очень>",
  icon = { texture = [[ui\ui_icon_equipment]], x = 4, y = 6, w = 2, h = 1 },
  set = { cost = 12345, inv_weight = 0.75 },
  hud = { set = { qa_marker = "hud of its own" } },
  sections = { { name = "items_qa_pm_gold_extra", base = "wpn_pm", set = { cost = 1 } },
               -- a row starts from what the item makes before it: its own section, an earlier row
               { name = "items_qa_pm_gold_variant", base = "items_qa_pm_gold", set = { qa_variant = 1 } },
               { name = "items_qa_pm_gold_variant2", base = "items_qa_pm_gold_variant" } },
  editor = { note = "passed by" },
}
'@
    # who sells it, who carries it and what makes it: each in the file of the side that owns it
    'trade\military.tradeasset' = @'
return {
  trade = 1,
  trader = "trade_military",
  stock = {
    { item = "items_qa_pm_gold", count = 2, chance = 0.25 },
    { item = "flask_empty", count = 0 },
  },
}
'@
    'loot\dead.lootasset' = @'
return { loot = 1, drops = { { item = "items_qa_pm_gold", count = { 0, 2 }, chance = 0.05, by = { bandit = 0.5 } } } }
'@
    'craft\workbench.recipeasset' = @'
return {
  recipe = 1,
  recipes = {
    { makes = "items_qa_pm_gold", at = "itm_repairkit_tier_1", parts = { "jar", "rope" } },
    { makes = "items_qa_pm_gold", at = "itm_repairkit_tier_2", count = 2, parts = { "jar", "rope", "jar", "rope" } },
  },
}
'@
    'bad\no_trader.tradeasset' = 'return { trade = 1, stock = { { item = "bread" } } }'
    # an item that says another file's lines: named in the log, never read
    'items\foreign.itemasset' = 'return { item = 1, id = "foreign", base = "bread", set = { qa_foreign = 1 }, sold = { { by = "trade_military" } }, loot = { count = 9 } }'
    'items\drink.itemasset' = @'
return {
  item = 1, id = "drink", base = "vodka",
  name = "Напиток",
  scene = { like = "vodka", time = 5000, helmet = true, fit = "cut", set = { items_qa_scene = true },
            hud = { base = "item_ea_vodka_hud", set = { item_visual = [[dynamics\weapons\wpn_eat\vodki\wpn_vodka_cmuphob_hud.ogf]] } } },
}
'@
    'items\mixin.itemasset' = 'return { item = 1, id = "mixin", abstract = true, set = { qa = 1 }, name = "never" }'
    'items\ak74.itemasset' = @'
-- a change of a stock weapon: its scope variants inherit it
return {
  item = 1,
  edit = "wpn_ak74",
  set = { cost = 4321, inv_weight = 9.5 },
  add = { ammo_class = { "ammo_9x18_pmm" } },
  hud = { set = { qa_marker = "changed" } },
}
'@
    'items\pm_icon.itemasset' = 'return { item = 1, edit = "wpn_pm", icon = "icons/pm.png" }'
    # two changes of one key: the later path wins, and `weapons\a` comes before `weapons2\a`
    'order\weapons\a.itemasset' = 'return { item = 1, edit = "wpn_pm", set = { qa_order = "weapons" } }'
    'order\weapons2\a.itemasset' = 'return { item = 1, edit = "wpn_pm", set = { qa_order = "weapons2" } }'
    # an icon on another texture: bandage_script inherits medkit_script and states its own cells
    'items\medkit_icon.itemasset' = 'return { item = 1, edit = "medkit_script", icon = { texture = [[ui\ui_actor_portrets]], x = 0, y = 0, w = 1, h = 1 } }'
    'bad\no_base.itemasset' = 'return { item = 1, id = "nobase" }'
    'bad\unknown_base.itemasset' = 'return { item = 1, id = "ub", base = "no_such_section_qa" }'
    'bad\exists.itemasset' = 'return { item = 1, section = "wpn_pm", base = "wpn_pm" }'
    'bad\both.itemasset' = 'return { item = 1, edit = "wpn_pm", id = "x" }'
    'bad\missing_edit.itemasset' = 'return { item = 1, edit = "no_such_qa" }'
    'bad\newer.itemasset' = 'return { item = 2, id = "newer", base = "wpn_pm" }'
    'bad\code.itemasset' = 'return { item = 1, id = "code", base = "wpn" .. "_pm" }'
    'bad\long.itemasset' = ('return { item = 1, base = "wpn_pm", section = "' + ('a' * 64) + '" }')
    'gamedata\configs\xms\qa.ltx' = @'
; the module's own overlay says more of a section its item made
[items_qa_pm_gold]
cost = 777

; and inherits a section of the game's own overlays
[items_qa_pda_child]:pda_show_animator
qa = 1

; the rows its items add sit in a section of their making: the overlay adds its own
[xms_craft]
items_qa_own = itm_repairkit_tier_1, bread, 1, jar
'@
    'patch\qa.ltxp' = (@'
[wpn_ak74]
+ammo_class = ammo_5.56x45_ss190

; a section of the game's own overlay: the base value stays
[da_hud_animations]
-wpn_hand_01 = no_such_member

#file misc\death_generic.ltx
[keep_items]
items_qa_marker = true

#FILE Misc/Trade/Trade_Presets.ltx
[merchant]
items_qa_marker = 1

#file plugins\itms_manager.ltx
[stackable]
items_qa_drink =

#file
[items_qa_system]
k = 1

[items_qa_list]
k = 1, 2.5, x
'@ + "`r`n; a parent of 600 characters names no section: the block is skipped`r`n[items_qa_long_parent] : " + ('a' * 600) +
        "`r`nk = 1`r`n`r`n; a list longer than any fixed buffer`r`n[items_qa_list]`r`n+big = " +
        ((0..699 | ForEach-Object { 'qa{0:d4}' -f $_ }) -join ',') + "`r`n+big = zz`r`n")
}

Write-Host '== A: the configs'
$published = Join-Path $qaAppData 'xms_generated'
$log = Invoke-Engine -Phase 'configs' -Commands @('xms_why death_generic', 'xms_why trade_presets') -WaitFor @('ITEMSQA done')
$s = { param($name) Said $log $name }
Assert-That 'the items are read, new and changes' ($log -match '\* XMS: \[items_qa\] 10 item\(s\) read from the module \(5 new, 5 change\(s\)\)')
Assert-That 'the traders, the loot and the recipes are read from their own files' ($log -match '\* XMS: \[items_qa\] 1 trader\(s\), 1 loot table\(s\) and 1 recipe file\(s\) read from the module \(5 row\(s\)\)')
Assert-That 'a trader file without its trader is left out' ($log -match '! XMS: \[items_qa\] bad\\no_trader\.tradeasset: left out - a trader file names the trader')
Assert-That 'stock and loot written in an item are named and never read' (($log -match "! XMS: \[items_qa\] items\\foreign\.itemasset: 'sold' is not an item's to say") -and
    ($log -match "! XMS: \[items_qa\] items\\foreign\.itemasset: 'loot' is not an item's to say") -and ((& $s 'foreign_set') -eq '1') -and
    ((& $s 'foreign_loot') -eq '<none>') -and ((& $s 'foreign_sold') -eq '<none>'))
Assert-That 'a new item is its section over its base' ((& $s 'pm_section') -eq 'true' -and (& $s 'pm_inherited') -ne '<none>') "fire_modes = $(& $s 'pm_inherited')"
Assert-That 'its own keys' ((& $s 'pm_weight') -eq '0.75')
Assert-That 'the module''s overlay defines it again: merged, the file''s keys win' (((& $s 'pm_cost') -eq '777') -and
    ($log -match '! XMS: \[items_qa\] section \[items_qa_pm_gold\] is defined by an item and by gamedata\\configs\\xms\\qa\.ltx: the file''s keys win'))
Assert-That 'the strings are the game''s ids' ((& $s 'pm_name_id') -eq 'items_qa_pm_gold_name' -and (& $s 'pm_short_id') -eq 'items_qa_pm_gold_name')
Assert-That 'and read in the game''s language' ((& $s 'pm_name') -in @('Золотой ПМ', 'Golden PM')) (& $s 'pm_name')
Assert-That 'a description is escaped and converted' ((& $s 'pm_descr') -eq 'Блестит <очень>') (& $s 'pm_descr')
Assert-That 'the icon in the game''s form' ((& $s 'pm_icon') -eq 'ui\ui_icon_equipment|4,6,2,1') (& $s 'pm_icon')
Assert-That 'its hud section, over the base''s' (((& $s 'pm_hud') -eq 'items_qa_pm_gold_hud') -and ((& $s 'pm_hud_visual') -match 'wpn_pm_hud') -and
    ((& $s 'pm_hud_marker') -eq 'hud of its own'))
Assert-That 'a section it makes' ((& $s 'pm_extra') -eq '1')
Assert-That 'a made section starts from the item''s own section, and from an earlier row' (((& $s 'variant_weight') -eq '0.75') -and
    ((& $s 'variant_of_variant') -eq '1')) "weight: $(& $s 'variant_weight'), of the variant: $(& $s 'variant_of_variant')"
Assert-That 'the module''s overlay adds a recipe of its own to the section the items made' (((& $s 'craft_overlay') -replace ' ', '') -eq 'itm_repairkit_tier_1,bread,1,jar') (& $s 'craft_overlay')
Assert-That 'of two changes of one key the later path wins, weapons before weapons2' ((& $s 'order_marker') -eq 'weapons2') (& $s 'order_marker')
Assert-That 'an icon is its texture and its cells: a variant with cells of its own keeps its texture' (((& $s 'icon_parent') -eq 'ui\ui_actor_portrets') -and
    ((& $s 'icon_variant') -eq '<none>')) "item: $(& $s 'icon_parent'), variant: $(& $s 'icon_variant')"
Assert-That 'a parent of 600 characters names no section' (((& $s 'long_parent') -eq 'false') -and
    ($log -match '! XMS: \[items_qa_long_parent\] cannot inherit missing section \[a{511}\]'))
Assert-That 'a list of 4899 characters takes one more member' ((& $s 'big_list') -eq '4902|zz') (& $s 'big_list')
Assert-That 'an item based on another item of the module waits for it' (((& $s 'child_cost') -eq '1') -and ((& $s 'child_parent_hud') -eq 'items_qa_pm_gold_hud'))
Assert-That 'a template is a section with no strings' ((& $s 'template') -eq '<none>')
Assert-That 'the recipes in [xms_craft]' ((& $s 'craft') -match '\d+=itm_repairkit_tier_1, items_qa_pm_gold, 1, jar, rope' -and
    (& $s 'craft') -match '\d+=itm_repairkit_tier_2, items_qa_pm_gold, 2, jar, rope, jar, rope') (& $s 'craft')
Assert-That 'a change of a stock weapon, its add and the module''s +key together' ((& $s 'ak74_ammo') -match 'ammo_9x18_pmm' -and (& $s 'ak74_ammo') -match 'ammo_5\.56x45_ss190' -and
    (& $s 'ak74_ammo') -match 'ammo_5\.45x39_fmj') (& $s 'ak74_ammo')
Assert-That 'reach the scope variants that inherit the key' (((& $s 'kobra_ammo') -eq (& $s 'ak74_ammo')) -and ((& $s 'ps01_ammo') -eq (& $s 'ak74_ammo'))) "kobra: $(& $s 'kobra_ammo')"
Assert-That 'a key a variant says itself stays its own' (((& $s 'kobra_weight') -eq '3.7') -and ((& $s 'kobra_cost') -eq '4321'))
Assert-That 'a change''s hud reaches the hud sections that inherit it' (((& $s 'ak74_hud_marker') -eq 'changed') -and ((& $s 'kobra_hud_marker') -eq 'changed'))
Assert-That 'a module patch of a game overlay''s section keeps the game''s value' ((& $s 'base_overlay') -eq 'anomaly_weapons\hud_hands_animation\*.omf') (& $s 'base_overlay')
Assert-That 'and a module overlay inherits a game overlay''s section' ((& $s 'base_overlay_child') -ne '<none>') (& $s 'base_overlay_child')
Assert-That 'a list of the system config' ((& $s 'system_list') -eq '1, 2.5, x') (& $s 'system_list')
Assert-That '#file alone goes back to the system config' ((& $s 'system_patch') -eq '1')
Assert-That 'a scene''s hud section in the system config' (((& $s 'drink_hud') -eq 'true') -and ((& $s 'drink_hud_visual') -match 'wpn_vodka_cmuphob_hud') -and
    ((& $s 'drink_hud_show') -match 'vodka_hands_eat'))
Assert-That 'the scene, read by the scene script''s ini, over the one it is like' ((& $s 'scene') -eq 'true' -and (& $s 'scene_snd') -match 'vodka' -and
    (& $s 'scene_tm') -eq '5000' -and (& $s 'scene_anm') -eq 'item_ea_items_qa_drink' -and (& $s 'scene_helm') -eq 'true') "snd $(& $s 'scene_snd'), tm $(& $s 'scene_tm'), anm $(& $s 'scene_anm')"
Assert-That 'loot: [item_count]' ((& $s 'loot_count') -eq '0, 2')
Assert-That 'loot: [base], and a community that inherits it' (((& $s 'loot_base') -eq '0.05') -and ((& $s 'loot_stalker') -eq '0.05')) "stalker: $(& $s 'loot_stalker')"
Assert-That 'loot: a community of its own' ((& $s 'loot_bandit') -eq '0.5')
Assert-That 'a #file patch of a .ltxp' ((& $s 'loot_keep') -eq 'true')
Assert-That 'sold: the trader''s supplies, and the supplies that inherit them' (((& $s 'sold_1') -eq '2, 0.25') -and ((& $s 'sold_2') -eq '2, 0.25'))
Assert-That 'a patch of a file an #include pulls in, and the sections inheriting it' (((& $s 'preset_merchant') -eq '1') -and ((& $s 'preset_inherited') -eq '1') -and
    ((& $s 'preset_other_trader') -eq '1'))
Assert-That 'a count of 0 takes an item off a trader''s shelf' ((& $s 'sold_gone') -eq '<none>') (& $s 'sold_gone')
Assert-That 'a scene''s own keys: how the cycle fits, and a key of the module''s' (((& $s 'scene_fit') -eq 'cut') -and ((& $s 'scene_own') -eq 'true'))
Assert-That 'a bare key in a plugin''s config, from a patch' ((& $s 'stackable') -eq '<bare>') (& $s 'stackable')
Assert-That 'an ini the game writes back is read as written' (((& $s 'writable') -eq '<none>') -and ((& $s 'writable_stock') -ne '<none>'))
$written = Join-Path $qaAppData 'items_qa_written.ltx'
Assert-That 'and what it saves carries no patch' ((Test-Path -LiteralPath $written) -and
    -not ([IO.File]::ReadAllText($written, $cp1251)).Contains('items_qa') -and ([IO.File]::ReadAllText($written, $cp1251)).Contains('wpn_knife'))
Assert-That 'the game''s own files are as they were' (-not (Test-Path -LiteralPath (Join-Path $qaGameData 'configs\misc\death_generic.ltx')))
Assert-That 'the patched configs say so, once each' ([regex]::Matches($log, '\* XMS: misc\\death_generic\.ltx: \d+ patch op\(s\) of modules applied').Count -eq 1)
Assert-That 'what is kept for other configs is counted' ($log -match '\* XMS: \d+ patch op\(s\) of 6 other config file\(s\) kept for when they are read')
Assert-That 'xms_why names the file' ($log -match 'ltx key \[misc\\death_generic\.ltx: keep_items\.items_qa_marker\]: items_qa = true beats the file')
foreach ($pair in @(
        @('no_base', 'a new item starts from a base'),
        @('unknown_base', 'an unknown base \[no_such_section_qa\]'),
        @('exists', 'the section \[wpn_pm\] exists already: write it as a change, with edit'),
        @('both', 'a change names its section with edit alone'),
        @('missing_edit', 'it changes \[no_such_qa\], which is not there'),
        @('code', 'line 1: only plain values'),
        @('long', 'the section \[a+\] is not a-z, 0-9, _, \. and -, under 64 characters'))) {
    Assert-That "refused: $($pair[0])" ($log -match "! XMS: \[items_qa\] bad\\$($pair[0])\.itemasset: left out - $($pair[1])")
}
Assert-That 'refused: made for a newer game' ($log -match '! items_qa\\bad\\newer\.itemasset: made for a newer game \(version 2, this game reads up to 1\)')
Assert-That 'an icon that is a picture is passed by, with its line' ($log -match 'pm_icon\.itemasset: the icon names a picture, which only a built module turns into a texture' -and
    (& $s 'pm_unchanged_icon') -ne '<none>')
$strings = Join-Path $published 'configs\text\rus\items_qa_items.xml'
Assert-That 'the strings are published per language' ((Test-Path -LiteralPath $strings) -and (Test-Path -LiteralPath (Join-Path $published 'configs\text\eng\items_qa_items.xml')))
if (Test-Path -LiteralPath $strings) {
    $text = [IO.File]::ReadAllText($strings, $cp1251)
    Assert-That 'in cp1251, escaped' ($text.Contains('Блестит &lt;очень&gt;') -and $text.Contains('items_qa_drink_name'))
    Assert-That 'an English install gets the English name' ([IO.File]::ReadAllText((Join-Path $published 'configs\text\eng\items_qa_items.xml'), $cp1251).Contains('Golden PM'))
}
$report = Join-Path $qaAppData 'xms_report.json'
Assert-That 'the report carries the file patches' ((Test-Path -LiteralPath $report) -and ([IO.File]::ReadAllText($report)).Contains('misc\\trade\\trade_presets.ltx: merchant.items_qa_marker'))
Assert-That 'nothing else of the module was refused' (-not ([regex]::Matches($log, '! XMS: \[items_qa\][^\r\n]*') | Where-Object { $_.Value -notmatch 'bad\\|defined by an item|pm_icon|foreign\.itemasset' }))

if (-not $SkipGame) {
    Write-Host '== B: a new game: the items are the game''s'
    $log = Invoke-Engine -Phase 'game' -Commands @() -Start 'server(all/single/alife/new) client(localhost)' -WaitFor @('ITEMSQA game done')
    foreach ($sec in @('items_qa_pm_gold', 'items_qa_drink', 'wpn_ak74_kobra')) {
        Assert-That "spawned and held: $sec" ((& { Said $log "held $sec" }) -ne '<not said>') (Said $log "held $sec")
    }
}

if ($Identity) {
    Write-Host '== C: no module - the configs are what they were'
    Get-ChildItem -LiteralPath $qaModules -Directory -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
    $dumpScript = Join-Path $qaGameData 'scripts\items_qa_dump.script'
    [IO.File]::WriteAllText($dumpScript, @'
local function dump(out, ini, title)
	out:write("== ", title, "\n")
	ini:section_for_each(function(s)
		out:write("[", s, "]\n")
		for i = 0, ini:line_count(s) - 1 do
			local _, k, v = ini:r_line(s, i, "", "")
			out:write(k, "=", v, "\n")
		end
	end)
end
local done = false
function on_game_start()
	if done then return end
	done = true
	local out = io.open(getFS():update_path("$app_data_root$", "items_qa_dump.txt"), "wb")
	dump(out, system_ini(), "system.ltx")
	dump(out, game_ini(), "game.ltx")
	for _, f in ipairs({ "misc\\death_generic.ltx", "misc\\trade\\trade_military.ltx", "items\\items\\animations_settings.ltx",
		"plugins\\itms_manager.ltx" }) do
		dump(out, ini_file(f), f)
	end
	out:close()
	printf("ITEMSQA dump written")
end
'@, $utf8)
    $dumps = @{}
    try {
        foreach ($side in @(@('installed', $GameRoot), @('built', $BuildRoot))) {
            Update-Runtime -From $side[1]
            $dump = Join-Path $qaAppData 'items_qa_dump.txt'
            Remove-Item -LiteralPath $dump -Force -ErrorAction SilentlyContinue
            $log = Invoke-Engine -Phase "identity-$($side[0])" -Commands @() -Start 'server(all/single/alife/new) client(localhost)' -WaitFor @('ITEMSQA dump written')
            Assert-That "$($side[0]): no XMS composition line without a module" (-not ($log -match '\* XMS: \d+ module\(s\) enabled|config patches applied|patch op\(s\)'))
            if (Test-Path -LiteralPath $dump) {
                $dumps[$side[0]] = [IO.File]::ReadAllBytes($dump)
                Copy-Item -LiteralPath $dump -Destination (Join-Path $resultRoot "dump-$($side[0]).txt") -Force
            }
        }
    }
    finally {
        Remove-Item -LiteralPath $dumpScript -Force -ErrorAction SilentlyContinue
        Update-Runtime
    }
    $same = $dumps.ContainsKey('installed') -and $dumps.ContainsKey('built') -and
        [Linq.Enumerable]::SequenceEqual([byte[]]$dumps['installed'], [byte[]]$dumps['built'])
    Assert-That 'without a module the configs are byte for byte what the installed engine reads' $same $(if ($dumps.ContainsKey('built')) { "$($dumps['built'].Length) bytes" })
}

if (-not $Keep) { Remove-Root }
Write-Host "== failures: $script:failed"
exit $script:failed
