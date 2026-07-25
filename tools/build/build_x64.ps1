[CmdletBinding()]
param(
    [ValidateSet("Mixed", "Release", "Release Master Gold")]
    [string]$Configuration = "Mixed",
    [string]$Target = "XR_3DA"
)

$ErrorActionPreference = "Stop"
$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
$solution = Join-Path $repositoryRoot "src\engine.sln"
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "Visual Studio Installer could not be found."
}

$visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudio) {
    throw "A Visual Studio installation with the x64 C++ toolchain is required."
}

$developerPrompt = Join-Path $visualStudio "Common7\Tools\VsDevCmd.bat"
$restore = "msbuild `"$solution`" /m /t:Restore /p:RestorePackagesConfig=true"
$build = "msbuild `"$solution`" /m /t:$Target /p:Configuration=`"$Configuration`" /p:Platform=x64"
$command = "call `"$developerPrompt`" -arch=x64 -host_arch=x64 && $restore && $build"

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
