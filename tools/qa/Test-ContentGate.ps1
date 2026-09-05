# The Batch 2 verification matrix, end to end on the QA rig.
#
# Every case mutates the installation, boots the real engine, and reads what the content system
# said. Nothing here is a unit test: the point is that the mount gate, the verifier, the latch
# and the play gate agree with each other on a real installation.
[CmdletBinding()]
param(
    [string]$Rig = "D:\Games\Dead Air\_qa\lightdiag"
)

$ErrorActionPreference = "Stop"
$probe = Join-Path $PSScriptRoot "Run-ContentProbe.ps1"
$db = Join-Path $Rig "database"
$meta = Join-Path $Rig ".dead-air-x64"
$latch = Join-Path $meta "content-incomplete.txt"
$manifest = Join-Path $meta "content-manifest.txt"
# The sounds bundle to mutilate is read from the installed manifest: its name carries the hash
# of its bytes and changes with every content release, so a written-down name goes stale.
$tab = [string][char]9
$soundsRow = Get-Content -LiteralPath $manifest | Where-Object {
    $_ -match ($tab + "xtra_dead_air_x64_content_sounds_00_[0-9a-f]{16}.xdb0" + $tab)
} | Select-Object -First 1
if (-not $soundsRow) { throw "the installed manifest declares no sounds_00 bundle" }
$soundsName = @($soundsRow -split $tab | Where-Object { $_ -like "xtra_dead_air_x64_content_sounds_00_*" })[0]
$sounds = Join-Path $db $soundsName
$soundsSize = @($soundsRow -split $tab)[1]
# How many bundles the installed manifest declares: the healthy-install expectations quote it.
$bundleCount = @(Get-Content -LiteralPath $manifest | Where-Object { $_ -match "^[0-9a-f]{64}" + $tab }).Count
if ($bundleCount -lt 1) { throw "the installed manifest declares no bundles" }
$start = @("start server(all/single/alife/new)")

$failures = [Collections.Generic.List[string]]::new()

function Invoke-Case {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][scriptblock]$Arrange,
        [Parameter(Mandatory)][scriptblock]$Cleanup,
        [string[]]$Expect = @(),
        [string[]]$Reject = @(),
        [int]$RunSeconds = 0,
        # A log line that ends the run early: the hash pass over the real content set takes a
        # minute or more after the level has loaded, so a case that needs its verdict waits for
        # the line that carries it instead of quitting on a fixed clock.
        [string]$WaitFor = ""
    )

    & $Arrange
    try {
        $probeArgs = @("-Label", $Name, "-Commands", $script:start, "-RunSeconds", $RunSeconds)
        if ($WaitFor) { $probeArgs += @("-WaitFor", $WaitFor) }
        $output = & pwsh -NoProfile -File $probe @probeArgs | Out-String
    }
    finally {
        & $Cleanup
    }

    Write-Host "--- $Name"
    foreach ($pattern in $Expect) {
        if ($output -notmatch [regex]::Escape($pattern)) {
            $script:failures.Add("$Name : expected '$pattern'")
            Write-Host "    MISSING: $pattern" -ForegroundColor Red
        }
    }
    foreach ($pattern in $Reject) {
        if ($output -match [regex]::Escape($pattern)) {
            $script:failures.Add("$Name : must not contain '$pattern'")
            Write-Host "    UNEXPECTED: $pattern" -ForegroundColor Red
        }
    }
    if ($output -match "stack trace|FATAL ERROR") {
        $script:failures.Add("$Name : the engine crashed")
        Write-Host "    CRASHED" -ForegroundColor Red
    }
    $output -split "`r?`n" | Where-Object { $_ -match "\[content\]|Cannot start a level" } |
        ForEach-Object { Write-Host "    $_" }
}

$noop = { }
$clearLatch = { Remove-Item -LiteralPath $latch -Force -ErrorAction SilentlyContinue }

Invoke-Case -Name "healthy install" -Arrange $noop -Cleanup $noop `
    -Expect @("$bundleCount bundle(s) present", "verified $bundleCount of $bundleCount") `
    -Reject @("Cannot start a level", "skipped") -RunSeconds 150 -WaitFor "] verified"

Invoke-Case -Name "second launch uses the state cache" -Arrange $noop -Cleanup $noop `
    -Expect @("$bundleCount bundle(s), 0 hashed") -Reject @("Cannot start a level") `
    -RunSeconds 150 -WaitFor "] verified"

