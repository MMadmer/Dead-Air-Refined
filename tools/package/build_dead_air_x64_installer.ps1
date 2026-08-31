[CmdletBinding()]
param(
    [string]$PortVersion = "1.4.0",
    [string]$ConverterPath = "D:\Games\Dead Air\tools\AXRToolset\bin\converter.exe",
    [switch]$CompatibilityArchiveOnly,
    [switch]$SkipArchive,
    # Full archive of the release this one follows. Given it, a patch archive is built next to
    # the full one carrying only the files that actually changed. Without it only the full
    # archive is produced (correct for the very first release, or after a version is pulled).
    [string]$PreviousFullArchive,
    # Also publish the full archive under its historical "-Update.zip" name. Clients older
    # than the rename look for exactly that asset and see no update without it, so keep this
    # on for at least one release after switching.
    [switch]$NoLegacyUpdateAlias,
    # Rebuild only the patch, from the payload tree an earlier run of this script already
    # produced ("$packageName-update-files"). Useful to cut a patch against a different base
    # without repeating the whole release build.
    [switch]$PatchOnly
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "dead_air_x64_compatibility_archive.ps1")

if ($PortVersion -notmatch '^\d+\.\d+\.\d+$') {
    throw "PortVersion must use numeric SemVer format, for example 1.0.1."
}

