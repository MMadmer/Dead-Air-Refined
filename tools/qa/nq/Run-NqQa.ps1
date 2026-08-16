<#
.SYNOPSIS
    Acceptance test of the NQ quest runtime inside the real game (PROJECT_RULES.md section 8).

.DESCRIPTION
    Builds an isolated QA root next to the installed game (<GameRoot>\_qa\nq) and runs the engine
    there on a hidden desktop, so the player's root, gamedata, database, appdata, MODS, JSGME state
    and saves are never touched. The QA module with the reference quests lives in the QA root, never
    in the installed game.

    Every launch runs one probe phase (tools\qa\nq\nq_qa_probe.script, copied into the mirrored
    gamedata and registered on the first "script =" line of the mirrored configs\script.ltx). The
    probe prints one "NQQA: <step> PASS|FAIL <detail>" line per check and finishes with
    "NQQA: DONE <passed>/<total>". This script prints every one of those lines, copies the engine
    log next to the results and fails the scenario on any FAIL, on a missing DONE line, or on any
    engine assert / crash signature in the log.

    Scenario Dialog additionally drives the real talk window: the probe prints "NQQA-AWAIT: <tag>",
    this script captures the hidden-desktop window as evidence and clicks the reply.

.EXAMPLE
    pwsh -NoProfile -File tools\qa\nq\Run-NqQa.ps1 -SaveName "admin - autosave" -ResultLabel run1
#>
[CmdletBinding()]
param(
    [string]$GameRoot = 'D:\Games\Dead Air',

    [string]$BuildRoot,

    # Save group copied from the player's savedgames; every derived save is created inside the QA root.
    [string]$SaveName = 'admin - autosave',

    [ValidatePattern('^[A-Za-z0-9_.-]+$')]
    [string]$ResultLabel = (Get-Date -Format 'yyyyMMdd-HHmmss'),

    [ValidateSet('All', 'Scout', 'Main', 'Reload', 'Dialog', 'LevelChange', 'ModuleRemoved',
        'ChangedGraph', 'EmptyModule', 'Control')]
    [string]$Scenario = 'All',

    [int]$TimeoutSeconds = 900,

    [switch]$KeepRoot,

    # Rebuild the QA root from scratch even when it already exists.
    [switch]$RebuildRoot,

    [string]$VidMode = '1024x768',

    # Talk-window reply list, measured on the captured screenshots at $VidMode. The capture is a
    # client-sized bitmap that PrintWindow fills starting at the window origin, so these are
    # "screenshot pixels" and the run converts them to screen and client coordinates itself.
    [int]$ReplyX = 300,
    [int]$FirstReplyY = 599,
    [int]$ReplyRowHeight = 20,

    # Row of the quest topic inside the NPC's own topic list (the vanilla entries come first).
    [int]$TopicRow = 3,

    # A click that produced nothing (the engine may still have been loading the window) is repeated.
    [int]$ClickAttempts = 4,
    [int]$ClickRetrySeconds = 12,

    # Diagnostics: capture the hidden-desktop window every N seconds of every phase (0 = off).
    [int]$CaptureEverySeconds = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------- layout
$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $BuildRoot) { $BuildRoot = Join-Path $repositoryRoot 'bin\x64\Release' }
$GameRoot = (Resolve-Path -LiteralPath $GameRoot).Path

$qaRoot = Join-Path $GameRoot '_qa\nq'
$qaAppData = Join-Path $qaRoot 'appdata'
$qaGameData = Join-Path $qaRoot 'gamedata'
$qaConfigs = Join-Path $qaGameData 'configs'
$qaSaves = Join-Path $qaAppData 'savedgames'
$qaLogs = Join-Path $qaAppData 'logs'
$qaModules = Join-Path $qaRoot 'modules'
$qaModule = Join-Path $qaModules 'nq_qa'
$qaEngine = Join-Path $qaRoot 'xrEngine.exe'

$compatGameData = Join-Path $repositoryRoot 'packaging\dead-air-x64\compatibility\gamedata'
$manifestPath = Join-Path $repositoryRoot 'packaging\dead-air-x64\installer\runtime-files.txt'
$helper = Join-Path $repositoryRoot 'tools\qa\Start-DetachedHiddenDesktopProcess.ps1'
$capture = Join-Path $repositoryRoot 'tools\qa\Capture-HiddenDesktopWindow.ps1'
$clicker = Join-Path $repositoryRoot 'tools\qa\Send-HiddenDesktopMouseClick.ps1'
$windowInfo = Join-Path $repositoryRoot 'tools\qa\Get-HiddenDesktopWindowInfo.ps1'
$focuser = Join-Path $repositoryRoot 'tools\qa\Focus-HiddenDesktopWindow.ps1'
$keySender = Join-Path $repositoryRoot 'tools\qa\Send-HiddenDesktopKey.ps1'

$resultRoot = Join-Path $PSScriptRoot "results\$ResultLabel"
$userSavedGames = Join-Path $GameRoot 'appdata\savedgames'
$layersKey = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers'

foreach ($required in @($manifestPath, $helper, $capture, $clicker, $windowInfo, $focuser, $keySender, $compatGameData)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing NQ QA dependency: $required" }
}

# ---------------------------------------------------------------------------- helpers
function Get-DirectoryManifest {
    param([Parameter(Mandatory)][string]$Root)
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) { return @() }
    return @(Get-ChildItem -LiteralPath $Root -File -Recurse | Sort-Object FullName | ForEach-Object {
            [pscustomobject]@{
                Path   = [IO.Path]::GetRelativePath($Root, $_.FullName)
                Length = $_.Length
                SHA256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
            }
        })
}

