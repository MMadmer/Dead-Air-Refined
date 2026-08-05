param(
    [Parameter(Mandatory)]
    [string]$ResultRoot,

    [Parameter(Mandatory)]
    [string]$FixtureRoot,

    [ValidateSet('All', 'Legacy', 'EncodeFalse', 'MalformedScov')]
    [string]$Suite = 'All',

    [ValidatePattern('^[A-Za-z0-9_.-]+$')]
    [string]$LoadSave = 'da111_latest',

    [switch]$SkipDeploymentHashCheck
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-PathWithin {
    param(
        [Parameter(Mandatory)][string]$Candidate,
        [Parameter(Mandatory)][string]$Container
    )

    $candidatePath = [IO.Path]::GetFullPath($Candidate).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $containerPath = [IO.Path]::GetFullPath($Container).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $comparison = [StringComparison]::OrdinalIgnoreCase
    return $candidatePath.Equals($containerPath, $comparison) -or
        $candidatePath.StartsWith($containerPath + [IO.Path]::DirectorySeparatorChar, $comparison)
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$gameRoot = (Resolve-Path (Join-Path $repositoryRoot '..')).Path
$engine = Join-Path $gameRoot 'xrEngine.exe'
$helper = Join-Path $repositoryRoot 'tools\qa\Start-DetachedHiddenDesktopProcess.ps1'
$qaDataRoot = Join-Path $PSScriptRoot 'serializer-compat'
$fsConfigTemplate = Join-Path $qaDataRoot 'qa_serializer_compat.template.ltx'
$userAppDataRoot = Join-Path $gameRoot 'appdata'
$userSavedGamesRoot = Join-Path $userAppDataRoot 'savedgames'
$requestedResultRoot = [IO.Path]::GetFullPath($ResultRoot)
if (Test-PathWithin -Candidate $requestedResultRoot -Container $userAppDataRoot) {
    throw 'ResultRoot must not be inside the user appdata directory.'
}
if ((Test-Path -LiteralPath $requestedResultRoot) -and
    @(Get-ChildItem -LiteralPath $requestedResultRoot -Force).Count) {
    throw 'The serializer compatibility result directory must be new or empty.'
}
New-Item -ItemType Directory -Path $requestedResultRoot -Force | Out-Null
$ResultRoot = (Resolve-Path -LiteralPath $requestedResultRoot).Path
$FixtureRoot = (Resolve-Path -LiteralPath $FixtureRoot).Path
$appDataRoot = Join-Path $ResultRoot 'appdata'
$savedGamesRoot = Join-Path $appDataRoot 'savedgames'
$logsRoot = Join-Path $appDataRoot 'logs'
$fsConfigName = "qa_serializer_compat_$($PID)_$([Guid]::NewGuid().ToString('N')).ltx"
$fsConfigRuntime = Join-Path $gameRoot $fsConfigName
New-Item -ItemType Directory -Path $savedGamesRoot, $logsRoot -Force | Out-Null

function Get-FileRecord {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [pscustomobject]@{
            Name = [IO.Path]::GetFileName($Path)
            Present = $false
            Length = $null
            SHA256 = $null
        }
    }

    $file = Get-Item -LiteralPath $Path
    [pscustomobject]@{
        Name = $file.Name
        Present = $true
        Length = $file.Length
        SHA256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
}

function Get-DirectoryManifest {
    param([Parameter(Mandatory)][string]$Root)

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        return @()
    }

    return @(Get-ChildItem -LiteralPath $Root -File -Recurse |
        Sort-Object FullName |
        ForEach-Object {
            [pscustomobject]@{
                Path = [IO.Path]::GetRelativePath($Root, $_.FullName)
                Length = $_.Length
                SHA256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
            }
        })
}

function Assert-X64Engine {
    if (-not (Test-Path -LiteralPath $engine -PathType Leaf)) {
        throw "Missing exact runtime: $engine"
    }

    $stream = [IO.File]::OpenRead($engine)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5A4D) {
            throw "The exact runtime is not a valid PE image: $engine"
        }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 0 -or $peOffset + 6 -gt $stream.Length) {
            throw "The exact runtime has an invalid PE header offset: $engine"
        }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550 -or $reader.ReadUInt16() -ne 0x8664) {
            throw "The exact runtime is not an x64 PE image: $engine"
        }
    }
    finally {
        $reader.Dispose()
    }
}