if ($PatchOnly -and -not $PreviousFullArchive) {
    throw "PatchOnly needs PreviousFullArchive."
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$productVersionHeader = Join-Path $repositoryRoot "src\xrCore\ProductVersion.h"
$productVersionSource = [IO.File]::ReadAllText($productVersionHeader)
$productVersionMatch = [regex]::Match(
    $productVersionSource,
    '(?m)^#define\s+DAR_VERSION_STRING\s+"(?<version>\d+\.\d+\.\d+)"\s*$'
)
# A patch rebuild works off an already-built payload tree, so it is free to target a version
# other than the one currently compiled.
if (-not $PatchOnly -and
    (-not $productVersionMatch.Success -or $PortVersion -ne $productVersionMatch.Groups['version'].Value)) {
    throw "PortVersion must match the version compiled into the game."
}

$artifactRoot = Join-Path $repositoryRoot "artifacts"
$packageName = "Dead-Air-Refined-$PortVersion"
$outputRoot = Join-Path $artifactRoot "$packageName-installer-files"
$rawOutputRoot = Join-Path $artifactRoot "$packageName-update-files"
# The complete payload. Named for what it is: a manual setup, the fallback every installation
# can always take. The patch beside it is the bandwidth-saving path.
$archivePath = Join-Path $artifactRoot "$packageName-Setup_Manual.zip"
$legacyArchivePath = Join-Path $artifactRoot "$packageName-Update.zip"
$patchArchivePath = Join-Path $artifactRoot "$packageName-Update_Patch.zip"
$patchOutputRoot = Join-Path $artifactRoot "$packageName-patch-files"
$runtimeRoot = Join-Path $repositoryRoot "bin\x64\Release"
$installerSource = Join-Path $repositoryRoot "packaging\dead-air-x64\installer\DeadAir-x64.iss"
$runtimeManifestPath = Join-Path $repositoryRoot "packaging\dead-air-x64\installer\runtime-files.txt"
$launcherSource = Join-Path $repositoryRoot "packaging\dead-air-x64\installer\UninstallLauncher.cpp"
$launcherOutputRoot = Join-Path $repositoryRoot "build\installer"
$launcherOutput = Join-Path $launcherOutputRoot "Uninstall Dead Air x64.exe"
$launcherObject = Join-Path $launcherOutputRoot "UninstallLauncher.obj"
$updaterSource = Join-Path $repositoryRoot "packaging\dead-air-x64\installer\DeadAirUpdater.cpp"
$updaterOutput = Join-Path $launcherOutputRoot "DeadAirUpdater.exe"
$updaterObject = Join-Path $launcherOutputRoot "DeadAirUpdater.obj"
# The content system's shared core, compiled straight into the updater rather than linked from
# the CMake build. These translation units depend on nothing but the standard library and the
# OS by design, and linking the engine's artefact would couple this executable to the engine's
# toolset and CRT settings for no benefit.
$contentSyncRoot = Join-Path $repositoryRoot "src\xrContentSync"
$contentSyncSources = @(
    "ContentCommit.cpp"
    "ContentDownload.cpp"
    "ContentHash.cpp"
    "ContentManifest.cpp"
    "ContentResolver.cpp"
    "ContentState.cpp"
) | ForEach-Object { Join-Path $contentSyncRoot $_ }
$contentFetcherOutput = Join-Path $launcherOutputRoot "DeadAirContent.exe"
$ioWin32Source = Join-Path $repositoryRoot "Externals\zlib\contrib\minizip\iowin32.c"
$ioWin32Object = Join-Path $launcherOutputRoot "iowin32.obj"
$zlibInclude = Join-Path $repositoryRoot "Externals\zlib"
$zlibLibrary = Join-Path $repositoryRoot "build\lib\x64\Release\zlib.lib"
$maintenanceOutputRoot = Join-Path $launcherOutputRoot "maintenance"
$maintenanceOutput = Join-Path $maintenanceOutputRoot "Dead-Air-Refined-Maintenance.exe"
$compatibilityArchive = Join-Path $launcherOutputRoot "xtra_dead_air_x64.xdb0"
$innoRoot = Join-Path $repositoryRoot "tools\third_party\inno-setup"
$innoCompilerRoot = Join-Path $innoRoot "compiler"
$innoCompiler = Join-Path $innoCompilerRoot "ISCC.exe"
$innoBootstrap = Join-Path $innoRoot "innosetup-7.0.2-x64.exe"
$innoBootstrapUrl =
    "https://github.com/jrsoftware/issrc/releases/download/is-7_0_2/innosetup-7.0.2-x64.exe"
$innoBootstrapSha256 = "5AD54CA3DEF786F8F4212552E54CC6D8D61329E2D24A1CFEE0571D42C2684FF1"
function Assert-PathInside {
    param(
        [Parameter(Mandatory)]
        [string]$Parent,
        [Parameter(Mandatory)]
        [string]$Child
    )

    $resolvedParent = [IO.Path]::GetFullPath($Parent).TrimEnd("\") + "\"
    $resolvedChild = [IO.Path]::GetFullPath($Child)
    if (-not $resolvedChild.StartsWith($resolvedParent, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside $resolvedParent : $resolvedChild"
    }
}

function Initialize-InnoSetupCompiler {
    if (Test-Path -LiteralPath $innoCompiler) {
        return
    }

    New-Item -ItemType Directory -Path $innoRoot -Force | Out-Null
    if (-not (Test-Path -LiteralPath $innoBootstrap)) {
        Invoke-WebRequest -Uri $innoBootstrapUrl -OutFile $innoBootstrap
    }

    $bootstrapHash = (Get-FileHash -LiteralPath $innoBootstrap -Algorithm SHA256).Hash
    if ($bootstrapHash -ne $innoBootstrapSha256) {
        throw "The Inno Setup bootstrap hash does not match the pinned official release."
    }

    $signature = Get-AuthenticodeSignature -LiteralPath $innoBootstrap
    if ($signature.Status -ne "Valid" -or $signature.SignerCertificate.Subject -notmatch "Pyrsys B\.V\.") {
        throw "The Inno Setup bootstrap does not have the expected valid Authenticode signature."
    }

    $arguments =
        '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER /DIR="' +
        $innoCompilerRoot +
        '"'
    $process = Start-Process -FilePath $innoBootstrap -ArgumentList $arguments -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $innoCompiler)) {
        throw "Inno Setup installation failed with exit code $($process.ExitCode)."
    }
}

function Get-VisualStudioDeveloperPrompt {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw "Visual Studio Installer was not found."
    }

    $visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $visualStudio) {
        throw "Visual Studio with the x64 C++ toolchain is required."
    }

    return Join-Path $visualStudio "Common7\Tools\VsDevCmd.bat"
}