function Assert-NoTargetEngine {
    $running = @(Get-Process xrEngine -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $qaEngine })
    if ($running.Count) {
        throw "The QA engine is already running (pid $($running.Id -join ', ')); stop it before starting."
    }
}

function Copy-TreeContents {
    # Merge copy: the compatibility layer is overlaid on top of an existing gamedata mirror, which
    # Copy-Item -Recurse cannot do without nesting the source directory inside the destination.
    # SkipEmpty (/S instead of /E) leaves empty directories behind: git cannot track one and the
    # deploy carries files, so a directory that is empty in a working tree reaches nobody's install.
    # Mirroring it anyway is not cosmetic - the engine answers a missing directory and an empty one
    # differently, which is how an empty configs\nq\kinds once hid a crash from this whole suite.
    param([Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][string]$Destination,
        [switch]$SkipEmpty)
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $recurse = if ($SkipEmpty) { '/S' } else { '/E' }
    $output = & robocopy $Source $Destination $recurse /NFL /NDL /NJH /NJS /NP /R:2 /W:1
    if ($LASTEXITCODE -ge 8) { throw "robocopy '$Source' -> '$Destination' failed ($LASTEXITCODE): $output" }
    $global:LASTEXITCODE = 0
}

# ---------------------------------------------------------------------------- QA root
function New-QaRoot {
    Write-Host "== building the QA root: $qaRoot"

    if (Test-Path -LiteralPath $qaRoot) {
        # The database entry is a junction; drop the link before the tree so the real content stays.
        $databaseLink = Join-Path $qaRoot 'database'
        if (Test-Path -LiteralPath $databaseLink) { [IO.Directory]::Delete($databaseLink, $false) }
        Remove-Item -LiteralPath $qaRoot -Recurse -Force
    }
    foreach ($directory in @($qaRoot, $qaAppData, $qaGameData, $qaSaves, $qaLogs, $qaModules,
            (Join-Path $qaAppData 'screenshots'))) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }

    Update-QaRuntime
    New-Item -ItemType Junction -Path (Join-Path $qaRoot 'database') -Target (Join-Path $GameRoot 'database') | Out-Null
    Copy-Item -LiteralPath (Join-Path $GameRoot 'fsgame.ltx') -Destination (Join-Path $qaRoot 'fsgame.ltx') -Force

    # Seed the derived caches so a run is not spent recompiling shaders or rebuilding collision data.
    foreach ($cache in @('shaders_cache', 'shaders_cache_oxr', 'cdb_cache', 'render-hardware.cache')) {
        $source = Join-Path $GameRoot "appdata\$cache"
        if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $qaAppData -Recurse -Force }
    }
    Update-QaGameData
    Update-QaUserLtx
}

function Update-QaRuntime {
    # The NQ engine changes are not deployed to the player's installation, so the acceptance test
    # runs the freshly built binaries; only libraries the build does not produce come from the
    # installed root (which stays read-only).
    $manifest = @(Get-Content -LiteralPath $manifestPath | Where-Object { $_.Trim() -ne '' })
    $fromBuild = 0
    foreach ($file in $manifest) {
        $built = Join-Path $BuildRoot $file
        $installed = Join-Path $GameRoot $file
        if (Test-Path -LiteralPath $built) { $source = $built; $fromBuild++ }
        elseif (Test-Path -LiteralPath $installed) { $source = $installed }
        else { throw "Neither bin\x64\Release nor the installed game has the runtime file: $file" }
        Copy-Item -LiteralPath $source -Destination (Join-Path $qaRoot $file) -Force
    }
    # Extra runtime libraries that only live in the install root (lua51, luabind rc4, legacy stubs).
    Get-ChildItem -LiteralPath $GameRoot -File -Filter '*.dll' | Where-Object { $manifest -notcontains $_.Name } |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $qaRoot $_.Name) -Force }
    Write-Host "== runtime: $fromBuild of $($manifest.Count) file(s) from $BuildRoot, the rest from the install root"
}

