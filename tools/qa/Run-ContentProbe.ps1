# Boots the QA rig once with a console probe appended to user.ltx and prints what the content
# system said. The rig is a full copy of the install, so this exercises the real startup path -
# FS init, the mount gate, ContentService::Initialize - rather than a unit harness.
[CmdletBinding()]
param(
    [string]$Rig = "D:\Games\Dead Air\_qa\lightdiag",
    [string]$Label = "probe",
    [string[]]$Commands = @(),
    # When set, the probe does NOT append `quit`: the engine keeps running for this many
    # seconds and is then killed. Needed for anything that happens on the loading-event queue,
    # which never gets a frame if `quit` is issued in the same user.ltx batch.
    [int]$RunSeconds = 0,
    [int]$TimeoutSeconds = 180
)

$ErrorActionPreference = "Stop"

$userLtx = Join-Path $Rig "appdata\user.ltx"
$backup = "$userLtx.content-probe-backup"
$log = Join-Path $Rig "appdata\logs\openxray_admin.log"

if (-not (Test-Path $backup)) {
    Copy-Item $userLtx $backup -Force
}

# Always rebuild the probe from the pristine copy: a previous run's `quit` must not accumulate.
$lines = [IO.File]::ReadAllLines($backup)
$tail = if ($RunSeconds -gt 0) { @("dar_content_state", "flush") } else { @("dar_content_state", "flush", "quit") }
$probe = @($lines) + $Commands + $tail
[IO.File]::WriteAllLines($userLtx, $probe)

if (Test-Path $log) { Remove-Item $log -Force }

$process = Start-Process -FilePath (Join-Path $Rig "xrEngine.exe") -WorkingDirectory $Rig `
    -ArgumentList "-noprefetch", "-nointro" -PassThru -WindowStyle Minimized
if ($RunSeconds -gt 0) {
    $exited = $process.WaitForExit($RunSeconds * 1000)
    if (-not $exited) { $process.Kill(); $process.WaitForExit(10000) }
} else {
    $exited = $process.WaitForExit($TimeoutSeconds * 1000)
    if (-not $exited) {
        $process.Kill()
        Write-Warning "engine did not exit within $TimeoutSeconds s - killed"
    }
}

Copy-Item $backup $userLtx -Force

Write-Host "=== $Label (exit $(if ($exited) { $process.ExitCode } else { 'timeout' })) ==="
if (-not (Test-Path $log)) {
    Write-Warning "no log produced"
    return
}
Select-String -Path $log -Pattern "\[content\]|Cannot start a level|FS: .* archives|FATAL|stack trace" -CaseSensitive:$false |
    ForEach-Object { $_.Line }