function Build-NativeHelpers {
    New-Item -ItemType Directory -Path $launcherOutputRoot -Force | Out-Null
    $developerPrompt = Get-VisualStudioDeveloperPrompt
    $command =
        'call "' + $developerPrompt + '" -arch=x64 -host_arch=x64 && ' +
        'cl.exe /nologo /O2 /EHsc /std:c++20 /utf-8 /MT /W4 /WX /DUNICODE /D_UNICODE ' +
        '/Fo:"' + $launcherObject + '" /Fe:"' + $launcherOutput + '" "' + $launcherSource + '" ' +
        '/link /SUBSYSTEM:WINDOWS shell32.lib user32.lib'
    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $launcherOutput)) {
        throw "The uninstaller launcher build failed."
    }

    if (-not (Test-Path -LiteralPath $zlibLibrary -PathType Leaf)) {
        throw "The x64 Release zlib library was not found: $zlibLibrary"
    }
    $contentSyncObjects = @()
    $contentSyncCompile = ""
    $sourceInclude = Join-Path $repositoryRoot "src"
    foreach ($source in $contentSyncSources) {
        $object = Join-Path $launcherOutputRoot ([IO.Path]::GetFileNameWithoutExtension($source) + ".obj")
        $contentSyncObjects += $object
        $contentSyncCompile +=
            'cl.exe /nologo /c /O2 /EHsc /std:c++20 /utf-8 /MD /W4 /WX /DUNICODE /D_UNICODE ' +
            '/I"' + $sourceInclude + '" /Fo:"' + $object + '" "' + $source + '" && '
    }
    $contentSyncLinkInputs = ($contentSyncObjects | ForEach-Object { '"' + $_ + '"' }) -join ' '

    $command =
        'call "' + $developerPrompt + '" -arch=x64 -host_arch=x64 && ' +
        'cl.exe /nologo /c /O2 /MD /W3 /WX /TC /DUNICODE /D_UNICODE /DZLIB_WINAPI ' +
        '/I"' + $zlibInclude + '" /Fo:"' + $ioWin32Object + '" "' + $ioWin32Source + '" && ' +
        $contentSyncCompile +
        'cl.exe /nologo /c /O2 /EHsc /std:c++20 /utf-8 /MD /W4 /WX /DUNICODE /D_UNICODE /DZLIB_WINAPI ' +
        '/I"' + $zlibInclude + '" /I"' + $sourceInclude + '" ' +
        '/Fo:"' + $updaterObject + '" "' + $updaterSource + '" && ' +
        'link.exe /nologo /OUT:"' + $updaterOutput + '" /SUBSYSTEM:WINDOWS ' +
        '"' + $updaterObject + '" "' + $ioWin32Object + '" ' + $contentSyncLinkInputs + ' "' + $zlibLibrary +
        '" bcrypt.lib shell32.lib user32.lib winhttp.lib'
    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $updaterOutput)) {
        throw "The update helper build failed."
    }

    # The installer's content fetcher IS the updater - the mode comes from the command line. A
    # byte copy rather than a second link keeps one binary and removes any possibility of the
    # two drifting apart.
    Copy-Item -LiteralPath $updaterOutput -Destination $contentFetcherOutput -Force
}

function Build-MaintenanceInstaller {
    if (Test-Path -LiteralPath $maintenanceOutputRoot) {
        Remove-Item -LiteralPath $maintenanceOutputRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $maintenanceOutputRoot -Force | Out-Null
    $arguments = @(
        "/DRepoRoot=$repositoryRoot",
        "/DPortVersion=$PortVersion",
        "/DOutputDirectory=$maintenanceOutputRoot",
        "/DLauncherPath=$launcherOutput",
        "/DCompatibilityArchive=$compatibilityArchive",
        "/DUpdaterPath=$updaterOutput",
        "/DMaintenanceOnly=1",
        $installerSource
    )
    & $innoCompiler @arguments
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $maintenanceOutput -PathType Leaf)) {
        throw "The maintenance installer build failed."
    }
}