function Assert-QaGameDataShape {
    # The mirror is only evidence if it has the shape a player gets. The player's own gamedata is
    # copied verbatim, empty directories included - those are real. What must never appear is an
    # empty directory the mirror invented, because nothing downstream of a commit can produce one.
    # Fails the run instead of letting a silent difference decide the result: an empty
    # configs\nq\kinds is exactly what once let a guaranteed crash pass this suite.
    param([Parameter(Mandatory)][string]$Root, [Parameter(Mandatory)][string]$Reference)

    $emptyIn = {
        param($base)
        @(Get-ChildItem -LiteralPath $base -Recurse -Directory -Force -ErrorAction SilentlyContinue |
            Where-Object { -not (Get-ChildItem -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue) } |
            ForEach-Object { $_.FullName.Substring($base.Length).TrimStart('\') })
    }
    $expected = [Collections.Generic.HashSet[string]]::new(
        [string[]](& $emptyIn $Reference), [StringComparer]::OrdinalIgnoreCase)
    $invented = @((& $emptyIn $Root) | Where-Object { -not $expected.Contains($_) })
    if ($invented.Count) {
        throw ("The mirrored gamedata invented {0} empty director{1} the player's install does not have: {2}" -f
            $invented.Count, $(if ($invented.Count -eq 1) { 'y' } else { 'ies' }), ($invented -join ', '))
    }
}

function Update-QaGameData {
    # The loose layer has to be a real copy: archive entries are registered under <root>\gamedata\,
    # so redirecting $game_data$ elsewhere would hide system.ltx and everything else.
    if (Test-Path -LiteralPath $qaGameData) { Remove-Item -LiteralPath $qaGameData -Recurse -Force }
    Copy-TreeContents -Source (Join-Path $GameRoot 'gamedata') -Destination $qaGameData
    # Refined's own compatibility layer carries the xms_nq*.script runtime and configs\nq\catalog.ltx.
    Copy-TreeContents -Source $compatGameData -Destination $qaGameData -SkipEmpty

    New-Item -ItemType Directory -Path (Join-Path $qaGameData 'scripts') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'nq_qa_probe.script') `
        -Destination (Join-Path $qaGameData 'scripts') -Force

    $scriptLtxPath = Join-Path $qaConfigs 'script.ltx'
    if (-not (Test-Path -LiteralPath $scriptLtxPath)) {
        throw 'The mirrored gamedata has no configs\script.ltx; the probe cannot be registered.'
    }
    $patched = $false
    $scriptLtx = @(Get-Content -LiteralPath $scriptLtxPath | ForEach-Object {
            if (-not $patched -and $_ -match '^\s*script\s*=') {
                $patched = $true
                "$_, nq_qa_probe"
            }
            else { $_ }
        })
    if (-not $patched) { throw "No 'script =' line was found in the mirrored script.ltx." }
    Set-Content -LiteralPath $scriptLtxPath -Value $scriptLtx -Encoding Default

    Assert-QaGameDataShape -Root $qaGameData -Reference (Join-Path $GameRoot 'gamedata')
}

function Update-QaUserLtx {
    # Start from the player's settings, then force exactly the knobs the procedure fixes.
    $userLtxSource = Join-Path $GameRoot 'appdata\user.ltx'
    $userLtx = if (Test-Path -LiteralPath $userLtxSource) { @(Get-Content -LiteralPath $userLtxSource) } else { @() }
    $overrides = [ordered]@{
        'renderer'              = 'renderer_r4'
        'vid_mode'              = $VidMode
        'vid_window_mode'       = 'st_opt_windowed'
        'rs_fullscreen'         = 'off'
        'rs_v_sync'             = 'off'
        'rs_fps_limit'          = '60'
        'g_pause_in_background' = '0'
        'g_god'                 = 'on'
        'snd_volume_music'      = '0'
        'snd_volume_eff'        = '0'
    }
    $userLtx = @($userLtx | Where-Object {
            $line = $_
            -not (@($overrides.Keys | Where-Object { $line -match "^\s*$_\s" }).Count)
        })
    foreach ($key in $overrides.Keys) { $userLtx += "$key $($overrides[$key])" }
    Set-Content -LiteralPath (Join-Path $qaAppData 'user.ltx') -Value $userLtx -Encoding Default
}

function Copy-SaveGroup {
    # The whole group travels together and unchanged, companions included. This runs again before
    # every scenario that starts from the base save: a level change makes the game write its own
    # autosave, and if that is the save the run starts from, the next run would begin mid-quest.
    $parts = @(Get-ChildItem -LiteralPath $userSavedGames -File | Where-Object { $_.Name -like "$SaveName.*" -and $_.Name -notlike '*.bak' })
    if (-not @($parts | Where-Object { $_.Extension -eq '.scop' }).Count) {
        throw "The player's savedgames has no complete group for '$SaveName'."
    }
    New-Item -ItemType Directory -Path $qaSaves -Force | Out-Null
    $parts | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $qaSaves -Force }
    Write-Host "== save group copied: $($parts.Name -join ', ')"
}

function Set-QaModule {
    param([ValidateSet('Full', 'Changed', 'Empty', 'None')][string]$Variant)
    if (Test-Path -LiteralPath $qaModules) { Remove-Item -LiteralPath $qaModules -Recurse -Force }
    New-Item -ItemType Directory -Path $qaModules -Force | Out-Null
    switch ($Variant) {
        'None' { return }
        'Empty' { Copy-TreeContents -Source (Join-Path $PSScriptRoot 'module-empty') -Destination $qaModule }
        default {
            Copy-TreeContents -Source (Join-Path $PSScriptRoot 'module') -Destination $qaModule
            if ($Variant -eq 'Changed') {
                Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'module-changed\linear_fetch.nqasset') `
                    -Destination (Join-Path $qaModule 'quests\linear_fetch.nqasset') -Force
            }
        }
    }
}

# ---------------------------------------------------------------------------- one launch
$script:failures = [Collections.Generic.List[string]]::new()
$script:summary = [Collections.Generic.List[object]]::new()

$fatalPattern = 'FATAL ERROR|stack trace:|Assertion|Expression\s*:|access violation|reader overflow|' +
    'Lua error|No available phrase to say'

