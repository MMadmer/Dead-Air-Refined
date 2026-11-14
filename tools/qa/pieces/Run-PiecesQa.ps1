<#
.SYNOPSIS
    The level pieces' acceptance run (docs/dead-air/XMS_ARCHITECTURE.md 8.3, TEST_MATRIX.md).

.DESCRIPTION
    Cuts the visuals inside -Box out of a compiled level of the game into a piece of a test module
    (cut_piece.py), then starts the game on that level from -SaveName in a QA root beside the game
    (_qa\pieces, built like tools\qa\nq's) on a hidden desktop, without the module and with it:

      Off         no module: three frames from the save's camera (with -CompareConsole), a save made
                  without the module
      On          the module: the same frames, the camera to -Away and back (the piece released and
                  loaded again), a save made with the module
      OnLoadsOff  the module, r__pieces_stream 0, loading the save made without it
      OffLoadsOn  no module, loading the save made with it
      R5, R6      the module on DirectX 12 and Vulkan
      Scout       no module, the camera's place only (for picking a box)

    The frames of On are compared with the three of Off (Compare-Frames.py); every launch wants the
    probe's DONE, the piece's log lines where the module is on and none where it is off, no fatal
    and exit code 0. Each launch is limited to -TimeoutSeconds. The player's installation, saves and
    settings are only read; everything is written inside the QA root.

    pwsh -NoProfile -File tools\qa\pieces\Run-PiecesQa.ps1 -Box <minx,miny,minz,maxx,maxy,maxz> -Away <x,y,z>
#>
[CmdletBinding()]
param(
    [string]$GameRoot = 'D:\Games\Dead Air',
    [string]$BuildRoot,
    [string]$SaveName = 'admin - autosave',
    [string]$Level = 'l01_escape',
    [string]$Box,
    [string]$Away,
    # Comma-separated: Scout, Off, On, OnLoadsOff, OffLoadsOn, R5, R6.
    [string]$Phases = 'Off,On,OnLoadsOff,OffLoadsOn',
    # The box may hold vegetation: the comparison draws both frames the stock way (-CompareConsole).
    [switch]$AllowTrees,
    [ValidatePattern('^[A-Za-z0-9_.-]+$')]
    [string]$ResultLabel = (Get-Date -Format 'yyyyMMdd-HHmmss'),
    [int]$TimeoutSeconds = 300,
    # Both frames are drawn the stock way: the clusters and the procedural vegetation take only the level's own
    # visuals (a piece's draw as models), which the comparison is not about.
    [string[]]$CompareConsole = @('r__clusters 0', 'r__trees 0', 'r__shrubs 0', 'r__reeds 0'),
    [string]$VidMode = '1024x768',
    [switch]$RebuildRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$phaseList = @($Phases -split '\s*,\s*' | Where-Object { $_ })
foreach ($phase in $phaseList) {
    if ($phase -notin @('Scout', 'Off', 'On', 'OnLoadsOff', 'OffLoadsOn', 'R5', 'R6')) { throw "Unknown phase: $phase" }
}

$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $BuildRoot) { $BuildRoot = Join-Path $repositoryRoot 'bin\x64\Release' }
$GameRoot = (Resolve-Path -LiteralPath $GameRoot).Path

$qaRoot = Join-Path $GameRoot '_qa\pieces'
$qaAppData = Join-Path $qaRoot 'appdata'
$qaGameData = Join-Path $qaRoot 'gamedata'
$qaSaves = Join-Path $qaAppData 'savedgames'
$qaLogs = Join-Path $qaAppData 'logs'
$qaModules = Join-Path $qaRoot 'modules'
$qaEngine = Join-Path $qaRoot 'xrEngine.exe'
$moduleId = 'pieces_qa'
$moduleStore = Join-Path $qaRoot "module-store\$moduleId"
$resultRoot = Join-Path $qaRoot "results\$ResultLabel"

$compatGameData = Join-Path $repositoryRoot 'packaging\dead-air-x64\compatibility\gamedata'
$manifestPath = Join-Path $repositoryRoot 'packaging\dead-air-x64\installer\runtime-files.txt'
$helper = Join-Path $repositoryRoot 'tools\qa\Start-DetachedHiddenDesktopProcess.ps1'
$capture = Join-Path $repositoryRoot 'tools\qa\Capture-HiddenDesktopWindow.ps1'
$compare = Join-Path $repositoryRoot 'tools\qa\Compare-Frames.py'
$cutter = Join-Path $PSScriptRoot 'cut_piece.py'
$userSavedGames = Join-Path $GameRoot 'appdata\savedgames'
$layersKey = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers'
$fatalPattern = 'FATAL ERROR|stack trace:|Assertion|Expression\s*:|access violation|reader overflow|Lua error|! \[pieces\]'

$saveOff = 'pieces_qa_off'
$saveOn = 'pieces_qa_on'

foreach ($required in @($manifestPath, $helper, $capture, $compare, $cutter, $compatGameData)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing pieces QA dependency: $required" }
}
if (@($phaseList | Where-Object { $_ -ne 'Scout' }).Count -and -not $Box) { throw '-Box is needed to cut the piece.' }
if ($phaseList -contains 'On' -and -not $Away) { throw '-Away is needed for the streaming of the On phase.' }

# ---------------------------------------------------------------------------- the QA root
function Copy-TreeContents {
    param([Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][string]$Destination, [switch]$SkipEmpty)
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $recurse = if ($SkipEmpty) { '/S' } else { '/E' }
    $output = & robocopy $Source $Destination $recurse /NFL /NDL /NJH /NJS /NP /R:2 /W:1
    if ($LASTEXITCODE -ge 8) { throw "robocopy '$Source' -> '$Destination' failed ($LASTEXITCODE): $output" }
    $global:LASTEXITCODE = 0
}

function New-QaRoot {
    Write-Host "== building the QA root: $qaRoot"
    if (Test-Path -LiteralPath $qaRoot) {
        # The database entry is a junction: the link goes first, the real content stays.
        $databaseLink = Join-Path $qaRoot 'database'
        if (Test-Path -LiteralPath $databaseLink) { [IO.Directory]::Delete($databaseLink, $false) }
        Remove-Item -LiteralPath $qaRoot -Recurse -Force
    }
    foreach ($directory in @($qaRoot, $qaAppData, $qaSaves, $qaLogs, $qaModules, (Join-Path $qaAppData 'screenshots'))) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    New-Item -ItemType Junction -Path (Join-Path $qaRoot 'database') -Target (Join-Path $GameRoot 'database') | Out-Null
    Copy-Item -LiteralPath (Join-Path $GameRoot 'fsgame.ltx') -Destination (Join-Path $qaRoot 'fsgame.ltx') -Force
    foreach ($cache in @('shaders_cache', 'shaders_cache_grizzly', 'shaders_cache_oxr', 'cdb_cache', 'render-hardware.cache')) {
        $source = Join-Path $GameRoot "appdata\$cache"
        if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $qaAppData -Recurse -Force }
    }
    Copy-TreeContents -Source (Join-Path $GameRoot 'gamedata') -Destination $qaGameData
    Copy-TreeContents -Source $compatGameData -Destination $qaGameData -SkipEmpty
}

