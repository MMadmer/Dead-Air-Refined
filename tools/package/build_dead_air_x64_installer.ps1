[CmdletBinding()]
param(
    [string]$PortVersion = "1.0.0",
    [string]$StandaloneSource = "D:\Games\Dead Air",
    [switch]$SkipArchive
)

$ErrorActionPreference = "Stop"
if ($PortVersion -notmatch '^\d+\.\d+\.\d+$') {
    throw "PortVersion must use numeric SemVer format, for example 1.0.1."
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$artifactRoot = Join-Path $repositoryRoot "artifacts"
$packageName = "Dead-Air-0.98b-x64-$PortVersion"
$outputRoot = Join-Path $artifactRoot "$packageName-installer-files"
$archivePath = Join-Path $artifactRoot "$packageName.zip"
$runtimeRoot = Join-Path $repositoryRoot "bin\x64\Release"
$installerSource = Join-Path $repositoryRoot "packaging\dead-air-x64\installer\DeadAir-x64.iss"
$runtimeManifestPath = Join-Path $repositoryRoot "packaging\dead-air-x64\installer\runtime-files.txt"
$launcherSource = Join-Path $repositoryRoot "packaging\dead-air-x64\installer\UninstallLauncher.cpp"
$launcherOutputRoot = Join-Path $repositoryRoot "build\installer"
$launcherOutput = Join-Path $launcherOutputRoot "Uninstall Dead Air x64.exe"
$launcherObject = Join-Path $launcherOutputRoot "UninstallLauncher.obj"
$innoRoot = Join-Path $repositoryRoot "tools\third_party\inno-setup"
$innoCompilerRoot = Join-Path $innoRoot "compiler"
$innoCompiler = Join-Path $innoCompilerRoot "ISCC.exe"
$innoBootstrap = Join-Path $innoRoot "innosetup-7.0.2-x64.exe"
$innoBootstrapUrl =
    "https://github.com/jrsoftware/issrc/releases/download/is-7_0_2/innosetup-7.0.2-x64.exe"
$innoBootstrapSha256 = "5AD54CA3DEF786F8F4212552E54CC6D8D61329E2D24A1CFEE0571D42C2684FF1"
$baseDatabaseFiles = @(
    "configs.xdb0",
    "levels.xdb0",
    "levels.xdb1",
    "levels.xdb2",
    "levels.xdb3",
    "levels.xdb4",
    "meshes.xdb0",
    "movie.xdb0",
    "sounds.xdb0",
    "sounds.xdb1",
    "textures.xdb0",
    "textures.xdb1",
    "textures.xdb2",
    "xtra.xdb0"
)
$baseRootFiles = @("credits.txt", "debug.cmd", "fsgame.ltx", "help.html", "readme.txt")

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

$missingStandaloneFiles = [Collections.Generic.List[string]]::new()
foreach ($fileName in $baseRootFiles) {
    $path = Join-Path $StandaloneSource $fileName
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $missingStandaloneFiles.Add($path)
    }
}
foreach ($fileName in $baseDatabaseFiles) {
    $path = Join-Path (Join-Path $StandaloneSource "database") $fileName
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $missingStandaloneFiles.Add($path)
    }
}
if ($missingStandaloneFiles.Count) {
    throw "The standalone Dead Air 0.98b source is incomplete:`n$($missingStandaloneFiles -join "`n")"
}

Initialize-InnoSetupCompiler
Build-UninstallLauncher

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
    "/DStandaloneSource=$([IO.Path]::GetFullPath($StandaloneSource))",
    "/DPortVersion=$PortVersion",
    "/DOutputDirectory=$outputRoot",
    "/DLauncherPath=$launcherOutput",
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
    ProductVersion = "0.98b-x64-$PortVersion"
    InstallerDirectory = $outputRoot
    InstallerFiles = $installerFiles.Count
    InstallerSizeGB = [math]::Round(($installerFiles | Measure-Object Length -Sum).Sum / 1GB, 3)
    Archive = if ($SkipArchive) { $null } else { $archivePath }
    ArchiveSizeGB = if ($SkipArchive) { $null } else { [math]::Round((Get-Item $archivePath).Length / 1GB, 3) }
}