function Assert-ExactBytes {
    param(
        [Parameter(Mandatory)][string]$Path,
        [AllowNull()][byte[]]$Expected
    )

    if ($null -eq $Expected) {
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            throw "Unexpected file: $Path"
        }
        return
    }

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing file: $Path"
    }
    $actual = [IO.File]::ReadAllBytes($Path)
    if ($actual.Length -ne $Expected.Length -or
        [Convert]::ToBase64String($actual) -ne [Convert]::ToBase64String($Expected)) {
        throw "Byte mismatch: $Path"
    }
}

function Remove-SaveFamily {
    param([Parameter(Mandatory)][string]$BaseName)

    Get-ChildItem -LiteralPath $savedGamesRoot -File |
        Where-Object Name -Like "$BaseName.*" |
        Remove-Item -Force
}

function Assert-NoTransientFiles {
    param([Parameter(Mandatory)][string[]]$BaseNames)

    $transient = @(Get-ChildItem -LiteralPath $savedGamesRoot -File | Where-Object {
        $fileName = $_.Name
        @($BaseNames | Where-Object { $fileName -like "$($_).*" }).Count -gt 0 -and
            $fileName -match '\.(tmp|txn|preserved)(\.|$)'
    })
    if ($transient.Count) {
        throw "Compatibility QA left $($transient.Count) transient save file(s)."
    }
}

function Copy-SaveEvidence {
    param(
        [Parameter(Mandatory)][string]$Destination,
        [Parameter(Mandatory)][string[]]$BaseNames
    )

    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Get-ChildItem -LiteralPath $savedGamesRoot -File | Where-Object {
        $fileName = $_.Name
        @($BaseNames | Where-Object { $fileName -like "$($_).*" }).Count -gt 0
    } | Copy-Item -Destination $Destination -Force
}

