[CmdletBinding()]
param(
    [string]$Version = "1.2.2",
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$binaryRoot = Join-Path $repositoryRoot "bin\x64\$Configuration"
$templateRoot = Join-Path $repositoryRoot "packaging\dead-air-x64"
$artifactRoot = Join-Path $repositoryRoot "artifacts"
$packageName = "Dead-Air-Refined-$Version"
$stageRoot = Join-Path $artifactRoot $packageName
$runtimeRoot = Join-Path $stageRoot "runtime"
$archivePath = Join-Path $artifactRoot "$packageName.zip"

if (-not (Test-Path -LiteralPath (Join-Path $binaryRoot "xrEngine.exe"))) {
    throw "The x64 $Configuration runtime has not been built."
}

New-Item -ItemType Directory -Path $artifactRoot -Force | Out-Null
if (Test-Path -LiteralPath $stageRoot) {
    Remove-Item -LiteralPath $stageRoot -Recurse -Force
}
if (Test-Path -LiteralPath $archivePath) {
    Remove-Item -LiteralPath $archivePath -Force
}
New-Item -ItemType Directory -Path $runtimeRoot -Force | Out-Null

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

Copy-Item -LiteralPath (Join-Path $templateRoot "Install-DeadAir-x64.ps1") -Destination $stageRoot
Copy-Item -LiteralPath (Join-Path $templateRoot "Uninstall-DeadAir-x64.ps1") -Destination $stageRoot
Copy-Item -LiteralPath (Join-Path $templateRoot "README_RU.md") -Destination $stageRoot

$manifest = [pscustomobject]@{
    Name = "Dead Air: Refined"
    Version = $Version
    Architecture = "AMD64"
    Files = $manifestFiles
}
$manifest | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath (Join-Path $stageRoot "package-manifest.json") -Encoding UTF8

Compress-Archive -Path (Join-Path $stageRoot "*") -DestinationPath $archivePath -CompressionLevel Optimal

Write-Host "Package directory: $stageRoot"
Write-Host "Archive: $archivePath"
Write-Host "Runtime files: $($runtimeFiles.Count)"
