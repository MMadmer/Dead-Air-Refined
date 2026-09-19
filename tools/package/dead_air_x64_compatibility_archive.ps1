# Compatibility archive builder for the Dead Air x64 packaging scripts.
# Dot-source this file, then call New-DeadAirCompatibilityArchive.
# The archive is always written as <WorkRoot>\xtra_dead_air_x64.xdb0.
#
# Packing and verification live in dead_air_x64_archive.ps1 and are shared with the content
# bundles. What stays here is what is specific to THIS archive: the core script overrides
# that must shadow the packed Dead Air copies, and the cp1251 text fix-ups. Neither may ever
# run over a content bundle, which is why they are not in the shared builder.
. (Join-Path $PSScriptRoot "dead_air_x64_archive.ps1")

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
        "scripts\ui_save_dialog.script",
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
    foreach ($localizedTextName in @("dead_air_x64.xml", "dead_air_x64_mods.xml", "dead_air_1_2_1.xml")) {
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

    New-XdbArchive `
        -StageRoot $compatibilityStageGameRoot `
        -OutputPath $compatibilityArchive `
        -UserDataPath $compatibilityUserData `
        -ConverterPath $converter `
        -VerifyRoot $compatibilityVerifyRoot
}