function Invoke-NqPhase {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Phase,
        [Parameter(Mandatory)][string]$LoadSave,
        [string]$WriteSave = '',
        [switch]$DriveTalkUi,
        [switch]$AllowNoNqLines,
        # Diagnostics this phase is designed to provoke. Everything else still fails the run:
        # a scenario that orphans a quest on purpose must not be read as "any warning is fine".
        [string[]]$ExpectedNqLines = @()
    )

    Assert-NoTargetEngine
    $phaseResult = Join-Path $resultRoot $Name
    New-Item -ItemType Directory -Path $phaseResult -Force | Out-Null

    Set-Content -LiteralPath (Join-Path $qaConfigs 'nq_qa.ltx') -Encoding Default -Value @(
        '[qa]', "phase = $Phase", "save  = $WriteSave", 'quit  = true')

    $oldLogs = @(Get-ChildItem -LiteralPath $qaLogs -File -ErrorAction SilentlyContinue)
    if ($oldLogs.Count) {
        $residue = Join-Path $phaseResult 'pre-run-logs'
        New-Item -ItemType Directory -Path $residue -Force | Out-Null
        $oldLogs | Move-Item -Destination $residue -Force
    }

    $processIdFile = Join-Path $phaseResult 'engine.pid'
    $launcherIdFile = Join-Path $phaseResult 'launcher.pid'
    $exitCodeFile = Join-Path $phaseResult 'exit-code.txt'
    # Never read a PID published by an earlier attempt with the same result label.
    Remove-Item -LiteralPath $processIdFile, $launcherIdFile, $exitCodeFile -Force -ErrorAction SilentlyContinue
    $desktopName = "DeadAirNq$([Guid]::NewGuid().ToString('N').Substring(0, 8))"

    # The fsltx is the bare file name and the working directory is the QA root (the launcher starts
    # the process in the executable's directory), so no path with spaces ever reaches a command line.
    # "-i" keeps the engine out of exclusive mouse mode (x_ray.cpp: captureInput). Without it SDL
    # runs in relative mode, ignores WM_MOUSEMOVE and takes motion from raw input only - which
    # cannot be injected into a hidden desktop, so the talk window could never be clicked.
    $arguments = '-i -fsltx fsgame.ltx -always_active -silent_error_mode -force_flushlog -r4 ' +
        "-start `"server($LoadSave/single/alife/load)`""

    Write-Host ""
    Write-Host "== [$Name] phase=$Phase load='$LoadSave' write='$WriteSave'"
    Write-Host "   xrEngine.exe $arguments   (cwd: $qaRoot, desktop: $desktopName)"

    # The QA copy lives outside the installed root, so it needs the same DPI layer as the original.
    New-Item -Path $layersKey -Force | Out-Null
    New-ItemProperty -Path $layersKey -Name $qaEngine -Value '~ HIGHDPIAWARE' -PropertyType String -Force | Out-Null

    $engineProcessId = 0
    $launcherProcessId = 0
    $runStartedUtc = [DateTime]::UtcNow
    try {
        & $helper -FilePath $qaEngine -Arguments $arguments -ProcessIdFile $processIdFile `
            -LauncherIdFile $launcherIdFile -ExitCodeFile $exitCodeFile -DesktopName $desktopName `
            -TimeoutSeconds $TimeoutSeconds | Out-Null
        $launcherProcessId = [int](Get-Content -LiteralPath $launcherIdFile -Raw)

        $deadline = [DateTime]::UtcNow.AddSeconds(60)
        while (-not (Test-Path -LiteralPath $processIdFile -PathType Leaf)) {
            if ([DateTime]::UtcNow -ge $deadline) { throw 'The hidden launcher did not publish the engine PID.' }
            Start-Sleep -Milliseconds 100
        }
        $engineProcessId = [int](Get-Content -LiteralPath $processIdFile -Raw)
        $engineProcess = Get-Process -Id $engineProcessId -ErrorAction Stop
        if ($engineProcess.Path -ne $qaEngine) {
            throw "The hidden launcher started an unexpected executable: $($engineProcess.Path)"
        }
        Write-Host "   engine pid $engineProcessId, launcher pid $launcherProcessId"

        if ($DriveTalkUi -or $CaptureEverySeconds -gt 0) {
            Watch-Run -EngineProcess $engineProcess -DesktopName $desktopName -PhaseResult $phaseResult -DriveTalkUi:$DriveTalkUi
        }
        if (-not $engineProcess.WaitForExit($TimeoutSeconds * 1000)) {
            throw "The engine did not exit within $TimeoutSeconds s."
        }
        # The launcher publishes the exit code after the engine is gone, so let it finish here -
        # the cleanup below would otherwise kill it mid-write and the code would read as missing.
        $launcher = Get-Process -Id $launcherProcessId -ErrorAction SilentlyContinue
        if ($launcher) { $launcher.WaitForExit(30000) | Out-Null }
    }
    finally {
        Remove-ItemProperty -Path $layersKey -Name $qaEngine -ErrorAction SilentlyContinue
        # Stop only what this harness created if anything interrupted the normal shutdown.
        foreach ($ownedProcessId in @($engineProcessId, $launcherProcessId)) {
            if ($ownedProcessId -le 0) { continue }
            $owned = Get-Process -Id $ownedProcessId -ErrorAction SilentlyContinue
            if ($owned -and -not $owned.HasExited) {
                Stop-Process -Id $ownedProcessId -Force -ErrorAction SilentlyContinue
                $owned.WaitForExit(15000) | Out-Null
            }
        }
    }

    # The launcher writes the exit code after the process object is already gone, so give it a
    # moment. It publishes whatever GetExitCodeProcess reported, which can be a negative NTSTATUS
    # or its own -1 when it could not read one; parse wide and fold into the unsigned form.
    # (0xFFFFFFFF cannot be written literally here: PowerShell parses it as the Int32 -1.)
    $exitCode = [uint32]::MaxValue
    $exitDeadline = [DateTime]::UtcNow.AddSeconds(15)
    while (-not (Test-Path -LiteralPath $exitCodeFile -PathType Leaf) -and [DateTime]::UtcNow -lt $exitDeadline) {
        Start-Sleep -Milliseconds 200
    }
    if (Test-Path -LiteralPath $exitCodeFile -PathType Leaf) {
        $parsed = [int64]0
        if ([int64]::TryParse((Get-Content -LiteralPath $exitCodeFile -Raw).Trim(), [ref]$parsed)) {
            $exitCode = [uint32]($parsed -band 0xFFFFFFFFL)
        }
    }
    $log = Get-ChildItem -LiteralPath $qaLogs -File -Filter '*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if (-not $log -or $log.LastWriteTimeUtc -lt $runStartedUtc.AddSeconds(-2)) {
        throw "[$Name] produced no fresh engine log."
    }
    $logDestination = Join-Path $phaseResult 'engine.log'
    Copy-Item -LiteralPath $log.FullName -Destination $logDestination -Force
    $logText = Get-Content -LiteralPath $logDestination -Raw

    return Test-PhaseLog -Name $Name -LogText $logText -LogPath $logDestination -ExitCode $exitCode -AllowNoNqLines:$AllowNoNqLines
}

function Test-PhaseLog {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][AllowEmptyString()][string]$LogText,
        [Parameter(Mandatory)][string]$LogPath,
        [Parameter(Mandatory)][uint32]$ExitCode,
        [switch]$AllowNoNqLines
    )

    $lines = @($LogText -split "`r?`n")
    $nqqa = @($lines | Where-Object { $_ -match 'NQQA' })
    foreach ($line in $nqqa) { Write-Host "   $($line -replace '^\[LUA\]\s*', '')" }

    $problems = [Collections.Generic.List[string]]::new()
    $failLines = @($nqqa | Where-Object { $_ -match 'NQQA: \S+ FAIL ' })
    foreach ($line in $failLines) { $problems.Add("check failed: $($line.Trim())") }

    $done = @($nqqa | Where-Object { $_ -match 'NQQA: DONE ' }) | Select-Object -Last 1
    if (-not $done) { $problems.Add('the probe never reached its DONE line') }
    elseif ($done -match 'failed=(\d+)') { if ([int]$Matches[1] -gt 0) { $problems.Add("probe reports $($Matches[1]) failed check(s)") } }

    $fatal = @($lines | Where-Object { $_ -match $fatalPattern })
    foreach ($line in $fatal) { $problems.Add("engine log signature: $($line.Trim())") }

    $nqErrors = @($lines | Where-Object { $_ -match '! \[nq\]|~ \[nq\]' })
    foreach ($line in $nqErrors) { Write-Host "   [nq diagnostic] $($line.Trim())" }
    if ($AllowNoNqLines -and $nqErrors.Count) {
        foreach ($line in $nqErrors) {
            $expected = $false
            foreach ($pattern in $ExpectedNqLines) { if ($line -match $pattern) { $expected = $true; break } }
            if (-not $expected) { $problems.Add("unexpected NQ diagnostic: $($line.Trim())") }
        }
    }
    if ($AllowNoNqLines -and ($LogText -match 'callbacks registered')) {
        $problems.Add('NQ registered its callbacks although no quest is loaded')
    }
    if ($ExitCode -ne 0) { $problems.Add("engine exit code $ExitCode") }

    $dumps = @(Get-ChildItem -LiteralPath $qaLogs -File -Filter '*.mdmp' -ErrorAction SilentlyContinue)
    foreach ($dump in $dumps) { $problems.Add("crash dump produced: $($dump.Name)") }

    $ok = ($problems.Count -eq 0)
    foreach ($problem in $problems) { $script:failures.Add("[$Name] $problem") }
    $script:summary.Add([pscustomobject]@{
            Scenario = $Name
            Result   = $(if ($ok) { 'PASS' } else { 'FAIL' })
            Checks   = "$(@($nqqa | Where-Object { $_ -match 'NQQA: \S+ PASS ' }).Count) pass / $($failLines.Count) fail"
            Log      = $LogPath
        })
    Write-Host "   -> $Name $(if ($ok) { 'PASS' } else { 'FAIL' })"
    return $ok
}

# ---------------------------------------------------------------------------- talk UI driver
# The capture helper fills a client-sized bitmap starting at the window origin, so a point read off
# a screenshot is "window space". The physical cursor needs screen coordinates and the posted
# messages need client coordinates; both follow from the window rect and the known client size.
function Get-ClickFrame {
    param([Parameter(Mandatory)][string]$DesktopName, [Parameter(Mandatory)][int]$ProcessId)

    $clientWidth, $clientHeight = ($VidMode -split 'x') | ForEach-Object { [int]$_ }
    $window = @(& $windowInfo -DesktopName $DesktopName -ProcessId $ProcessId | Where-Object Visible) |
        Select-Object -First 1
    if (-not $window) { return $null }
    # Win11 keeps the side and bottom borders equal, so the side border gives the caption height.
    $border = [Math]::Max(0, [int](($window.Width - $clientWidth) / 2))
    $caption = [Math]::Max(0, $window.Height - $clientHeight - $border)
    return [pscustomobject]@{
        ScreenX = $window.X; ScreenY = $window.Y; Border = $border; Caption = $caption
        Width = $window.Width; Height = $window.Height
    }
}

function Invoke-ReplyKey {
    param(
        [Parameter(Mandatory)][string]$DesktopName,
        [Parameter(Mandatory)][int]$ProcessId,
        [Parameter(Mandatory)][int]$Row
    )

    # Every reply line carries a digit accelerator: CUITalkDialogWnd::AddQuestion does
    # SetAccelerator(SDL_SCANCODE_1 - 1 + number), and the list is numbered 1..9 then 0 for the
    # tenth. A keystroke needs no cursor, so it works on a hidden desktop where the engine's own
    # UI cursor cannot be positioned (CUICursor drives itself from relative motion or from the
    # system cursor of a desktop that has no display bounds).
    if ($Row -lt 1 -or $Row -gt 10) { throw "reply row $Row has no accelerator" }
    $vk = if ($Row -eq 10) { 0x30 } else { 0x30 + $Row }
    & $keySender -DesktopName $DesktopName -ProcessId $ProcessId -VirtualKey $vk | Out-Null
    return "row $($Row): accelerator key '$(if ($Row -eq 10) { 0 } else { $Row })' (VK 0x{0:X2})" -f $vk
}

function Invoke-ReplyClick {
    param(
        [Parameter(Mandatory)][string]$DesktopName,
        [Parameter(Mandatory)][int]$ProcessId,
        [Parameter(Mandatory)][int]$Row,
        [AllowNull()][object]$Frame
    )

    $shotX = $ReplyX
    $shotY = $FirstReplyY + ($Row - 1) * $ReplyRowHeight
    if ($Frame) {
        $screenX = $Frame.ScreenX + $shotX
        $screenY = $Frame.ScreenY + $shotY
        $clientX = $shotX - $Frame.Border
        $clientY = $shotY - $Frame.Caption
    }
    else { $screenX, $screenY, $clientX, $clientY = $shotX, $shotY, $shotX, $shotY }
    # Move first and let the engine apply the motion, then press: it dispatches a button event
    # immediately but only feeds the accumulated motion to the UI at the end of the frame's batch.
    & $clicker -DesktopName $DesktopName -ProcessId $ProcessId -X $screenX -Y $screenY `
        -ClientX $clientX -ClientY $clientY -MoveOnly | Out-Null
    Start-Sleep -Milliseconds 500
    & $clicker -DesktopName $DesktopName -ProcessId $ProcessId -X $screenX -Y $screenY `
        -ClientX $clientX -ClientY $clientY | Out-Null
    return "row $($Row): shot ($shotX,$shotY) -> screen ($screenX,$screenY) client ($clientX,$clientY)"
}

function Watch-Run {
    param(
        [Parameter(Mandatory)][Diagnostics.Process]$EngineProcess,
        [Parameter(Mandatory)][string]$DesktopName,
        [Parameter(Mandatory)][string]$PhaseResult,
        [switch]$DriveTalkUi
    )

    $shots = Join-Path $PhaseResult 'screens'
    New-Item -ItemType Directory -Path $shots -Force | Out-Null
    # Tag -> row in the reply list the probe is waiting for. The quest topic sits behind the NPC's
    # own entries; every later list contains only the quest's own replies.
    $rows = @{ topic = $TopicRow; reply1 = 1; reply2 = 2; reply3 = 3 }
    $handled = 0
    $logFile = $null
    $shotIndex = 0
    $nextShot = [DateTime]::UtcNow
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $frame = $null
    $pendingTag = $null
    $pendingRow = 0
    $attempts = 0
    $nextClick = [DateTime]::MaxValue

    while (-not $EngineProcess.HasExited -and [DateTime]::UtcNow -lt $deadline) {
        if ($CaptureEverySeconds -gt 0 -and [DateTime]::UtcNow -ge $nextShot) {
            $nextShot = [DateTime]::UtcNow.AddSeconds($CaptureEverySeconds)
            $shotIndex++
            $periodic = Join-Path $shots ("periodic-{0:d3}.png" -f $shotIndex)
            try { & $capture -DesktopName $DesktopName -ProcessId $EngineProcess.Id -OutputPath $periodic | Out-Null }
            catch { Write-Warning "periodic capture failed: $_" }
        }
        if (-not $DriveTalkUi) { Start-Sleep -Milliseconds 500; continue }
        if (-not $logFile) {
            $logFile = Get-ChildItem -LiteralPath $qaLogs -File -Filter '*.log' -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
            Start-Sleep -Milliseconds 500
            continue
        }
        $awaits = @()
        try {
            $stream = [IO.File]::Open($logFile.FullName, 'Open', 'Read', 'ReadWrite')
            try {
                $reader = [IO.StreamReader]::new($stream)
                $awaits = @($reader.ReadToEnd() -split "`r?`n" | Where-Object { $_ -match 'NQQA-AWAIT: (\S+)' })
            }
            finally { $stream.Dispose() }
        }
        catch { Start-Sleep -Milliseconds 500; continue }

        # A new request supersedes the previous one: the probe only asks again once it has moved on.
        if ($awaits.Count -gt $handled) {
            $line = $awaits[$handled]
            $handled++
            if ($line -notmatch 'NQQA-AWAIT: (\S+)') { continue }
            $pendingTag = $Matches[1]
            $pendingRow = if ($rows.ContainsKey($pendingTag)) { $rows[$pendingTag] } else { 1 }
            $attempts = 0
            $nextClick = [DateTime]::UtcNow.AddSeconds(3)   # let the window finish opening
            # The window is not the input desktop's foreground window, so it is told it is active
            # before the pointer messages arrive; SDL only tracks the mouse for a focused window.
            try { & $focuser -DesktopName $DesktopName -ProcessId $EngineProcess.Id | Out-Null }
            catch { Write-Warning "focus failed: $_" }
            if (-not $frame) {
                $frame = Get-ClickFrame -DesktopName $DesktopName -ProcessId $EngineProcess.Id
                if ($frame) {
                    Write-Host ("   [talk ui] window at ({0},{1}) {2}x{3}, border {4}, caption {5}" -f
                        $frame.ScreenX, $frame.ScreenY, $frame.Width, $frame.Height, $frame.Border, $frame.Caption)
                }
                else { Write-Warning 'the talk-UI driver could not read the window rectangle' }
            }
            continue
        }

        if ($pendingTag -and $attempts -lt $ClickAttempts -and [DateTime]::UtcNow -ge $nextClick) {
            $attempts++
            $stem = "{0:d2}-{1}-try{2}" -f $handled, $pendingTag, $attempts
            $before = Join-Path $shots "$stem-before.png"
            try { & $capture -DesktopName $DesktopName -ProcessId $EngineProcess.Id -OutputPath $before | Out-Null }
            catch { Write-Warning "capture failed for $pendingTag : $_" }
            try {
                # the accelerator key is the reliable path; the click stays as a fallback
                if ($attempts -le 2) {
                    $where = Invoke-ReplyKey -DesktopName $DesktopName -ProcessId $EngineProcess.Id `
                        -Row $pendingRow
                }
                else {
                    $where = Invoke-ReplyClick -DesktopName $DesktopName -ProcessId $EngineProcess.Id `
                        -Row $pendingRow -Frame $frame
                }
                Write-Host "   [talk ui] $pendingTag attempt $attempts -> $where"
            }
            catch { Write-Warning "reply input failed for $pendingTag : $_" }
            Start-Sleep -Milliseconds 1500
            $after = Join-Path $shots "$stem-after.png"
            try { & $capture -DesktopName $DesktopName -ProcessId $EngineProcess.Id -OutputPath $after | Out-Null } catch { }
            $nextClick = [DateTime]::UtcNow.AddSeconds($ClickRetrySeconds)
            continue
        }
        Start-Sleep -Milliseconds 500
    }
}

# ---------------------------------------------------------------------------- .scov inspection
function Test-ScovHasNqBlob {
    param([Parameter(Mandatory)][string]$Save)
    $path = Join-Path $qaSaves "$Save.scov"
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
    # Core pseudo-namespace xms.nq -> chunk id 0x584DFF01, little endian in the .scov directory.
    $needle = [byte[]](0x01, 0xFF, 0x4D, 0x58)
    $bytes = [IO.File]::ReadAllBytes($path)
    for ($i = 0; $i -le $bytes.Length - 4; ++$i) {
        if ($bytes[$i] -eq $needle[0] -and $bytes[$i + 1] -eq $needle[1] -and
            $bytes[$i + 2] -eq $needle[2] -and $bytes[$i + 3] -eq $needle[3]) { return $true }
    }
    return $false
}

# ---------------------------------------------------------------------------- scenarios
function Assert-SaveExists {
    param([Parameter(Mandatory)][string]$Save, [Parameter(Mandatory)][string]$Producer)
    if (-not (Test-Path -LiteralPath (Join-Path $qaSaves "$Save.scop"))) {
        throw "'$Save' is missing in the QA root; run -Scenario $Producer first."
    }
}

$SAVE_MID = 'nq_qa_mid'
$SAVE_SCOV = 'nq_qa_scov'
$SAVE_DLG = 'nq_qa_dlg'
$SAVE_NOMOD = 'nq_qa_nomod'

function Invoke-Scout {
    Set-QaModule -Variant Full
    Copy-SaveGroup
    Invoke-NqPhase -Name 'Scout' -Phase 'scout' -LoadSave $SaveName | Out-Null
}

function Invoke-Main {
    Set-QaModule -Variant Full
    Copy-SaveGroup
    Get-ChildItem -LiteralPath $qaSaves -File | Where-Object { $_.Name -like 'nq_qa_*' } | Remove-Item -Force
    Invoke-NqPhase -Name 'Main' -Phase 'p1' -LoadSave $SaveName -WriteSave $SAVE_MID | Out-Null
    if (-not (Test-Path -LiteralPath (Join-Path $qaSaves "$SAVE_MID.scop"))) {
        $script:failures.Add('[Main] the mid-quest save was not written')
    }
}

function Invoke-Reload {
    Assert-SaveExists -Save $SAVE_MID -Producer 'Main'
    Set-QaModule -Variant Full
    Invoke-NqPhase -Name 'Reload' -Phase 'p2' -LoadSave $SAVE_MID -WriteSave $SAVE_SCOV | Out-Null
    $hasBlob = Test-ScovHasNqBlob -Save $SAVE_SCOV
    Write-Host "   [k] $SAVE_SCOV.scov carries the xms.nq chunk (0x584DFF01): $hasBlob"
    if (-not $hasBlob) { $script:failures.Add('[Reload] the save written right after a load has no NQ chunk in .scov') }
}

function Invoke-Dialog {
    Assert-SaveExists -Save $SAVE_SCOV -Producer 'Reload'
    Set-QaModule -Variant Full
    Invoke-NqPhase -Name 'Dialog' -Phase 'p3' -LoadSave $SAVE_SCOV -WriteSave $SAVE_DLG -DriveTalkUi | Out-Null
}

function Invoke-LevelChange {
    Assert-SaveExists -Save $SAVE_DLG -Producer 'Dialog'
    Set-QaModule -Variant Full
    Remove-Item -LiteralPath (Join-Path $qaAppData 'nq_qa_marker.txt') -Force -ErrorAction SilentlyContinue
    Invoke-NqPhase -Name 'LevelChange' -Phase 'p4' -LoadSave $SAVE_DLG | Out-Null
}

function Invoke-ModuleRemoved {
    Assert-SaveExists -Save $SAVE_MID -Producer 'Main'
    Set-QaModule -Variant None
    Invoke-NqPhase -Name 'ModuleRemoved' -Phase 'p5' -LoadSave $SAVE_MID -WriteSave $SAVE_NOMOD -AllowNoNqLines | Out-Null
    Set-QaModule -Variant Full
    Assert-SaveExists -Save $SAVE_NOMOD -Producer 'ModuleRemoved'
    Invoke-NqPhase -Name 'ModuleBack' -Phase 'p6' -LoadSave $SAVE_NOMOD | Out-Null
}

function Invoke-ChangedGraph {
    Assert-SaveExists -Save $SAVE_MID -Producer 'Main'
    Set-QaModule -Variant Changed
    Invoke-NqPhase -Name 'ChangedGraph' -Phase 'p7' -LoadSave $SAVE_MID | Out-Null
    Set-QaModule -Variant Full
}

function Invoke-EmptyModule {
    Assert-SaveExists -Save $SAVE_MID -Producer 'Main'
    Set-QaModule -Variant Empty
    # The save is mid-quest and the module loses its quests under it, so the runtime is supposed to
    # fail the orphaned PDA task and say so (xms_nq_task.script:230-236). That one line is the
    # scenario working, not a defect; any other NQ diagnostic still fails the phase.
    Invoke-NqPhase -Name 'EmptyModule' -Phase 'p8' -LoadSave $SAVE_MID -AllowNoNqLines `
        -ExpectedNqLines @('quest is no longer installed, its PDA task \S+ was closed') | Out-Null
    Set-QaModule -Variant Full
}