function New-UpdateArchive {
    if ($SkipArchive) {
        return
    }

    Assert-PathInside -Parent $artifactRoot -Child $rawOutputRoot
    Assert-PathInside -Parent $artifactRoot -Child $archivePath
    if (Test-Path -LiteralPath $rawOutputRoot) {
        Remove-Item -LiteralPath $rawOutputRoot -Recurse -Force
    }
    if (Test-Path -LiteralPath $archivePath) {
        Remove-Item -LiteralPath $archivePath -Force
    }
    New-Item -ItemType Directory -Path $rawOutputRoot -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $rawOutputRoot "database") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $rawOutputRoot ".dead-air-x64") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $rawOutputRoot "appdata\savedgames") -Force | Out-Null

    foreach ($file in $manifestFiles) {
        Copy-Item -LiteralPath (Join-Path $runtimeRoot $file) -Destination $rawOutputRoot
    }
    Copy-Item -LiteralPath $compatibilityArchive -Destination (Join-Path $rawOutputRoot "database\xtra_dead_air_x64.xdb0")
    Copy-Item -LiteralPath $launcherOutput -Destination (Join-Path $rawOutputRoot "Uninstall Dead Air Refined.exe")
    Copy-Item -LiteralPath $updaterOutput -Destination (Join-Path $rawOutputRoot "DeadAirUpdater.exe")
    Copy-Item -LiteralPath $maintenanceOutput -Destination (Join-Path $rawOutputRoot ".dead-air-x64\Dead-Air-Refined-Maintenance.exe")
    Copy-Item -LiteralPath $runtimeManifestPath -Destination (Join-Path $rawOutputRoot ".dead-air-x64\runtime-files.txt")

    $manifest = [Collections.Generic.List[string]]::new()
    $manifest.Add("schema=dead-air-refined.update/1")
    $manifest.Add("version=$PortVersion")
    Get-ChildItem -LiteralPath $rawOutputRoot -Recurse -File |
        Sort-Object { $_.FullName.Substring($rawOutputRoot.Length + 1) } |
        ForEach-Object {
            $relative = $_.FullName.Substring($rawOutputRoot.Length + 1).Replace('\', '/')
            $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            $manifest.Add("$hash`t$($_.Length)`t$relative")
        }
    [IO.File]::WriteAllText(
        (Join-Path $rawOutputRoot "update-manifest.txt"),
        [string]::Join("`n", $manifest) + "`n",
        [Text.UTF8Encoding]::new($false)
    )

    $sevenZip = (Get-Command 7z.exe -ErrorAction Stop).Source
    Push-Location $rawOutputRoot
    try {
        & $sevenZip a -tzip -mx=9 $archivePath "*"
        if ($LASTEXITCODE -ne 0) {
            throw "7-Zip failed with exit code $LASTEXITCODE."
        }
    }
    finally {
        Pop-Location
    }

    if (-not $NoLegacyUpdateAlias) {
        Assert-PathInside -Parent $artifactRoot -Child $legacyArchivePath
        Copy-Item -LiteralPath $archivePath -Destination $legacyArchivePath -Force
    }
}

function Get-ArchiveManifest {
    param([Parameter(Mandatory)][string]$ArchivePath)

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        $entry = $zip.GetEntry("update-manifest.txt")
        if (-not $entry) {
            throw "The previous archive has no update-manifest.txt: $ArchivePath"
        }
        $reader = [IO.StreamReader]::new($entry.Open(), [Text.UTF8Encoding]::new($false))
        try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
    }
    finally {
        $zip.Dispose()
    }

    $lines = $text -split "`n"
    if ($lines[0] -notmatch '^schema=dead-air-refined\.update/1$') {
        throw "The previous archive must be a full one (schema /1), got: $($lines[0])"
    }
    if ($lines[1] -notmatch '^version=(?<version>\d+\.\d+\.\d+)$') {
        throw "The previous archive has no usable version line: $($lines[1])"
    }

    $files = @{}
    foreach ($line in $lines | Select-Object -Skip 2) {
        if (-not $line.Trim()) { continue }
        $parts = $line -split "`t"
        if ($parts.Count -ne 3) { throw "Malformed manifest line: $line" }
        $files[$parts[2]] = $parts[0]
    }
    [pscustomobject]@{ Version = $Matches['version']; Files = $files }
}

