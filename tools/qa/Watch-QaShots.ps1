# Grabs the QA rig's window each time a probe asks for a frame: a log line "[qa] SHOT <name>"
# becomes <Out>\<name>.png. The engine's own capture draws the scene without the UI, so anything
# about menus, dialogs or the HUD needs the window itself.
#
# Start it next to the probe (Start-Job, or a second shell) BEFORE the engine; it stops on the
# probe's done marker or at the ceiling.
[CmdletBinding()]
param(
    [string]$Rig = "D:\Games\Dead Air\_qa\lightdiag",
    [Parameter(Mandatory)][string]$Out,
    [int]$Seconds = 180,
    [string]$DoneMarker = "DA_WATER_PROBE_DONE",
    [string]$DesktopName = "DeadAirQA"
)

$ErrorActionPreference = "Stop"
$log = Join-Path $Rig "appdata\logs\grizzly_admin.log"
$idFile = Join-Path ([IO.Path]::GetTempPath()) "dar-probe-engine.pid"
$capture = Join-Path $PSScriptRoot "Capture-HiddenDesktopWindow.ps1"
New-Item -ItemType Directory -Force -Path $Out | Out-Null

$taken = @{}
$clock = [Diagnostics.Stopwatch]::StartNew()
# The log of the run before is still there when this starts, done marker and all: only a log the
# new engine has written since counts.
$started = Get-Date
while ($clock.Elapsed.TotalSeconds -lt $Seconds) {
    if ((Test-Path -LiteralPath $log) -and (Get-Item -LiteralPath $log).LastWriteTime -gt $started) {
        $lines = @(Get-Content -LiteralPath $log -ErrorAction SilentlyContinue)
        foreach ($line in $lines) {
            if ($line -match '\[qa\] SHOT (\S+)' -and -not $taken.ContainsKey($Matches[1])) {
                $name = $Matches[1]
                $taken[$name] = $true
                $engine = [int](Get-Content -LiteralPath $idFile -Raw).Trim()
                & $capture -DesktopName $DesktopName -ProcessId $engine -OutputPath (Join-Path $Out "$name.png") | Out-Null
            }
        }
        if ($lines -match [regex]::Escape($DoneMarker)) { break }
    }
    Start-Sleep -Milliseconds 150
}
Write-Output ("{0} frame(s) -> {1}" -f $taken.Count, $Out)