Invoke-Case -Name "missing bundle" `
    -Arrange { Move-Item -LiteralPath $sounds "$sounds.stash" -Force } `
    -Cleanup { Move-Item -LiteralPath "$sounds.stash" $sounds -Force; & $clearLatch } `
    -Expect @("is missing", "Cannot start a level") -RunSeconds 45

Invoke-Case -Name "truncated bundle" `
    -Arrange {
        Copy-Item -LiteralPath $sounds "$sounds.stash" -Force
        $bytes = [IO.File]::ReadAllBytes($sounds)
        [IO.File]::WriteAllBytes($sounds, $bytes[0..($bytes.Length - 4097)])
    } `
    -Cleanup { Move-Item -LiteralPath "$sounds.stash" $sounds -Force; & $clearLatch } `
    -Expect @("size does not match the manifest", "the manifest says $soundsSize", "Cannot start a level") `
    -RunSeconds 45

Invoke-Case -Name "corrupt index, correct size" `
    -Arrange {
        Copy-Item -LiteralPath $sounds "$sounds.stash" -Force
        $bytes = [IO.File]::ReadAllBytes($sounds)
        $bytes[4] = 0xFF; $bytes[5] = 0xFF; $bytes[6] = 0xFF; $bytes[7] = 0x7F
        [IO.File]::WriteAllBytes($sounds, $bytes)
    } `
    -Cleanup { Move-Item -LiteralPath "$sounds.stash" $sounds -Force; & $clearLatch } `
    -Expect @("invalid index", "Cannot start a level") -RunSeconds 45

Invoke-Case -Name "flipped data byte" `
    -Arrange {
        Copy-Item -LiteralPath $sounds "$sounds.stash" -Force
        $bytes = [IO.File]::ReadAllBytes($sounds)
        $mid = [int]($bytes.Length * 0.8)
        $bytes[$mid] = $bytes[$mid] -bxor 0xFF
        [IO.File]::WriteAllBytes($sounds, $bytes)
    } `
    -Cleanup { Move-Item -LiteralPath "$sounds.stash" $sounds -Force; & $clearLatch } `
    -Expect @("does not match the manifest") -RunSeconds 150 -WaitFor "does not match the manifest"

Invoke-Case -Name "stale and unrecognised leftovers do not block play" `
    -Arrange {
        Copy-Item -LiteralPath $sounds (Join-Path $db "xtra_dead_air_x64_content_sounds_00_deadbeefdeadbeef.xdb0") -Force
        Copy-Item -LiteralPath $sounds (Join-Path $db "xtra_dead_air_x64_content_weather_07_0123456789abcdef.xdb0") -Force
    } `
    -Cleanup {
        Remove-Item -LiteralPath (Join-Path $db "xtra_dead_air_x64_content_sounds_00_deadbeefdeadbeef.xdb0"),
            (Join-Path $db "xtra_dead_air_x64_content_weather_07_0123456789abcdef.xdb0") -Force
        & $clearLatch
    } `
    -Expect @("stale bundle", "unrecognised bundle-shaped archive") `
    -Reject @("Cannot start a level")

Invoke-Case -Name "no manifest is recovery, not silence" `
    -Arrange { Move-Item -LiteralPath $manifest "$manifest.stash" -Force } `
    -Cleanup { Move-Item -LiteralPath "$manifest.stash" $manifest -Force; & $clearLatch } `
    -Expect @("manifest unavailable", "state=recovery", "Cannot start a level") -RunSeconds 45

Invoke-Case -Name "a leftover latch on an intact install clears itself" `
    -Arrange {
        [IO.File]::WriteAllText($latch,
            "schema=dead-air-refined.content-incomplete/1`nversion=1.4.0`nreason=repair-commit`ntime=1`n")
    } `
    -Cleanup { & $clearLatch } `
    -Expect @("the incomplete latch is set", "the incomplete latch is cleared") `
    -RunSeconds 150 -WaitFor "the incomplete latch is cleared"

if (Test-Path -LiteralPath $latch) {
    $failures.Add("the rig was left latched")
}

Write-Host ""
if ($failures.Count -eq 0) {
    Write-Host "all content gate cases passed" -ForegroundColor Green
}
else {
    Write-Host "FAILURES:" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}
