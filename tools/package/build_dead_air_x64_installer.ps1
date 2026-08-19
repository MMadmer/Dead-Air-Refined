[CmdletBinding()]
param(
    [string]$PortVersion = "1.3.4",
    [string]$ConverterPath = "D:\Games\Dead Air\tools\AXRToolset\bin\converter.exe",
    [switch]$CompatibilityArchiveOnly,
    [switch]$SkipArchive
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "dead_air_x64_compatibility_archive.ps1")

if ($PortVersion -notmatch '^\d+\.\d+\.\d+$') {
    throw "PortVersion must use numeric SemVer format, for example 1.0.1."
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$productVersionHeader = Join-Path $repositoryRoot "src\xrCore\ProductVersion.h"
$productVersionSource = [IO.File]::ReadAllText($productVersionHeader)
$productVersionMatch = [regex]::Match(
    $productVersionSource,
    '(?m)^#define\s+DAR_VERSION_STRING\s+"(?<version>\d+\.\d+\.\d+)"\s*$'
)
if (-not $productVersionMatch.Success -or $PortVersion -ne $productVersionMatch.Groups['version'].Value) {
    throw "PortVersion must match the version compiled into the game."
}

$artifactRoot = Join-Path $repositoryRoot "artifacts"
$packageName = "Dead-Air-Refined-$PortVersion"
$outputRoot = Join-Path $artifactRoot "$packageName-installer-files"
$rawOutputRoot = Join-Path $artifactRoot "$packageName-update-files"
$archivePath = Join-Path $artifactRoot "$packageName-Update.zip"
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
    $command =
        'call "' + $developerPrompt + '" -arch=x64 -host_arch=x64 && ' +
        'cl.exe /nologo /c /O2 /MD /W3 /WX /TC /DUNICODE /D_UNICODE /DZLIB_WINAPI ' +
        '/I"' + $zlibInclude + '" /Fo:"' + $ioWin32Object + '" "' + $ioWin32Source + '" && ' +
        'cl.exe /nologo /c /O2 /EHsc /std:c++20 /utf-8 /MD /W4 /WX /DUNICODE /D_UNICODE /DZLIB_WINAPI ' +
        '/I"' + $zlibInclude + '" /Fo:"' + $updaterObject + '" "' + $updaterSource + '" && ' +
        'link.exe /nologo /OUT:"' + $updaterOutput + '" /SUBSYSTEM:WINDOWS ' +
        '"' + $updaterObject + '" "' + $ioWin32Object + '" "' + $zlibLibrary + '" bcrypt.lib shell32.lib user32.lib'
    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $updaterOutput)) {
        throw "The update helper build failed."
    }
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

$checksumFiles = @(Get-ChildItem -LiteralPath $outputRoot -File)
if (-not $SkipArchive) {
    $checksumFiles += Get-Item -LiteralPath $archivePath
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
    UpdateArchive = if ($SkipArchive) { $null } else { $archivePath }
    UpdateArchiveSizeMB = if ($SkipArchive) { $null } else { [math]::Round((Get-Item $archivePath).Length / 1MB, 1) }
}