function Assert-CurrentDeployment {
    if ($SkipDeploymentHashCheck) {
        return
    }

    foreach ($runtimeName in @('xrGame.dll', 'xrScriptEngine.dll')) {
        $built = Join-Path $repositoryRoot "bin\x64\Release\$runtimeName"
        $deployed = Join-Path $gameRoot $runtimeName
        if (-not (Test-Path -LiteralPath $built -PathType Leaf) -or
            -not (Test-Path -LiteralPath $deployed -PathType Leaf) -or
            (Get-FileHash -LiteralPath $built -Algorithm SHA256).Hash -ne
                (Get-FileHash -LiteralPath $deployed -Algorithm SHA256).Hash) {
            throw "$runtimeName is not the exact currently built Release runtime."
        }
    }

    $packagedScript = Join-Path $repositoryRoot `
        'packaging\dead-air-x64\compatibility\gamedata\scripts\alife_storage_manager.script'
    $deployedScript = Join-Path $gameRoot 'gamedata\scripts\alife_storage_manager.script'
    if (-not (Test-Path -LiteralPath $deployedScript -PathType Leaf) -or
        (Get-FileHash -LiteralPath $packagedScript -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $deployedScript -Algorithm SHA256).Hash) {
        throw 'alife_storage_manager.script is not the exact packaged compatibility script.'
    }
}

function Copy-LoadFixture {
    foreach ($extension in @('.scop', '.scoc', '.scov')) {
        $source = Join-Path $FixtureRoot "$LoadSave$extension"
        $destination = Join-Path $savedGamesRoot "$LoadSave$extension"
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Missing load fixture: $source"
        }
        if ([IO.Path]::GetFullPath($source) -ne [IO.Path]::GetFullPath($destination)) {
            Copy-Item -LiteralPath $source -Destination $destination -Force
        }
    }

    $thumbnail = Join-Path $FixtureRoot "$LoadSave.dds"
    $thumbnailDestination = Join-Path $savedGamesRoot "$LoadSave.dds"
    if ((Test-Path -LiteralPath $thumbnail -PathType Leaf) -and
        [IO.Path]::GetFullPath($thumbnail) -ne [IO.Path]::GetFullPath($thumbnailDestination)) {
        Copy-Item -LiteralPath $thumbnail -Destination $thumbnailDestination -Force
    }
}

function Invoke-HiddenSuite {
    param(
        [Parameter(Mandatory)][string]$ScriptFile,
        [Parameter(Mandatory)][string]$SuiteResult
    )

    $runningEngine = @(Get-Process xrEngine -ErrorAction SilentlyContinue | Where-Object Path -EQ $engine)
    if ($runningEngine.Count) {
        throw 'The target xrEngine.exe is already running.'
    }

    New-Item -ItemType Directory -Path $SuiteResult -Force | Out-Null
    $residueRoot = Join-Path $SuiteResult 'pre-run-logs'
    $oldLogs = @(Get-ChildItem -LiteralPath $logsRoot -File)
    if ($oldLogs.Count) {
        New-Item -ItemType Directory -Path $residueRoot -Force | Out-Null
        $oldLogs | Move-Item -Destination $residueRoot -Force
    }

    $processIdFile = Join-Path $SuiteResult 'engine.pid'
    $launcherIdFile = Join-Path $SuiteResult 'launcher.pid'
    $exitCodeFile = Join-Path $SuiteResult 'exit-code.txt'
    $scriptFullPath = (Resolve-Path -LiteralPath (Join-Path $qaDataRoot $ScriptFile)).Path
    $scriptPath = [IO.Path]::GetRelativePath($gameRoot, $scriptFullPath).Replace('\', '/')
    if ([IO.Path]::IsPathRooted($scriptPath) -or $scriptPath -match '^\.\.(?:/|$)') {
        throw "The runtime Lua script is outside the exact game root: $scriptFullPath"
    }
    $scriptPath = $scriptPath.Replace("'", "\'")

    if ($appDataRoot -match '[|\r\n]') {
        throw 'ResultRoot contains characters unsupported by the filesystem configuration.'
    }
    $appDataPath = $appDataRoot.TrimEnd('\', '/') + '\'
    $fsConfigContents = [IO.File]::ReadAllText($fsConfigTemplate)
    if ([regex]::Matches($fsConfigContents, '__QA_APP_DATA_ROOT__').Count -ne 1) {
        throw 'The serializer compatibility filesystem template must contain exactly one appdata token.'
    }
    $fsConfigContents = $fsConfigContents.Replace('__QA_APP_DATA_ROOT__', $appDataPath)
    [IO.File]::WriteAllText($fsConfigRuntime, $fsConfigContents, [Text.UTF8Encoding]::new($false))

    $engineProcessId = 0
    $launcherProcessId = 0
    $desktopName = "DeadAirSerializer$([Guid]::NewGuid().ToString('N').Substring(0, 8))"

    try {
        $arguments = "-i -silent_error_mode -force_flushlog -always_active -r4 -fsltx $fsConfigName" +
            " -start `"server($LoadSave/single/alife/load)`"" +
            " -`$ run_string dofile('$scriptPath')()"
        $runStartedUtc = [DateTime]::UtcNow

        & $helper `
            -FilePath $engine `
            -Arguments $arguments `
            -ProcessIdFile $processIdFile `
            -LauncherIdFile $launcherIdFile `
            -ExitCodeFile $exitCodeFile `
            -DesktopName $desktopName `
            -TimeoutSeconds 600 | Out-Null
        $launcherProcessId = [int](Get-Content -LiteralPath $launcherIdFile -Raw)

        $pidDeadline = [DateTime]::UtcNow.AddSeconds(30)
        while (-not (Test-Path -LiteralPath $processIdFile -PathType Leaf)) {
            if ([DateTime]::UtcNow -ge $pidDeadline) {
                throw 'The hidden launcher did not publish the engine PID.'
            }
            Start-Sleep -Milliseconds 100
        }

        $engineProcessId = [int](Get-Content -LiteralPath $processIdFile -Raw)
        $engineProcess = Get-Process -Id $engineProcessId -ErrorAction Stop
        if ($engineProcess.Path -ne $engine) {
            throw "The hidden launcher started an unexpected executable: $($engineProcess.Path)"
        }
        $engineProcess.PriorityClass = [Diagnostics.ProcessPriorityClass]::High
        if (-not $engineProcess.WaitForExit(600000)) {
            throw 'The serializer compatibility engine process did not finish.'
        }

        $exitDeadline = [DateTime]::UtcNow.AddSeconds(15)
        while (-not (Test-Path -LiteralPath $exitCodeFile -PathType Leaf)) {
            if ([DateTime]::UtcNow -ge $exitDeadline) {
                throw 'The hidden launcher did not publish the engine exit code.'
            }
            Start-Sleep -Milliseconds 100
        }

        $exitCode = [uint32](Get-Content -LiteralPath $exitCodeFile -Raw)
        if ($exitCode -ne 0) {
            throw "The serializer compatibility run exited with code $exitCode."
        }

        $launcher = Get-Process -Id $launcherProcessId -ErrorAction SilentlyContinue
        if ($launcher -and -not $launcher.WaitForExit(15000)) {
            throw 'The hidden serializer compatibility launcher did not exit.'
        }
        if (Get-Process xrEngine -ErrorAction SilentlyContinue | Where-Object Path -EQ $engine) {
            throw 'The serializer compatibility engine is still running after completion.'
        }

        $log = Get-ChildItem -LiteralPath $logsRoot -File |
            Sort-Object LastWriteTimeUtc -Descending |
            Select-Object -First 1
        if (-not $log -or $log.LastWriteTimeUtc -lt $runStartedUtc.AddSeconds(-2)) {
            throw 'The serializer compatibility run did not produce a fresh engine log.'
        }
        $logDestination = Join-Path $SuiteResult 'engine.log'
        Copy-Item -LiteralPath $log.FullName -Destination $logDestination -Force
        return $logDestination
    }
    finally {
        # Stop only processes created by this harness if a failure interrupts normal shutdown.
        if ($engineProcessId -le 0 -and (Test-Path -LiteralPath $processIdFile -PathType Leaf)) {
            [void][int]::TryParse((Get-Content -LiteralPath $processIdFile -Raw), [ref]$engineProcessId)
        }
        if ($launcherProcessId -le 0 -and (Test-Path -LiteralPath $launcherIdFile -PathType Leaf)) {
            [void][int]::TryParse((Get-Content -LiteralPath $launcherIdFile -Raw), [ref]$launcherProcessId)
        }
        foreach ($ownedProcessId in @($engineProcessId, $launcherProcessId)) {
            if ($ownedProcessId -le 0) {
                continue
            }
            $ownedProcess = Get-Process -Id $ownedProcessId -ErrorAction SilentlyContinue
            if ($ownedProcess -and -not $ownedProcess.HasExited) {
                Stop-Process -Id $ownedProcessId -Force -ErrorAction SilentlyContinue
                $ownedProcess.WaitForExit(15000)
            }
        }
        Remove-Item -LiteralPath $fsConfigRuntime -Force -ErrorAction SilentlyContinue
    }
}

$legacyCases = @(
    [pscustomobject]@{
        Name = 'compat_legacy_noop_existing'
        Previous = [Text.Encoding]::ASCII.GetBytes('legacy-noop-existing:v1')
        Expected = [Text.Encoding]::ASCII.GetBytes('legacy-noop-existing:v1')
    },
    [pscustomobject]@{ Name = 'compat_legacy_noop_absent'; Previous = $null; Expected = $null },
    [pscustomobject]@{
        Name = 'compat_legacy_append_existing'
        Previous = [Text.Encoding]::ASCII.GetBytes('legacy-append-existing:v1')
        Expected = [byte[]]([Text.Encoding]::ASCII.GetBytes('legacy-append-existing:v1') +
            [Text.Encoding]::ASCII.GetBytes('|lua-append-existing'))
    },
    [pscustomobject]@{
        Name = 'compat_legacy_append_absent'
        Previous = $null
        Expected = [Text.Encoding]::ASCII.GetBytes('lua-append-absent')
    },
    [pscustomobject]@{
        Name = 'compat_legacy_delete_existing'
        Previous = [Text.Encoding]::ASCII.GetBytes('legacy-delete-existing:v1')
        Expected = $null
    },
    [pscustomobject]@{ Name = 'compat_legacy_delete_absent'; Previous = $null; Expected = $null },
    [pscustomobject]@{
        Name = 'compat_legacy_create_existing'
        Previous = [Text.Encoding]::ASCII.GetBytes('legacy-create-existing:old')
        Expected = [Text.Encoding]::ASCII.GetBytes('legacy-create-existing:new')
    },
    [pscustomobject]@{
        Name = 'compat_legacy_create_absent'
        Previous = $null
        Expected = [Text.Encoding]::ASCII.GetBytes('legacy-create-absent:new')
    },
    [pscustomobject]@{
        Name = 'compat_legacy_zero_existing'
        Previous = [Text.Encoding]::ASCII.GetBytes('legacy-zero-existing:old')
        Expected = [byte[]]::new(0)
    },
    [pscustomobject]@{
        Name = 'compat_legacy_zero_absent'
        Previous = $null
        Expected = [byte[]]::new(0)
    }
)

function Invoke-LegacySuite {
    $suiteResult = Join-Path $ResultRoot 'legacy'
    New-Item -ItemType Directory -Path $suiteResult -Force | Out-Null

    foreach ($case in $legacyCases) {
        Remove-SaveFamily -BaseName $case.Name
        if ($null -ne $case.Previous) {
            [IO.File]::WriteAllBytes((Join-Path $savedGamesRoot "$($case.Name).scoc"), $case.Previous)
        }
    }

    $initialManifest = foreach ($case in $legacyCases) {
        Get-FileRecord -Path (Join-Path $savedGamesRoot "$($case.Name).scoc")
    }
    $initialManifest | ConvertTo-Json -Depth 3 |
        Set-Content -LiteralPath (Join-Path $suiteResult 'initial-scoc.json') -Encoding utf8

    $logPath = Invoke-HiddenSuite -ScriptFile 'legacy_semantics.lua' -SuiteResult $suiteResult
    $logText = Get-Content -LiteralPath $logPath -Raw
    $fatalPattern = 'QA_SERIALIZER_COMPAT_ERROR|FATAL ERROR|stack trace:|Expression\s*:|Lua error|' +
        'reader overflow|Asynchronous save transaction failed|Script save capture failed|' +
        'Save-extension capture failed|Concurrent save capture request was rejected|Discarded [0-9]+'
    if ($logText -notmatch 'QA_SERIALIZER_COMPAT_LEGACY_DONE' -or $logText -match $fatalPattern) {
        throw 'Legacy compatibility QA failed its completion or fatal-error gate.'
    }

    foreach ($case in $legacyCases) {
        $escapedName = [regex]::Escape($case.Name)
        if ([regex]::Matches($logText, "QA_SERIALIZER_COMPAT_LEGACY_CALLBACK $escapedName ").Count -ne 1 -or
            [regex]::Matches($logText, "QA_SERIALIZER_COMPAT_LEGACY_COMMITTED $escapedName").Count -ne 1 -or
            [regex]::Matches($logText, "Game $escapedName\.scop is successfully saved").Count -ne 1) {
            throw "Legacy compatibility log proof is incomplete for $($case.Name)."
        }

        foreach ($extension in @('.scop', '.scov')) {
            $path = Join-Path $savedGamesRoot "$($case.Name)$extension"
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                throw "Legacy compatibility QA is missing $path."
            }
        }
        Assert-ExactBytes -Path (Join-Path $savedGamesRoot "$($case.Name).scoc") -Expected $case.Expected
    }

    $baseNames = [string[]]$legacyCases.Name
    Assert-NoTransientFiles -BaseNames $baseNames
    Copy-SaveEvidence -Destination (Join-Path $suiteResult 'savedgames') -BaseNames $baseNames
    $finalManifest = foreach ($case in $legacyCases) {
        foreach ($extension in @('.scop', '.scoc', '.scov')) {
            Get-FileRecord -Path (Join-Path $savedGamesRoot "$($case.Name)$extension")
        }
    }
    $finalManifest | ConvertTo-Json -Depth 3 |
        Set-Content -LiteralPath (Join-Path $suiteResult 'final-save-group.json') -Encoding utf8
}

function Invoke-EncodeFalseSuite {
    $suiteResult = Join-Path $ResultRoot 'encode-false'
    $targetName = 'compat_encode_false'
    New-Item -ItemType Directory -Path $suiteResult -Force | Out-Null
    Remove-SaveFamily -BaseName $targetName

    foreach ($extension in @('.scop', '.scoc', '.scov')) {
        Copy-Item -LiteralPath (Join-Path $FixtureRoot "$LoadSave$extension") `
            -Destination (Join-Path $savedGamesRoot "$targetName$extension") -Force
    }
    $before = foreach ($extension in @('.scop', '.scoc', '.scov')) {
        Get-FileRecord -Path (Join-Path $savedGamesRoot "$targetName$extension")
    }
    $before | ConvertTo-Json -Depth 3 |
        Set-Content -LiteralPath (Join-Path $suiteResult 'before-save-group.json') -Encoding utf8

    $logPath = Invoke-HiddenSuite -ScriptFile 'encode_false_preservation.lua' -SuiteResult $suiteResult
    $logText = Get-Content -LiteralPath $logPath -Raw
    $fatalPattern = 'QA_SERIALIZER_COMPAT_ERROR|FATAL ERROR|stack trace:|Expression\s*:|Lua error|' +
        'reader overflow|Asynchronous save transaction failed|Save-extension capture failed|' +
        'Concurrent save capture request was rejected|Discarded [0-9]+'
    if ($logText -notmatch 'QA_SERIALIZER_COMPAT_ENCODE_FALSE_DONE' -or $logText -match $fatalPattern) {
        throw 'Encode-false compatibility QA failed its completion or fatal-error gate.'
    }
    if ([regex]::Matches($logText, 'QA_SERIALIZER_COMPAT_ENCODE_FALSE').Count -ne 2 -or
        [regex]::Matches($logText, "Script save capture failed for '$targetName(?:\.scop)?'").Count -ne 1 -or
        [regex]::Matches($logText, "Game $targetName\.scop is successfully saved").Count -ne 0) {
        throw 'Encode-false compatibility log proof is incomplete.'
    }

    $after = foreach ($extension in @('.scop', '.scoc', '.scov')) {
        Get-FileRecord -Path (Join-Path $savedGamesRoot "$targetName$extension")
    }
    for ($index = 0; $index -lt $before.Count; ++$index) {
        if (-not $after[$index].Present -or
            $after[$index].Length -ne $before[$index].Length -or
            $after[$index].SHA256 -ne $before[$index].SHA256) {
            throw "Encode-false changed $($before[$index].Name)."
        }
    }
    $after | ConvertTo-Json -Depth 3 |
        Set-Content -LiteralPath (Join-Path $suiteResult 'after-save-group.json') -Encoding utf8
    Assert-NoTransientFiles -BaseNames @($targetName)
    Copy-SaveEvidence -Destination (Join-Path $suiteResult 'savedgames') -BaseNames @($targetName)
}

