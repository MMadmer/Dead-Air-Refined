[CmdletBinding()]
param(
    [string]$Version = "1.3.0",
    [string]$Configuration = "Release",
    [string]$ConverterPath = "D:\Games\Dead Air\tools\AXRToolset\bin\converter.exe",
    [string]$CompatibilityArchivePath
)

$ErrorActionPreference = "Stop"
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
. (Join-Path $PSScriptRoot "dead_air_x64_compatibility_archive.ps1")

$binaryRoot = Join-Path $repositoryRoot "bin\x64\$Configuration"
$templateRoot = Join-Path $repositoryRoot "packaging\dead-air-x64"
$artifactRoot = Join-Path $repositoryRoot "artifacts"
$packageName = "Dead-Air-Refined-$Version"
$stageRoot = Join-Path $artifactRoot $packageName
$runtimeRoot = Join-Path $stageRoot "runtime"
$databaseRoot = Join-Path $stageRoot "database"
$archivePath = Join-Path $artifactRoot "$packageName.zip"
$compatibilityWorkRoot = Join-Path $repositoryRoot "build\installer"
$compatibilityArchiveName = "xtra_dead_air_x64.xdb0"

if (-not (Test-Path -LiteralPath (Join-Path $binaryRoot "xrEngine.exe"))) {
    throw "The x64 $Configuration runtime has not been built."
}

# The engine and the compatibility shaders are version-coupled, so the archive is
# rebuilt with the runtime unless an already verified one is supplied explicitly.
if ($CompatibilityArchivePath) {
    $compatibilityArchive = [IO.Path]::GetFullPath($CompatibilityArchivePath)
    if (-not (Test-Path -LiteralPath $compatibilityArchive -PathType Leaf)) {
        throw "The compatibility archive was not found: $compatibilityArchive"
    }
}
else {
    $archiveArguments = @{
        RepositoryRoot = $repositoryRoot
        ConverterPath = $ConverterPath
        WorkRoot = $compatibilityWorkRoot
    }
    New-DeadAirCompatibilityArchive @archiveArguments
    $compatibilityArchive = Join-Path $compatibilityWorkRoot $compatibilityArchiveName
}

New-Item -ItemType Directory -Path $artifactRoot -Force | Out-Null
if (Test-Path -LiteralPath $stageRoot) {
    Remove-Item -LiteralPath $stageRoot -Recurse -Force
}
if (Test-Path -LiteralPath $archivePath) {
    Remove-Item -LiteralPath $archivePath -Force
}
New-Item -ItemType Directory -Path $runtimeRoot -Force | Out-Null
New-Item -ItemType Directory -Path $databaseRoot -Force | Out-Null

$runtimeFiles = Get-ChildItem -LiteralPath $binaryRoot -File |
    Where-Object Extension -In ".exe", ".dll" |
    Sort-Object Name

if (-not $runtimeFiles) {
    throw "No runtime PE files were found."
}

$manifestFiles = [Collections.Generic.List[object]]::new()
foreach ($file in $runtimeFiles) {
    Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $runtimeRoot $file.Name)
    $manifestFiles.Add([pscustomobject]@{
        Name = $file.Name
        Size = $file.Length
        Sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    })
}

$databaseManifestFiles = [Collections.Generic.List[object]]::new()
$compatibilityArchiveItem = Get-Item -LiteralPath $compatibilityArchive
Copy-Item -LiteralPath $compatibilityArchive -Destination (Join-Path $databaseRoot $compatibilityArchiveName)
$databaseManifestFiles.Add([pscustomobject]@{
    Name = $compatibilityArchiveName
    Size = $compatibilityArchiveItem.Length
    Sha256 = (Get-FileHash -LiteralPath $compatibilityArchive -Algorithm SHA256).Hash
})

Copy-Item -LiteralPath (Join-Path $templateRoot "Install-DeadAir-x64.ps1") -Destination $stageRoot
Copy-Item -LiteralPath (Join-Path $templateRoot "Uninstall-DeadAir-x64.ps1") -Destination $stageRoot
Copy-Item -LiteralPath (Join-Path $templateRoot "README_RU.md") -Destination $stageRoot

# Files goes to the game root, DatabaseFiles goes to <root>\database.
$manifest = [pscustomobject]@{
    Name = "Dead Air: Refined"
    Version = $Version
    Architecture = "AMD64"
    Files = $manifestFiles
    DatabaseFiles = $databaseManifestFiles
}
$manifest | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath (Join-Path $stageRoot "package-manifest.json") -Encoding UTF8

Compress-Archive -Path (Join-Path $stageRoot "*") -DestinationPath $archivePath -CompressionLevel Optimal

Write-Host "Package directory: $stageRoot"
Write-Host "Archive: $archivePath"
Write-Host "Runtime files: $($runtimeFiles.Count)"
Write-Host "Database files: $($databaseManifestFiles.Count) ($compatibilityArchive)"
