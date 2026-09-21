# Boots a QA clone of the game with bottom-up ASLR switched off for the engine process, the
# condition under which the archive views used to take the address space below 2 GB away from
# LuaJIT, and waits until a level is loaded. A clone only: the run owns the clone's log.
#
# -ExpectStarved turns the verdict around for binaries without the low-address arena (any build up
# to 1.4.2): they have to die starved, which shows that the condition is real.
# -KeepAslr leaves the process alone, for the same run under ordinary conditions.
# -WaitFor replaces the "level is up" marker, e.g. with the last line of qa_luajit_probe.script.
# -SoakSeconds keeps the level running after the marker; the engine has to stay up all the way.
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Rig,

    [string]$Start = "server(all/single/alife/new)",

    [string]$WaitFor = '\* Loading continuation:',

    [switch]$ExpectStarved,

    [switch]$KeepAslr,

    # For a probe that ends with "quit": how long the engine may take to leave on its own once the
    # marker is in the log. The save such a probe wrote is committed on the way out.
    [ValidateRange(0, 600)]
    [int]$ExitGraceSeconds = 0,

    [ValidateRange(0, 3600)]
    [int]$SoakSeconds = 0,

    # The first boot of a clone caches HUD animations for minutes before the level is up.
    [ValidateRange(30, 3600)]
    [int]$TimeoutSeconds = 1500,

    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$rigPath = (Resolve-Path $Rig).Path
$engine = Join-Path $rigPath "xrEngine.exe"
if (-not (Test-Path -LiteralPath $engine)) {
    throw "No xrEngine.exe in $rigPath"
}
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repoRoot "build\qa\luajit-lowmem"
}
$outputPath = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null

function Get-RigEngine {
    Get-Process xrEngine -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $engine }
}

$running = Get-RigEngine
if ($running) {
    throw "An engine of this clone is already running (pid $($running.Id -join ', '))."
}

$engineArguments = '-nointro -silent_error_mode -force_flushlog -always_active -start "{0}" client(localhost)' -f $Start
if ($KeepAslr) {
    $filePath = $engine
    $arguments = $engineArguments
} else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $installationPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installationPath) {
        throw "Visual Studio C++ tools were not found"
    }

    $developerShell = Join-Path $installationPath "Common7\Tools\VsDevCmd.bat"
    $launcherSource = Join-Path $PSScriptRoot "launch_bottom_up_off.c"
    $launcherObject = Join-Path $outputPath "launch_bottom_up_off.obj"
    $filePath = Join-Path $outputPath "launch_bottom_up_off.exe"
    $compile = 'call "{0}" -arch=amd64 -host_arch=amd64 >nul && ' +
        'cl.exe /nologo /O2 /MD /W4 /WX /TC /Fo"{1}" /Fe"{2}" "{3}"'
    $compile = $compile -f $developerShell, $launcherObject, $filePath, $launcherSource
    & cmd.exe /d /s /c $compile | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "launcher compilation failed with exit code $LASTEXITCODE"
    }
    $arguments = '"{0}" {1}' -f $engine, $engineArguments
}

$logDirectory = Join-Path $rigPath "appdata\logs"
# Whatever the engine calls its log - the name follows the application, and the baseline run
# here is a build from before it was renamed.
Get-ChildItem -LiteralPath $logDirectory -Filter "*.log" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -notlike "*_lua.log" } | Remove-Item -Force

$launcherIdFile = Join-Path $outputPath "engine-boot-launcher.pid"
$startedIdFile = Join-Path $outputPath "engine-boot-started.pid"
Remove-Item -LiteralPath $launcherIdFile, $startedIdFile -Force -ErrorAction SilentlyContinue

& (Join-Path $repoRoot "tools\qa\Start-DetachedHiddenDesktopProcess.ps1") `
    -FilePath $filePath -Arguments $arguments -ProcessIdFile $startedIdFile `
    -LauncherIdFile $launcherIdFile -DesktopName "DeadAirLowMemQA" -TimeoutSeconds $TimeoutSeconds | Out-Null

