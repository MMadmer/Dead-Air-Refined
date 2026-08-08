# Shared compatibility archive builder for the Dead Air x64 packaging scripts.
# Dot-source this file, then call New-DeadAirCompatibilityArchive.
# The archive is always written as <WorkRoot>\xtra_dead_air_x64.xdb0.

function New-DeadAirCompatibilityArchive {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$RepositoryRoot,
        [Parameter(Mandatory)]
        [string]$ConverterPath,
        [Parameter(Mandatory)]
        [string]$WorkRoot
    )

    $ErrorActionPreference = "Stop"

    $compatibilityRoot = Join-Path $RepositoryRoot "packaging\dead-air-x64\compatibility"
    $compatibilityGameRoot = Join-Path $compatibilityRoot "gamedata"
    $compatibilityUserData = Join-Path $compatibilityRoot "xdb_userdata.ltx"
    $compatibilityArchive = Join-Path $WorkRoot "xtra_dead_air_x64.xdb0"
    $compatibilityStageRoot = Join-Path $WorkRoot "compatibility-stage"
    $compatibilityStageGameRoot = Join-Path $compatibilityStageRoot "gamedata"
    $compatibilityVerifyRoot = Join-Path $WorkRoot "compatibility-verify"
    $coreCompatibilityOverrides = @(
        "scripts\ui_load_dialog.script",
        "shaders\gl\dof.h"
    )

    $converter = [IO.Path]::GetFullPath($ConverterPath)
    if (-not (Test-Path -LiteralPath $converter -PathType Leaf)) {
        throw "AXRToolset converter was not found: $converter"
    }

    New-Item -ItemType Directory -Path $WorkRoot -Force | Out-Null
    if (Test-Path -LiteralPath $compatibilityStageRoot) {
        Remove-Item -LiteralPath $compatibilityStageRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $compatibilityStageGameRoot -Force | Out-Null
    Get-ChildItem -LiteralPath $compatibilityGameRoot | Copy-Item -Destination $compatibilityStageGameRoot -Recurse

    # These core scripts must shadow the original packed Dead Air copies.
    foreach ($relativePath in $coreCompatibilityOverrides) {
        $sourcePath = Join-Path $RepositoryRoot "res\gamedata\$relativePath"
        $destinationPath = Join-Path $compatibilityStageGameRoot $relativePath
        New-Item -ItemType Directory -Path (Split-Path -Parent $destinationPath) -Force | Out-Null
        Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
    }

    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    [Text.Encoding]::RegisterProvider([Text.CodePagesEncodingProvider]::Instance)
    $windows1251 = [Text.Encoding]::GetEncoding(
        1251,
        [Text.EncoderExceptionFallback]::new(),
        [Text.DecoderExceptionFallback]::new()
    )
    foreach ($localizedTextName in @("dead_air_x64.xml", "dead_air_1_2_1.xml")) {
        $localizedTextPath =
            Join-Path $compatibilityStageGameRoot "configs\text\rus\$localizedTextName"
        try {
            $localizedText = [IO.File]::ReadAllText($localizedTextPath, $utf8)
        }
        catch [Text.DecoderFallbackException] {
            $localizedText = [IO.File]::ReadAllText($localizedTextPath, $windows1251)
        }
        $localizedText = $localizedText.Replace('encoding="utf-8"', 'encoding="windows-1251"')
        [IO.File]::WriteAllText($localizedTextPath, $localizedText, $windows1251)
    }

    $gravityGunScriptPath =
        Join-Path $compatibilityStageGameRoot "scripts\bind_gr_gun.script"
    try {
        $gravityGunScript = [IO.File]::ReadAllText($gravityGunScriptPath, $utf8)
    }
    catch [Text.DecoderFallbackException] {
        $gravityGunScript = [IO.File]::ReadAllText($gravityGunScriptPath, $windows1251)
    }
    [IO.File]::WriteAllText($gravityGunScriptPath, $gravityGunScript, $windows1251)

    & $converter -pack -xdb -xdb_ud $compatibilityUserData -out $compatibilityArchive $compatibilityStageGameRoot
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $compatibilityArchive -PathType Leaf)) {
        throw "The Dead Air x64 compatibility archive build failed."
    }

    if (Test-Path -LiteralPath $compatibilityVerifyRoot) {
        Remove-Item -LiteralPath $compatibilityVerifyRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $compatibilityVerifyRoot -Force | Out-Null
    & $converter -unpack -xdb -dir $compatibilityVerifyRoot $compatibilityArchive
    if ($LASTEXITCODE -ne 0) {
        throw "The Dead Air x64 compatibility archive verification failed."
    }

    $sourceFiles = Get-ChildItem -LiteralPath $compatibilityStageGameRoot -Recurse -File
    $verifiedFiles = Get-ChildItem -LiteralPath $compatibilityVerifyRoot -Recurse -File
    if ($sourceFiles.Count -ne $verifiedFiles.Count) {
        throw "The Dead Air x64 compatibility archive file count is invalid."
    }

    foreach ($sourceFile in $sourceFiles) {
        $relativePath = $sourceFile.FullName.Substring($compatibilityStageGameRoot.Length + 1)
        $verifiedFile = Join-Path $compatibilityVerifyRoot $relativePath
        if (-not (Test-Path -LiteralPath $verifiedFile -PathType Leaf) -or
            (Get-FileHash -LiteralPath $sourceFile.FullName).Hash -ne
                (Get-FileHash -LiteralPath $verifiedFile).Hash) {
            throw "Compatibility archive verification failed: $relativePath"
        }
    }
}
