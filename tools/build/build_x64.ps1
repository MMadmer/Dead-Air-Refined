[CmdletBinding()]
param(
    [ValidateSet("Mixed", "Release", "Release Master Gold")]
    [string]$Configuration = "Release",
    [string[]]$Target = @("xrGame", "XR_3DA", "xrRender_R4"),
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
$solution = Join-Path $repositoryRoot "src\engine.sln"
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"

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

if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "Visual Studio Installer could not be found."
}

Apply-RequiredPatch `
    -Repository (Join-Path $repositoryRoot "Externals\LuaJIT") `
    -Patch (Join-Path $repositoryRoot "patches\luajit-dead-air-bytecode.patch")
Apply-RequiredPatch `
    -Repository (Join-Path $repositoryRoot "Externals\xrLuaFix\lua-marshal") `
    -Patch (Join-Path $repositoryRoot "patches\lua-marshal-decode-error.patch")

$visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudio) {
    throw "A Visual Studio installation with the x64 C++ toolchain is required."
}

$developerPrompt = Join-Path $visualStudio "Common7\Tools\VsDevCmd.bat"
$restore = "msbuild `"$solution`" /m /t:Restore /p:RestorePackagesConfig=true"
$targetList = $Target -join ";"
$build = "msbuild `"$solution`" /m /t:$targetList /p:Configuration=`"$Configuration`" /p:Platform=x64"
$cleanBuild = if ($Clean) {
    " && msbuild `"$solution`" /m /t:Clean /p:Configuration=`"$Configuration`" /p:Platform=x64"
} else {
    ""
}
$command = "call `"$developerPrompt`" -arch=x64 -host_arch=x64 && $restore$cleanBuild && $build"

Push-Location $repositoryRoot
try {
    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "x64 build failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}