# Both are the same starvation: either the state itself or its first allocations get nothing.
$starved = 'not enough memory|Cannot initialize script virtual machine|Cannot create the Lua virtual machine'
$verdict = "timeout"
$log = $null
$started = $null
$clock = [Diagnostics.Stopwatch]::StartNew()
while ($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
    Start-Sleep -Milliseconds 500
    if (-not $started -and (Test-Path -LiteralPath $startedIdFile)) {
        $raw = Get-Content -LiteralPath $startedIdFile -Raw -ErrorAction SilentlyContinue
        if ($raw -and $raw.Trim()) {
            $started = Get-Process -Id ([int]$raw.Trim()) -ErrorAction SilentlyContinue
        }
    }
    if (-not $log) {
        $log = Get-ChildItem -LiteralPath $logDirectory -Filter "*.log" -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -notlike "*_lua.log" } |
            Select-Object -First 1 -ExpandProperty FullName
    }
    if ($log) {
        if (Select-String -LiteralPath $log -Pattern $starved -Quiet -ErrorAction SilentlyContinue) {
            $verdict = "starved"
            break
        }
        if (Select-String -LiteralPath $log -Pattern $WaitFor -Quiet -ErrorAction SilentlyContinue) {
            $verdict = "reached"
            break
        }
    }
    if ($started -and $started.HasExited) {
        # The last lines reach the file when the process is already gone.
        $verdict = if ($log -and (Select-String -LiteralPath $log -Pattern $starved -Quiet)) { "starved" }
            elseif ($log -and (Select-String -LiteralPath $log -Pattern $WaitFor -Quiet)) { "reached" }
            else { "exited" }
        break
    }
}

if ($verdict -eq "reached" -and $SoakSeconds -gt 0) {
    $soak = [Diagnostics.Stopwatch]::StartNew()
    while ($soak.Elapsed.TotalSeconds -lt $SoakSeconds) {
        Start-Sleep -Milliseconds 1000
        if ($started.HasExited) {
            $verdict = "died in the soak"
            break
        }
        if (Select-String -LiteralPath $log -Pattern $starved -Quiet -ErrorAction SilentlyContinue) {
            $verdict = "starved"
            break
        }
    }
}

# Under the launcher the engine sits in its job object, so stopping the launcher takes it along.
if ($started -and -not $started.HasExited -and $verdict -eq "reached" -and $ExitGraceSeconds -gt 0) {
    $started.WaitForExit($ExitGraceSeconds * 1000) | Out-Null
}
if ($started -and -not $started.HasExited) {
    Stop-Process -Id $started.Id -Force -Confirm:$false
    $started.WaitForExit(15000) | Out-Null
}
# A crashing engine is still writing its report for a few seconds after the verdict is in.
$left = $null
$rundown = [Diagnostics.Stopwatch]::StartNew()
do {
    $left = Get-RigEngine
    if ($left) { Start-Sleep -Milliseconds 500 }
} while ($left -and $rundown.Elapsed.TotalSeconds -lt 60)
if ($left) {
    throw "The engine survived the run (pid $($left.Id -join ', '))."
}

$mode = if ($KeepAslr) { "default ASLR" } else { "bottom-up ASLR off" }
Write-Host ("verdict: {0} after {1:n0} s ({2}, {3})" -f $verdict, $clock.Elapsed.TotalSeconds, $mode, $Start)
if ($log) {
    Select-String -LiteralPath $log -Pattern 'LuaJIT low memory' | Select-Object -First 3 |
        ForEach-Object { Write-Host ("  " + $_.Line.Trim()) }
    $failures = (Select-String -LiteralPath $log -Pattern $starved | Measure-Object).Count
    Write-Host ("  starvation lines: {0}" -f $failures)
    $label = "{0}-{1}" -f $verdict, [DateTime]::Now.ToString("HHmmss")
    Copy-Item -LiteralPath $log -Destination (Join-Path $outputPath ("engine-boot-{0}.log" -f $label)) -Force
}

$expected = if ($ExpectStarved) { "starved" } else { "reached" }
if ($verdict -ne $expected) {
    throw "Expected the run to end as '$expected', got '$verdict'."
}