function Invoke-MalformedScovSuite {
    $suiteResult = Join-Path $ResultRoot 'malformed-scov'
    $targetName = 'compat_malformed_scov'
    New-Item -ItemType Directory -Path $suiteResult -Force | Out-Null
    Remove-SaveFamily -BaseName $targetName

    foreach ($extension in @('.scop', '.scoc', '.scov')) {
        Copy-Item -LiteralPath (Join-Path $FixtureRoot "$LoadSave$extension") `
            -Destination (Join-Path $savedGamesRoot "$targetName$extension") -Force
    }
    [IO.File]::WriteAllBytes(
        (Join-Path $savedGamesRoot "$targetName.scov"), [byte[]](0x53, 0x4F, 0x56, 0x31))

    $before = foreach ($extension in @('.scop', '.scoc', '.scov')) {
        Get-FileRecord -Path (Join-Path $savedGamesRoot "$targetName$extension")
    }
    $before | ConvertTo-Json -Depth 3 |
        Set-Content -LiteralPath (Join-Path $suiteResult 'before-load-group.json') -Encoding utf8

    $logPath = Invoke-HiddenSuite -ScriptFile 'malformed_scov_preservation.lua' -SuiteResult $suiteResult
    $logText = Get-Content -LiteralPath $logPath -Raw
    $fatalPattern = 'QA_SERIALIZER_COMPAT_ERROR|FATAL ERROR|stack trace:|Expression\s*:|Lua error|reader overflow'
    if ([regex]::Matches($logText, 'QA_SERIALIZER_COMPAT_MALFORMED_REQUESTED').Count -ne 1 -or
        [regex]::Matches($logText, 'QA_SERIALIZER_COMPAT_MALFORMED_SURVIVED').Count -ne 1 -or
        [regex]::Matches($logText, "Cannot load save extensions '.*$targetName\.scov'").Count -ne 1 -or
        $logText -match "Game $targetName(?:\.scop)? is successfully loaded" -or
        $logText -match $fatalPattern) {
        throw 'Malformed-sidecar compatibility QA failed its preservation gate.'
    }

    $after = foreach ($extension in @('.scop', '.scoc', '.scov')) {
        Get-FileRecord -Path (Join-Path $savedGamesRoot "$targetName$extension")
    }
    for ($index = 0; $index -lt $before.Count; ++$index) {
        if (-not $after[$index].Present -or
            $after[$index].Length -ne $before[$index].Length -or
            $after[$index].SHA256 -ne $before[$index].SHA256) {
            throw "Malformed-sidecar load changed $($before[$index].Name)."
        }
    }
    $after | ConvertTo-Json -Depth 3 |
        Set-Content -LiteralPath (Join-Path $suiteResult 'after-load-group.json') -Encoding utf8
    Assert-NoTransientFiles -BaseNames @($targetName)
    Copy-SaveEvidence -Destination (Join-Path $suiteResult 'savedgames') -BaseNames @($targetName)
}

foreach ($requiredPath in @($helper, $fsConfigTemplate)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Missing serializer compatibility QA dependency: $requiredPath"
    }
}

Assert-X64Engine
Assert-CurrentDeployment
$userSaveBaseline = ConvertTo-Json -InputObject @(Get-DirectoryManifest -Root $userSavedGamesRoot) -Depth 3 -Compress
try {
    Copy-LoadFixture

    if ($Suite -in @('All', 'Legacy')) {
        Invoke-LegacySuite
    }
    if ($Suite -in @('All', 'EncodeFalse')) {
        Invoke-EncodeFalseSuite
    }
    if ($Suite -in @('All', 'MalformedScov')) {
        Invoke-MalformedScovSuite
    }
}
finally {
    $userSaveAfter = ConvertTo-Json -InputObject @(Get-DirectoryManifest -Root $userSavedGamesRoot) -Depth 3 -Compress
    if ($userSaveAfter -ne $userSaveBaseline) {
        throw 'The serializer compatibility QA detected a change in the user save directory.'
    }
}

[pscustomobject]@{
    Suite = $Suite
    ResultRoot = $ResultRoot
    FixtureRoot = $FixtureRoot
    AppDataRoot = $appDataRoot
    Engine = $engine
    EngineSHA256 = (Get-FileHash -LiteralPath $engine -Algorithm SHA256).Hash
    UserSaveRootTouched = $false
}