function Invoke-Control {
    Set-QaModule -Variant None
    Copy-SaveGroup
    Invoke-NqPhase -Name 'Control' -Phase 'p9' -LoadSave $SaveName -AllowNoNqLines | Out-Null
    Set-QaModule -Variant Full
}

# ---------------------------------------------------------------------------- main
New-Item -ItemType Directory -Path $resultRoot -Force | Out-Null
Assert-NoTargetEngine

if ($RebuildRoot -or -not (Test-Path -LiteralPath $qaEngine)) {
    New-QaRoot
    Copy-SaveGroup
}
else {
    Write-Host "== reusing the QA root: $qaRoot"
    Update-QaRuntime
    Update-QaGameData
    Update-QaUserLtx
    if (-not (Test-Path -LiteralPath (Join-Path $qaSaves "$SaveName.scop"))) { Copy-SaveGroup }
}

$userSaveBaseline = ConvertTo-Json -InputObject @(Get-DirectoryManifest -Root $userSavedGames) -Depth 3 -Compress
Set-Content -LiteralPath (Join-Path $resultRoot 'player-savedgames-before.json') -Value $userSaveBaseline -Encoding utf8

# A scenario that throws (launcher trouble, missing prerequisite save) is a failure of that
# scenario only: the remaining ones still run and the summary stays complete.
function Invoke-Scenario {
    param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][scriptblock]$Body)
    try { & $Body }
    catch {
        $script:failures.Add("[$Name] $($_.Exception.Message)")
        $script:summary.Add([pscustomobject]@{ Scenario = $Name; Result = 'FAIL'; Checks = 'not run'; Log = '' })
        Write-Host "   -> $Name FAIL ($($_.Exception.Message))"
    }
}

