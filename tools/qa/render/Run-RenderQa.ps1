<#
.SYNOPSIS
    Runs the render QA sweep described in PROJECT_RULES.md section 8.1.

.DESCRIPTION
    Builds an isolated runtime next to the installed game and measures there, so the
    player's game root, appdata, gamedata, MODS, JSGME state and saves are never modified.

    The runtime is not a random binary: every executable and library is copied from the
    installed game root and verified against bin\x64\Release, so the measurement runs on
    exactly the deployed candidate. Content lives behind a junction to the real database.

    The loose gamedata layer has to be a real copy rather than a redirect: archive entries
    are registered under <root>\gamedata\, so pointing $game_data$ elsewhere would hide
    system.ltx and every other packed file.

    Input, cursor and actor position are never automated. The QA script drives the yaw
    through ordinary runtime calls and quits when the sweep is over.
#>
[CmdletBinding()]
param(
    [string]$GameRoot = "D:\Games\Dead Air",
    [string]$BuildRoot = "D:\Games\Dead Air\DeadAir-x64\bin\x64\Release",
    [string]$SaveName = "optimization_test",
    [string]$Label = "render",
    [ValidateSet("fullscreen", "borderless", "windowed")]
    [string]$WindowMode = "fullscreen",
    [string]$VidMode = "2560x1440",
    [ValidateSet("on", "off")]
    [string]$VSync = "off",
    [int]$FpsLimit = 0,
    [int]$TimeoutSeconds = 180,
    # Extra console-variable lines appended to the generated user.ltx (diagnostics only).
    [string[]]$ExtraConsole = @(),
    # Extra engine command-line switches (diagnostics only), e.g. "-no_occq".
    [string[]]$ExtraArgs = @(),
    [switch]$KeepRuntime
)

$ErrorActionPreference = "Stop"

$root = (Resolve-Path -LiteralPath $GameRoot).Path
$manifestPath = Join-Path $PSScriptRoot "..\..\..\packaging\dead-air-x64\installer\runtime-files.txt"
$manifestPath = (Resolve-Path -LiteralPath $manifestPath).Path

$running = Get-Process -Name "xrEngine" -ErrorAction SilentlyContinue
if ($running) {
    throw "Close the game first: xrEngine is running (pid $($running.Id -join ', '))."
}

foreach ($extension in @("scop", "scoc")) {
    $savePart = Join-Path $root "appdata\savedgames\$SaveName.$extension"
    if (-not (Test-Path -LiteralPath $savePart)) {
        throw "Save part is missing: $savePart"
    }
}

$qaRoot = Join-Path $root "_qa\$Label"
$qaAppData = Join-Path $qaRoot "appdata"
$qaGameData = Join-Path $qaRoot "gamedata"
$qaSaves = Join-Path $qaAppData "savedgames"
$qaLogs = Join-Path $qaAppData "logs"
$qaEngine = Join-Path $qaRoot "xrEngine.exe"

if (Test-Path -LiteralPath $qaRoot) {
    # The database entry is a junction; remove it before the tree so the real content stays.
    $databaseLink = Join-Path $qaRoot "database"
    if (Test-Path -LiteralPath $databaseLink) {
        [IO.Directory]::Delete($databaseLink, $false)
    }
    Remove-Item -LiteralPath $qaRoot -Recurse -Force
}
$qaShots = Join-Path $qaAppData "screenshots"
foreach ($directory in @($qaRoot, $qaAppData, $qaGameData, $qaSaves, $qaLogs, $qaShots)) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}

