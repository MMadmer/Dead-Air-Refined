# Builds and runs the smoke test of the vendored upscaler SDKs (Externals\streamline,
# Externals\fidelityfx-sdk, Externals\fidelityfx-dx11) outside the engine build.
#
# The test links the CMake targets of cmake\XRay.Dependencies.cmake exactly as a renderer would and
# checks, on this machine's GPU: FSR 3.1 DX11 context create, three dispatches and destroy; the FFX
# DX12 loader, its upscaler and frame generation providers and one upscaler context; the NVIDIA
# signature of sl.interposer.dll, slInit through the delay-loaded import library and which of DLSS,
# DLSS-G, Reflex and PCL the adapter supports. Run it after every SDK update.
#
#   tools\qa\sdk\Run-SdkSmoke.ps1                        # Release
#   tools\qa\sdk\Run-SdkSmoke.ps1 -Configuration Debug   # the /MDd libraries
#
# It builds into build\qa-sdk-smoke, its own folder, so it never touches the engine's build
# directory and can run while the engine builds.
[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug")][string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..\..")).Path
$source = Join-Path $repo "tools\qa\sdk"
$build = Join-Path $repo "build\qa-sdk-smoke"

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "no Visual Studio with the x64 C++ tools" }
$devCmd = Join-Path $vs "Common7\Tools\VsDevCmd.bat"

New-Item -ItemType Directory -Force -Path $build | Out-Null
$bat = Join-Path $build "build.cmd"
Set-Content -LiteralPath $bat -Encoding ASCII -Value @(
    "@call `"$devCmd`" -arch=x64 -host_arch=x64 >nul"
    "@cmake -S `"$source`" -B `"$build`" -G `"Ninja Multi-Config`" `"-DCMAKE_CONFIGURATION_TYPES=Debug;Release`" -Wno-dev >nul || exit /b 1"
    "@cmake --build `"$build`" --config $Configuration || exit /b 1"
)
& cmd.exe /d /c "`"$bat`""
if ($LASTEXITCODE -ne 0) { throw "the smoke test did not build" }

$bin = Join-Path $build $Configuration
& (Join-Path $bin "sdk_smoke.exe")
$failed = $LASTEXITCODE -ne 0
& (Join-Path $bin "sdk_loader_smoke.exe")
$failed = $failed -or ($LASTEXITCODE -ne 0)
if ($failed) { exit 1 }
