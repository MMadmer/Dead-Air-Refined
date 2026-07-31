[CmdletBinding()]
param(
    [string]$PortVersion = "0.9.0",
    [string]$ConverterPath = "D:\Games\Dead Air\tools\AXRToolset\bin\converter.exe",
    [switch]$CompatibilityArchiveOnly,
    [switch]$SkipArchive
)

$ErrorActionPreference = "Stop"
if ($PortVersion -notmatch '^\d+\.\d+\.\d+$') {
    throw "PortVersion must use numeric SemVer format, for example 1.0.1."
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$artifactRoot = Join-Path $repositoryRoot "artifacts"
$packageName = "Dead-Air-Refined-$PortVersion"
$outputRoot = Join-Path $artifactRoot "$packageName-installer-files"
$archivePath = Join-Path $artifactRoot "$packageName.zip"
$runtimeRoot = Join-Path $repositoryRoot "bin\x64\Release"
$installerSource = Join-Path $repositoryRoot "packaging\dead-air-x64\installer\DeadAir-x64.iss"
$runtimeManifestPath = Join-Path $repositoryRoot "packaging\dead-air-x64\installer\runtime-files.txt"
$launcherSource = Join-Path $repositoryRoot "packaging\dead-air-x64\installer\UninstallLauncher.cpp"
$launcherOutputRoot = Join-Path $repositoryRoot "build\installer"
$launcherOutput = Join-Path $launcherOutputRoot "Uninstall Dead Air x64.exe"
$launcherObject = Join-Path $launcherOutputRoot "UninstallLauncher.obj"
$compatibilityRoot = Join-Path $repositoryRoot "packaging\dead-air-x64\compatibility"
$compatibilityGameRoot = Join-Path $compatibilityRoot "gamedata"
$compatibilityUserData = Join-Path $compatibilityRoot "xdb_userdata.ltx"
$compatibilityArchive = Join-Path $launcherOutputRoot "xtra_dead_air_x64.xdb0"
$compatibilityStageRoot = Join-Path $launcherOutputRoot "compatibility-stage"
$compatibilityStageGameRoot = Join-Path $compatibilityStageRoot "gamedata"
$compatibilityVerifyRoot = Join-Path $launcherOutputRoot "compatibility-verify"
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

function Build-UninstallLauncher {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw "Visual Studio Installer was not found."
    }

    $visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $visualStudio) {
        throw "Visual Studio with the x64 C++ toolchain is required."
    }

    New-Item -ItemType Directory -Path $launcherOutputRoot -Force | Out-Null
    $developerPrompt = Join-Path $visualStudio "Common7\Tools\VsDevCmd.bat"
    $command =
        'call "' + $developerPrompt + '" -arch=x64 -host_arch=x64 && ' +
        'cl.exe /nologo /O2 /EHsc /std:c++20 /MT /DUNICODE /D_UNICODE ' +
        '/Fo:"' + $launcherObject + '" /Fe:"' + $launcherOutput + '" "' + $launcherSource + '" ' +
        '/link /SUBSYSTEM:WINDOWS shell32.lib user32.lib'
    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $launcherOutput)) {
        throw "The uninstaller launcher build failed."
    }
}

function Build-CompatibilityArchive {
    $converter = [IO.Path]::GetFullPath($ConverterPath)
    if (-not (Test-Path -LiteralPath $converter -PathType Leaf)) {
        throw "AXRToolset converter was not found: $converter"
    }

    New-Item -ItemType Directory -Path $launcherOutputRoot -Force | Out-Null
    if (Test-Path -LiteralPath $compatibilityStageRoot) {
        Remove-Item -LiteralPath $compatibilityStageRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $compatibilityStageGameRoot -Force | Out-Null
    Get-ChildItem -LiteralPath $compatibilityGameRoot | Copy-Item -Destination $compatibilityStageGameRoot -Recurse

    $localizedTextPath =
        Join-Path $compatibilityStageGameRoot "configs\text\rus\dead_air_x64.xml"
    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    $localizedText = [IO.File]::ReadAllText($localizedTextPath, $utf8)
    $localizedText = $localizedText.Replace('encoding="utf-8"', 'encoding="windows-1251"')
    [Text.Encoding]::RegisterProvider([Text.CodePagesEncodingProvider]::Instance)
    $windows1251 = [Text.Encoding]::GetEncoding(
        1251,
        [Text.EncoderExceptionFallback]::new(),
        [Text.DecoderExceptionFallback]::new()
    )
    [IO.File]::WriteAllText($localizedTextPath, $localizedText, $windows1251)

    $gravityGunScriptPath =
        Join-Path $compatibilityStageGameRoot "scripts\bind_gr_gun.script"
    $gravityGunScript = [IO.File]::ReadAllText($gravityGunScriptPath, $utf8)
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
Build-UninstallLauncher
Build-CompatibilityArchive

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
    $installerSource
)
& $innoCompiler @compilerArguments
if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup compilation failed with exit code $LASTEXITCODE."
}

Copy-Item -LiteralPath (Join-Path $repositoryRoot "packaging\dead-air-x64\README_RU.md") -Destination $outputRoot
$checksums = Get-ChildItem -LiteralPath $outputRoot -File |
    Sort-Object Name |
    ForEach-Object {
        "{0} *{1}" -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $_.Name
    }
$checksums | Set-Content -LiteralPath (Join-Path $outputRoot "SHA256SUMS.txt") -Encoding ASCII

if (-not $SkipArchive) {
    $sevenZip = (Get-Command 7z.exe -ErrorAction Stop).Source
    Push-Location $outputRoot
    try {
        & $sevenZip a -tzip -mx=0 $archivePath "*"
        if ($LASTEXITCODE -ne 0) {
            throw "7-Zip failed with exit code $LASTEXITCODE."
        }
    }
    finally {
        Pop-Location
    }
}

$installerFiles = Get-ChildItem -LiteralPath $outputRoot -File
[pscustomobject]@{
    ProductVersion = $PortVersion
    InstallerDirectory = $outputRoot
    InstallerFiles = $installerFiles.Count
    InstallerSizeGB = [math]::Round(($installerFiles | Measure-Object Length -Sum).Sum / 1GB, 3)
    Archive = if ($SkipArchive) { $null } else { $archivePath }
    ArchiveSizeGB = if ($SkipArchive) { $null } else { [math]::Round((Get-Item $archivePath).Length / 1GB, 3) }
}