# Runtime: copied from the installed root, verified against the build output.
$manifest = Get-Content -LiteralPath $manifestPath | Where-Object { $_.Trim() -ne "" }
foreach ($file in $manifest) {
    $installed = Join-Path $root $file
    $built = Join-Path $BuildRoot $file
    if (-not (Test-Path -LiteralPath $installed)) {
        throw "The installed game is missing a runtime file: $file"
    }
    if (Test-Path -LiteralPath $built) {
        $installedHash = (Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash
        $builtHash = (Get-FileHash -LiteralPath $built -Algorithm SHA256).Hash
        if ($installedHash -ne $builtHash) {
            throw "The installed $file does not match bin\x64\Release; deploy the candidate first."
        }
    }
    Copy-Item -LiteralPath $installed -Destination (Join-Path $qaRoot $file) -Force
}

# Content: read-only, shared with the real installation.
New-Item -ItemType Junction -Path (Join-Path $qaRoot "database") -Target (Join-Path $root "database") | Out-Null

# Loose gamedata: a copy, because the QA script and its registration go on top of it.
Get-ChildItem -LiteralPath (Join-Path $root "gamedata") |
    Copy-Item -Destination $qaGameData -Recurse -Force
New-Item -ItemType Directory -Path (Join-Path $qaGameData "scripts") -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "qa_render_profile.script") `
    -Destination (Join-Path $qaGameData "scripts") -Force

$scriptLtxPath = Join-Path $qaGameData "configs\script.ltx"
if (-not (Test-Path -LiteralPath $scriptLtxPath)) {
    throw "The mirrored gamedata has no configs\script.ltx; the QA script cannot be registered."
}
$scriptLtx = Get-Content -LiteralPath $scriptLtxPath
$patched = $false
$scriptLtx = $scriptLtx | ForEach-Object {
    if (-not $patched -and $_ -match '^\s*script\s*=') {
        $patched = $true
        "$_, qa_render_profile"
    }
    else {
        $_
    }
}
if (-not $patched) {
    throw "No 'script =' line was found in the mirrored script.ltx."
}
Set-Content -LiteralPath $scriptLtxPath -Value $scriptLtx -Encoding Default

Copy-Item -LiteralPath (Join-Path $root "fsgame.ltx") -Destination (Join-Path $qaRoot "fsgame.ltx") -Force

# The whole save group travels together and unchanged, including the companion chunks.
Get-ChildItem -LiteralPath (Join-Path $root "appdata\savedgames") -File |
    Where-Object { $_.Name -like "$SaveName.*" } |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $qaSaves -Force }

# Seed the derived caches, otherwise every run recompiles shaders and rebuilds the collision
# cache while being measured: frame rate then climbs throughout the window and the numbers are
# systematically pessimistic. Copies, so the player's caches are never written to.
foreach ($cache in @("shaders_cache", "shaders_cache_oxr", "cdb_cache", "render-hardware.cache")) {
    $source = Join-Path $root "appdata\$cache"
    if (Test-Path -LiteralPath $source) {
        Copy-Item -LiteralPath $source -Destination $qaAppData -Recurse -Force
    }
}

$windowModeToken = switch ($WindowMode) {
    "fullscreen" { "st_opt_fullscreen" }
    "borderless" { "st_opt_fullscreen_borderless" }
    "windowed"   { "st_opt_windowed" }
}

# Start from the player's settings so the measurement matches their configuration, then
# override exactly the knobs the procedure fixes.
$userLtxSource = Join-Path $root "appdata\user.ltx"
$userLtx = if (Test-Path -LiteralPath $userLtxSource) { Get-Content -LiteralPath $userLtxSource } else { @() }
$overrides = [ordered]@{
    "renderer"              = "renderer_r4"
    "vid_mode"              = $VidMode
    "vid_window_mode"       = $windowModeToken
    "rs_fullscreen"         = $(if ($WindowMode -eq "fullscreen") { "on" } else { "off" })
    "rs_v_sync"             = $VSync
    "rs_fps_limit"          = "$FpsLimit"
    "g_pause_in_background" = "0"
}
$userLtx = $userLtx | Where-Object {
    $line = $_
    -not ($overrides.Keys | Where-Object { $line -match "^\s*$_\s" })
}
foreach ($key in $overrides.Keys) {
    $userLtx += "$key $($overrides[$key])"
}
foreach ($line in $ExtraConsole) {
    $userLtx += $line
}
Set-Content -LiteralPath (Join-Path $qaAppData "user.ltx") -Value $userLtx -Encoding Default

# DPI: the installed executable carries HIGHDPIAWARE, and this copy lives elsewhere, so it
# needs the same layer or the requested mode collapses to a logical resolution.
$layersKey = "HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers"
New-Item -Path $layersKey -Force | Out-Null
New-ItemProperty -Path $layersKey -Name $qaEngine -Value "~ HIGHDPIAWARE" -PropertyType String -Force | Out-Null

$arguments = @(
    "-fsltx", "fsgame.ltx",
    "-always_active", "-silent_error_mode", "-force_flushlog", "-r4"
) + $ExtraArgs + @(
    "-start", "server($SaveName/single/alife/load)"
)

Write-Host "Runtime: $qaRoot"
Write-Host "Launching: xrEngine.exe $($arguments -join ' ')"

try {
    $process = Start-Process -FilePath $qaEngine -WorkingDirectory $qaRoot -ArgumentList $arguments -PassThru
    Write-Host "pid: $($process.Id)"

    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        Write-Warning "The engine did not exit within $TimeoutSeconds s; terminating this run's process only."
        Stop-Process -Id $process.Id -Force
        $process.WaitForExit(15000) | Out-Null
    }
    Write-Host "exit code: $($process.ExitCode)"
}
finally {
    Remove-ItemProperty -Path $layersKey -Name $qaEngine -ErrorAction SilentlyContinue
}

$log = Get-ChildItem -LiteralPath $qaLogs -Filter "*.log" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if ($log) {
    Write-Host "log: $($log.FullName)"
    Select-String -LiteralPath $log.FullName -Pattern "\[RENDER_QA\]" |
        ForEach-Object { $_.Line }
}
else {
    Write-Warning "No log file was produced in $qaLogs."
}

if (-not $KeepRuntime) {
    Write-Host "Runtime kept for inspection; remove $qaRoot when done."
}
