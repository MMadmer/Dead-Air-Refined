# Shared locks of the DirectX 12 feature work (PLAN.md 0.3-0.5): one ninja build and one stand run at
# a time. Dot-source it:
#
#   . tools\qa\dx12\Dx12Locks.ps1
#   Invoke-Ninja 'xrRenderPC_R5 xrRenderPC_R4'
#
# The lock directory is the one the session's scratchpad locked.ps1 uses, so older helpers and other
# agents stay mutually excluded. $env:DX12_LOCK_DIR overrides it. Lock order: ninja.lock before
# stand.lock, never the other way round.
$script:Dx12LockDir = if ($env:DX12_LOCK_DIR) { $env:DX12_LOCK_DIR } else {
    'C:\Users\admin\AppData\Local\Temp\claude\D--Games-Dead-Air-DeadAir-x64\663ffd4e-302f-4d92-9244-b15c665cf3ee\scratchpad'
}
$script:Dx12Repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..')).Path

# A lock file carries its holder (pid, process start, tag). A holder that died leaves its lock behind;
# such a lock is taken over. Empty files (the older locked.ps1) are never judged stale.
function Test-Dx12LockStale([string]$path) {
    $text = Get-Content -LiteralPath $path -Raw -ErrorAction SilentlyContinue
    if (-not $text -or $text -notmatch 'pid=(\d+) start=(\d+)') { return $false }
    $holder = Get-Process -Id ([int]$Matches[1]) -ErrorAction SilentlyContinue
    if (-not $holder) { return $true }
    try { return $holder.StartTime.ToUniversalTime().Ticks -ne [long]$Matches[2] } catch { return $false }
}

function Enter-Lock([string]$name, [string]$tag = $env:DX12_LANE) {
    New-Item -ItemType Directory -Force -Path $script:Dx12LockDir | Out-Null
    $path = Join-Path $script:Dx12LockDir "$name.lock"
    # A waiter leaves a marker, so a holder that loops over runs yields the lock between them.
    $marker = Join-Path $script:Dx12LockDir "$name.wait-$PID"
    $self = Get-Process -Id $PID
    $stamp = "pid=$PID start=$($self.StartTime.ToUniversalTime().Ticks) tag=$tag since=$(Get-Date -Format s)"
    $waited = 0
    try {
        while ($true) {
            try {
                $stream = [IO.File]::Open($path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
                # "since" is the moment the lock was taken, not when the wait began.
                $bytes = [Text.Encoding]::ASCII.GetBytes(($stamp -replace 'since=\S+$', "since=$(Get-Date -Format s)"))
                $stream.Write($bytes, 0, $bytes.Length)
                $stream.Dispose()
                return $path
            }
            catch [IO.IOException] {
                if (-not (Test-Path -LiteralPath $marker)) { Set-Content -LiteralPath $marker -Value $stamp -ErrorAction SilentlyContinue }
                $text = Get-Content -LiteralPath $path -Raw -ErrorAction SilentlyContinue
                if (Test-Dx12LockStale $path) {
                    # Moved aside first: a lock somebody created in the meantime is put back, not deleted.
                    $aside = "$path.stale-$PID"
                    try { [IO.File]::Move($path, $aside) } catch { continue }
                    if ((Get-Content -LiteralPath $aside -Raw -ErrorAction SilentlyContinue) -eq $text) {
                        Write-Host "took over stale $name.lock ($text)"
                        Remove-Item -LiteralPath $aside -Force -ErrorAction SilentlyContinue
                    } else {
                        try { [IO.File]::Move($aside, $path) } catch { Remove-Item -LiteralPath $aside -Force -ErrorAction SilentlyContinue }
                    }
                    continue
                }
                Start-Sleep -Seconds 5
                $waited += 5
                if ($waited % 60 -eq 0) { Write-Host "waiting for $name.lock ($waited s): $text" }
            }
        }
    } finally { Remove-Item -LiteralPath $marker -Force -ErrorAction SilentlyContinue }
}

# Releases the lock; when another live process is waiting for it, pauses longer than a waiter's poll
# so that a caller looping over runs or builds cannot starve the others.
function Exit-Lock([string]$path) {
    if (-not $path) { return }
    Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    $name = [IO.Path]::GetFileNameWithoutExtension($path)
    $waiters = Get-ChildItem -LiteralPath $script:Dx12LockDir -Filter "$name.wait-*" -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '\.wait-(\d+)$' -and [int]$Matches[1] -ne $PID -and (Get-Process -Id ([int]$Matches[1]) -ErrorAction SilentlyContinue) }
    if ($waiters) { Start-Sleep -Seconds 7 }
}

# Runs a command line inside the Visual Studio x64 developer environment; returns its exit code.
function Invoke-VsCommand([string]$command, [string]$workDir = $script:Dx12Repo) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'A Visual Studio installation with the x64 C++ toolchain is required.' }
    $devCmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
    $bat = Join-Path $env:TEMP ('dx12vs-' + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.cmd')
    # UTF-8 console first: cl prints its localized /showIncludes lines in the console code page, and
    # under 866 (the Bash tool) ninja matches none of them and records no header dependencies.
    Set-Content -LiteralPath $bat -Encoding ASCII -Value "@chcp 65001 >nul`r`n@call `"$devCmd`" -arch=x64 -host_arch=x64 >nul`r`n@cd /d `"$workDir`"`r`n$command"
    try {
        & cmd.exe /d /c "`"$bat`"" 2>&1 | ForEach-Object { Write-Host "$_" }
        return $LASTEXITCODE
    } finally { Remove-Item -LiteralPath $bat -Force -ErrorAction SilentlyContinue }
}

# ninja under ninja.lock (-Held: the caller holds it already). Prints ninja's output, returns the exit
# code: $code = Invoke-Ninja 'xrRenderPC_R5 xrRenderPC_R4'
function Invoke-Ninja([string]$targets = '', [switch]$Held) {
    $lock = if ($Held) { $null } else { Enter-Lock 'ninja' }
    try {
        return Invoke-VsCommand "ninja -C build\ninja-x64 $targets"
    } finally { Exit-Lock $lock }
}
