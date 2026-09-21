# Boots the QA rig once with a console probe appended to user.ltx and prints what the content
# system said. The rig is a full copy of the install, so this exercises the real startup path -
# FS init, the mount gate, ContentService::Initialize - rather than a unit harness.
[CmdletBinding()]
param(
    [string]$Rig = "D:\Games\Dead Air\_qa\lightdiag",
    [string]$Label = "probe",
    [string[]]$Commands = @(),
    # When set, the probe does NOT append `quit`: the engine keeps running and is then killed.
    # Needed for anything that happens on the loading-event queue, which never gets a frame if
    # `quit` is issued in the same user.ltx batch.
    #
    # This is a CEILING, not a duration. With -WaitFor the probe stops as soon as the log says
    # what it was waiting for; a level that loads in eight seconds should cost eight seconds,
    # not the ceiling. Without -WaitFor there is nothing to watch for and it does sleep.
    [int]$RunSeconds = 0,
    # Regex the probe polls the log for while the engine runs. The moment it matches, the engine
    # is killed and the probe returns. Poll rather than wait on the process, because the engine
    # is not going to exit on its own - the whole point of -RunSeconds is that it keeps running.
    [string]$WaitFor,
    [int]$TimeoutSeconds = 180
)

$ErrorActionPreference = "Stop"

$matched = $false
$userLtx = Join-Path $Rig "appdata\user.ltx"
$backup = "$userLtx.content-probe-backup"
$log = Join-Path $Rig "appdata\logs\xfined-ray_admin.log"

if (-not (Test-Path $backup)) {
    Copy-Item $userLtx $backup -Force
}

# Always rebuild the probe from the pristine copy: a previous run's `quit` must not accumulate.
$lines = [IO.File]::ReadAllLines($backup)
$tail = if ($RunSeconds -gt 0) { @("dar_content_state", "flush") } else { @("dar_content_state", "flush", "quit") }
$probe = @($lines) + $Commands + $tail
[IO.File]::WriteAllLines($userLtx, $probe)

if (Test-Path $log) { Remove-Item $log -Force }

# On the hidden desktop, like the rest of the QA tooling. A minimised window is still a window:
# it takes focus on creation and sits in the taskbar, which is unacceptable for something that
# runs while somebody is working on the machine.
$idFile = Join-Path ([IO.Path]::GetTempPath()) "dar-probe-engine.pid"
Remove-Item -LiteralPath $idFile -Force -ErrorAction SilentlyContinue
$launcherFile = Join-Path ([IO.Path]::GetTempPath()) "dar-probe-launcher.pid"
& (Join-Path $PSScriptRoot "Start-DetachedHiddenDesktopProcess.ps1") `
    -FilePath (Join-Path $Rig "xrEngine.exe") -Arguments "-noprefetch -nointro" `
    -ProcessIdFile $idFile -LauncherIdFile $launcherFile -TimeoutSeconds $TimeoutSeconds | Out-Null

# The launcher is detached, so the pid file appears a moment after it returns. Poll rather
# than sleep a guessed amount: the engine may still be creating the desktop.
$process = $null
$waitPid = [Diagnostics.Stopwatch]::StartNew()
while ($waitPid.Elapsed.TotalSeconds -lt 60) {
    if (Test-Path -LiteralPath $idFile) {
        $raw = (Get-Content -LiteralPath $idFile -Raw -ErrorAction SilentlyContinue)
        if ($raw -and $raw.Trim()) {
            $process = Get-Process -Id ([int]$raw.Trim()) -ErrorAction SilentlyContinue
            if ($process) { break }
        }
    }
    Start-Sleep -Milliseconds 200
}
if (-not $process) {
    throw "The engine did not start on the hidden desktop (no pid in $idFile after 60 s)."
}
if ($RunSeconds -gt 0) {
    if ($WaitFor) {
        # 250 ms is under a frame at any playable rate, so the poll cannot be what makes a fast
        # load look slow. The engine appends to the log as it goes, so reading it while the
        # process holds it open is fine - and a torn read just misses the line for one tick.
        $deadline = [Diagnostics.Stopwatch]::StartNew()
        $matched = $false
        while ($deadline.Elapsed.TotalSeconds -lt $RunSeconds) {
            if ($process.HasExited) { break }
            if ((Test-Path $log) -and (Select-String -Path $log -Pattern $WaitFor -Quiet -ErrorAction SilentlyContinue)) {
                $matched = $true
                break
            }
            Start-Sleep -Milliseconds 250
        }
        Write-Host ("waited {0:N1}s for /{1}/{2}" -f $deadline.Elapsed.TotalSeconds, $WaitFor,
            $(if ($matched) { "" } else { " - NOT SEEN, ceiling reached" }))
    }
    else {
        $process.WaitForExit($RunSeconds * 1000) | Out-Null
    }
    $exited = $process.HasExited
    if (-not $exited) { $process.Kill(); $process.WaitForExit(10000) | Out-Null }
} else {
    $exited = $process.WaitForExit($TimeoutSeconds * 1000)
    if (-not $exited) {
        $process.Kill()
        Write-Warning "engine did not exit within $TimeoutSeconds s - killed"
    }
}

Copy-Item $backup $userLtx -Force

# A probe stopped because the log said what it was waiting for did not time out, and calling
# it a timeout hides real timeouts among the successes.
$outcome = if ($exited) { "exit $($process.ExitCode)" }
    elseif ($matched) { "stopped at the marker" }
    elseif ($RunSeconds -gt 0) { "stopped at the ${RunSeconds}s ceiling" }
    else { "TIMEOUT" }
Write-Host "=== $Label ($outcome) ==="
if (-not (Test-Path $log)) {
    Write-Warning "no log produced"
    return
}
Select-String -Path $log -Pattern "\[content\]|Cannot start a level|FS: .* archives|FATAL|stack trace" -CaseSensitive:$false |
    ForEach-Object { $_.Line }
