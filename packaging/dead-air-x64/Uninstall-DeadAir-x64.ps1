[CmdletBinding()]
param(
    [string]$GameRoot,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

function Resolve-GameRoot {
    param([string]$RequestedRoot)

    $candidates = @()
    if ($RequestedRoot) {
        $candidates += $RequestedRoot
    }
    $candidates += (Get-Location).Path
    $candidates += (Split-Path -Parent $PSScriptRoot)

    foreach ($candidate in $candidates) {
        $resolved = [IO.Path]::GetFullPath($candidate)
        if (Test-Path -LiteralPath (Join-Path $resolved ".dead-air-x64\install-state.json")) {
            return $resolved
        }
    }

    throw "A Dead Air x64 installation state was not found. Pass the game root with -GameRoot."
}

function Get-FileHashValue {
    param([string]$Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

$root = Resolve-GameRoot $GameRoot
$controlRoot = [IO.Path]::GetFullPath((Join-Path $root ".dead-air-x64"))
$statePath = Join-Path $controlRoot "install-state.json"
$state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
$backupDirectory = [IO.Path]::GetFullPath($state.BackupDirectory)

if (-not $backupDirectory.StartsWith(
        $controlRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "The recorded backup path is outside the x64 control directory."
}

$runningEngine = Get-Process -Name "xrEngine" -ErrorAction SilentlyContinue |
    Where-Object {
        try {
            $_.Path -and [IO.Path]::GetFullPath($_.Path).StartsWith(
                $root + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase)
        }
        catch {
            $false
        }
    }
if ($runningEngine) {
    throw "Close Dead Air before uninstalling the x64 runtime."
}

if (-not $Force) {
    foreach ($record in $state.Files) {
        $destination = Join-Path $root $record.Name
        if ((Test-Path -LiteralPath $destination) -and
            (Get-FileHashValue $destination) -ne $record.InstalledSha256) {
            throw "Runtime file was modified after installation: $($record.Name). Use -Force to restore anyway."
        }
    }
}

foreach ($record in $state.Files) {
    $destination = Join-Path $root $record.Name
    $backup = Join-Path $backupDirectory $record.Name

    if ($record.HadOriginal) {
        if (-not (Test-Path -LiteralPath $backup)) {
            throw "Original runtime backup is missing: $($record.Name)"
        }
        Copy-Item -LiteralPath $backup -Destination $destination -Force
    }
    elseif (Test-Path -LiteralPath $destination) {
        Remove-Item -LiteralPath $destination -Force
    }
}

Remove-Item -LiteralPath $statePath -Force
Remove-Item -LiteralPath $backupDirectory -Recurse -Force
if (-not (Get-ChildItem -LiteralPath $controlRoot -Force)) {
    Remove-Item -LiteralPath $controlRoot -Force
}

Write-Host "Dead Air x64 was removed and the original x86 runtime was restored."
Write-Host "Content, saves, gamedata, database, MODS and JSGME state were not changed."