function Update-QaRuntime {
    # The freshly built binaries; only what the build does not make comes from the installed root.
    $manifest = @(Get-Content -LiteralPath $manifestPath | Where-Object { $_.Trim() -ne '' })
    foreach ($file in $manifest) {
        $built = Join-Path $BuildRoot $file
        $installed = Join-Path $GameRoot $file
        $source = if (Test-Path -LiteralPath $built) { $built } elseif (Test-Path -LiteralPath $installed) { $installed } `
            else { throw "Neither the build nor the installed game has the runtime file: $file" }
        Copy-Item -LiteralPath $source -Destination (Join-Path $qaRoot $file) -Force
    }
    Get-ChildItem -LiteralPath $GameRoot -File -Filter '*.dll' | Where-Object { $manifest -notcontains $_.Name } |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $qaRoot $_.Name) -Force }
    # The content service wants the installation's manifest beside the executable, copied, never linked.
    $meta = Join-Path $GameRoot '.dead-air-x64'
    $target = Join-Path $qaRoot '.dead-air-x64'
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    foreach ($file in @('content-manifest.txt', 'content-state.txt')) {
        $source = Join-Path $meta $file
        if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $target $file) -Force }
    }
    Remove-Item -LiteralPath (Join-Path $target 'content-incomplete.txt') -Force -ErrorAction SilentlyContinue

    New-Item -ItemType Directory -Path (Join-Path $qaGameData 'scripts') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'pieces_qa_probe.script') -Destination (Join-Path $qaGameData 'scripts') -Force
    $scriptLtxPath = Join-Path $qaGameData 'configs\script.ltx'
    $lines = @(Get-Content -LiteralPath $scriptLtxPath)
    if (-not @($lines | Where-Object { $_ -match 'pieces_qa_probe' }).Count) {
        $patched = $false
        $lines = @($lines | ForEach-Object {
                if (-not $patched -and $_ -match '^\s*script\s*=') { $patched = $true; "$_, pieces_qa_probe" } else { $_ }
            })
        if (-not $patched) { throw "No 'script =' line in the mirrored script.ltx." }
        Set-Content -LiteralPath $scriptLtxPath -Value $lines -Encoding Default
    }
}

function Set-QaUserLtx {
    param([string[]]$Extra = @())
    $source = Join-Path $GameRoot 'appdata\user.ltx'
    $userLtx = if (Test-Path -LiteralPath $source) { @(Get-Content -LiteralPath $source) } else { @() }
    $overrides = [ordered]@{
        'vid_mode' = $VidMode; 'vid_window_mode' = 'st_opt_windowed'; 'rs_fullscreen' = 'off'; 'rs_v_sync' = 'off'
        'rs_fps_limit' = '60'; 'g_pause_in_background' = '0'; 'g_god' = 'on'; 'snd_volume_music' = '0'
        'snd_volume_eff' = '0'
    }
    $keys = @($overrides.Keys) + @($Extra | ForEach-Object { ($_ -split '\s+')[0] })
    $userLtx = @($userLtx | Where-Object { $line = $_; -not @($keys | Where-Object { $line -match "^\s*$_\s" }).Count })
    foreach ($key in $overrides.Keys) { $userLtx += "$key $($overrides[$key])" }
    $userLtx += $Extra
    Set-Content -LiteralPath (Join-Path $qaAppData 'user.ltx') -Value $userLtx -Encoding Default
}

function Set-Module {
    param([bool]$On)
    $live = Join-Path $qaModules $moduleId
    if (Test-Path -LiteralPath $live) { Remove-Item -LiteralPath $live -Recurse -Force }
    if ($On) { Copy-TreeContents -Source $moduleStore -Destination $live }
}

function Copy-BaseSave {
    $parts = @(Get-ChildItem -LiteralPath $userSavedGames -File | Where-Object { $_.Name -like "$SaveName.*" -and $_.Name -notlike '*.bak' })
    if (-not @($parts | Where-Object { $_.Extension -eq '.scop' }).Count) { throw "No save group '$SaveName' in the player's savedgames." }
    $parts | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $qaSaves -Force }
}

# ---------------------------------------------------------------------------- one launch
$script:failures = [Collections.Generic.List[string]]::new()
$script:summary = [Collections.Generic.List[object]]::new()

function Invoke-PiecesLaunch {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][bool]$ModuleOn,
        [Parameter(Mandatory)][string]$LoadSave,
        [string]$Renderer = '-r4',
        [bool]$Shots = $false,
        [string]$AwayTo = '',
        [string]$WriteSave = '',
        [string[]]$Console = @()
    )
    # An engine of the last launch may still be closing its device after its log ended.
    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    while (@(Get-Process xrEngine -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $qaEngine }).Count) {
        if ([DateTime]::UtcNow -ge $deadline) { throw 'The QA engine is already running.' }
        Start-Sleep -Milliseconds 500
    }
    $phaseResult = Join-Path $resultRoot $Name
    $shotDir = Join-Path $phaseResult 'screens'
    New-Item -ItemType Directory -Path $shotDir -Force | Out-Null
    Set-Module -On $ModuleOn
    Set-QaUserLtx -Extra $Console
    $ini = @('[qa]', "shots = $(if ($Shots) { 'true' } else { 'false' })")
    if ($AwayTo) { $ini += "away = $AwayTo" }
    if ($WriteSave) { $ini += "save = $WriteSave" }
    Set-Content -LiteralPath (Join-Path $qaGameData 'configs\pieces_qa.ltx') -Value $ini -Encoding Default
    Get-ChildItem -LiteralPath $qaLogs -File -ErrorAction SilentlyContinue | Remove-Item -Force

    $pidFile = Join-Path $phaseResult 'engine.pid'
    $launcherFile = Join-Path $phaseResult 'launcher.pid'
    $exitFile = Join-Path $phaseResult 'exit-code.txt'
    Remove-Item -LiteralPath $pidFile, $launcherFile, $exitFile -Force -ErrorAction SilentlyContinue
    $desktop = "DeadAirPieces$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
    $arguments = "-i -fsltx fsgame.ltx -always_active -silent_error_mode -force_flushlog $Renderer " +
        "-start `"server($LoadSave/single/alife/load)`""
    Write-Host ""
    Write-Host "== [$Name] module=$ModuleOn load='$LoadSave' write='$WriteSave' $Renderer $($Console -join '; ')"

    New-Item -Path $layersKey -Force | Out-Null
    New-ItemProperty -Path $layersKey -Name $qaEngine -Value '~ HIGHDPIAWARE' -PropertyType String -Force | Out-Null
    $engineId = 0
    $launcherId = 0
    $started = [DateTime]::UtcNow
    $shotsTaken = 0
    try {
        & $helper -FilePath $qaEngine -Arguments $arguments -ProcessIdFile $pidFile -LauncherIdFile $launcherFile `
            -ExitCodeFile $exitFile -DesktopName $desktop -TimeoutSeconds $TimeoutSeconds | Out-Null
        $launcherId = [int](Get-Content -LiteralPath $launcherFile -Raw)
        $deadline = [DateTime]::UtcNow.AddSeconds(60)
        while (-not (Test-Path -LiteralPath $pidFile -PathType Leaf)) {
            if ([DateTime]::UtcNow -ge $deadline) { throw 'The hidden launcher did not publish the engine PID.' }
            Start-Sleep -Milliseconds 100
        }
        $engineId = [int](Get-Content -LiteralPath $pidFile -Raw)
        $engine = Get-Process -Id $engineId -ErrorAction Stop
        $deadline = $started.AddSeconds($TimeoutSeconds)
        $nextNote = [DateTime]::UtcNow.AddSeconds(120)
        while (-not $engine.HasExited -and [DateTime]::UtcNow -lt $deadline) {
            if ([DateTime]::UtcNow -ge $nextNote) {
                $nextNote = [DateTime]::UtcNow.AddSeconds(120)
                Write-Host ("   ... {0:n0} s" -f ([DateTime]::UtcNow - $started).TotalSeconds)
            }
            $log = Get-ChildItem -LiteralPath $qaLogs -File -Filter '*.log' -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($log) {
                $text = ''
                try {
                    $stream = [IO.File]::Open($log.FullName, 'Open', 'Read', 'ReadWrite')
                    try { $text = [IO.StreamReader]::new($stream).ReadToEnd() } finally { $stream.Dispose() }
                }
                catch { }
                $shotLines = @([regex]::Matches($text, 'PIECESQA: SHOT: (\w+)') | ForEach-Object { $_.Groups[1].Value })
                while ($shotsTaken -lt $shotLines.Count) {
                    $target = Join-Path $shotDir "$($shotLines[$shotsTaken]).png"
                    $shotsTaken++
                    try { & $capture -DesktopName $desktop -ProcessId $engineId -OutputPath $target | Out-Null; Write-Host "   [shot] $target" }
                    catch { Write-Warning "capture failed: $_" }
                }
            }
            Start-Sleep -Milliseconds 250
        }
        if (-not $engine.HasExited) { $script:failures.Add("[$Name] the engine did not exit within $TimeoutSeconds s") }
        $engine.WaitForExit(60000) | Out-Null
        $launcher = Get-Process -Id $launcherId -ErrorAction SilentlyContinue
        if ($launcher) { $launcher.WaitForExit(30000) | Out-Null }
    }
    finally {
        Remove-ItemProperty -Path $layersKey -Name $qaEngine -ErrorAction SilentlyContinue
        foreach ($owned in @($engineId, $launcherId)) {
            if ($owned -le 0) { continue }
            $process = Get-Process -Id $owned -ErrorAction SilentlyContinue
            if ($process -and -not $process.HasExited) {
                Stop-Process -Id $owned -Force -ErrorAction SilentlyContinue
                $process.WaitForExit(15000) | Out-Null
            }
        }
    }
    $seconds = ([DateTime]::UtcNow - $started).TotalSeconds

    $exitCode = [uint32]::MaxValue
    $exitDeadline = [DateTime]::UtcNow.AddSeconds(60)
    while (-not (Test-Path -LiteralPath $exitFile -PathType Leaf) -and [DateTime]::UtcNow -lt $exitDeadline) { Start-Sleep -Milliseconds 200 }
    if (Test-Path -LiteralPath $exitFile -PathType Leaf) {
        $parsed = [int64]0
        if ([int64]::TryParse((Get-Content -LiteralPath $exitFile -Raw).Trim(), [ref]$parsed)) { $exitCode = [uint32]($parsed -band 0xFFFFFFFFL) }
    }
    $log = Get-ChildItem -LiteralPath $qaLogs -File -Filter '*.log' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $log) { $script:failures.Add("[$Name] no engine log"); return @() }
    $logCopy = Join-Path $phaseResult 'engine.log'
    Copy-Item -LiteralPath $log.FullName -Destination $logCopy -Force
    $lines = @(Get-Content -LiteralPath $logCopy -Encoding 1251)

    $problems = [Collections.Generic.List[string]]::new()
    if (-not @($lines | Where-Object { $_ -match 'PIECESQA: DONE' }).Count) { $problems.Add('the probe never reached DONE') }
    foreach ($line in @($lines | Where-Object { $_ -match $fatalPattern })) { $problems.Add("log: $($line.Trim())") }
    if ($exitCode -ne 0) { $problems.Add("exit code $exitCode") }
    foreach ($dump in @(Get-ChildItem -LiteralPath $qaLogs -File -Filter '*.mdmp' -ErrorAction SilentlyContinue)) { $problems.Add("crash dump $($dump.Name)") }
    $pieceLines = @($lines | Where-Object { $_ -match '\[pieces\]|PIECESQA: (camera|away|back|saved)|visual overlays' })
    foreach ($line in $pieceLines) { Write-Host "   $($line.Trim())" }
    if ($ModuleOn -and -not @($pieceLines | Where-Object { $_ -match '\[pieces\] .*: attached' }).Count) { $problems.Add('the piece never attached') }
    if (-not $ModuleOn -and @($pieceLines | Where-Object { $_ -match '\[pieces\]' }).Count) { $problems.Add('piece lines without the module') }
    foreach ($problem in $problems) { $script:failures.Add("[$Name] $problem"); Write-Host "   problem: $problem" }
    $script:summary.Add([pscustomobject]@{ Phase = $Name; Result = $(if ($problems.Count) { 'FAIL' } else { 'PASS' }); Seconds = [int]$seconds; Log = $logCopy })
    Write-Host ("   -> $Name {0} in {1:n0} s" -f $(if ($problems.Count) { 'FAIL' } else { 'PASS' }), $seconds)
    return $lines
}

# ---------------------------------------------------------------------------- the run
if ($RebuildRoot -or -not (Test-Path -LiteralPath (Join-Path $qaRoot 'fsgame.ltx'))) { New-QaRoot }
Update-QaRuntime
New-Item -ItemType Directory -Path $resultRoot -Force | Out-Null

if ($Box) {
    if (Test-Path -LiteralPath $moduleStore) { Remove-Item -LiteralPath $moduleStore -Recurse -Force }
    $cutArgs = @('--game', $GameRoot, '--level', $Level, "--box=$Box", '--module', $moduleStore, '--id', $moduleId)
    if ($AllowTrees) { $cutArgs += '--allow-trees' }
    & python $cutter @cutArgs
    if ($LASTEXITCODE -ne 0) { throw "cut_piece.py failed ($LASTEXITCODE)" }
}

$roots = 0
foreach ($phase in $phaseList) {
    switch ($phase) {
        'Scout' { Copy-BaseSave; [void](Invoke-PiecesLaunch -Name Scout -ModuleOn $false -LoadSave $SaveName) }
        'Off' {
            Copy-BaseSave
            [void](Invoke-PiecesLaunch -Name Off -ModuleOn $false -LoadSave $SaveName -Shots $true -WriteSave $saveOff `
                -Console $CompareConsole)
        }
        'On' {
            Copy-BaseSave
            $lines = Invoke-PiecesLaunch -Name On -ModuleOn $true -LoadSave $SaveName -Shots $true -AwayTo $Away `
                -WriteSave $saveOn -Console $CompareConsole
            $attached = @($lines | Where-Object { $_ -match '\[pieces\] .*: attached' })
            $released = @($lines | Where-Object { $_ -match '\[pieces\] .*: released' })
            $hidden = @($lines | Where-Object { $_ -match 'visual overlays on .*: 0 attached, (\d+) base' } | ForEach-Object { [int]$Matches[1] })
            $rootLine = @($attached | Select-Object -First 1)
            if ($rootLine -and $rootLine[0] -match '(\d+) root\(s\)') { $roots = [int]$Matches[1] }
            if ($attached.Count -lt 2 -or $released.Count -lt 1) {
                $script:failures.Add("[On] streaming: $($attached.Count) attach(es), $($released.Count) release(s); want 2 and 1")
            }
            if (-not $hidden.Count -or $hidden[0] -ne $roots) {
                $script:failures.Add("[On] the cut box hid $(if ($hidden.Count) { $hidden[0] } else { 0 }) base visual(s), the piece has $roots root(s)")
            }
        }
        'OnLoadsOff' {
            [void](Invoke-PiecesLaunch -Name OnLoadsOff -ModuleOn $true -LoadSave $saveOff -Console @('r__pieces_stream 0'))
        }
        'OffLoadsOn' { [void](Invoke-PiecesLaunch -Name OffLoadsOn -ModuleOn $false -LoadSave $saveOn) }
        'R5' { Copy-BaseSave; [void](Invoke-PiecesLaunch -Name R5 -ModuleOn $true -LoadSave $SaveName -Renderer '-r5') }
        'R6' { Copy-BaseSave; [void](Invoke-PiecesLaunch -Name R6 -ModuleOn $true -LoadSave $SaveName -Renderer '-r6') }
    }
}

# The frames with the piece against the three without it.
$off = Join-Path $resultRoot 'Off\screens'
$on = Join-Path $resultRoot 'On\screens'
if ((Test-Path -LiteralPath (Join-Path $off 'c.png')) -and (Test-Path -LiteralPath (Join-Path $on 'b.png'))) {
    Write-Host ""
    # Three frames of one launch calibrate the run-to-run motion (wind in the leaves, NPCs) leave-one-out.
    Write-Host "== frames: On a, then On b, against Off a, b, c"
    foreach ($shot in @('a', 'b')) {
        & python $compare (Join-Path $off 'a.png') (Join-Path $off 'b.png') (Join-Path $on "$shot.png") `
            --ref (Join-Path $off 'c.png') --heatmap (Join-Path $resultRoot "on-$shot-vs-off.png")
        if ($LASTEXITCODE -ne 0) { $script:failures.Add("frames ($shot): Compare-Frames.py exit code $LASTEXITCODE") }
    }
    $global:LASTEXITCODE = 0
}
elseif ($phaseList -contains 'On' -and $phaseList -contains 'Off') { $script:failures.Add('frames: a shot is missing') }

Write-Host ""
$script:summary | Format-Table -AutoSize | Out-String | Write-Host
if ($script:failures.Count) {
    Write-Host "FAILURES:"
    foreach ($failure in $script:failures) { Write-Host "  $failure" }
    exit 1
}
Write-Host "PASS ($resultRoot)"
exit 0