try {
    switch ($Scenario) {
        'Scout' { Invoke-Scenario 'Scout' { Invoke-Scout } }
        'Main' { Invoke-Scenario 'Main' { Invoke-Main } }
        'Reload' { Invoke-Scenario 'Reload' { Invoke-Reload } }
        'Dialog' { Invoke-Scenario 'Dialog' { Invoke-Dialog } }
        'LevelChange' { Invoke-Scenario 'LevelChange' { Invoke-LevelChange } }
        'ModuleRemoved' { Invoke-Scenario 'ModuleRemoved' { Invoke-ModuleRemoved } }
        'ChangedGraph' { Invoke-Scenario 'ChangedGraph' { Invoke-ChangedGraph } }
        'EmptyModule' { Invoke-Scenario 'EmptyModule' { Invoke-EmptyModule } }
        'Control' { Invoke-Scenario 'Control' { Invoke-Control } }
        default {
            Invoke-Scenario 'Main' { Invoke-Main }
            Invoke-Scenario 'Reload' { Invoke-Reload }
            Invoke-Scenario 'Dialog' { Invoke-Dialog }
            Invoke-Scenario 'LevelChange' { Invoke-LevelChange }
            Invoke-Scenario 'ModuleRemoved' { Invoke-ModuleRemoved }
            Invoke-Scenario 'ChangedGraph' { Invoke-ChangedGraph }
            Invoke-Scenario 'EmptyModule' { Invoke-EmptyModule }
            Invoke-Scenario 'Control' { Invoke-Control }
        }
    }
}
finally {
    $userSaveAfter = ConvertTo-Json -InputObject @(Get-DirectoryManifest -Root $userSavedGames) -Depth 3 -Compress
    Set-Content -LiteralPath (Join-Path $resultRoot 'player-savedgames-after.json') -Value $userSaveAfter -Encoding utf8
    if ($userSaveAfter -ne $userSaveBaseline) {
        $script:failures.Add("the player's savedgames directory changed during the run")
    }
    Remove-ItemProperty -Path $layersKey -Name $qaEngine -ErrorAction SilentlyContinue

    Write-Host ""
    Write-Host "==== NQ QA summary ($ResultLabel) ===="
    $script:summary | Format-Table -AutoSize | Out-String | Write-Host
    if ($script:failures.Count) {
        Write-Host "FAILURES:"
        foreach ($failure in $script:failures) { Write-Host "  - $failure" }
    }
    else { Write-Host "all scenarios passed" }
    $script:summary | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $resultRoot 'summary.json') -Encoding utf8
    Set-Content -LiteralPath (Join-Path $resultRoot 'failures.txt') -Value @($script:failures) -Encoding utf8
    if (-not $KeepRoot) { Write-Host "QA root kept for the next run: $qaRoot" }
}

if ($script:failures.Count) { exit 1 }