function New-PatchArchive {
    if ($SkipArchive -or -not $PreviousFullArchive) {
        return $null
    }
    if (-not (Test-Path -LiteralPath $PreviousFullArchive -PathType Leaf)) {
        throw "PreviousFullArchive was not found: $PreviousFullArchive"
    }

    $previous = Get-ArchiveManifest -ArchivePath $PreviousFullArchive
    if ($previous.Version -eq $PortVersion) {
        throw "PreviousFullArchive is the same version as this build ($PortVersion)."
    }

    Assert-PathInside -Parent $artifactRoot -Child $patchOutputRoot
    Assert-PathInside -Parent $artifactRoot -Child $patchArchivePath
    if (Test-Path -LiteralPath $patchOutputRoot) {
        Remove-Item -LiteralPath $patchOutputRoot -Recurse -Force
    }
    if (Test-Path -LiteralPath $patchArchivePath) {
        Remove-Item -LiteralPath $patchArchivePath -Force
    }
    New-Item -ItemType Directory -Path $patchOutputRoot -Force | Out-Null

    # The manifest still describes the WHOLE target version - that is what lets the applier
    # end up in exactly the state a full install would reach, and what tells it which stale
    # files to drop. Only the payload is trimmed.
    $manifest = [Collections.Generic.List[string]]::new()
    $manifest.Add("schema=dead-air-refined.update/2")
    $manifest.Add("version=$PortVersion")
    $manifest.Add("kind=patch")
    $manifest.Add("base=$($previous.Version)")

    $packed = 0
    $packedBytes = 0L
    $totalBytes = 0L
    Get-ChildItem -LiteralPath $rawOutputRoot -Recurse -File |
        Where-Object { $_.FullName.Substring($rawOutputRoot.Length + 1) -ne "update-manifest.txt" } |
        Sort-Object { $_.FullName.Substring($rawOutputRoot.Length + 1) } |
        ForEach-Object {
            $relative = $_.FullName.Substring($rawOutputRoot.Length + 1).Replace('\', '/')
            $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            $manifest.Add("$hash`t$($_.Length)`t$relative")
            $totalBytes += $_.Length
            if ($previous.Files[$relative] -ne $hash) {
                $destination = Join-Path $patchOutputRoot $relative.Replace('/', '\')
                New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
                Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
                $packed++
                $packedBytes += $_.Length
            }
        }

    [IO.File]::WriteAllText(
        (Join-Path $patchOutputRoot "update-manifest.txt"),
        [string]::Join("`n", $manifest) + "`n",
        [Text.UTF8Encoding]::new($false)
    )

    $sevenZip = (Get-Command 7z.exe -ErrorAction Stop).Source
    Push-Location $patchOutputRoot
    try {
        & $sevenZip a -tzip -mx=9 $patchArchivePath "*"
        if ($LASTEXITCODE -ne 0) {
            throw "7-Zip failed with exit code $LASTEXITCODE."
        }
    }
    finally {
        Pop-Location
    }

    [pscustomobject]@{
        Path = $patchArchivePath
        Base = $previous.Version
        PackedFiles = $packed
        TotalFiles = $manifest.Count - 4
        PackedMB = [math]::Round($packedBytes / 1MB, 1)
        FullMB = [math]::Round($totalBytes / 1MB, 1)
    }
}

function Build-CompatibilityArchive {
    $arguments = @{
        RepositoryRoot = $repositoryRoot
        ConverterPath = $ConverterPath
        WorkRoot = $launcherOutputRoot
    }
    New-DeadAirCompatibilityArchive @arguments
}

if ($CompatibilityArchiveOnly) {
    Build-CompatibilityArchive
    Get-Item -LiteralPath $compatibilityArchive
    return
}

if ($PatchOnly) {
    if (-not (Test-Path -LiteralPath $rawOutputRoot -PathType Container)) {
        throw "No payload tree to diff against: $rawOutputRoot"
    }
    New-PatchArchive
    return
}

if (-not (Test-Path -LiteralPath (Join-Path $runtimeRoot "xrEngine.exe"))) {
    throw "Build the x64 Release runtime before creating the installer."
}

$manifestFiles = Get-Content -LiteralPath $runtimeManifestPath |
    ForEach-Object Trim |
    Where-Object { $_ }
$actualRuntimeFiles = Get-ChildItem -LiteralPath $runtimeRoot -File |
    Where-Object Extension -In ".exe", ".dll" |
    Sort-Object Name |
    ForEach-Object Name
$manifestDifference = Compare-Object $manifestFiles $actualRuntimeFiles
if ($manifestDifference) {
    throw "runtime-files.txt does not match the x64 Release output:`n$($manifestDifference | Out-String)"
}

Initialize-InnoSetupCompiler
Build-NativeHelpers
Build-CompatibilityArchive
Build-MaintenanceInstaller

New-Item -ItemType Directory -Path $artifactRoot -Force | Out-Null
Assert-PathInside -Parent $artifactRoot -Child $outputRoot
Assert-PathInside -Parent $artifactRoot -Child $archivePath
if (Test-Path -LiteralPath $outputRoot) {
    Remove-Item -LiteralPath $outputRoot -Recurse -Force
}
if (Test-Path -LiteralPath $archivePath) {
    Remove-Item -LiteralPath $archivePath -Force
}
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

$compilerArguments = @(
    "/DRepoRoot=$repositoryRoot",
    "/DPortVersion=$PortVersion",
    "/DOutputDirectory=$outputRoot",
    "/DLauncherPath=$launcherOutput",
    "/DCompatibilityArchive=$compatibilityArchive",
    "/DUpdaterPath=$updaterOutput",
    "/DMaintenancePath=$maintenanceOutput",
    $installerSource
)
& $innoCompiler @compilerArguments
if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup compilation failed with exit code $LASTEXITCODE."
}

Copy-Item -LiteralPath (Join-Path $repositoryRoot "packaging\dead-air-x64\README_RU.md") -Destination $outputRoot
New-UpdateArchive
$patch = New-PatchArchive

$checksumFiles = @(Get-ChildItem -LiteralPath $outputRoot -File)
if (-not $SkipArchive) {
    $checksumFiles += Get-Item -LiteralPath $archivePath
    if (-not $NoLegacyUpdateAlias) {
        $checksumFiles += Get-Item -LiteralPath $legacyArchivePath
    }
    if ($patch) {
        $checksumFiles += Get-Item -LiteralPath $patch.Path
    }
}
$checksums = $checksumFiles |
    Sort-Object Name |
    ForEach-Object {
        "{0} *{1}" -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $_.Name
    }
$checksums | Set-Content -LiteralPath (Join-Path $outputRoot "SHA256SUMS.txt") -Encoding ASCII

$installerFiles = Get-ChildItem -LiteralPath $outputRoot -File
[pscustomobject]@{
    ProductVersion = $PortVersion
    InstallerDirectory = $outputRoot
    InstallerFiles = $installerFiles.Count
    InstallerSizeGB = [math]::Round(($installerFiles | Measure-Object Length -Sum).Sum / 1GB, 3)
    FullArchive = if ($SkipArchive) { $null } else { $archivePath }
    FullArchiveSizeMB = if ($SkipArchive) { $null } else { [math]::Round((Get-Item $archivePath).Length / 1MB, 1) }
    LegacyAlias = if ($SkipArchive -or $NoLegacyUpdateAlias) { $null } else { $legacyArchivePath }
    PatchArchive = if ($patch) { $patch.Path } else { $null }
    PatchBaseVersion = if ($patch) { $patch.Base } else { $null }
    PatchFiles = if ($patch) { "$($patch.PackedFiles) of $($patch.TotalFiles)" } else { $null }
    PatchArchiveSizeMB = if ($patch) { [math]::Round((Get-Item $patch.Path).Length / 1MB, 1) } else { $null }
}
