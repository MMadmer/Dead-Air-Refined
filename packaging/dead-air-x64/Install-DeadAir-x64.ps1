[CmdletBinding()]
param(
    [string]$GameRoot
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
        if ((Test-Path -LiteralPath (Join-Path $resolved "fsgame.ltx")) -and
            (Test-Path -LiteralPath (Join-Path $resolved "database"))) {
            return $resolved
        }
    }

    throw "Dead Air root was not found. Pass it with -GameRoot."
}

function Get-FileHashValue {
    param([string]$Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Get-ManifestEntries {
    param($Entries)

    # Packages built before the database section simply omit it.
    if (-not $Entries) {
        return ,@()
    }

    return @($Entries)
}

function Test-Amd64Pe {
    param([string]$Path)

    # Read the PE machine field without loading the target executable.
    $stream = [IO.File]::OpenRead($Path)
    try {
        $reader = [IO.BinaryReader]::new($stream)
        if ($reader.ReadUInt16() -ne 0x5A4D) {
            return $false
        }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadUInt32()
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            return $false
        }
        return $reader.ReadUInt16() -eq 0x8664
    }
    finally {
        $stream.Dispose()
    }
}

$root = Resolve-GameRoot $GameRoot
$runtimeRoot = Join-Path $PSScriptRoot "runtime"
$databaseSourceRoot = Join-Path $PSScriptRoot "database"
$databaseRoot = Join-Path $root "database"
$packageManifestPath = Join-Path $PSScriptRoot "package-manifest.json"
$controlRoot = Join-Path $root ".dead-air-x64"
$statePath = Join-Path $controlRoot "install-state.json"

if (-not (Test-Path -LiteralPath $runtimeRoot) -or -not (Test-Path -LiteralPath $packageManifestPath)) {
    throw "The package is incomplete: runtime or package-manifest.json is missing."
}

if (Test-Path -LiteralPath $statePath) {
    throw "Dead Air x64 is already installed here. Run Uninstall-DeadAir-x64.ps1 first."
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
    throw "Close Dead Air before installing the x64 runtime."
}

$packageManifest = Get-Content -LiteralPath $packageManifestPath -Raw | ConvertFrom-Json
$runtimeEntries = Get-ManifestEntries $packageManifest.Files
$databaseEntries = Get-ManifestEntries $packageManifest.DatabaseFiles
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$backupDirectory = Join-Path $controlRoot "backup-$timestamp"
$databaseBackupDirectory = Join-Path $backupDirectory "database"
$records = [Collections.Generic.List[object]]::new()
$databaseRecords = [Collections.Generic.List[object]]::new()
$installedFiles = [Collections.Generic.List[string]]::new()
$installedDatabaseFiles = [Collections.Generic.List[string]]::new()

New-Item -ItemType Directory -Path $backupDirectory -Force | Out-Null

try {
    foreach ($file in $runtimeEntries) {
        $source = Join-Path $runtimeRoot $file.Name
        $destination = Join-Path $root $file.Name

        if (-not (Test-Path -LiteralPath $source)) {
            throw "Missing runtime file: $($file.Name)"
        }
        if ((Get-FileHashValue $source) -ne $file.Sha256) {
            throw "Package hash mismatch: $($file.Name)"
        }
        if (-not (Test-Amd64Pe $source)) {
            throw "Runtime file is not AMD64 PE: $($file.Name)"
        }

        $hadOriginal = Test-Path -LiteralPath $destination
        $originalHash = $null
        if ($hadOriginal) {
            $originalHash = Get-FileHashValue $destination
            Copy-Item -LiteralPath $destination -Destination (Join-Path $backupDirectory $file.Name) -Force
        }

        $records.Add([pscustomobject]@{
            Name = $file.Name
            HadOriginal = $hadOriginal
            OriginalSha256 = $originalHash
            InstalledSha256 = $file.Sha256
        })
    }

    foreach ($file in $databaseEntries) {
        $source = Join-Path $databaseSourceRoot $file.Name
        $destination = Join-Path $databaseRoot $file.Name

        if (-not (Test-Path -LiteralPath $source)) {
            throw "Missing database file: $($file.Name)"
        }
        if ((Get-FileHashValue $source) -ne $file.Sha256) {
            throw "Package hash mismatch: database\$($file.Name)"
        }

        $hadOriginal = Test-Path -LiteralPath $destination
        $originalHash = $null
        if ($hadOriginal) {
            $originalHash = Get-FileHashValue $destination
            New-Item -ItemType Directory -Path $databaseBackupDirectory -Force | Out-Null
            Copy-Item -LiteralPath $destination -Destination (Join-Path $databaseBackupDirectory $file.Name) -Force
        }

        $databaseRecords.Add([pscustomobject]@{
            Name = $file.Name
            HadOriginal = $hadOriginal
            OriginalSha256 = $originalHash
            InstalledSha256 = $file.Sha256
        })
    }

    foreach ($file in $runtimeEntries) {
        $source = Join-Path $runtimeRoot $file.Name
        $destination = Join-Path $root $file.Name
        Copy-Item -LiteralPath $source -Destination $destination -Force
        $installedFiles.Add($destination)
        if ((Get-FileHashValue $destination) -ne $file.Sha256) {
            throw "Installed file verification failed: $($file.Name)"
        }
    }

    foreach ($file in $databaseEntries) {
        $source = Join-Path $databaseSourceRoot $file.Name
        $destination = Join-Path $databaseRoot $file.Name
        Copy-Item -LiteralPath $source -Destination $destination -Force
        $installedDatabaseFiles.Add($destination)
        if ((Get-FileHashValue $destination) -ne $file.Sha256) {
            throw "Installed file verification failed: database\$($file.Name)"
        }
    }

    $state = [pscustomobject]@{
        Version = $packageManifest.Version
        InstalledAt = (Get-Date).ToString("o")
        BackupDirectory = $backupDirectory
        Files = $records
        DatabaseFiles = $databaseRecords
    }
    $state | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $statePath -Encoding UTF8
}
catch {
    foreach ($record in $databaseRecords) {
        $destination = Join-Path $databaseRoot $record.Name
        $backup = Join-Path $databaseBackupDirectory $record.Name
        if ($record.HadOriginal -and (Test-Path -LiteralPath $backup)) {
            Copy-Item -LiteralPath $backup -Destination $destination -Force
        }
        elseif ($installedDatabaseFiles.Contains($destination) -and (Test-Path -LiteralPath $destination)) {
            Remove-Item -LiteralPath $destination -Force
        }
    }
    foreach ($record in $records) {
        $destination = Join-Path $root $record.Name
        $backup = Join-Path $backupDirectory $record.Name
        if ($record.HadOriginal -and (Test-Path -LiteralPath $backup)) {
            Copy-Item -LiteralPath $backup -Destination $destination -Force
        }
        elseif ($installedFiles.Contains($destination) -and (Test-Path -LiteralPath $destination)) {
            Remove-Item -LiteralPath $destination -Force
        }
    }
    throw
}

Write-Host "Dead Air x64 $($packageManifest.Version) installed successfully."
Write-Host "Game root: $root"
foreach ($record in $databaseRecords) {
    Write-Host "Database archive installed: $($record.Name)"
}
Write-Host "Saves, gamedata, MODS, JSGME state and other database archives were not changed."

