[CmdletBinding()]
param(
    [ValidateSet("Debug", "Mixed", "Release", "ReleaseMasterGold")]
    [string]$Configuration = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$outputDirectory = Join-Path $repositoryRoot "bin\x64\$Configuration"
$configurePreset = if ($Configuration -eq "ReleaseMasterGold") {
    "windows-x64-master-gold"
} else {
    "windows-x64"
}
$buildDirectory = if ($Configuration -eq "ReleaseMasterGold") {
    Join-Path $repositoryRoot "build\ninja-x64-master-gold"
} else {
    Join-Path $repositoryRoot "build\ninja-x64"
}

function Apply-RequiredPatch {
    param(
        [Parameter(Mandatory)]
        [string]$Repository,
        [Parameter(Mandatory)]
        [string]$Patch
    )

    & git -C $Repository apply --reverse --check --ignore-space-change --ignore-whitespace $Patch 2>$null
    if ($LASTEXITCODE -eq 0) {
        return
    }

    & git -C $Repository apply --check --ignore-space-change --ignore-whitespace $Patch
    if ($LASTEXITCODE -ne 0) {
        throw "Required patch cannot be applied: $Patch"
    }

    & git -C $Repository apply --whitespace=nowarn $Patch
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to apply required patch: $Patch"
    }
}

function Remove-BuildDirectory {
    param(
        [Parameter(Mandatory)]
        [string]$Path
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $repositoryPrefix = $repositoryRoot.TrimEnd("\") + "\"
    if (-not $fullPath.StartsWith($repositoryPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a directory outside the repository: $Path"
    }

    if (Test-Path -LiteralPath $fullPath) {
        Remove-Item -LiteralPath $fullPath -Recurse -Force
    }
}

if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "Visual Studio Installer could not be found."
}

foreach ($submodule in @("Externals\SDL", "Externals\DirectXMath", "Externals\DirectXTex", "Externals\mimalloc")) {
    if (-not (Test-Path -LiteralPath (Join-Path $repositoryRoot "$submodule\.git"))) {
        throw "Required submodules are missing. Run: git submodule update --init --recursive"
    }
}

Apply-RequiredPatch `
    -Repository (Join-Path $repositoryRoot "Externals\LuaJIT") `
    -Patch (Join-Path $repositoryRoot "patches\luajit-dead-air-bytecode.patch")
Apply-RequiredPatch `
    -Repository (Join-Path $repositoryRoot "Externals\xrLuaFix\lua-marshal") `
    -Patch (Join-Path $repositoryRoot "patches\lua-marshal-decode-error.patch")

if ($Clean) {
    Remove-BuildDirectory -Path $buildDirectory
    Remove-BuildDirectory -Path $outputDirectory
}

$visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudio) {
    throw "A Visual Studio installation with the x64 C++ toolchain is required."
}

$developerPrompt = Join-Path $visualStudio "Common7\Tools\VsDevCmd.bat"
$presetSuffix = switch ($Configuration) {
    "Debug" { "debug" }
    "Mixed" { "mixed" }
    "Release" { "release" }
    "ReleaseMasterGold" { "release-master-gold" }
}
$command = "call `"$developerPrompt`" -arch=x64 -host_arch=x64 && cmake --preset $configurePreset && cmake --build --preset windows-x64-$presetSuffix"

Push-Location $repositoryRoot
try {
    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "CMake x64 build failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}
