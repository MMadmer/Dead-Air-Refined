<#
.SYNOPSIS
    Pictures of a module in the real game, from the QA root Run-ModuleInGameQa.ps1 kept (-Keep): a probe module
    of its own puts the player at each view and takes a screenshot. The installed game is not touched; everything
    is under the system temp folder.

.DESCRIPTION
    -Views names a text file of lines `name = x, y, z, yaw degrees, hour`: where the player stands (the feet),
    which way they look (0 is +Z, a positive angle turns towards -X) and the hour of the game's clock. The views
    are taken in the alphabetical order of their names. The pictures land in -Out as shot_<n>.jpg in that order,
    with the game's log beside them (game.log).

    A view that puts the player into a character kills it: stand on free ground.
#>
[CmdletBinding()]
param(
    # the views: "name = x, y, z, yaw degrees, hour" lines
    [Parameter(Mandatory)] [string]$Views,
    [Parameter(Mandatory)] [string]$Out,
    [string]$Repository = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path,
    [int]$TimeoutSeconds = 420
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$work = Join-Path ([IO.Path]::GetTempPath()) 'dar-module-qa'
$root = Join-Path $work 'root'
$engine = Join-Path $root 'xrEngine.exe'
if (-not (Test-Path -LiteralPath $engine)) { throw "No kept QA root at ${root} - run Run-ModuleInGameQa.ps1 -Keep first" }
$helper = Join-Path $Repository 'tools\qa\Start-DetachedHiddenDesktopProcess.ps1'
$log = Join-Path $root 'appdata\logs\grizzly_admin.log'
$shots = Join-Path $root 'appdata\screenshots'
$utf8 = [Text.UTF8Encoding]::new($false)

$probe = Join-Path $root 'modules\zz_shots'
New-Item -ItemType Directory -Path (Join-Path $probe 'scripts') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $probe 'gamedata\configs') -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $probe 'mod.ltx'), "[module]`r`nid = zz_shots`r`nname = zz_shots`r`nversion = 1.0`r`napi = 1`r`ntarget = any`r`nmode = *`r`n", $utf8)
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'module_shots_register.script') -Destination (Join-Path $probe 'scripts\register.script') -Force
[IO.File]::WriteAllText((Join-Path $probe 'gamedata\configs\shots.ltx'), "[shots]`r`n" + ((Get-Content -LiteralPath $Views) -join "`r`n") + "`r`n", $utf8)

if (Test-Path -LiteralPath $shots) { Get-ChildItem -LiteralPath $shots -File | Remove-Item -Force }
Get-Process xrEngine -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $engine } | Stop-Process -Force
for ($try = 0; (Test-Path -LiteralPath $log) -and $try -lt 120; $try++) {
    try { Remove-Item -LiteralPath $log -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 250 }
}
$pidFile = Join-Path $work 'shots-engine.pid'
$launcherFile = Join-Path $work 'shots-launcher.pid'
Remove-Item -LiteralPath $pidFile, $launcherFile -Force -ErrorAction SilentlyContinue
$arguments = '-i -fsltx fsgame.ltx -always_active -silent_error_mode -force_flushlog -r4 -nointro -start "server(all/single/alife/new) client(localhost)"'
$desktop = "DeadAirShots$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$process = $null
try {
    & $helper -FilePath $engine -Arguments $arguments -ProcessIdFile $pidFile -LauncherIdFile $launcherFile -DesktopName $desktop -TimeoutSeconds ($TimeoutSeconds + 60) | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    while (-not (Test-Path -LiteralPath $pidFile -PathType Leaf)) {
        if ([DateTime]::UtcNow -ge $deadline) { throw 'The hidden launcher did not publish the engine PID.' }
        Start-Sleep -Milliseconds 100
    }
    $process = Get-Process -Id ([int](Get-Content -LiteralPath $pidFile -Raw)) -ErrorAction Stop
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline -and -not $process.HasExited) {
        Start-Sleep -Milliseconds 500
        if ((Test-Path -LiteralPath $log) -and (Select-String -LiteralPath $log -Pattern 'SHOTS done' -Quiet -Encoding 1251 -ErrorAction SilentlyContinue)) { break }
    }
    Start-Sleep -Seconds 2
}
finally {
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force; $process.WaitForExit(20000) | Out-Null }
    if (Test-Path -LiteralPath $launcherFile) {
        $launcher = Get-Process -Id ([int](Get-Content -LiteralPath $launcherFile -Raw)) -ErrorAction SilentlyContinue
        if ($launcher) { Stop-Process -Id $launcher.Id -Force -ErrorAction SilentlyContinue }
    }
    # the probe leaves the kept root as it found it
    Remove-Item -LiteralPath $probe -Recurse -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Path $Out -Force | Out-Null
if (Test-Path -LiteralPath $log) { Copy-Item -LiteralPath $log -Destination (Join-Path $Out 'game.log') -Force }
$n = 0
if (Test-Path -LiteralPath $shots) {
    foreach ($f in (Get-ChildItem -LiteralPath $shots -File | Sort-Object LastWriteTime)) {
        $n++
        Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $Out ("shot_{0:00}{1}" -f $n, $f.Extension)) -Force
    }
}
Write-Host "shots: $n -> $Out"
if (Test-Path -LiteralPath $log) { Select-String -LiteralPath $log -Pattern 'SHOTS ' -Encoding 1251 | ForEach-Object { $_.Line.Trim() } }
